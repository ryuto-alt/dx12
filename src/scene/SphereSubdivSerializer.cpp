// SphereSubdiv のシリアライザ登録（W1-C13）。ComponentSerializerList.inc の lane-C ブロックから呼ばれる。
//   JSON のキーは "sphereSubdiv" {slices, stacks}。従来値（16x16）のときは書かない＝旧シーンの diff を増やさない。
#include "scene/ComponentSerializerRegistry.h"

#include <nlohmann/json.hpp>

#include "ecs/components/SphereSubdiv.h"

namespace dx12e
{
namespace
{
bool SubHas(const entt::registry& r, entt::entity e)
{
    const SphereSubdiv* s = r.try_get<SphereSubdiv>(e);
    return s && (s->slices != 16u || s->stacks != 16u);
}

void SubSave(const entt::registry& r, entt::entity e, nlohmann::json& out)
{
    const SphereSubdiv& s = r.get<SphereSubdiv>(e);
    out = nlohmann::json::object();
    out["slices"] = s.slices;
    out["stacks"] = s.stacks;
}

void SubLoad(entt::registry& r, entt::entity e, const nlohmann::json& in)
{
    SphereSubdiv s;
    if (in.is_object())
    {
        if (in.contains("slices") && in["slices"].is_number_unsigned()) s.slices = in["slices"].get<u32>();
        if (in.contains("stacks") && in["stacks"].is_number_unsigned()) s.stacks = in["stacks"].get<u32>();
    }
    // 極端な値でメッシュを作らない（既存の SpawnSphere の下限と、頂点数の上限）。
    if (s.slices < 3u) s.slices = 3u;
    if (s.stacks < 2u) s.stacks = 2u;
    if (s.slices > 256u) s.slices = 256u;
    if (s.stacks > 256u) s.stacks = 256u;
    s.applied = false;   // SpawnSphere は 16x16 で作ってある。次のフレーム境界で作り直す
    r.emplace_or_replace<SphereSubdiv>(e, s);
}

void SubRemove(entt::registry& r, entt::entity e) { r.remove<SphereSubdiv>(e); }
} // namespace

void RegisterSphereSubdivSerializer(std::vector<ComponentSerializer>& out)
{
    ComponentSerializer cs;
    cs.typeName = "sphereSubdiv";
    cs.has      = &SubHas;
    cs.save     = &SubSave;
    cs.load     = &SubLoad;
    cs.remove   = &SubRemove;
    out.push_back(cs);
}

} // namespace dx12e
