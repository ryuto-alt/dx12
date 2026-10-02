#pragma once
// ===========================================================================
// GPU 駆動のインスタンス群（設計 docs/SCENE_FORMAT_DESIGN.md §4.2 = 4-3）: Application 側の状態。実装は ApplicationInstanceGpu.cpp。
// ---------------------------------------------------------------------------
//   ・しきい値（settings.json "instance_gpu_threshold"。0 = 無効。既定 1024）以上のインスタンスを持つ不透明の群だけが GPU 経路に乗る。
//     BuildDrawList はそれらを DrawItem へ展開せず `frame` へ積み、描画パス（本体 / 深度プリパス / 速度 / CSM / スポット / ポイント）が
//     InstanceGpuCuller で群ごとにカリング → ExecuteIndirect する。
//   ・★無効（0）/ 条件を満たさない群 / 知覚・パストレーサーの要求フレームは従来の DrawItem 展開のまま（絵・挙動は不変）。
// ===========================================================================
#include <DirectXMath.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>

#include "core/Types.h"
#include "renderer/DrawItem.h"
#include "renderer/instancegpu/InstanceGpuCuller.h"

namespace dx12e
{
struct MeshRenderer;
namespace instgroup { struct InstanceSet; }

// 群ごとの永続状態（entity をキー）
struct GpuInstGroupRt
{
    std::unique_ptr<instgpu::InstanceGpuCuller::Group> gpu;
    // アップロード済みの内容の印（これが今フレームの群と違えば Upload し直す）
    u64 uploadedSetId = 0;
    u64 uploadedWorldKey = 0;
    u64 uploadedBoundsHash = 0;
    u32 uploadedCount = 0;
    // サブメッシュ構成の印（メッシュのポインタ + インデックス数）。変われば Group を作り直す
    u64 meshSig = 0;
    u32 submeshCount = 0;
    u32 maxList = 0;
    instgpu::LocalBounds bounds;
    u64 lastFrame = 0;     // 最後に使ったフレーム（古い群の掃除用）
    bool failed = false;   // 作成 / アップロードに失敗した（以後このフレームは描かない。次の内容変化で再挑戦）
    u32 lastCounts[instgpu::kLists] = {};   // 主ビューの LOD ごとの可視数（読み戻し。数フレーム遅れ）
    bool countsValid = false;
};

// 今フレーム GPU 経路に乗る群（BuildDrawList が積む）
struct GpuInstFrameGroup
{
    entt::entity e = entt::null;
    const MeshRenderer* renderer = nullptr;
    std::shared_ptr<instgroup::InstanceSet> set;   // 実体（このフレームの間は保持）
    u32 count = 0;
    DirectX::XMFLOAT4X4 groupWorld{};
    bool moved = false;                            // 前フレームから群が動いた（速度用に前フレームのワールドが要る）
    DirectX::XMFLOAT4X4 prevGroupWorld{};
    u64 guid = 0;
};

// ピッキング用の球の表（群の内容が変わらない間キャッシュ。ピックが起きた群だけ作る）
struct GpuInstPickCache
{
    u64 setId = 0;
    u64 worldKey = 0;
    instgpu::LocalBounds lb;
    std::vector<instgpu::InstanceRecord> recs;
};

struct GpuInstStats
{
    u32 groups = 0;             // 今フレーム GPU 経路の群の数
    u32 instances = 0;          // その合計インスタンス数
    u32 culls = 0;              // 今フレームに記録したカリング（ビュー × 群）
    u32 culled_skipped = 0;     // 同じビューの続きで省いたカリング
    u32 draws = 0;              // ExecuteIndirect の数
    u32 uploads = 0;            // 今フレームのアップロード数
    u64 gpuBytes = 0;
    u32 visibleMain[instgpu::kLists] = {};   // 主ビューの LOD ごとの可視数（数フレーム遅れの読み戻し）
    bool visibleValid = false;
    u32 cpuFallbackGroups = 0;  // しきい値を超えたが GPU 経路に乗れず従来の展開になった群（理由は reasons）
    std::string reasons;
};

struct GpuInstState
{
    std::unique_ptr<instgpu::InstanceGpuCuller> culler;
    bool unavailable = false;                // 初期化に失敗した（.cso が無い等）。以後は試さない
    u32 threshold = 1024;                    // settings.json "instance_gpu_threshold"（0 = 無効）
    std::unordered_map<u32, GpuInstGroupRt> groups;
    std::unordered_map<u32, GpuInstPickCache> pick;   // ピッキング用の球の表
    std::vector<DrawItem> overlayItems;      // エディタの輪郭用（描画リスト + 選択 / ホバー中の群の展開。必要なフレームだけ作る）
    std::vector<GpuInstFrameGroup> frame;    // 今フレーム GPU 経路の群（BuildDrawList が毎フレーム作り直す）
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> cmdSig;   // DrawIndexedInstanced 1 本
    DirectX::XMFLOAT3 camPos{0, 0, 0};       // LOD の基準（メインカメラ位置。BuildDrawList で確定）
    GpuInstStats stats;                      // 今フレーム（BuildDrawList でリセット）
    GpuInstStats lastStats;                  // 前フレームの確定値（perf_stats が読む）
    bool forceCpuThisFrame = false;          // 知覚 / パストレーサーの要求フレーム: 従来の展開にする
    u64 frameCounter = 0;
};

} // namespace dx12e
