#pragma once
// ===========================================================================
// 植生 F1: GPU カリング（compute）。D3D12 デバイスだけで動く（窓なしのヘッドレステストで回せる）。
// ---------------------------------------------------------------------------
//   ・専用ルートシグネチャ（メインの 62/64 DWORD には触れない。バッファはルート記述子、HZB だけ専用ヒープのテーブル）。
//   ・レイヤーごとの GPU リソース（インスタンス表 / チャンク表 / compact ストリーム / カウンタ / 間接描画引数）を持つ。
//   ・Cull() が「リセット → ビューごとのカリング → 引数の確定 → 描画用の状態遷移 + カウンタの読み戻しコピー」まで記録する。
//   ・描画側（FoliageRenderer）は Layer の VisibleView() / ArgsResource() / ArgsOffset() を使って ExecuteIndirect する。
//   ・式は FoliageMath.h の EvaluateInstance と一対一（tests/foliage_gpu_test.cpp が GPU = CPU を検証する）。
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "renderer/foliage/FoliageMath.h"
#include "renderer/foliage/FoliageTypes.h"

namespace dx12e::foliage
{

// レイヤーの GPU リソースを作るための記述（CPU 側の実体は呼び出し側が持ち続けてよいが、Create 中しか参照しない）
struct LayerGpuDesc
{
    const FoliageInstance* instances = nullptr;
    u32 instanceCount = 0;
    const FoliageChunk* chunks = nullptr;
    u32 chunkCount = 0;
    u32 numViews = 1;             // 1 = 主ビューのみ / 2..3 = + 影カスケード
    u32 slotsPerView = 1;         // S = variantCount × lodStride
    u32 lodStride = 1;            // 1 variant あたりの list 数（= 最大 LOD 数）
    u32 capMain = 131072;         // 主ビューの list あたりの容量（個）
    u32 capShadow = 32768;        // 影ビューの list あたりの容量（個）
    std::vector<u32> groupSlot;       // 描画グループ → list 番号（variant * lodStride + lod）
    std::vector<u32> groupIndexCount; // 描画グループのインデックス数（間接引数の IndexCountPerInstance）
};

// 1 回のカリング（ビュー 1 つぶん）
struct CullDispatch
{
    u32 view = 0;                 // 0 = 主ビュー / 1.. = 影カスケード
    CullParams p;
    bool hzb = false;             // 前フレーム HZB による遮蔽（主ビューのみ）
    DirectX::XMFLOAT4X4 prevVP{}; // 前フレームのジッタ付き viewProj（行ベクトル規約）
    f32 hzbW = 0, hzbH = 0;       // HZB mip0 の解像度
    u32 hzbMips = 0;
    f32 rectInflatePx = 6.0f;
};

struct LayerCounters
{
    bool valid = false;
    u32 numViews = 0, slotsPerView = 0;
    std::vector<u32> counts;      // [view * S + slot]（容量を超えた分も含む生の個数）
};

class FoliageCuller
{
public:
    class Layer;

    FoliageCuller();
    ~FoliageCuller();
    FoliageCuller(const FoliageCuller&) = delete;
    FoliageCuller& operator=(const FoliageCuller&) = delete;

    // shaderDir = .cso のあるフォルダ（末尾に区切りを含む）。frameCount = 同時に飛ばすフレーム数（読み戻しの多重化）。
    bool Initialize(ID3D12Device* device, const std::wstring& shaderDir, u32 frameCount, std::string* err = nullptr);
    void Shutdown();
    bool IsReady() const { return m_ready; }

    // 使用中の HZB リソース（前フレームの深度ピラミッド）。null なら以後の HZB 指定は無視される。
    // ★リソースは NON_PIXEL_SHADER_RESOURCE で読める状態で渡すこと。リソースが替わる（リサイズ）たびに呼ぶ。
    void SetHzbResource(ID3D12Resource* hzb, u32 mips);

    // レイヤーの GPU リソースを作る。アップロードのコピーを cmd へ記録する。
    // uploadKeepAlive にステージングが積まれる＝呼び出し側が GPU 完了後に解放すること（DeferredRelease へ渡す / WaitIdle 後に破棄）。
    std::unique_ptr<Layer> CreateLayer(const LayerGpuDesc& desc, ID3D12GraphicsCommandList* cmd,
                                       std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadKeepAlive,
                                       std::string* err = nullptr);

    // 1 レイヤーぶんのカリング一式を cmd へ記録する（compute）。終わると Layer は描画用の状態（VB / INDIRECT_ARGUMENT）になる。
    // restoreHeap: HZB 用に専用ヒープを張った後、これを SetDescriptorHeaps し直す（null なら何もしない）。
    // frameSlot: 0..frameCount-1（統計の読み戻しの多重化）。
    // ★cmd の compute 状態（ルートシグネチャ / PSO / ヒープ）を書き換える。グラフィックス側のバインドは壊さない。
    bool Cull(ID3D12GraphicsCommandList* cmd, Layer& layer, const Affine34& layerWorld,
              const std::vector<CullDispatch>& dispatches, u32 frameSlot, ID3D12DescriptorHeap* restoreHeap);

    // frameSlot の前回の Cull の結果カウンタを読む（そのスロットの GPU 完了後に呼ぶ）。
    static bool ReadCounters(Layer& layer, u32 frameSlot, LayerCounters& out);

    // GPU 時間の計測（任意）。Cull の前後に Begin/End を挟むのは呼び出し側。

    // ---- 診断 ----
    u32 FrameCount() const { return m_frameCount; }

    class Layer
    {
    public:
        ~Layer();
        u32 NumViews() const { return m_numViews; }
        u32 SlotsPerView() const { return m_S; }
        u32 GroupCount() const { return m_G; }
        u32 CapOf(u32 view) const { return view == 0 ? m_capMain : m_capShadow; }
        u32 ListBase(u32 view) const;   // インスタンス単位
        // slot の compact ストリーム（stride 64B, PER_INSTANCE 用）。全容量ぶんを指す（個数は間接引数の InstanceCount）。
        D3D12_VERTEX_BUFFER_VIEW VisibleView(u32 view, u32 slot) const;
        ID3D12Resource* ArgsResource() const { return m_args.Get(); }
        u64 ArgsOffset(u32 view, u32 group) const { return static_cast<u64>(view * m_G + group) * 20u; }
        ID3D12Resource* VisibleResource() const { return m_visible.Get(); }
        u64 GpuBytes() const { return m_gpuBytes; }
        u32 InstanceCount() const { return m_instanceCount; }
        f32 MaxScale() const { return m_maxScale; }
        bool CulledOnce() const { return m_culledOnce; }
        u32 Version() const { return m_version; }
        void SetVersion(u32 v) { m_version = v; }

    private:
        friend class FoliageCuller;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_src, m_chunks, m_groupSlot, m_visible, m_counters, m_args, m_chunkCounts;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_readback;   // frameCount 枚
        std::vector<bool> m_readbackPending;
        u32 m_instanceCount = 0, m_chunkCount = 0, m_numViews = 1, m_S = 1, m_lodStride = 1, m_G = 0;
        u32 m_capMain = 0, m_capShadow = 0;
        f32 m_maxScale = 1.0f;
        u64 m_gpuBytes = 0;
        u32 m_version = 0;
        bool m_culledOnce = false;
        D3D12_RESOURCE_STATES m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_STATES m_argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };

private:
    bool m_ready = false;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    u32 m_frameCount = 3;
};

} // namespace dx12e::foliage
