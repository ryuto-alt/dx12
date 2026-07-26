#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <unordered_map>
#include <vector>

#include "core/Types.h"
#include "graphics/FrameResources.h"
#include "renderer/AccelerationStructure.h"

namespace dx12e
{
class GraphicsDevice;
class Mesh;

// DXR の加速構造（BLAS / TLAS）のライフサイクルを持つクラス（計画09 Step 1）。
//
// 契約:
//  - BLAS は **Mesh 単位・LOD0 固定**でキャッシュする。LOD が切り替わるたびに
//    BLAS を作り直すのは論外だし、影 / GI の精度は LOD0 で持つべき。
//  - TLAS は **毎フレーム全再構築**（NVIDIA のベストプラクティス:
//    "Build the Top-Level Acceleration Structure rather than Update"）。
//  - **半透明とスキンドは入れない。** 半透明を入れると any-hit が要る（2〜10 倍遅い）。
//    スキンドは変形後の頂点が GPU のどこにも存在しない（頂点シェーダ内スキニング）ので
//    compute スキニング（計画09 Step 4）を作るまで原理的に無理。
//    → 影は「TLAS に入るもの = RT / 入らないもの = CSM」の**排他ハイブリッド**にする
//      （判定は DrawItem.h の IsRaytracedItem() に一本化してある）。
//  - 全ジオメトリは `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`。any-hit を一切使わない。
//
// 同期はすべて nullptr の UAV バリアで行う（AS は状態遷移できない）。RtUavBarrier() 参照。
//
// ★ECS に依存しない。DrawItem の走査は Application 側でやり、ここへは
//   「メッシュ + ワールド行列」だけを流し込む（DecalSystem と同じ流儀。
//   Renderer モジュールに entt を持ち込まないため）。
class RaytracingScene
{
public:
    // TLAS へ入れるインスタンスの上限。262144（Application::kMaxInstances）をそのまま流すと
    // INSTANCE_DESC だけで 16.8MB/フレームの UPLOAD 帯域になる。
    static constexpr u32 kMaxRtInstances = 32768;

    struct Stats
    {
        u32 instances          = 0;   // TLAS に入ったインスタンス数
        u32 blasCount          = 0;   // キャッシュ中の BLAS 個数
        u32 blasBuiltThisFrame = 0;
        u32 skippedSkinned     = 0;   // スキンドで除外した DrawItem 数（CSM が担当する）
        u32 skippedTransparent = 0;   // 半透明で除外した DrawItem 数（同上）
        u32 droppedOverLimit   = 0;   // 上限超過で切った数
        u64 blasBytes          = 0;   // BLAS 実サイズ合計（PrebuildInfo の実測値）
        u64 blasTriangles      = 0;   // BLAS に入っている三角形の総数
        u64 tlasBytes          = 0;
        u64 scratchBytes       = 0;
        u64 instanceDescBytes  = 0;
    };

    // Gate 3（Device5）と Gate 6（AS バッファの確保）を見る。false = DXR を諦める。
    // ★ここで小さい AS バッファを 1 本確保して、D3D12MA が
    //   D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE を初期状態に取れることを確認する
    //   （計画09 §12 の未確認項目 7）。
    bool Initialize(GraphicsDevice& device);
    void Shutdown();

    // シーンを作り直したとき（Play/Stop / シーン切替）に呼ぶ。
    // entt は entity id を再利用するし、Mesh* も解放後に同じアドレスへ再確保され得るので、
    // 内容比較だけでは「別物なのに一致した」を防げない（00-COORDINATION N30 と同じ話）。
    void Invalidate();

    bool IsInitialized() const { return m_initialized; }

    // --- 毎フレームの流し込み ---------------------------------------------
    // BeginFrame → AddInstance × N → Build の順で 1 回だけ呼ぶ。
    void BeginFrame(const DirectX::XMFLOAT3& cameraPos, u32 skippedSkinned, u32 skippedTransparent);
    void AddInstance(const Mesh* mesh, const DirectX::XMMATRIX& world,
                     const DirectX::XMFLOAT3& center);

    struct BuildDesc
    {
        u32 frameIndex   = 0;
        u32 maxInstances = kMaxRtInstances;
        // 1 フレームで新規構築する BLAS の上限。シーンロード直後のスパイクを分散させる。
        u32 maxBlasBuildsPerFrame = 64;
    };
    // BLAS の遅延構築と TLAS 再構築を cmd へ積む。
    // 戻り値: この後 RayQuery を投げてよいか（TLAS が有効か）。
    bool Build(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd, const BuildDesc& d);

    bool IsReady() const { return m_tlasValid; }
    D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress() const { return m_tlas.GetGpuAddress(); }
    const Stats& GetStats() const { return m_stats; }

private:
    struct BlasEntry
    {
        RtBuffer as;
        u32 geometryVersion = 0;   // Mesh::GetGeometryVersion()（VB 再作成の検出）
        D3D12_GPU_VIRTUAL_ADDRESS vbAddress = 0;   // 念のためのアドレス一致確認
        D3D12_GPU_VIRTUAL_ADDRESS ibAddress = 0;
        u32 indexCount  = 0;
        u32 triangles   = 0;
        u64 sizeInBytes = 0;
        u64 lastUsedFrame = 0;
    };

    // 既にあれば返す。無ければ cmd にビルドを積んで作る。作れなければ nullptr。
    const BlasEntry* EnsureBlas(GraphicsDevice& device, ID3D12GraphicsCommandList4* cmd,
                                const Mesh* mesh, u32& budget);

    std::unordered_map<const Mesh*, BlasEntry> m_blas;
    RtBuffer m_blasScratch;                                 // BLAS ビルド用（連続ビルドは UAV バリアで直列化）
    RtBuffer m_tlas;
    RtBuffer m_tlasScratch;
    RtBuffer m_instanceDescs[FrameResources::kFrameCount];   // UPLOAD リング（CPU が毎フレーム書く）

    struct PendingInstance
    {
        const Mesh*         mesh;
        DirectX::XMFLOAT4X4 world;    // ノード変換合成済みのメッシュワールド
        float               distSq;   // カメラからの距離²（上限超過時の足切り用）
    };
    std::vector<PendingInstance> m_pending;
    DirectX::XMFLOAT3 m_cameraPos{0, 0, 0};

    bool  m_tlasValid      = false;
    bool  m_initialized    = false;
    bool  m_cmdList4Failed = false;   // Gate 4 に落ちた（以後 no-op）
    u64   m_frameCounter   = 0;
    Stats m_stats;
};

} // namespace dx12e
