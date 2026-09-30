#include "editor/FoliageOps.h"

#include <algorithm>
#include <chrono>

#include "editor/ScenePick.h"
#include "renderer/foliage/FoliageLayerOps.h"
#include "terrain/HeightField.h"
#include "terrain/TerrainSplatMap.h"

namespace dx12e::foliage_ops
{
using namespace DirectX;

XMFLOAT3 LayerXform::ToWorld(f32 x, f32 y, f32 z) const
{
    XMFLOAT3 o;
    XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorSet(x, y, z, 1), world));
    return o;
}
XMFLOAT3 LayerXform::ToLocal(f32 x, f32 y, f32 z) const
{
    XMFLOAT3 o;
    XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorSet(x, y, z, 1), inv));
    return o;
}
XMFLOAT3 LayerXform::NormalToLocal(const XMFLOAT3& n) const
{
    // 直交 + 一様スケールの Transform 前提（逆行列の 3x3 を掛けて正規化）
    XMFLOAT3 o;
    XMStoreFloat3(&o, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&n), inv)));
    return o;
}
f32 LayerXform::UniformScale() const { return XMVectorGetX(XMVector3Length(world.r[0])); }

LayerXform MakeLayerXform(const entt::registry& reg, entt::entity e)
{
    LayerXform x;
    x.world = ComputeWorldMatrix(reg, e);
    XMVECTOR det;
    x.inv = XMMatrixInverse(&det, x.world);
    return x;
}

bool BuildSurface(entt::registry& reg, const std::vector<DrawItem>* drawItems, entt::entity layerEntity,
                  const SurfaceRequest& req, BuiltSurface& out, std::string& err)
{
    out = {};
    entt::entity terr = req.terrain;
    const bool wantTerrain = (req.mode == "terrain" || req.mode == "auto");
    if (wantTerrain && terr == entt::null)
        for (auto [ent, t] : reg.view<Terrain>().each()) { terr = ent; break; }
    if (req.mode == "terrain" && terr == entt::null)
    {
        err = "シーンに地形がありません（dx12_terrain_create で作るか、surface に flat / raycast を指定してください）";
        return false;
    }
    if (req.hasRegion) { out.region = req.region; out.regionKnown = true; }

    if (req.mode == "flat" || (req.mode == "auto" && terr == entt::null))
    {
        const f32 h = req.flatHeight;
        out.desc = "flat";
        out.fn = [h](f32, f32, foliage::SurfaceSample& s) { s.y = h; s.nx = 0; s.ny = 1; s.nz = 0; return true; };
        return true;
    }
    if (req.mode == "raycast")
    {
        const f32 top = req.rayFrom;
        out.desc = "raycast";
        entt::registry* regp = &reg;
        out.fn = [regp, drawItems, top, layerEntity](f32 wx, f32 wz, foliage::SurfaceSample& s)
        {
            const auto hits = RaycastSceneRay(*regp, drawItems, {wx, top, wz}, {0, -1, 0}, top + 2000.0f);
            for (const auto& h : hits)
            {
                if (h.entity == layerEntity || h.isIcon) continue;
                s.y = h.worldPos.y; s.nx = h.worldNormal.x; s.ny = h.worldNormal.y; s.nz = h.worldNormal.z;
                return true;
            }
            return false;
        };
        return true;
    }
    if (!reg.valid(terr) || !reg.all_of<Terrain>(terr)) { err = "地形のエンティティが無効です"; return false; }
    Terrain& t = reg.get<Terrain>(terr);
    if (!t._hf || !t._hf->IsValid()) { err = "地形の高さ配列が無効です（dx12_get_log でロードエラーを確認してください）"; return false; }
    const XMFLOAT3 tpos = reg.all_of<Transform>(terr) ? reg.get<Transform>(terr).position : XMFLOAT3{0, 0, 0};
    std::shared_ptr<HeightField> hf = t._hf;
    std::shared_ptr<TerrainSplatMap> splat = t._splat;
    const NameTag* nt = reg.try_get<NameTag>(terr);
    out.desc = "terrain:" + (nt ? nt->name : std::string("Terrain"));
    if (!out.regionKnown)
    {
        const f32 hs = hf->HalfSize();
        out.region = {tpos.x - hs, tpos.z - hs, tpos.x + hs, tpos.z + hs};
        out.regionKnown = true;
    }
    out.fn = [hf, splat, tpos](f32 wx, f32 wz, foliage::SurfaceSample& s)
    {
        const f32 lx = wx - tpos.x, lz = wz - tpos.z;
        const f32 hs = hf->HalfSize();
        if (lx < -hs || lx > hs || lz < -hs || lz > hs) return false;
        s.y = hf->SampleHeight(lx, lz) + tpos.y;
        const auto n = hf->SampleNormal(lx, lz);
        s.nx = n.x; s.ny = n.y; s.nz = n.z;
        if (splat && splat->IsValid())
        {
            const i32 tx = splat->LocalToTexelX(lx, hf->WorldSize()), tz = splat->LocalToTexelZ(lz, hf->WorldSize());
            const size_t idx = (static_cast<size_t>(tz) * splat->Size() + static_cast<size_t>(tx)) * 4;
            for (int k = 0; k < 4; ++k) s.splat[k] = static_cast<f32>(splat->Pixels()[idx + k]) / 255.0f;
        }
        return true;
    };
    return true;
}

foliage::SurfaceFn ToLocalSurface(const LayerXform& lx, WorldSurface world)
{
    return [lx, world](f32 x, f32 z, foliage::SurfaceSample& out)
    {
        const auto w = lx.ToWorld(x, 0.0f, z);
        foliage::SurfaceSample s;
        if (!world(w.x, w.z, s)) return false;
        const auto p = lx.ToLocal(w.x, s.y, w.z);
        out = s;
        out.y = p.y;
        const auto n = lx.NormalToLocal({s.nx, s.ny, s.nz});
        out.nx = n.x; out.ny = n.y; out.nz = n.z;
        return true;
    };
}

namespace
{
double MsBetween(std::chrono::high_resolution_clock::time_point a, std::chrono::high_resolution_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}
} // namespace

EditResult ScatterLayer(FoliageLayer& layer, const LayerXform& lx, const foliage::ScatterParams& p, const XMFLOAT4& regionW,
                        const WorldSurface& surface, bool replace)
{
    EditResult r;
    foliage::ScatterRegion rg;
    {
        f32 x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
        const f32 cx[2] = {regionW.x, regionW.z}, cz[2] = {regionW.y, regionW.w};
        for (int i = 0; i < 4; ++i)
        {
            const auto q = lx.ToLocal(cx[i & 1], 0.0f, cz[i >> 1]);
            x0 = std::min(x0, q.x); x1 = std::max(x1, q.x); z0 = std::min(z0, q.z); z1 = std::max(z1, q.z);
        }
        rg.x0 = x0; rg.z0 = z0; rg.x1 = x1; rg.z1 = z1;
    }
    if (!replace) foliage::EnsureLoaded(layer);
    auto set = std::make_shared<foliage::FoliageInstanceSet>();
    if (!replace && layer._set) set->instances = layer._set->instances;
    r.before = (!replace && layer._set) ? layer._set->Count() : (layer._set ? layer._set->Count() : 0u);
    const auto t0 = std::chrono::high_resolution_clock::now();
    r.stats = foliage::ScatterPoisson(p, rg, ToLocalSurface(lx, surface), set->instances,
                                      (!replace && layer._set) ? &layer._set->instances : nullptr);
    const auto t1 = std::chrono::high_resolution_clock::now();
    set->Rebuild();
    const auto t2 = std::chrono::high_resolution_clock::now();
    r.msScatter = MsBetween(t0, t1);
    r.msChunks = MsBetween(t1, t2);
    r.after = set->Count();
    r.changed = r.stats.accepted;
    foliage::ReplaceSet(layer, set);
    return r;
}

EditResult BrushAdd(FoliageLayer& layer, const LayerXform& lx, const foliage::ScatterParams& p,
                    const std::vector<std::pair<f32, f32>>& centers, f32 radius, const WorldSurface& surface)
{
    EditResult r;
    foliage::EnsureLoaded(layer);
    auto set = std::make_shared<foliage::FoliageInstanceSet>();
    if (layer._set) set->instances = layer._set->instances;
    r.before = set->Count();
    const foliage::SurfaceFn local = ToLocalSurface(lx, surface);
    const f32 rl = radius / std::max(1e-3f, lx.UniformScale());
    foliage::ScatterParams sp = p;
    u32 k = 0;
    for (const auto& c : centers)
    {
        const auto lc = lx.ToLocal(c.first, 0.0f, c.second);
        foliage::ScatterRegion rg;
        rg.circle = true; rg.cx = lc.x; rg.cz = lc.z; rg.radius = rl;
        sp.seed = p.seed + k++ * 7919u;
        const foliage::ScatterStats st = foliage::ScatterPoisson(sp, rg, local, set->instances, &set->instances);
        r.stats.accepted += st.accepted;
        r.stats.candidates += st.candidates;
    }
    set->Rebuild();
    r.after = set->Count();
    r.changed = r.after - r.before;
    foliage::ReplaceSet(layer, set);
    return r;
}

EditResult BrushErase(FoliageLayer& layer, const LayerXform& lx, const std::vector<std::pair<f32, f32>>& centers, f32 radius,
                      f32 fraction, u32 seed)
{
    EditResult r;
    foliage::EnsureLoaded(layer);
    auto set = std::make_shared<foliage::FoliageInstanceSet>();
    if (layer._set) set->instances = layer._set->instances;
    r.before = set->Count();
    const f32 rl = radius / std::max(1e-3f, lx.UniformScale());
    u32 k = 0;
    for (const auto& c : centers)
    {
        const auto lc = lx.ToLocal(c.first, 0.0f, c.second);
        r.changed += foliage::EraseInCircle(set->instances, lc.x, lc.z, rl, fraction, seed + k++);
    }
    set->Rebuild();
    r.after = set->Count();
    foliage::ReplaceSet(layer, set);
    return r;
}

} // namespace dx12e::foliage_ops
