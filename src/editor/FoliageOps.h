#pragma once
// ===========================================================================
// 植生 F1: 散布 / ブラシの編集操作（MCP とエディタのツール窓が同じ関数を使う）。
// ---------------------------------------------------------------------------
// 表面（地形 / 平面 / メッシュ表面）の問い合わせをワールド XZ の関数にして、FoliageScatter（純ロジック）へ渡す。
// レイヤー（FoliageLayer を持つエンティティ）のローカル空間への往復（Transform）もここで解決する。
// 編集は必ず「新しい FoliageInstanceSet を作って差し替える」（コピーオンライト。Undo は古いポインタへ戻すだけ）。
// ===========================================================================
#include <functional>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include <entt/entt.hpp>

#include "core/Types.h"
#include "ecs/Components.h"
#include "renderer/DrawItem.h"
#include "renderer/foliage/FoliageScatter.h"

namespace dx12e::foliage_ops
{

// 世界 ↔ レイヤーのローカル（行ベクトル規約の world 行列とその逆）
struct LayerXform
{
    DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();
    DirectX::XMMATRIX inv = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 ToWorld(f32 x, f32 y, f32 z) const;
    DirectX::XMFLOAT3 ToLocal(f32 x, f32 y, f32 z) const;
    DirectX::XMFLOAT3 NormalToLocal(const DirectX::XMFLOAT3& n) const;
    f32 UniformScale() const;   // 一様スケールの近似（ブラシ半径の換算用）
};
LayerXform MakeLayerXform(const entt::registry& reg, entt::entity layerEntity);

// 世界の表面（(wx,wz) → 高さ・法線・スプラット重み）。false = そこに表面が無い。
using WorldSurface = std::function<bool(f32 wx, f32 wz, foliage::SurfaceSample& out)>;

struct SurfaceRequest
{
    std::string mode = "auto";                 // auto | terrain | flat | raycast
    entt::entity terrain = entt::null;         // null = 最初の地形（terrain / auto のとき）
    f32 flatHeight = 0.0f;                     // flat の高さ（ワールド Y）
    f32 rayFrom = 500.0f;                      // raycast のレイ開始の高さ
    bool hasRegion = false;
    DirectX::XMFLOAT4 region{0, 0, 0, 0};      // [x0,z0,x1,z1]（ワールド XZ）
};
struct BuiltSurface
{
    WorldSurface fn;
    std::string desc;                          // "terrain:<name>" / "flat" / "raycast"
    DirectX::XMFLOAT4 region{0, 0, 0, 0};      // 使う領域（指定が無く地形なら地形全面）
    bool regionKnown = false;
};
// 失敗は err に理由（日本語）を入れて false。drawItems は raycast のブロードフェーズ用（null でも動く）。
bool BuildSurface(entt::registry& reg, const std::vector<DrawItem>* drawItems, entt::entity layerEntity,
                  const SurfaceRequest& req, BuiltSurface& out, std::string& err);

// ワールドの表面 → レイヤーのローカル座標の SurfaceFn
foliage::SurfaceFn ToLocalSurface(const LayerXform& lx, WorldSurface world);

struct EditResult
{
    foliage::ScatterStats stats;
    u32 before = 0, after = 0, changed = 0;
    double msScatter = 0, msChunks = 0;
};

// 領域（ワールド XZ の矩形）へ散布。replace = 既存を捨てる。layer._set を差し替える（_needsSave が立つ）。
EditResult ScatterLayer(FoliageLayer& layer, const LayerXform& lx, const foliage::ScatterParams& p,
                        const DirectX::XMFLOAT4& regionWorld, const WorldSurface& surface, bool replace);
// 円ブラシで追加（中心はワールド XZ。points の各点に順に置く）。
EditResult BrushAdd(FoliageLayer& layer, const LayerXform& lx, const foliage::ScatterParams& p,
                    const std::vector<std::pair<f32, f32>>& centersWorld, f32 radiusWorld, const WorldSurface& surface);
// 円ブラシで削除。
EditResult BrushErase(FoliageLayer& layer, const LayerXform& lx, const std::vector<std::pair<f32, f32>>& centersWorld,
                      f32 radiusWorld, f32 fraction, u32 seed);

} // namespace dx12e::foliage_ops
