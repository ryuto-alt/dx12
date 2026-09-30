#include "sequencer/SeqBinding.h"

#include <unordered_map>

namespace dx12e::seq
{

const char* ResolveViaName(ResolveVia v)
{
    switch (v)
    {
    case ResolveVia::Override: return "override";
    case ResolveVia::Guid:     return "guid";
    case ResolveVia::Path:     return "path";
    case ResolveVia::Name:     return "name";
    case ResolveVia::Scene:    return "scene";
    default:                   return "none";
    }
}

bool BindingSet::AllResolved() const
{
    for (const auto& r : byBinding) if (!r.Resolved()) return false;
    return true;
}

std::vector<std::uint8_t> BindingSet::EnabledMask() const
{
    std::vector<std::uint8_t> m(byBinding.size(), 0);
    for (std::size_t i = 0; i < byBinding.size(); ++i) m[i] = byBinding[i].Resolved() ? 1 : 0;
    return m;
}

namespace
{
std::vector<std::string> TrackIdsOf(const Binding& b)
{
    std::vector<std::string> ids;
    ids.reserve(b.tracks.size());
    for (const auto& t : b.tracks) ids.push_back(t.id);
    return ids;
}
} // namespace

BindingResolution ResolveBinding(const Binding& b, const IBindingResolver& resolver,
                                 const BindingOverrides* overrides, std::vector<BindingIssue>* issues)
{
    BindingResolution r;
    auto issue = [&](BindingIssue::Code code, std::string msg, bool withTracks) {
        if (!issues) return;
        BindingIssue i;
        i.code = code;
        i.bindingId = b.id;
        if (withTracks) i.trackIds = TrackIdsOf(b);
        i.message = std::move(msg);
        issues->push_back(std::move(i));
    };
    const std::string label = b.name.empty() ? b.id : b.name;

    if (b.kind == BindingKind::Scene)
    {
        r.via = ResolveVia::Scene;
        r.target = kSceneTarget;
        return r;
    }
    if (b.kind == BindingKind::Spawnable)
    {
        issue(BindingIssue::Code::SpawnableUnsupported, "「" + label + "」は spawnable(未対応)なのでトラックを無効化した", true);
        return r;
    }

    // ① 上書き
    if (overrides)
    {
        const auto it = overrides->find(b.id);
        if (it != overrides->end() && !it->second.empty())
        {
            const TargetId t = resolver.FindByGuid(it->second);
            if (t != kNoTarget)
            {
                r.via = ResolveVia::Override;
                r.target = t;
                return r;
            }
        }
    }
    // ② guid
    bool guidTried = false;
    if (!b.hint.guid.empty())
    {
        guidTried = true;
        const TargetId t = resolver.FindByGuid(b.hint.guid);
        if (t != kNoTarget)
        {
            r.via = ResolveVia::Guid;
            r.target = t;
            return r;
        }
    }
    std::vector<TargetId> cand;
    // ③ 階層パス
    if (!b.hint.path.empty())
    {
        resolver.FindByPath(b.hint.path, cand);
        if (!cand.empty())
        {
            r.via = ResolveVia::Path;
            r.target = cand.front();
            r.candidates = static_cast<int>(cand.size());
            if (guidTried) issue(BindingIssue::Code::FellBack, "「" + label + "」は guid で見つからず、パス " + b.hint.path + " で解決した(hint の更新を推奨)", false);
            if (cand.size() > 1)
                issue(BindingIssue::Code::AmbiguousPath, "「" + label + "」のパス " + b.hint.path + " に該当する対象が " + std::to_string(cand.size()) + " 個ある(先頭を採用)", false);
            return r;
        }
    }
    // ④ 名前
    cand.clear();
    if (!b.hint.name.empty())
    {
        resolver.FindByName(b.hint.name, cand);
        if (!cand.empty())
        {
            r.via = ResolveVia::Name;
            r.target = cand.front();
            r.candidates = static_cast<int>(cand.size());
            if (guidTried || !b.hint.path.empty())
                issue(BindingIssue::Code::FellBack, "「" + label + "」は guid / パスで見つからず、名前 " + b.hint.name + " で解決した(hint の更新を推奨)", false);
            if (cand.size() > 1)
                issue(BindingIssue::Code::AmbiguousName, "名前「" + b.hint.name + "」のエンティティが " + std::to_string(cand.size()) + " 個ある(最後に作られた方 = 先頭を採用)", false);
            return r;
        }
    }
    issue(BindingIssue::Code::Unresolved, "「" + label + "」が見つからない(トラック " + std::to_string(b.tracks.size()) + " 本を無効化。データは保持)", true);
    return r;
}

BindingSet ResolveBindings(const Sequence& seq, const IBindingResolver& resolver, const BindingOverrides* overrides)
{
    BindingSet set;
    set.byBinding.reserve(seq.bindings.size());
    for (const Binding& b : seq.bindings) set.byBinding.push_back(ResolveBinding(b, resolver, overrides, &set.issues));

    // 同じ対象に解決された別バインディング(scene を除く)
    std::unordered_map<TargetId, std::size_t> first;
    for (std::size_t i = 0; i < set.byBinding.size(); ++i)
    {
        const BindingResolution& r = set.byBinding[i];
        if (!r.Resolved() || r.via == ResolveVia::Scene) continue;
        const auto it = first.find(r.target);
        if (it == first.end()) { first.emplace(r.target, i); continue; }
        BindingIssue is;
        is.code = BindingIssue::Code::SharedTarget;
        is.bindingId = seq.bindings[i].id;
        is.message = "バインディング「" + (seq.bindings[i].name.empty() ? seq.bindings[i].id : seq.bindings[i].name)
                   + "」は「" + (seq.bindings[it->second].name.empty() ? seq.bindings[it->second].id : seq.bindings[it->second].name)
                   + "」と同じ対象に解決された(同じプロパティを書くと後ろが勝つ)";
        set.issues.push_back(std::move(is));
    }
    return set;
}

// ---------------------------------------------------------------------------
TargetId MapBindingResolver::FindByGuid(std::string_view guidHex) const
{
    for (const auto& e : m_entities) if (!e.guid.empty() && e.guid == guidHex) return e.id;
    return kNoTarget;
}

void MapBindingResolver::FindByPath(std::string_view path, std::vector<TargetId>& out) const
{
    for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it)
        if (!it->path.empty() && it->path == path) out.push_back(it->id);
}

void MapBindingResolver::FindByName(std::string_view name, std::vector<TargetId>& out) const
{
    for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it)
        if (!it->name.empty() && it->name == name) out.push_back(it->id);
}

} // namespace dx12e::seq
