#include "renderer/foliage/FoliageLayerOps.h"

#include <algorithm>

#include "core/Logger.h"
#include "renderer/foliage/FoliageIO.h"

namespace dx12e::foliage
{

std::vector<std::string> ParseLodList(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size() && out.size() < kMaxLods)
    {
        size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        std::string t = s.substr(i, j - i);
        const auto b = t.find_first_not_of(" \t\r\n");
        const auto e = t.find_last_not_of(" \t\r\n");
        t = (b == std::string::npos) ? std::string() : t.substr(b, e - b + 1);
        if (!t.empty()) out.push_back(std::move(t));
        i = j + 1;
    }
    return out;
}

u32 CountVariants(const FoliageLayer& l)
{
    u32 n = 0;
    for (u32 i = 0; i < kMaxVariants; ++i)
    {
        if (ParseLodList(VariantString(l, i)).empty()) break;
        ++n;
    }
    return n;
}

bool EnsureLoaded(FoliageLayer& l, std::string* err)
{
    if (l._set) return true;
    if (l._loadTried) return false;
    l._loadTried = true;
    if (l.instancePath.empty()) return false;
    auto s = std::make_shared<FoliageInstanceSet>();
    std::string e;
    if (!LoadFoliageAsset(l.instancePath, *s, &e))
    {
        if (err) *err = e;
        Logger::Warn("植生: {} を読めません（{}）", l.instancePath, e);
        return false;
    }
    l._set = std::move(s);
    l._diskSet = l._set.get();
    return true;
}

void ReplaceSet(FoliageLayer& l, std::shared_ptr<FoliageInstanceSet> s)
{
    l._set = std::move(s);
    l._loadTried = true;
    l._needsSave = true;
}

bool FlushSidecar(FoliageLayer& l, const std::string& entityName, const std::string& assetsDir, std::string* err, bool force)
{
    if (!l._set) return false;
    if (!force && !l._needsSave && !l.instancePath.empty() && l._set.get() == l._diskSet) return false;
    if (l.instancePath.empty()) l.instancePath = MakeFoliageRelPath(entityName);
    std::string base = assetsDir;
    if (!base.empty() && base.back() != '/' && base.back() != '\\') base += '/';
    if (!SaveFoliageFile(base + l.instancePath, *l._set, err)) return false;
    l._needsSave = false;
    l._diskSet = l._set.get();
    return true;
}

CullParams MakeLayerCullParams(const FoliageLayer& l, u32 variantCount, const u32 lodCount[kMaxVariants])
{
    CullParams p;
    p.cullDist = std::max(l.cullDistance, 1.0f);
    p.lodDist[0] = std::max(l.lodDist0, 0.1f);
    p.lodDist[1] = std::max(l.lodDist1, p.lodDist[0] + 0.1f);
    p.lodDist[2] = std::max(l.lodDist2, p.lodDist[1] + 0.1f);
    p.thinStart = std::clamp(l.thinStart, 0.0f, 0.999f);
    // クロスフェード帯は互いに重ならないよう、隣り合う切替距離の比から上限を決める
    f32 fade = std::clamp(l.lodFade, 0.0f, 0.45f);
    for (int k = 0; k < 2; ++k)
    {
        const f32 ratio = p.lodDist[k + 1] / p.lodDist[k];   // D_{k+1}(1-f) > D_k(1+f) ⇔ f < (r-1)/(r+1)
        fade = std::min(fade, 0.95f * (ratio - 1.0f) / (ratio + 1.0f));
    }
    p.lodFade = std::max(fade, 0.0f);
    p.variantCount = std::clamp(variantCount, 1u, kMaxVariants);
    for (u32 i = 0; i < kMaxVariants; ++i) p.lodCount[i] = std::clamp(lodCount ? lodCount[i] : 1u, 1u, kMaxLods);
    p.shadowMaxLod = static_cast<u32>(std::clamp(l.shadowMaxLod, 0, static_cast<i32>(kMaxLods) - 1));
    p.radiusScale = l.windEnabled ? 1.15f : 1.0f;
    return p;
}

} // namespace dx12e::foliage
