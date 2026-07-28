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

    // レイのヒット点で頂点属性とマテリアルを引くための表（計画09 Step 5 / バインドレス）。
    // TLAS の InstanceID がそのままこの配列の添字になる。
    // ★shaders/raytracing/RtBindless.hlsli の GeometryInfo とバイト単位で一致させること。
    // ★マテリアル用の別テーブルは作らない。このエンジンは 1 Mesh = 1 Material なので
    //   分けても重複排除の利得が無く、ヒット点での依存ロードが 1 段増えるだけになる
    //   （NVIDIA: "Avoid indirections in accessing index, vertex, and material data"）。
    struct GeometryInfo
    {
        u32 vbSrvIndex;        // 属性用 VB（96B インターリーブ）の raw SRV。0xFFFFFFFF = 無効
        u32 ibSrvIndex;        // u32 インデックスバッファの raw SRV
        u32 baseColorSrvIndex; // アルベドテクスチャの SRV。0xFFFFFFFF = テクスチャ無し
        u32 flags;             // bit0 = スキンド（位置は変形後バッファ / 属性は元 VB）
    };
    static_assert(sizeof(GeometryInfo) == 16, "RtBindless.hlsli の GeometryInfo と一致させること");
    static constexpr u32 kGeomFlagSkinned = 1u;

    struct Stats
    {
        u32 instances          = 0;   // TLAS に入ったインスタンス数
        u32 blasCount          = 0;   // キャッシュ中の BLAS 個数
        u32 blasBuiltThisFrame = 0;
        u32 skippedSkinned     = 0;   // スキンドで除外した DrawItem 数（compute スキニングが無い時のみ）
        u32 skippedTransparent = 0;   // 半透明で除外した DrawItem 数（CSM が担当する）
        u32 skinnedInstances   = 0;   // TLAS に入ったスキンドインスタンス数
        u32 skinnedRebuilds    = 0;   // このフレームで再構築したスキンド BLAS 数
        u32 skinnedStale       = 0;   // 予算切れで前フレームの BLAS を流用した数
        u64 skinnedBlasBytes   = 0;   // スキンド BLAS の実サイズ合計
        u64 skinnedTriangles   = 0;   // 同 三角形数
        // バインドレス（計画09 Step 5）
        u32 geoInfoWritten     = 0;   // GeometryInfo を書いたインスタンス数
        u32 geoInfoWithAlbedo  = 0;   // うちアルベドテクスチャの SRV が有効だった数
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
                     const DirectX::XMFLOAT3& center,
                     const GeometryInfo& geo = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u});

    // スキンド用（計画09 Step 4）。compute スキニングが書いた変形後頂点バッファを指定する。
    // key は「エンティティ × サブメッシュ」で一意な値を呼び出し側が作って渡すこと。
    //   ★Mesh* をキーにしてはいけない。ResourceManager がモデルパス単位で Mesh* を
    //     キャッシュしていて複数エンティティが共有するので、同じキャラ 10 体が
    //     全員同じポーズの BLAS を共有してしまう。
    // 変形後頂点はオブジェクトローカル空間なので、world は静的と同じものを渡す。
    // poseHash が前フレームと同じなら BLAS の再構築も省く（変形後頂点が変わっていないため）。
    void AddSkinnedInstance(u64 key, const Mesh* mesh,
                            D3D12_GPU_VIRTUAL_ADDRESS deformedVb, u32 vertexCount, u64 poseHash,
                            const DirectX::XMMATRIX& world, const DirectX::XMFLOAT3& center,
                            const GeometryInfo& geo = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u});

    struct BuildDesc
    {
        u32 frameIndex   = 0;
        u32 maxInstances = kMaxRtInstances;
        // 1 フレームで新規構築する BLAS の上限。シーンロード直後のスパイクを分散させる。
        u32 maxBlasBuildsPerFrame = 64;
        // 1 フレームで再構築するスキンド BLAS の三角形数の上限。
        // ★根拠: BLAS のリビルドは実測 ~100M tris/sec（SOTTR / GTC 2019）。RT の AS 構築に
        //   割ける予算を 2ms とすると 20 万三角形が上限になる。超過分はこのフレームの
        //   再構築を見送り、前フレームの BLAS をそのまま使う（＝ポーズが 1 フレーム古くなる）。
        //   Metro Exodus も 2 万インスタンス中 5〜6 千が古いまま出荷している。
        // ponytail: これを恒常的に超えるようになったら refit（PREFER_FAST_TRACE|ALLOW_UPDATE +
        //   PERFORM_UPDATE）へ移行すること。refit は ~1000M tris/sec で 10 倍速い。
        u32 maxSkinnedTrianglesPerFrame = 200000;
    };
    // BLAS の遅延構築と TLAS 再構築を cmd へ積む。
    // 戻り値: この後 RayQuery を投げてよいか（TLAS が有効か）。
    bool Build(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd, const BuildDesc& d);

    bool IsReady() const { return m_tlasValid; }
    D3D12_GPU_VIRTUAL_ADDRESS GetTlasAddress() const { return m_tlas.GetGpuAddress(); }
    // GeometryInfo テーブルの GPU アドレス（ルート SRV で渡す）。0 = このフレームは無効。
    // ★TLAS と同じくルート SRV にする。毎フレーム作り直してもディスクリプタを
    //   張り直さずに済むのが利点（RtCommon.hlsli の TLAS と同じ理由）。
    D3D12_GPU_VIRTUAL_ADDRESS GetGeometryInfoAddress() const { return m_geoInfoAddress; }
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
        u64 poseHash    = 0;   // スキンドのみ。前フレームと同じなら再構築を省く
    };

    // 既にあれば返す。無ければ cmd にビルドを積んで作る。作れなければ nullptr。
    const BlasEntry* EnsureBlas(GraphicsDevice& device, ID3D12GraphicsCommandList4* cmd,
                                const Mesh* mesh, u32& budget);

    // スキンド用。毎フレーム同じ AS バッファへ再構築する（変形後頂点が毎フレーム変わるため）。
    // triangleBudget を使い切ったら再構築を見送り、既存の BLAS をそのまま返す。
    const BlasEntry* EnsureSkinnedBlas(GraphicsDevice& device, ID3D12GraphicsCommandList4* cmd,
                                       u64 key, const Mesh* mesh,
                                       D3D12_GPU_VIRTUAL_ADDRESS deformedVb, u32 vertexCount,
                                       u64 poseHash, u32& triangleBudget);

    std::unordered_map<const Mesh*, BlasEntry> m_blas;
    std::unordered_map<u64, BlasEntry>         m_skinnedBlas;   // キーは呼び出し側が作る u64
    RtBuffer m_blasScratch;                                 // BLAS ビルド用（連続ビルドは UAV バリアで直列化）
    RtBuffer m_tlas;
    RtBuffer m_tlasScratch;
    RtBuffer m_instanceDescs[FrameResources::kFrameCount];   // UPLOAD リング（CPU が毎フレーム書く）
    RtBuffer m_geoInfos[FrameResources::kFrameCount];        // 同上（GeometryInfo テーブル）
    D3D12_GPU_VIRTUAL_ADDRESS m_geoInfoAddress = 0;

    struct PendingInstance
    {
        const Mesh*         mesh;
        DirectX::XMFLOAT4X4 world;    // ノード変換合成済みのメッシュワールド
        float               distSq;   // カメラからの距離²（上限超過時の足切り用）
        // --- スキンドのみ（skinnedKey != 0 で判別）---
        u64                       skinnedKey = 0;
        D3D12_GPU_VIRTUAL_ADDRESS deformedVb = 0;
        u32                       vertexCount = 0;
        u64                       poseHash = 0;
        // ★ここに持たせるのが必須。m_pending は Build 中に 2 回並べ替えられ、
        //   予算切れで要素も落ちるので、呼び出し順から添字を復元できない。
        GeometryInfo              geo{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u};
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
