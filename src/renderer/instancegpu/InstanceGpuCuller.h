#pragma once
// ===========================================================================
// GPU 駆動のインスタンス群（設計 §4.2 = 4-3）: 永続バッファ + チャンク → インスタンスの 2 段カリング + LOD + 間接引数（compute）。
// ---------------------------------------------------------------------------
//   ・D3D12 デバイスだけで動く（窓なしのヘッドレステストで回せる）。専用ルートシグネチャ（メインの 62/64 DWORD には触れない）。
//     バッファは全部ルート記述子＝記述子ヒープは触らない（描画中の任意の位置から Cull を記録できる）。
//   ・群ごとに Group（永続: インスタンス表 / チャンク表 / 前フレーム表。作業用: 可視ストリーム / カウンタ / 間接引数）を持つ。
//     インスタンス表は「群の内容 or Transform が変わった時」だけ Upload し直す（毎フレームの memcpy はしない）。
//   ・Cull() は 1 ビュー（主ビュー / 深度プリパス / 影カスケード / スポット / ポイントの面）ぶんを記録する。
//     可視ストリームと間接引数は群に 1 組だけ＝次のビューの Cull が上書きする。★描画は Cull の直後に記録すること。
//   ・出力は list（LOD）ごとの連続区間 1 本: VisibleView()（slot1 = MeshInstanceData 64B）/ VisiblePrevView()（slot2 = 48B）。
//     ExecuteIndirect の引数は ArgsOffset(submesh, list)（StartInstanceLocation が list の先頭）。
//   ・式は InstanceGpuMath.h と一対一（tests/instance_gpu_test.cpp が GPU = CPU を検証する）。
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "renderer/instancegpu/InstanceGpuMath.h"

namespace dx12e::instgpu
{

// 1 ビューのカリング指示
struct CullView
{
    ViewParams view;
    f32 color[4] = {1, 1, 1, 1};   // 全インスタンス共通の色（MeshInstanceData.color）
    bool writePrev = false;        // 前フレームのストリーム（速度用）も出す
};

class InstanceGpuCuller
{
public:
    class Group;

    InstanceGpuCuller();
    ~InstanceGpuCuller();
    InstanceGpuCuller(const InstanceGpuCuller&) = delete;
    InstanceGpuCuller& operator=(const InstanceGpuCuller&) = delete;

    // shaderDir = .cso のあるフォルダ（末尾に区切りを含む）。frameCount = 同時に飛ばすフレーム数（定数リング / 読み戻しの多重化）。
    bool Initialize(ID3D12Device* device, const std::wstring& shaderDir, u32 frameCount, std::string* err = nullptr);
    void Shutdown();
    bool IsReady() const { return m_ready; }

    // 群の GPU リソースを作る（まだ空）。submeshCount = 描く下位メッシュ数 / indexCounts = [submesh * kLists + list] のインデックス数。
    std::unique_ptr<Group> CreateGroup(u32 submeshCount, const std::vector<u32>& indexCounts, std::string* err = nullptr);

    // インスタンス表を（再）アップロードする。cmd へコピーを記録し、古いバッファは DeferredRelease へ渡す（フライト中のフレームが読んでいても安全）。
    //   prev = nullptr: 前フレームは「現在と同じ」（速度 0。インスタンス表自身を前フレームとして読む）。
    //   prev != nullptr（count 個）: 前フレームの world（専用の 48B 表）。
    // 戻り値 false = VRAM 不足など。
    //   keepAlive != nullptr: ステージングをここへ積む（呼び出し側が GPU 完了後に解放。ヘッドレステスト用）。nullptr = DeferredRelease へ。
    bool Upload(Group& g, ID3D12GraphicsCommandList* cmd, const InstanceRecord* rec, u32 count, const PrevRecord* prev,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive = nullptr, std::string* err = nullptr);
    // 前フレームの表を「現在と同じ」へ戻す（動いた翌フレームに呼ぶ。バッファは遅延解放）。
    void ClearPrev(Group& g);

    // 同じビュー（同じ Cull 指示）を続けて描くときに Cull を省くための判定（直近の Cull と同じ内容で、必要な前フレームの出力も済んでいれば false）。
    static u64 KeyOf(const CullView& v);
    static bool NeedsCull(const Group& g, const CullView& v);

    // 1 ビューぶんのカリング一式（Reset → Count → Scan → Finalize → Emit）を cmd へ記録する。
    // 終わると Group の可視ストリームと間接引数は描画用の状態（VERTEX_AND_CONSTANT_BUFFER / INDIRECT_ARGUMENT）。
    // frameSlot: 0..frameCount-1。countersReadback = true で個数を読み戻しバッファへコピー（統計用）。
    // ★cmd の compute 状態（ルートシグネチャ / PSO）を書き換える。グラフィックスのバインドは壊さない（呼び出し側は PSO の記憶を捨てること）。
    bool Cull(ID3D12GraphicsCommandList* cmd, Group& g, const CullView& v, u32 frameSlot, bool countersReadback);

    // frameSlot の前回の読み戻しコピーの結果（list ごとの個数）。そのスロットの GPU 完了後に呼ぶ。
    static bool ReadCounters(Group& g, u32 frameSlot, u32 outCounts[kLists]);

    class Group
    {
    public:
        ~Group();
        u32 Count() const { return m_count; }
        u32 ChunkCount() const { return m_chunkCount; }
        u32 SubmeshCount() const { return m_submeshCount; }
        bool HasData() const { return m_inst != nullptr && m_count > 0; }
        bool HasPrev() const { return m_prev != nullptr; }
        bool CulledOnce() const { return m_culledOnce; }
        u64 GpuBytes() const { return m_gpuBytes; }
        // 全容量（N × 64B）を指す slot1 のビュー。個数は間接引数の InstanceCount、先頭は StartInstanceLocation。
        D3D12_VERTEX_BUFFER_VIEW VisibleView() const;
        D3D12_VERTEX_BUFFER_VIEW VisiblePrevView() const;   // slot2（Cull で writePrev=true だったときだけ有効）
        ID3D12Resource* ArgsResource() const { return m_args.Get(); }
        u64 ArgsOffset(u32 submesh, u32 list) const { return static_cast<u64>(submesh * kLists + list) * 20u; }
        ID3D12Resource* VisibleResource() const { return m_out.Get(); }
        ID3D12Resource* VisiblePrevResource() const { return m_outPrev.Get(); }
        // 直近の Cull のパラメータのハッシュ（同じビューを続けて描くとき Cull を省くための印）。0 = 無効。
        u64 LastCullKey() const { return m_lastCullKey; }
        void InvalidateCullKey() { m_lastCullKey = 0; }

    private:
        friend class InstanceGpuCuller;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_inst, m_chunks, m_prev, m_indexCounts;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_out, m_outPrev, m_chunkCounts, m_counters, m_args;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_readback;
        std::vector<bool> m_readbackPending;
        u32 m_count = 0, m_chunkCount = 0, m_submeshCount = 1;
        u64 m_gpuBytes = 0;
        u64 m_lastCullKey = 0;
        bool m_culledOnce = false;
        bool m_outPrevValid = false;   // 直近の Cull で outPrev を書いた
        D3D12_RESOURCE_STATES m_outState = D3D12_RESOURCE_STATE_COMMON, m_outPrevState = D3D12_RESOURCE_STATE_COMMON,
                              m_argsState = D3D12_RESOURCE_STATE_COMMON;
        bool m_chunkCountsUav = false, m_countersUav = false;   // COMMON → UAV へ遷移済み
    };

private:
    bool m_ready = false;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    u32 m_frameCount = 3;
};

} // namespace dx12e::instgpu
