#include "core/SequencerBinding.h"

#include <random>

#include "ecs/Components.h"

namespace dx12e
{
namespace seqhost
{

seq::TargetId ToTarget(entt::entity e)
{
    if (e == entt::null) return seq::kNoTarget;
    return static_cast<seq::TargetId>(entt::to_integral(e)) + 1;
}

entt::entity ToEntity(seq::TargetId t)
{
    if (t == seq::kNoTarget || t == seq::kSceneTarget) return entt::null;
    return static_cast<entt::entity>(static_cast<std::uint32_t>(t - 1));
}

std::uint64_t EnsureEntityGuid(entt::registry& reg, entt::entity e)
{
    if (e == entt::null || !reg.valid(e)) return 0;
    static std::mt19937_64 rng{ std::random_device{}() };
    const auto fresh = []() {
        std::uint64_t v = 0;
        while (v == 0) v = rng();
        return v;
    };
    if (auto* g = reg.try_get<EntityGuid>(e))
    {
        if (g->value == 0) g->value = fresh();
        return g->value;
    }
    return reg.emplace<EntityGuid>(e, EntityGuid{ fresh() }).value;
}

namespace
{
void AppendEscaped(std::string& out, const std::string& name)
{
    for (const char c : name)
    {
        if (c == '/') out += "%2F";
        else if (c == '%') out += "%25";
        else out.push_back(c);
    }
}
} // namespace

std::string EntityHierarchyPath(const entt::registry& reg, entt::entity e)
{
    std::vector<entt::entity> chain;
    for (entt::entity cur = e; cur != entt::null && reg.valid(cur) && chain.size() < 64;)
    {
        chain.push_back(cur);
        const auto* tf = reg.try_get<Transform>(cur);
        cur = tf ? tf->parent : entt::null;
    }
    std::string path;
    for (std::size_t i = chain.size(); i-- > 0;)
    {
        const auto* tag = reg.try_get<NameTag>(chain[i]);
        if (!path.empty()) path.push_back('/');
        AppendEscaped(path, tag ? tag->name : std::string());
    }
    return path;
}

seq::BindingHint MakeBindingHint(entt::registry& reg, entt::entity e, bool ensureGuid)
{
    seq::BindingHint h;
    if (e == entt::null || !reg.valid(e)) return h;
    std::uint64_t g = 0;
    if (ensureGuid) g = EnsureEntityGuid(reg, e);
    else if (const auto* gp = reg.try_get<EntityGuid>(e)) g = gp->value;
    if (g != 0) h.guid = FormatEntityGuidHex(g);
    h.path = EntityHierarchyPath(reg, e);
    if (const auto* tag = reg.try_get<NameTag>(e)) h.name = tag->name;
    return h;
}

seq::TargetId EngineBindingResolver::FindByGuid(std::string_view guidHex) const
{
    if (!m_haveGuid)
    {
        m_haveGuid = true;
        for (auto [e, g] : m_reg.view<const EntityGuid>().each())
            if (g.value != 0) m_byGuid.emplace(g.value, e);
    }
    const std::uint64_t g = ParseEntityGuidHex(std::string(guidHex));
    if (g == 0) return seq::kNoTarget;
    const auto it = m_byGuid.find(g);
    return it == m_byGuid.end() ? seq::kNoTarget : ToTarget(it->second);
}

void EngineBindingResolver::FindByPath(std::string_view path, std::vector<seq::TargetId>& out) const
{
    if (!m_havePath)
    {
        m_havePath = true;
        for (auto [e, tag] : m_reg.view<const NameTag>().each())
        {
            (void)tag;
            m_byPath[EntityHierarchyPath(m_reg, e)].push_back(e);
        }
    }
    const auto it = m_byPath.find(std::string(path));
    if (it == m_byPath.end()) return;
    for (const entt::entity e : it->second) out.push_back(ToTarget(e));
}

void EngineBindingResolver::FindByName(std::string_view name, std::vector<seq::TargetId>& out) const
{
    if (!m_haveName)
    {
        m_haveName = true;
        for (auto [e, tag] : m_reg.view<const NameTag>().each()) m_byName[tag.name].push_back(e);
    }
    const auto it = m_byName.find(std::string(name));
    if (it == m_byName.end()) return;
    for (const entt::entity e : it->second) out.push_back(ToTarget(e));
}

} // namespace seqhost
} // namespace dx12e
