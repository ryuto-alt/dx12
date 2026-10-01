// ===========================================================================
// MCP: インスタンス群（InstanceGroup。docs/SCENE_FORMAT_DESIGN.md §4.1 の 4-1）
//   instance_group … op = convert / explode / info / add / remove / set
//     convert … 同じモデル・同じ材質・同じ構成・同じ親の静的配置を 1 エンティティ（インスタンス群）へまとめる
//               （targets を渡すとその中だけ。省略は自動＝同じ条件が minCount 個（既定 16）以上あるものだけ。preview で結果だけ見る）
//     explode … 群を個別エンティティへ戻す
//     info    … 個数・モデル・AABB・サイドカーのパス・先頭のインスタンス
//     add / remove / set … インスタンスの追加・削除・変更（Undo 可）
// ★convert / explode はまとめて 1 つの Undo。Play 中は不可（Editor モードで）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"
#include "ecs/InstanceGroup.h"
#include "editor/InstanceGroupCommand.h"
#include "scene/InstanceGroupOps.h"

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)

namespace
{
using nlohmann::json;
using instgroup::InstanceTRS;

bool ParseTrs(const json& j, InstanceTRS& out)
{
    auto num3 = [](const json& a, DirectX::XMFLOAT3& v) -> bool {
        if (!a.is_array() || a.size() != 3) return false;
        for (const auto& x : a) if (!x.is_number()) return false;
        v = {a[0].get<f32>(), a[1].get<f32>(), a[2].get<f32>()};
        return true;
    };
    InstanceTRS t;
    if (j.is_array())
    {
        if (j.size() != 3 && j.size() != 6 && j.size() != 9) return false;
        for (const auto& x : j) if (!x.is_number()) return false;
        t.p = {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>()};
        if (j.size() >= 6) t.r = {j[3].get<f32>(), j[4].get<f32>(), j[5].get<f32>()};
        if (j.size() >= 9) t.s = {j[6].get<f32>(), j[7].get<f32>(), j[8].get<f32>()};
        out = t;
        return true;
    }
    if (j.is_object())
    {
        if (!j.contains("position") || !num3(j["position"], t.p)) return false;
        if (j.contains("rotation") && !num3(j["rotation"], t.r)) return false;
        if (j.contains("scale") && !num3(j["scale"], t.s)) return false;
        out = t;
        return true;
    }
    return false;
}

json TrsJson(const InstanceTRS& t)
{
    return json{{"position", {t.p.x, t.p.y, t.p.z}}, {"rotation", {t.r.x, t.r.y, t.r.z}}, {"scale", {t.s.x, t.s.y, t.s.z}}};
}
} // namespace

void Application::RegisterMcpInstanceGroupMethods()
{
    McpMeta m;
    m.summary  = "インスタンス群（同じモデル・同じ材質の静的配置 N 個を 1 エンティティにまとめる圧縮表現）の変換・展開・編集。"
                 "op=convert: 同じ条件の静的配置を群へまとめる（targets 省略 = シーン全体から自動、同じ条件が minCount 個以上のものだけ。preview:true で何も変えず結果だけ）。"
                 "条件を満たさないものは変換せず skipped に理由を集計して返す。op=explode: 群を個別エンティティへ戻す。op=info: 個数・AABB・サイドカーのパス・先頭のインスタンス。"
                 "op=add / remove / set: インスタンスの追加・削除・変更。インスタンスは群の Transform からの相対 TRS（位置 / Euler 度 / スケール）。"
                 "実体は <シーン名>.inst/<guid>.jsonl（1 行 1 インスタンス）。読み込みが速く、エンティティ数・シーンファイルが小さくなる";
    m.keywords = "instance group instancing convert explode 群 インスタンス 変換 展開 まとめる 軽量 大量配置 圧縮";
    m.category = "scene"; m.group = "instance_group"; m.target = "instance_group";
    m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 120000; m.idempotent = false;
    m.aliases = {"dx12_instance_group"};
    m.params = {
        P("op", "enum", true, "convert|explode|info|add|remove|set", nullptr, nullptr, nullptr, "操作"),
        P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "群の entity id（explode / info / add / remove / set）"),
        P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "群の名前（entity の代わり）"),
        P("targets", "array", false, nullptr, nullptr, nullptr, nullptr, "convert: まとめる対象（entity id か名前の配列）。省略 = 自動"),
        P("minCount", "int", false, nullptr, "2", "1000000", "16", "convert（自動）: 同じ条件の配置がこの個数以上あるものだけまとめる"),
        P("preview", "bool", false, nullptr, nullptr, nullptr, "false", "convert: 何も変えず、まとめられる群の数と skip の理由だけ返す"),
        P("instances", "array", false, nullptr, nullptr, nullptr, nullptr, "add: 追加するインスタンス。各要素は [px,py,pz] / [px,py,pz,rx,ry,rz] / [px,py,pz,rx,ry,rz,sx,sy,sz] か {position,rotation,scale}"),
        P("indices", "array", false, nullptr, nullptr, nullptr, nullptr, "remove: 消すインスタンス番号（残りは詰まる）"),
        P("index", "int", false, nullptr, "0", "100000000", nullptr, "set: 変更するインスタンス番号"),
        P("value", "any", false, nullptr, nullptr, nullptr, nullptr, "set: 新しい値（instances の要素と同じ形）"),
        P("offset", "int", false, nullptr, "0", "100000000", "0", "info: 返すインスタンスの開始番号"),
        P("limit", "int", false, nullptr, "0", "1000", "10", "info: 返すインスタンスの数"),
        P("all", "bool", false, nullptr, nullptr, nullptr, "false", "explode: シーン内のすべての群を展開する（entity / name は不要）"),
    };
    m.next = {{"get_bounds", "群の合成 AABB を見る"}, {"screenshot", "変換前後で絵を比べる"}, {"save_scene", "サイドカーごと保存する"}};
    m.examples = {{"{\"op\":\"convert\",\"preview\":true}", "まとめられる群を調べるだけ"},
                  {"{\"op\":\"convert\",\"minCount\":16}", "同じ条件が 16 個以上のものを自動でまとめる"},
                  {"{\"op\":\"explode\",\"name\":\"SM_Module07 \\u00d71070\"}", "個別エンティティへ戻す"}};
    McpDefine("instance_group", McpMeta(m), DX12E_MCP_HANDLER
        {
            const std::string op = params.value("op", std::string());
            auto& reg = m_scene->GetRegistry();
            const std::string assetsDir = PathResolver::AssetsDir();

            auto resolveGroup = [&]() -> entt::entity {
                const entt::entity e = ResolveMcpEntity(*m_scene, params);
                if (!reg.all_of<InstanceGroup>(e))
                    throw McpError(McpErr::InvalidParam, "このエンティティはインスタンス群ではありません",
                                   "op:\"convert\" でまとめるか、InstanceGroup を持つエンティティを指定してください");
                return e;
            };
            auto groupJson = [&](entt::entity e) -> json {
                json j;
                j["entity"] = static_cast<u32>(e);
                if (const auto* nt = reg.try_get<NameTag>(e)) j["name"] = nt->name;
                const auto& g = reg.get<InstanceGroup>(e);
                j["count"] = g._set ? g._set->Count() : 0u;
                if (const auto* mr = reg.try_get<MeshRenderer>(e)) j["modelPath"] = mr->modelPath;
                return j;
            };

            if (op == "convert")
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は変換できません", "先に dx12_stop で Editor へ戻してください");
                instgroup::ConvertOptions opt;
                opt.dryRun   = params.value("preview", false);
                opt.minCount = static_cast<u32>(McpIntParam(params, "minCount", 16, 2, 1000000));
                if (params.contains("targets"))
                {
                    if (!params["targets"].is_array()) throw McpError(McpErr::InvalidParam, "targets は配列（entity id か名前）", "例: targets:[\"Box_1\",\"Box_2\"] か entity id の配列。省略するとシーン全体から自動でまとめる");
                    for (const auto& t : params["targets"])
                    {
                        json one = json::object();
                        if (t.is_number_integer()) one["entity"] = t.get<u32>();
                        else if (t.is_string()) one["name"] = t.get<std::string>();
                        else throw McpError(McpErr::InvalidParam, "targets の要素は entity id か名前", "数値（entity id）か文字列（名前）だけを入れる");
                        opt.targets.push_back(ResolveMcpEntity(*m_scene, one));
                    }
                    if (opt.targets.size() < 2) throw McpError(McpErr::InvalidParam, "targets は 2 つ以上", "1 つだけならまとめる意味がありません");
                }
                const auto t0 = std::chrono::steady_clock::now();
                instgroup::ConvertResult r = instgroup::ConvertToGroups(*m_scene, assetsDir, opt);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (!opt.dryRun && !r.undo.empty() && m_editorCtx)
                {
                    m_editorCtx->undoSystem.PushCommand(std::make_unique<InstanceGroupCommand>(m_scene.get(), assetsDir, std::move(r.undo), /*exploded=*/false));
                    m_editorCtx->selectedEntities.clear();
                    if (!r.groups.empty() && r.groups.front().entity != entt::null) m_editorCtx->Select(r.groups.front().entity);
                }
                json groups = json::array();
                for (size_t i = 0; i < r.groups.size() && i < 50; ++i)
                {
                    json g = {{"name", r.groups[i].name}, {"modelPath", r.groups[i].modelPath}, {"count", r.groups[i].count}};
                    if (r.groups[i].entity != entt::null) g["entity"] = static_cast<u32>(r.groups[i].entity);
                    groups.push_back(std::move(g));
                }
                json skipped = json::object();
                for (const auto& [reason, n] : r.skipped) skipped[reason] = n;
                resp["ok"] = true;
                resp["result"] = {
                    {"preview", opt.dryRun}, {"candidates", r.candidates}, {"converted", r.converted}, {"groupsCreated", r.groupsCreated},
                    {"entitiesBefore", r.entitiesBefore}, {"entitiesAfter", r.entitiesAfter},
                    {"groups", groups}, {"groupsShown", groups.size()}, {"skipped", skipped}, {"ms", ms},
                    {"note", opt.dryRun ? "preview: 何も変えていません" : "Undo は 1 回で全部戻ります。save_scene で <シーン名>.inst/ へサイドカーが書かれます"},
                };
            }
            else if (op == "explode")
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は展開できません", "先に dx12_stop で Editor へ戻してください");
                std::vector<entt::entity> targets;
                if (params.value("all", false))
                    for (auto ge : reg.view<InstanceGroup>()) targets.push_back(ge);
                else
                    targets.push_back(resolveGroup());
                const auto t0 = std::chrono::steady_clock::now();
                std::vector<instgroup::GroupUndoRecord> recs;
                u32 created = 0;
                for (entt::entity ge : targets)
                {
                    instgroup::ExplodeResult r = instgroup::ExplodeGroup(*m_scene, ge, assetsDir);
                    if (!r.ok) throw McpError(McpErr::InvalidParam, r.error, "群のエンティティを dx12_list_entities で確かめる");
                    created += r.created;
                    recs.push_back(std::move(r.undo));
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (m_editorCtx && !recs.empty())
                {
                    m_editorCtx->undoSystem.PushCommand(std::make_unique<InstanceGroupCommand>(m_scene.get(), assetsDir, std::move(recs), /*exploded=*/true));
                    m_editorCtx->selectedEntities.clear();
                }
                resp["ok"] = true;
                resp["result"] = {{"groups", targets.size()}, {"created", created}, {"ms", ms}, {"note", "個別エンティティに戻しました（名前は「<モデル名>_0001」…、guid は新しい値）。Undo で群に戻せます"}};
            }
            else if (op == "info")
            {
                const entt::entity e = resolveGroup();
                json j = groupJson(e);
                const auto& g = reg.get<InstanceGroup>(e);
                if (const auto* t = reg.try_get<Transform>(e))
                    if (t->parent != entt::null && reg.valid(t->parent))
                        if (const auto* pn = reg.try_get<NameTag>(t->parent)) j["parent"] = pn->name;
                DirectX::XMFLOAT3 mn, mx; bool hasMesh = false;
                if (McpWorldAabb(reg, e, mn, mx, hasMesh) && hasMesh)
                    j["bounds"] = {{"min", {mn.x, mn.y, mn.z}}, {"max", {mx.x, mx.y, mx.z}}};
                if (const auto* mr = reg.try_get<MeshRenderer>(e)) j["submeshes"] = static_cast<u32>(mr->meshes.size());
                j["collider"] = reg.all_of<MeshCollider>(e) ? "meshCollider(static)" : "none";
                if (m_editorCtx && !m_editorCtx->currentScenePath.empty())
                {
                    if (const auto* gu = reg.try_get<EntityGuid>(e); gu && gu->value != 0)
                        j["sidecar"] = instgroup::SidecarPathFor(m_editorCtx->currentScenePath, FormatEntityGuidHex(gu->value));
                }
                const u32 off = static_cast<u32>(McpIntParam(params, "offset", 0, 0, 100000000));
                const u32 lim = static_cast<u32>(McpIntParam(params, "limit", 10, 0, 1000));
                json arr = json::array();
                if (g._set)
                    for (u32 i = off; i < g._set->Count() && arr.size() < lim; ++i)
                    {
                        json it = TrsJson(g._set->items[i]);
                        it["index"] = i;
                        arr.push_back(std::move(it));
                    }
                j["instances"] = arr;
                resp["ok"] = true;
                resp["result"] = j;
            }
            else if (op == "add" || op == "remove" || op == "set")
            {
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は編集できません", "先に dx12_stop で Editor へ戻してください");
                const entt::entity e = resolveGroup();
                McpUndo().TrackByJsonKey(e, "instanceGroup");
                instgroup::InstanceEdit r;
                if (op == "add")
                {
                    if (!params.contains("instances") || !params["instances"].is_array() || params["instances"].empty())
                        throw McpError(McpErr::InvalidParam, "instances（配列）が要ります", "例: [[0,0,0],[2,0,0,0,90,0]]");
                    std::vector<InstanceTRS> items;
                    items.reserve(params["instances"].size());
                    size_t k = 0;
                    for (const auto& j : params["instances"])
                    {
                        InstanceTRS t;
                        if (!ParseTrs(j, t))
                            throw McpError(McpErr::InvalidParam, "instances[" + std::to_string(k) + "] の形が不正です",
                                           "[px,py,pz] / [px,py,pz,rx,ry,rz] / [px,py,pz,rx,ry,rz,sx,sy,sz] か {position,rotation,scale}");
                        items.push_back(t);
                        ++k;
                    }
                    r = instgroup::AddInstances(*m_scene, e, items);
                }
                else if (op == "remove")
                {
                    if (!params.contains("indices") || !params["indices"].is_array())
                        throw McpError(McpErr::InvalidParam, "indices（配列）が要ります", "例: indices:[0,5,7]（消すインスタンス番号。dx12_instance_group {op:\"info\"} で番号を確かめる）");
                    std::vector<u32> idx;
                    for (const auto& v : params["indices"])
                        if (v.is_number_integer() && v.get<i64>() >= 0) idx.push_back(static_cast<u32>(v.get<i64>()));
                    r = instgroup::RemoveInstances(*m_scene, e, idx);
                }
                else
                {
                    if (!params.contains("index") || !params.contains("value"))
                        throw McpError(McpErr::InvalidParam, "index と value が要ります", "例: index:3, value:[10,0,5,0,90,0]");
                    InstanceTRS t;
                    if (!ParseTrs(params["value"], t))
                        throw McpError(McpErr::InvalidParam, "value の形が不正です", "[px,py,pz,(rx,ry,rz,(sx,sy,sz))] か {position,rotation,scale}");
                    r = instgroup::SetInstance(*m_scene, e, params["index"].get<u32>(), t);
                }
                if (!r.ok) throw McpError(McpErr::InvalidParam, r.error, "index は 0 以上、個数未満。op:\"info\" で個数を確かめる");
                resp["ok"] = true;
                resp["result"] = groupJson(e);
            }
            else
            {
                throw McpError(McpErr::InvalidParam, "op は convert / explode / info / add / remove / set のどれか", "", {"convert", "explode", "info", "add", "remove", "set"});
            }
        });
}

} // namespace dx12e
