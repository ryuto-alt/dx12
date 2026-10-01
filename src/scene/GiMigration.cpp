#include "scene/GiMigration.h"

#include <algorithm>
#include <cmath>

#include "ecs/Components.h"
#include "renderer/Mesh.h"
#include "scene/Scene.h"

namespace dx12e::gi
{

using namespace DirectX;

namespace
{

constexpr float kEps = 1.0e-4f;

float Ext(const Bounds& b, int axis)
{
    const float lo = axis == 0 ? b.mn.x : (axis == 1 ? b.mn.y : b.mn.z);
    const float hi = axis == 0 ? b.mx.x : (axis == 1 ? b.mx.y : b.mx.z);
    return hi - lo;
}
float Lo(const Bounds& b, int axis) { return axis == 0 ? b.mn.x : (axis == 1 ? b.mn.y : b.mn.z); }

bool IsHuge(const Box& b)
{
    return (b.mx.x - b.mn.x) > kFitHugeExtent || (b.mx.z - b.mn.z) > kFitHugeExtent;
}

Bounds Union(const std::vector<const Box*>& v)
{
    Bounds r;
    for (const Box* b : v)
    {
        if (!r.valid) { r.mn = b->mn; r.mx = b->mx; r.valid = true; continue; }
        r.mn = {std::min(r.mn.x, b->mn.x), std::min(r.mn.y, b->mn.y), std::min(r.mn.z, b->mn.z)};
        r.mx = {std::max(r.mx.x, b->mx.x), std::max(r.mx.y, b->mx.y), std::max(r.mx.z, b->mx.z)};
    }
    return r;
}

} // namespace

Bounds ChooseBounds(const std::vector<Box>& boxes)
{
    std::vector<const Box*> stat, statSmall, all, allSmall;
    for (const Box& b : boxes)
    {
        all.push_back(&b);
        if (!IsHuge(b)) allSmall.push_back(&b);
        if (!b.isDynamic)
        {
            stat.push_back(&b);
            if (!IsHuge(b)) statSmall.push_back(&b);
        }
    }
    // 動かない物を優先。その中でも巨大な背景は、他に箱があれば無視する。
    const std::vector<const Box*>* pick =
        !statSmall.empty() ? &statSmall : (!stat.empty() ? &stat : (!allSmall.empty() ? &allSmall : &all));
    Bounds r = Union(*pick);
    if (!r.valid) return r;
    // 巨大な箱しか無いときは、中心のまわりを切り取る（4096 個の上限で間隔が広がりすぎないように）。
    const float cx = (r.mn.x + r.mx.x) * 0.5f, cz = (r.mn.z + r.mx.z) * 0.5f;
    const float h = kFitHugeClamp * 0.5f;
    if (r.mx.x - r.mn.x > kFitHugeClamp) { r.mn.x = cx - h; r.mx.x = cx + h; }
    if (r.mx.z - r.mn.z > kFitHugeClamp) { r.mn.z = cz - h; r.mx.z = cz + h; }
    return r;
}

DdgiSettings FitDdgiToBounds(const DdgiSettings& base, const Bounds& boundsIn)
{
    Bounds b = boundsIn;
    if (!b.valid)
    {
        b.mn = {-8.0f, 0.0f, -8.0f};
        b.mx = { 8.0f, kFitMinHeight, 8.0f};
        b.valid = true;
    }
    // 小さな物 1 個だけのシーンでも、まわりに歩ける広さのぶん張る（中心は動かさない）。
    {
        const float cx = (b.mn.x + b.mx.x) * 0.5f, cz = (b.mn.z + b.mx.z) * 0.5f;
        if (b.mx.x - b.mn.x < kFitMinWidth) { b.mn.x = cx - kFitMinWidth * 0.5f; b.mx.x = cx + kFitMinWidth * 0.5f; }
        if (b.mx.z - b.mn.z < kFitMinWidth) { b.mn.z = cz - kFitMinWidth * 0.5f; b.mx.z = cz + kFitMinWidth * 0.5f; }
    }
    // 平らなシーンでも人の背丈より上までプローブを置く。
    if (b.mx.y - b.mn.y < kFitMinHeight) b.mx.y = b.mn.y + kFitMinHeight;

    const int maxProbes = static_cast<int>(DdgiVolume::kMaxProbes);
    float s = kFitSpacing;
    int   n[3] = {2, 2, 2};
    bool  ok = false;
    for (; s <= 100.0f + kEps; s += kFitSpacingStep)
    {
        const float pad = 0.5f * s;
        int prod = 1;
        bool within = true;
        for (int a = 0; a < 3; ++a)
        {
            const float size = Ext(b, a) + 2.0f * pad;
            n[a] = std::max(2, static_cast<int>(std::ceil(size / s - kEps)) + 1);
            if (n[a] > 32) within = false;
            prod *= n[a];
        }
        if (within && prod <= maxProbes) { ok = true; break; }
    }
    if (!ok)
    {
        // 3 km を超える範囲（実用上ありえない）。上限へ丸めて最大間隔で置く。
        s = 100.0f;
        for (int a = 0; a < 3; ++a) n[a] = std::min(n[a], 32);
        while (n[0] * n[1] * n[2] > maxProbes)
        {
            int big = 0;
            for (int a = 1; a < 3; ++a) if (n[a] > n[big]) big = a;
            --n[big];
        }
    }

    DdgiSettings out = base;
    out.enabled     = true;
    out.followCamera = false;   // シーンへ合わせた固定ボリューム
    out.probeCountX = n[0];
    out.probeCountY = n[1];
    out.probeCountZ = n[2];
    out.spacing     = s;
    // 格子は範囲の中心に揃える（張り出しが左右・上下で均等になる）。
    float* org[3] = {&out.originX, &out.originY, &out.originZ};
    for (int a = 0; a < 3; ++a)
    {
        const float c = Lo(b, a) + 0.5f * Ext(b, a);
        *org[a] = c - 0.5f * static_cast<float>(n[a] - 1) * s;
    }
    return out;
}

DdgiSettings FollowCameraDefaults(const DdgiSettings& base)
{
    DdgiSettings out = base;
    out.enabled      = true;
    out.followCamera = true;
    out.probeCountX  = kFollowCountX;
    out.probeCountY  = kFollowCountY;
    out.probeCountZ  = kFollowCountZ;
    out.spacing      = kFollowSpacing;
    out.spacing1     = kFollowSpacing1;
    return out;
}

std::vector<Box> CollectStaticBoxes(const entt::registry& reg)
{
    std::vector<Box> out;
    for (auto [e, mr, tf] : reg.view<const MeshRenderer, const Transform>().each())
    {
        (void)tf;
        if (reg.all_of<GridPlane>(e)) continue;   // エディタ用の巨大グリッド床
        const XMMATRIX world = ComputeWorldMatrix(reg, e);
        XMVECTOR mn = XMVectorReplicate(3.402823466e+38f);
        XMVECTOR mx = XMVectorReplicate(-3.402823466e+38f);
        bool any = false;
        for (const Mesh* mesh : mr.meshes)
        {
            if (!mesh) continue;
            const XMFLOAT3 amn = mesh->GetAABBMin();
            const XMFLOAT3 amx = mesh->GetAABBMax();
            for (int c = 0; c < 8; ++c)
            {
                XMVECTOR p = XMVectorSet((c & 1) ? amx.x : amn.x, (c & 2) ? amx.y : amn.y,
                                         (c & 4) ? amx.z : amn.z, 1.0f);
                p = XMVector3Transform(p, world);
                mn = XMVectorMin(mn, p);
                mx = XMVectorMax(mx, p);
            }
            any = true;
        }
        if (!any) continue;
        Box b;
        XMStoreFloat3(&b.mn, mn);
        XMStoreFloat3(&b.mx, mx);
        if (const RigidBody* rb = reg.try_get<RigidBody>(e))
            b.isDynamic = (rb->motionType == MotionType::Dynamic);
        out.push_back(b);
    }
    return out;
}

State Capture(const Scene& scene)
{
    State s;
    s.gi   = scene.GetGiSettings();
    s.ddgi = scene.GetDdgiSettings();
    s.ssgi = scene.GetSsgiSettings();
    s.rt   = scene.GetRtSettings();
    const entt::registry& reg = scene.GetRegistry();
    for (auto [e, dl] : reg.view<const DirectionalLight>().each())
        s.ambient.emplace_back(e, dl.ambient);
    return s;
}

void Restore(Scene& scene, const State& s)
{
    scene.GetGiSettings()   = s.gi;
    scene.GetDdgiSettings() = s.ddgi;
    scene.GetSsgiSettings() = s.ssgi;
    scene.GetRtSettings()   = s.rt;
    entt::registry& reg = scene.GetRegistry();
    for (const auto& [e, amb] : s.ambient)
        if (reg.valid(e) && reg.all_of<DirectionalLight>(e))
            reg.get<DirectionalLight>(e).ambient = amb;
}

Result ApplyNew(Scene& scene, const Options& opt)
{
    Result r;
    r.ddgi = scene.GetDdgiSettings();
    if (!opt.dxrSupported)
    {
        r.reason = opt.whyNot ? opt.whyNot
                              : "この GPU はレイトレーシング（DXR 1.1）に対応していないため、新しい GI は使えません（従来の GI のままです）";
        return r;
    }

    entt::registry& reg = scene.GetRegistry();
    DdgiSettings dd = scene.GetDdgiSettings();
    const bool keepGrid = !opt.refitGrid && dd.enabled;
    if (!keepGrid)
    {
        if (opt.fitToScene)
        {
            const Bounds b = ChooseBounds(CollectStaticBoxes(reg));
            r.fromGeometry = b.valid;
            dd = FitDdgiToBounds(dd, b);
        }
        else
        {
            dd = FollowCameraDefaults(dd);   // 既定: カメラ追従（シーンの AABB に依らない）
        }
        r.fitted = true;
    }
    dd.enabled         = true;
    dd.bounceIntensity = 1.0f;   // 新の既定: 多重バウンス ON（GI_S2S3_REPORT の「new の既定構成」）
    if (dd.intensity <= 0.0f) dd.intensity = 1.0f;

    scene.GetGiSettings().mode = GiMode::New;
    scene.GetDdgiSettings()    = dd;
    scene.GetSsgiSettings().enabled     = true;
    scene.GetRtSettings().shadowEnabled = true;
    for (auto [e, dl] : reg.view<DirectionalLight>().each())
    {
        (void)e;
        if (dl.ambient != 0.0f) ++r.ambientChanged;
        dl.ambient = 0.0f;   // 環境光は DDGI が受け持つ。定数の ambient は 0
    }
    r.applied = true;
    r.ddgi    = dd;
    return r;
}

Result ApplyLegacy(Scene& scene)
{
    Result r;
    entt::registry& reg = scene.GetRegistry();
    scene.GetGiSettings().mode      = GiMode::Legacy;
    scene.GetDdgiSettings().enabled = false;
    for (auto [e, dl] : reg.view<DirectionalLight>().each())
    {
        (void)e;
        if (dl.ambient == 0.0f) { dl.ambient = kUndoAmbientBack; ++r.ambientChanged; }
    }
    r.applied = true;
    r.ddgi    = scene.GetDdgiSettings();
    return r;
}

Result RefitGrid(Scene& scene)
{
    Result r;
    const Bounds b = ChooseBounds(CollectStaticBoxes(scene.GetRegistry()));
    r.fromGeometry = b.valid;
    r.fitted       = true;
    DdgiSettings dd = FitDdgiToBounds(scene.GetDdgiSettings(), b);
    dd.enabled = true;
    scene.GetDdgiSettings() = dd;
    r.applied = true;
    r.ddgi    = dd;
    return r;
}

Result UseFollowCamera(Scene& scene)
{
    Result r;
    DdgiSettings dd = FollowCameraDefaults(scene.GetDdgiSettings());
    scene.GetDdgiSettings() = dd;
    r.applied = true;
    r.fitted  = true;
    r.ddgi    = dd;
    return r;
}

const char* ModeLabel(GiMode m)
{
    return m == GiMode::New ? "新" : "旧";
}

} // namespace dx12e::gi
