// VfxComponent のシリアライザ登録（W1-BD4）。ComponentSerializerList.inc の lane-B ブロックから呼ばれる。
//   JSON のキーは "vfx"。既定値のフィールドは書かない（既存のシーンの diff を増やさない）。
#include "scene/ComponentSerializerRegistry.h"

#include <nlohmann/json.hpp>

#include "ecs/components/VfxComponent.h"

namespace dx12e
{
namespace
{
bool VfxHas(const entt::registry& r, entt::entity e) { return r.all_of<VfxComponent>(e); }

void VfxSave(const entt::registry& r, entt::entity e, nlohmann::json& out)
{
    const VfxComponent& v = r.get<VfxComponent>(e);
    out = nlohmann::json::object();
    out["asset"] = v.asset;
    if (!v.autoplay)         out["autoplay"]  = false;
    if (v.timeScale != 1.0f) out["timeScale"] = v.timeScale;
    if (v.seed != 0)         out["seed"]      = v.seed;
}

void VfxLoad(entt::registry& r, entt::entity e, const nlohmann::json& in)
{
    VfxComponent v;
    if (in.is_object())
    {
        if (in.contains("asset") && in["asset"].is_string()) v.asset = in["asset"].get<std::string>();
        if (in.contains("autoplay") && in["autoplay"].is_boolean()) v.autoplay = in["autoplay"].get<bool>();
        if (in.contains("timeScale") && in["timeScale"].is_number()) v.timeScale = in["timeScale"].get<float>();
        if (in.contains("seed") && in["seed"].is_number_unsigned()) v.seed = in["seed"].get<u32>();
    }
    r.emplace_or_replace<VfxComponent>(e, std::move(v));
}

void VfxRemove(entt::registry& r, entt::entity e) { r.remove<VfxComponent>(e); }
} // namespace

void RegisterVfxComponentSerializer(std::vector<ComponentSerializer>& out)
{
    ComponentSerializer cs;
    cs.typeName = "vfx";
    cs.has      = &VfxHas;
    cs.save     = &VfxSave;
    cs.load     = &VfxLoad;
    cs.remove   = &VfxRemove;
    out.push_back(cs);
}

} // namespace dx12e
