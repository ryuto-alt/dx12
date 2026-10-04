#include "scene/InstanceGroupOps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "core/Logger.h"
#include "ecs/Components.h"
#include "ecs/EditorFlags.h"
#include "renderer/Mesh.h"
// SkeletalAnimation は unique_ptr メンバを持つので、all_of でもストレージの実体化に完全型が要る。
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/AnimGraphRuntime.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

using json = nlohmann::json;
using namespace DirectX;

namespace dx12e::instgroup
{

namespace
{
u64 NewGuid()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    u64 v = 0;
    while (v == 0) v = rng();
    return v;
}

u64 EnsureGuid(entt::registry& reg, entt::entity e)
{
    auto* g = reg.try_get<EntityGuid>(e);
    if (!g) { const u64 v = NewGuid(); reg.emplace<EntityGuid>(e, EntityGuid{ v }); return v; }
    if (g->value == 0) g->value = NewGuid();
    return g->value;
}

// 群にできるエンティティの JSON キー（これ以外のキーがあれば変換しない）。
// ★キーを足すときは「共有メッシュ・共有材質で全インスタンスが同じ見た目 / 当たりになる」ものだけにすること。
bool IsAllowedKey(const std::string& k)
{
    static const char* const kAllowed[] = {
        "meshRenderer", "material", "materialTextureOverrides", "materialAssets", "color",
        "shader", "shaderAlphaBlend", "shaderEffectValue", "shaderParams", "shaderParamsB",
        "uvScroll", "flipbook", "rigidBody", "meshCollider",
    };
    for (const char* a : kAllowed) if (k == a) return true;
    return false;
}

std::string ModelStem(const std::string& modelPath)
{
    std::string s = std::filesystem::path(modelPath).stem().string();
    return s.empty() ? std::string("Model") : s;
}

void CollectStrings(const json& j, std::unordered_set<std::string>& out)
{
    if (j.is_string()) { out.insert(j.get<std::string>()); return; }
    if (j.is_array()) { for (const auto& v : j) CollectStrings(v, out); return; }
    if (j.is_object()) { for (auto it = j.begin(); it != j.end(); ++it) CollectStrings(it.value(), out); }
}

XMFLOAT3 Euler(const Transform& t) { return t.rotation; }

// ログを抑えて破棄する（Scene::Remove は 1 体ごとに Info を出すので使わない）
void DestroyQuiet(entt::registry& reg, entt::entity e)
{
    if (reg.valid(e)) reg.destroy(e);
}

entt::entity Instantiate(Scene& scene, const std::string& json, const std::string& assetsDir, bool keepGuid)
{
    return SceneSerializer::InstantiateEntity(scene, json, assetsDir, keepGuid);
}
} // namespace

// ---------------------------------------------------------------------------
// 変換
// ---------------------------------------------------------------------------
ConvertResult ConvertToGroups(Scene& scene, const std::string& assetsDir, const ConvertOptions& opt)
{
    ConvertResult res;
    auto& reg = scene.GetRegistry();
    for (auto e : reg.view<const NameTag, const Transform>()) { (void)e; ++res.entitiesBefore; }

    // 子を持つエンティティ（群にできない）
    std::unordered_set<entt::entity> hasChildren;
    for (auto [e, t] : reg.view<const Transform>().each())
        if (t.parent != entt::null && reg.valid(t.parent)) hasChildren.insert(t.parent);

    // 他から（guid か名前で）参照されうる文字列。Trigger / LuaScript を持つエンティティの JSON にある全文字列。
    // 候補の guid（16 桁 hex）または名前と一致するものは変換しない（参照が切れる）。
    std::unordered_set<std::string> refStrings;
    for (auto e : reg.view<const NameTag, const Transform>())
    {
        if (!reg.all_of<Trigger>(e) && !reg.all_of<LuaScript>(e)) continue;
        json ej = json::parse(SceneSerializer::SerializeEntity(scene, e, assetsDir), nullptr, false);
        if (!ej.is_object()) continue;
        ej.erase("name"); ej.erase("guid"); ej.erase("parentGuid");
        CollectStrings(ej, refStrings);
    }

    // シーケンサー（assets/sequences/*.dxseq）のバインディングは guid / 名前 / 階層パスでエンティティを引く。
    // 群にすると名前と guid が変わって再生時に解決できなくなるので、シーケンスのどこかに出てくる文字列
    // （パスは "/" で分けた各要素も）と一致するエンティティは変換しない（安全側。他の文字列との偶然の一致でも見送るだけ）。
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path seqDir = fs::path(assetsDir) / "sequences";
        if (fs::is_directory(seqDir, ec))
        {
            for (fs::recursive_directory_iterator it(seqDir, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec) || it->path().extension() != ".dxseq") continue;
                std::ifstream f(it->path(), std::ios::binary);
                if (!f) continue;
                json sj = json::parse(f, nullptr, /*allow_exceptions=*/false);
                if (sj.is_discarded()) continue;
                std::unordered_set<std::string> strs;
                CollectStrings(sj, strs);
                for (const std::string& s : strs)
                {
                    refStrings.insert(s);
                    size_t b = 0;
                    while (b <= s.size())
                    {
                        const size_t e2 = s.find('/', b);
                        const std::string part = s.substr(b, e2 == std::string::npos ? std::string::npos : e2 - b);
                        if (!part.empty()) refStrings.insert(part);
                        if (e2 == std::string::npos) break;
                        b = e2 + 1;
                    }
                }
            }
        }
    }

    std::unordered_set<entt::entity> targetSet(opt.targets.begin(), opt.targets.end());
    const bool explicitMode = !opt.targets.empty();

    struct Bucket { entt::entity parent; std::vector<entt::entity> members; std::string modelPath; };
    std::unordered_map<std::string, Bucket> buckets;   // 署名 → 候補
    std::vector<std::string> bucketOrder;              // 決定論のため出現順を持つ

    auto skip = [&](const std::string& reason) { ++res.skipped[reason]; };

    auto view = reg.view<const NameTag, const Transform, const MeshRenderer>(entt::exclude<GridPlane, InstanceGroup>);
    for (auto [e, nt, t, mr] : view.each())
    {
        if (explicitMode && !targetSet.count(e)) continue;
        ++res.candidates;
        if (mr.meshes.empty()) { skip("メッシュが読めていない"); continue; }
        if (mr.modelPath.empty() || mr.modelPath.rfind("__", 0) == 0) { skip("プリミティブ / 生成メッシュ"); continue; }
        if (hasChildren.count(e)) { skip("子を持つ"); continue; }
        if (t.useQuaternion) { skip("クォータニオン姿勢（物理で動いた）"); continue; }
        if (reg.all_of<SkeletalAnimation>(e) || reg.all_of<NodeAnimationComp>(e)) { skip("スキン / ノードアニメ"); continue; }
        if (reg.all_of<VirtualGeometry>(e)) { skip("仮想ジオメトリ"); continue; }
        if (reg.all_of<EditorHidden>(e) || reg.all_of<EntityDisabled>(e)) { skip("非表示 / 無効"); continue; }
        if (reg.all_of<Terrain>(e) || reg.all_of<SculptMesh>(e)) { skip("地形 / スカルプト"); continue; }
        if (mr.uvScaleU != 1.0f || mr.uvScaleV != 1.0f) { skip("uvTiling（頂点を書き換えるので共有メッシュと衝突）"); continue; }
        if (const auto* rb = reg.try_get<RigidBody>(e))
        {
            if (rb->motionType != MotionType::Static) { skip("動く剛体（静的のみ対応）"); continue; }
            if (!reg.all_of<MeshCollider>(e)) { skip("剛体にメッシュコライダーが無い"); continue; }
        }
        else if (reg.all_of<MeshCollider>(e)) { skip("メッシュコライダーに剛体が無い"); continue; }

        const auto* g = reg.try_get<EntityGuid>(e);
        if (g && g->value != 0 && refStrings.count(FormatEntityGuidHex(g->value))) { skip("guid で参照されている（Trigger / Lua / シーケンサー）"); continue; }
        if (refStrings.count(nt.name)) { skip("名前で参照されている（Trigger / Lua / シーケンサー）"); continue; }

        json ej = json::parse(SceneSerializer::SerializeEntity(scene, e, assetsDir), nullptr, false);
        if (!ej.is_object()) { skip("直列化できない"); continue; }
        ej.erase("name"); ej.erase("guid"); ej.erase("parentGuid"); ej.erase("parent"); ej.erase("transform");
        bool bad = false;
        for (auto it = ej.begin(); it != ej.end(); ++it)
            if (!IsAllowedKey(it.key())) { skip("コンポーネント " + it.key() + " を持つ"); bad = true; break; }
        if (bad) continue;

        const u32 parentId = (t.parent != entt::null && reg.valid(t.parent)) ? static_cast<u32>(entt::to_integral(t.parent)) : 0xFFFFFFFFu;
        const std::string sig = std::to_string(parentId) + "|" + ej.dump();
        auto it = buckets.find(sig);
        if (it == buckets.end())
        {
            it = buckets.emplace(sig, Bucket{ t.parent, {}, mr.modelPath }).first;
            bucketOrder.push_back(sig);
        }
        it->second.members.push_back(e);
    }

    const u32 minCount = explicitMode ? 2u : (std::max)(2u, opt.minCount);
    for (const std::string& sig : bucketOrder)
    {
        Bucket& b = buckets[sig];
        if (b.members.size() < minCount)
        {
            res.skipped["同じ条件の配置が少ない（" + std::to_string(minCount) + " 個未満）"] += static_cast<u32>(b.members.size());
            continue;
        }

        // 並び: guid 昇順（guid が無いものは entity 番号順で後ろ）。インスタンス番号 = この順。
        std::vector<std::pair<u64, entt::entity>> order;
        order.reserve(b.members.size());
        for (entt::entity m : b.members)
        {
            const auto* g = reg.try_get<EntityGuid>(m);
            order.emplace_back(g ? g->value : 0ull, m);
        }
        std::sort(order.begin(), order.end(), [](const auto& x, const auto& y) {
            const bool xz = x.first == 0, yz = y.first == 0;
            if (xz != yz) return !xz;                       // guid ありが先
            if (x.first != y.first) return x.first < y.first;
            return entt::to_integral(x.second) < entt::to_integral(y.second);
        });

        GroupSummary gs;
        gs.modelPath = b.modelPath;
        gs.count = static_cast<u32>(order.size());
        gs.name = ModelStem(b.modelPath) + " \xC3\x97" + std::to_string(order.size());   // 「<モデル名> ×N」（× は UTF-8）
        if (opt.dryRun)
        {
            res.groups.push_back(gs);
            res.converted += gs.count;
            ++res.groupsCreated;
            continue;
        }

        // ---- 実行: Undo 用の記録（変換前）→ 群の実体を作る → 最初のメンバーを群に変える → 残りを消す ----
        GroupUndoRecord rec;
        rec.memberJson.reserve(order.size());
        std::vector<InstanceTRS> items;
        items.reserve(order.size());
        for (auto& [gv, m] : order)
        {
            const u64 guid = EnsureGuid(reg, m);   // 記録に guid を載せるため先に振る
            rec.memberGuids.push_back(guid);
            rec.memberJson.push_back(SceneSerializer::SerializeEntity(scene, m, assetsDir));
            const Transform& t = reg.get<Transform>(m);
            InstanceTRS it;
            it.p = t.position; it.r = Euler(t); it.s = t.scale;
            items.push_back(it);
        }
        if (b.parent != entt::null && reg.valid(b.parent)) rec.parentGuid = EnsureGuid(reg, b.parent);

        const entt::entity first = order.front().second;
        auto set = NewSet(std::move(items));
        for (size_t k = 1; k < order.size(); ++k) DestroyQuiet(reg, order[k].second);

        // 群の Transform は恒等（インスタンスは元のローカル TRS のまま。親は元と同じ）
        Transform& gt = reg.get<Transform>(first);
        const entt::entity parent = gt.parent;
        const int sib = gt.siblingOrder;
        gt = Transform{};
        gt.parent = parent;
        gt.siblingOrder = sib;
        reg.get<NameTag>(first).name = gs.name;
        reg.get<EntityGuid>(first).value = NewGuid();   // 群は新しい実体（元の guid は参照されていないことを確認済み）
        reg.emplace_or_replace<InstanceGroup>(first, InstanceGroup{ set });

        rec.groupGuid = reg.get<EntityGuid>(first).value;
        rec.groupJson = SceneSerializer::SerializeEntity(scene, first, assetsDir);
        gs.entity = first;
        res.groups.push_back(gs);
        res.converted += gs.count;
        ++res.groupsCreated;
        res.undo.push_back(std::move(rec));
    }

    res.entitiesAfter = 0;
    for (auto e : reg.view<const NameTag, const Transform>()) { (void)e; ++res.entitiesAfter; }
    if (opt.dryRun) res.entitiesAfter = res.entitiesBefore - res.converted + res.groupsCreated;
    return res;
}

// ---------------------------------------------------------------------------
// 展開
// ---------------------------------------------------------------------------
ExplodeResult ExplodeGroup(Scene& scene, entt::entity group, const std::string& assetsDir)
{
    ExplodeResult res;
    auto& reg = scene.GetRegistry();
    if (!reg.valid(group) || !reg.all_of<InstanceGroup>(group))
    {
        res.ok = false; res.error = "インスタンス群ではありません";
        return res;
    }
    const InstanceGroup& g = reg.get<InstanceGroup>(group);
    if (!g._set) { res.ok = false; res.error = "インスタンスの実体がありません"; return res; }
    const auto set = g._set;   // 共有（この後エンティティを消しても実体は残る）
    const Transform gt = reg.get<Transform>(group);
    const entt::entity parent = gt.parent;
    const std::string stem = reg.all_of<MeshRenderer>(group) ? ModelStem(reg.get<MeshRenderer>(group).modelPath) : std::string("Instance");

    // Undo 用（群側）
    res.undo.groupGuid = EnsureGuid(reg, group);
    res.undo.groupJson = SceneSerializer::SerializeEntity(scene, group, assetsDir);
    if (parent != entt::null && reg.valid(parent)) res.undo.parentGuid = EnsureGuid(reg, parent);

    // テンプレート JSON（群の全コンポーネントから instanceGroup / guid を除いたもの）
    json tj = json::parse(res.undo.groupJson, nullptr, false);
    if (!tj.is_object()) { res.ok = false; res.error = "群を直列化できません"; return res; }
    tj.erase("instanceGroup"); tj.erase("guid"); tj.erase("parentGuid"); tj.erase("parent");

    // 群の Transform が恒等でなければ、インスタンスの TRS を群の行列で合成して分解する（恒等ならビット一致でコピー）
    const bool identity = gt.position.x == 0 && gt.position.y == 0 && gt.position.z == 0
        && gt.rotation.x == 0 && gt.rotation.y == 0 && gt.rotation.z == 0
        && gt.scale.x == 1 && gt.scale.y == 1 && gt.scale.z == 1 && !gt.useQuaternion;
    const XMMATRIX gm = gt.GetWorldMatrix();

    SceneSerializer::NameIndexScope nameScope(scene);   // 1 体ずつの名前重複検査が全走査（体数の 2 乗）にならないように
    const u32 n = set->Count();
    res.entities.reserve(n);
    res.undo.memberJson.reserve(n);
    res.undo.memberGuids.reserve(n);
    char numbuf[16];
    for (u32 i = 0; i < n; ++i)
    {
        InstanceTRS it = set->items[i];
        if (!identity)
        {
            XMVECTOR s, q, p;
            if (XMMatrixDecompose(&s, &q, &p, LocalMatrix(it) * gm))
            {
                XMFLOAT4 qf;
                XMStoreFloat3(&it.s, s); XMStoreFloat3(&it.p, p); XMStoreFloat4(&qf, q);
                it.r = QuaternionToEulerDegrees(qf);
            }
        }
        std::snprintf(numbuf, sizeof(numbuf), "_%04u", i + 1);
        json ej = tj;
        ej["name"] = stem + numbuf;
        ej["transform"] = {
            {"position", json::array({it.p.x, it.p.y, it.p.z})},
            {"rotation", json::array({it.r.x, it.r.y, it.r.z})},
            {"scale",    json::array({it.s.x, it.s.y, it.s.z})},
        };
        const entt::entity e = Instantiate(scene, ej.dump(), assetsDir, /*keepGuid=*/false);
        if (e == entt::null) continue;
        if (parent != entt::null && reg.valid(parent)) reg.get<Transform>(e).parent = parent;
        res.entities.push_back(e);
    }
    res.created = static_cast<u32>(res.entities.size());
    for (entt::entity e : res.entities)
    {
        res.undo.memberGuids.push_back(EnsureGuid(reg, e));
        res.undo.memberJson.push_back(SceneSerializer::SerializeEntity(scene, e, assetsDir));
    }
    DestroyQuiet(reg, group);
    return res;
}

// ---------------------------------------------------------------------------
// Undo / Redo
// ---------------------------------------------------------------------------
void ApplyUndoRecord(Scene& scene, const std::string& assetsDir, const GroupUndoRecord& rec, bool toGroup)
{
    auto& reg = scene.GetRegistry();
    entt::entity parent = rec.parentGuid != 0 ? FindEntityByGuid(reg, rec.parentGuid) : entt::null;
    if (toGroup)
    {
        for (u64 g : rec.memberGuids)
            if (const entt::entity e = FindEntityByGuid(reg, g); e != entt::null) DestroyQuiet(reg, e);
        const entt::entity ge = Instantiate(scene, rec.groupJson, assetsDir, /*keepGuid=*/true);
        if (ge != entt::null && parent != entt::null && reg.valid(parent)) reg.get<Transform>(ge).parent = parent;
    }
    else
    {
        if (const entt::entity e = FindEntityByGuid(reg, rec.groupGuid); e != entt::null) DestroyQuiet(reg, e);
        SceneSerializer::NameIndexScope nameScope(scene);
        for (const std::string& mj : rec.memberJson)
        {
            const entt::entity e = Instantiate(scene, mj, assetsDir, /*keepGuid=*/true);
            if (e != entt::null && parent != entt::null && reg.valid(parent)) reg.get<Transform>(e).parent = parent;
        }
    }
}

// ---------------------------------------------------------------------------
// インスタンスの編集
// ---------------------------------------------------------------------------
namespace
{
InstanceGroup* GroupOf(Scene& scene, entt::entity e, InstanceEdit& out)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(e) || !reg.all_of<InstanceGroup>(e)) { out.ok = false; out.error = "インスタンス群ではありません"; return nullptr; }
    return &reg.get<InstanceGroup>(e);
}
} // namespace

InstanceEdit AddInstances(Scene& scene, entt::entity group, const std::vector<InstanceTRS>& items)
{
    InstanceEdit r;
    InstanceGroup* g = GroupOf(scene, group, r);
    if (!g) return r;
    auto n = g->_set ? CloneSet(*g->_set) : NewSet(std::vector<InstanceTRS>{});
    n->items.insert(n->items.end(), items.begin(), items.end());
    r.count = n->Count();
    g->_set = std::move(n);
    return r;
}

InstanceEdit RemoveInstances(Scene& scene, entt::entity group, const std::vector<u32>& indices)
{
    InstanceEdit r;
    InstanceGroup* g = GroupOf(scene, group, r);
    if (!g) return r;
    if (!g->_set) return r;
    std::vector<char> kill(g->_set->items.size(), 0);
    for (u32 i : indices) if (i < kill.size()) kill[i] = 1;
    std::vector<InstanceTRS> keep;
    keep.reserve(kill.size());
    for (size_t i = 0; i < kill.size(); ++i) if (!kill[i]) keep.push_back(g->_set->items[i]);
    auto n = NewSet(std::move(keep));
    r.count = n->Count();
    g->_set = std::move(n);
    return r;
}

InstanceEdit SetInstance(Scene& scene, entt::entity group, u32 index, const InstanceTRS& value)
{
    InstanceEdit r;
    InstanceGroup* g = GroupOf(scene, group, r);
    if (!g) return r;
    if (!g->_set || index >= g->_set->Count()) { r.ok = false; r.error = "インスタンス番号が範囲外です"; return r; }
    auto n = CloneSet(*g->_set);
    n->items[index] = value;
    r.count = n->Count();
    g->_set = std::move(n);
    return r;
}

} // namespace dx12e::instgroup
