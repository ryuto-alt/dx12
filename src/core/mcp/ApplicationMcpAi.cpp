// ===========================================================================
// MCP: ゲーム AI（Brain）の観測
// ---------------------------------------------------------------------------
//   brain_state {entity|name}  … 1 体の Brain の黒板・知覚・得点の内訳・移動を JSON で返す
//   brain_state {}             … Brain を持つエンティティの一覧（今の行動つき）
// 読み取り専用（シーンのデータを変えない）。Brain は Play 中だけ動くので、実行時の状態は
// Play 中（step_frames で止めている間も含む）にしか無い。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;

namespace
{
using json = nlohmann::json;

json Vec3Json(const f32* p) { return json::array({ p[0], p[1], p[2] }); }

json BbValueJson(const entt::registry& reg, const ai::BbValue& v)
{
    switch (v.index())
    {
    case 1: return { {"type", "number"}, {"value", std::get<f64>(v)} };
    case 2: return { {"type", "bool"}, {"value", std::get<bool>(v)} };
    case 3: return { {"type", "string"}, {"value", std::get<std::string>(v)} };
    case 4:
    {
        const ai::BbVec3& p = std::get<ai::BbVec3>(v);
        return { {"type", "vec3"}, {"value", json::array({ p.x, p.y, p.z })} };
    }
    case 5:
    {
        const u32 id = std::get<ai::BbEntity>(v).id;
        const entt::entity e = static_cast<entt::entity>(id);
        json j = { {"type", "entity"}, {"value", id} };
        if (reg.valid(e))
            if (const NameTag* nt = reg.try_get<NameTag>(e)) j["name"] = nt->name;
        return j;
    }
    default: return { {"type", "none"} };
    }
}

std::string NameOf(const entt::registry& reg, u32 id)
{
    const entt::entity e = static_cast<entt::entity>(id);
    if (!reg.valid(e)) return {};
    const NameTag* nt = reg.try_get<NameTag>(e);
    return nt ? nt->name : std::string();
}
} // namespace

void Application::RegisterMcpAiMethods()
{
    McpDefine("brain_state", "entity:int,name:string", DX12E_MCP_HANDLER
        {
            ai::AiSystem* aiSys = m_aiSystem.get();
            auto& reg = m_scene->GetRegistry();
            const bool one = params.contains("entity") || params.contains("name");

            // ---- 一覧（引数なし）----
            if (!one)
            {
                json list = json::array();
                for (auto [e, br] : reg.view<Brain>().each())
                {
                    json b = { {"entityId", static_cast<u32>(entt::to_integral(e))},
                               {"name", NameOf(reg, static_cast<u32>(entt::to_integral(e)))},
                               {"enabled", br.enabled} };
                    const ai::BrainState* st = aiSys ? aiSys->GetBrain(e) : nullptr;
                    b["running"] = st != nullptr;
                    if (st && st->current >= 0 && st->current < static_cast<i32>(st->actions.size()))
                        b["action"] = st->actions[static_cast<size_t>(st->current)].name;
                    list.push_back(b);
                }
                std::sort(list.begin(), list.end(),
                          [](const json& a, const json& b) { return a["entityId"].get<u32>() < b["entityId"].get<u32>(); });
                resp["brains"] = list;
                resp["mode"] = busyPlaying ? "Playing" : "Editor";
                resp["note"] = "entity か name を渡すとその Brain の黒板・知覚・得点の内訳を返す";
                return;
            }

            const entt::entity e = ResolveMcpEntity(*m_scene, params);
            if (!reg.all_of<Brain>(e))
                throw McpError(McpErr::NotFound, "entity has no Brain component",
                               "dx12_set_component で component=\"brain\" を付けるか、その Lua の OnStart で "
                               "ai.brain(self) を呼ぶこと。一覧は引数なしの brain_state");
            const ai::BrainState* st = aiSys ? aiSys->GetBrain(e) : nullptr;
            if (!st)
                throw McpError(McpErr::ModeConflict, "brain has no runtime state (not playing yet)",
                               "Brain は Play 中だけ動く。dx12_play の後に dx12_step_frames で数フレーム進めてから呼ぶ"
                               "（enabled=false の Brain も状態を持たない）");

            const Brain& cfg = reg.get<Brain>(e);
            const u32 id = static_cast<u32>(entt::to_integral(e));
            resp["entityId"] = id;
            resp["name"] = NameOf(reg, id);
            resp["time"] = aiSys->Time();
            resp["tick"] = aiSys->Tick();
            resp["enabled"] = cfg.enabled;

            // ---- 行動と選んだ理由（得点の内訳）----
            const ai::Decision& d = st->last;
            json cur = nullptr;
            if (st->current >= 0 && st->current < static_cast<i32>(st->actions.size()))
                cur = { {"name", st->actions[static_cast<size_t>(st->current)].name},
                        {"since", aiSys->Time() - st->enteredAt}, {"done", st->currentDone} };
            resp["action"] = cur;
            json acts = json::array();
            for (size_t i = 0; i < d.actions.size(); ++i)
            {
                const ai::ActionEval& ae = d.actions[i];
                json cons = json::array();
                for (const ai::ConsiderationEval& ce : ae.considerations)
                    cons.push_back({ {"name", ce.name}, {"input", ce.input}, {"value", ce.raw},
                                     {"x", ce.x}, {"score", ce.score}, {"missing", ce.missing} });
                json a = { {"name", ae.name}, {"weight", ae.weight}, {"product", ae.product},
                           {"score", ae.score}, {"bonus", ae.bonus}, {"final", ae.final},
                           {"cooldown", ae.cooldown}, {"considerations", cons} };
                if (i < st->cooldownUntil.size() && st->cooldownUntil[i] > aiSys->Time())
                    a["cooldownLeft"] = st->cooldownUntil[i] - aiSys->Time();
                acts.push_back(a);
            }
            resp["decision"] = { {"chosen", (d.chosen >= 0 && d.chosen < static_cast<i32>(st->actions.size()))
                                                ? json(st->actions[static_cast<size_t>(d.chosen)].name) : json(nullptr)},
                                 {"reason", ai::DecisionReasonName(d.reason)}, {"time", d.time}, {"actions", acts} };
            json hist = json::array();
            for (const auto& sw : st->history)
                hist.push_back({ {"time", sw.time}, {"from", sw.from}, {"to", sw.to},
                                 {"reason", ai::DecisionReasonName(sw.reason)} });
            resp["history"] = hist;
            resp["switches"] = st->switches;

            // ---- 黒板（キーの辞書順）----
            json bb = json::object();
            for (const auto& [k, v] : st->bb.All()) bb[k] = BbValueJson(reg, v);
            resp["blackboard"] = bb;

            // ---- 知覚 ----
            json targets = json::array();
            for (size_t i = 0; i < st->targets.size(); ++i)
            {
                const ai::TargetMemory& m = st->targets[i];
                json tj = { {"visible", m.visible}, {"seen", m.seen}, {"awareness", m.awareness},
                            {"distance", m.dist}, {"known", m.known},
                            {"primary", static_cast<i32>(i) == st->primary},
                            {"lastSeenAge", m.lastSeen > -1e8 ? json(aiSys->Time() - m.lastSeen) : json(nullptr)},
                            {"lastHeardAge", m.lastHeard > -1e8 ? json(aiSys->Time() - m.lastHeard) : json(nullptr)} };
                if (m.entity != 0xffffffffu) { tj["entity"] = m.entity; tj["name"] = NameOf(reg, m.entity); }
                else tj["entity"] = nullptr;   // brain:remember で外から与えた記憶
                if (m.known) tj["knownPos"] = Vec3Json(m.knownPos);
                targets.push_back(tj);
            }
            json heard = json::array();
            for (const auto& h : st->heard)
            {
                json hj = { {"pos", Vec3Json(h.pos)}, {"radius", h.radius}, {"loudness", h.loudness},
                            {"age", aiSys->Time() - h.time}, {"tag", h.tag}, {"occluded", h.occluded} };
                if (h.source != 0xffffffffu) { hj["source"] = h.source; hj["sourceName"] = NameOf(reg, h.source); }
                heard.push_back(hj);
            }
            json rays = json::array();
            for (const auto& r : st->rays)
                rays.push_back({ {"from", Vec3Json(r.from)}, {"to", Vec3Json(r.to)}, {"blocked", r.blocked},
                                 {"candidate", r.candidate}, {"target", r.target} });
            resp["perception"] = { {"eye", Vec3Json(st->eye)}, {"yaw", st->yaw},
                                   {"sightRange", cfg.sightRange}, {"sightFov", cfg.sightFov},
                                   {"nearSense", cfg.nearSense}, {"targets", targets}, {"heard", heard},
                                   {"rays", rays} };

            // ---- 移動（群衆）----
            if (const nav::NavCrowdAgent* ag = aiSys->GetAgent(e))
            {
                f32 av[3]{};
                aiSys->ActualVelocity(e, av);
                json corners = json::array();
                for (size_t i = 0; i + 2 < ag->corners.size(); i += 3)
                    corners.push_back({ ag->corners[i], ag->corners[i + 1], ag->corners[i + 2] });
                resp["movement"] = { {"state", nav::NavMoveStateName(ag->targetState)}, {"partial", ag->partial},
                                     {"pos", Vec3Json(ag->npos)}, {"vel", Vec3Json(ag->vel)},
                                     {"desiredVel", Vec3Json(ag->dvel)},
                                     {"speed", std::sqrt(av[0] * av[0] + av[2] * av[2])},
                                     {"maxSpeed", ag->params.maxSpeed},
                                     {"target", Vec3Json(ag->corridor.Target())},
                                     {"distanceToGoal", aiSys->DistanceToGoal(e)},
                                     {"corners", corners}, {"neighbors", ag->neis.size()},
                                     {"corridorPolys", ag->corridor.Path().size()} };
            }
            else
            {
                resp["movement"] = nullptr;
            }
            if (!st->lastError.empty()) resp["lastError"] = st->lastError;
            resp["errorCount"] = st->errorCount;
            resp["note"] = "decision.actions が「なぜその行動か」（final = weight × 考慮事項の積 + 粘り）。"
                           "考慮事項の value は黒板の値、x は 0..1 に正規化した入力、score はカーブの出力";
        });
}

} // namespace dx12e
