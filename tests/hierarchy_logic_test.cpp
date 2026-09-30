// ヒエラルキーの純ロジック（editor/HierarchyLogic.h）と、エディタ専用フラグの保存互換の単体テスト（フェーズ 1b）。
//   ・型の分類 / 検索クエリ（名前 + コンポーネント名 + タグ。接頭辞 tag: c: t: n: is:）
//   ・親→子の索引（兄弟は (siblingOrder, id)）/ 表示行 / 構造の指紋
//   ・範囲選択 / ドロップ位置（前・中・後）→ 親替え + 兄弟順の計画（適用して並びを検証）
//   ・hidden / locked / disabled の祖先伝播 / Alt+クリック（これだけ表示 → もう一度で全部戻す）の状態遷移
//   ・Undo コマンド（EditorFlagCommand）
//   ・シーン JSON: フラグが無いエンティティの JSON は 1 キーも増えない / 旧 JSON（キー無し）は従来どおり / 往復
// GPU も窓も要らない。

#include "editor/HierarchyLogic.h"
#include "editor/HierarchyCommands.h"

#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::hier;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

entt::entity Make(entt::registry& reg, const char* name, entt::entity parent = entt::null)
{
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{name});
    Transform t;
    t.parent = parent;
    reg.emplace<Transform>(e, t);
    return e;
}

std::vector<std::string> Names(const entt::registry& reg, const std::vector<entt::entity>& v)
{
    std::vector<std::string> r;
    for (entt::entity e : v) r.push_back(reg.get<NameTag>(e).name);
    return r;
}
std::vector<std::string> ChildNames(const entt::registry& reg, const Index& idx, entt::entity p)
{
    const auto [b, n] = idx.Children(p);
    std::vector<std::string> r;
    for (size_t i = 0; i < n; ++i) r.push_back(reg.get<NameTag>(b[i]).name);
    return r;
}
bool Eq(const std::vector<std::string>& a, std::initializer_list<const char*> b)
{
    if (a.size() != b.size()) return false;
    size_t i = 0;
    for (const char* s : b) if (a[i++] != s) return false;
    return true;
}

// 計画を registry へ適用する（パネルと同じ手順: 親を替えてから番号を書く）
void Apply(entt::registry& reg, const DropPlan& p)
{
    for (entt::entity m : p.moving) reg.get<Transform>(m).parent = p.newParent;
    for (const OrderChange& o : p.orders) reg.get<Transform>(o.e).siblingOrder = o.order;
}

// ---------------------------------------------------------------------------
void TestTypes()
{
    entt::registry reg;
    auto a = Make(reg, "a"); reg.emplace<MeshRenderer>(a);
    auto b = Make(reg, "b"); reg.emplace<PointLight>(b);
    auto c = Make(reg, "c"); reg.emplace<CameraComponent>(c);
    auto d = Make(reg, "d"); reg.emplace<UIRect>(d);
    auto e = Make(reg, "e"); reg.emplace<RigidBody>(e); reg.emplace<MeshRenderer>(e);
    auto f = Make(reg, "f"); reg.emplace<EditorFolder>(f);
    auto g = Make(reg, "g");
    CHECK(ClassifyTypes(reg, a) == kTMesh);
    CHECK(ClassifyTypes(reg, b) == kTLight);
    CHECK(ClassifyTypes(reg, c) == kTCamera);
    CHECK(ClassifyTypes(reg, d) == kTUi);
    CHECK(ClassifyTypes(reg, e) == (kTMesh | kTPhysics));   // 複数の型
    CHECK(ClassifyTypes(reg, f) == kTFolder);
    CHECK(ClassifyTypes(reg, g) == 0);
    CHECK(kAllTypes == (1u << kTypeCount) - 1u);
    // 型の key が重複していない
    for (int i = 0; i < kTypeCount; ++i)
        for (int j = i + 1; j < kTypeCount; ++j)
            CHECK(std::string(kTypes[i].key) != kTypes[j].key && kTypes[i].bit != kTypes[j].bit);
}

void TestQuery()
{
    entt::registry reg;
    auto door = Make(reg, "Door_A"); reg.emplace<MeshRenderer>(door);
    auto lamp = Make(reg, "Lamp");   reg.emplace<PointLight>(lamp);
    Tag tg; tg.tags = {"enemy", "boss"};
    auto orc = Make(reg, "Orc");     reg.emplace<Tag>(orc, tg); reg.emplace<MeshRenderer>(orc);
    auto cam = Make(reg, "MainCam"); reg.emplace<CameraComponent>(cam);

    auto match = [&](const char* q, entt::entity e, unsigned chips = 0) {
        return Matches(reg, e, reg.get<NameTag>(e).name, ParseQuery(q), chips);
    };
    // 名前（大文字小文字を無視）
    CHECK(match("door", door));
    CHECK(match("DOOR_a", door));
    CHECK(!match("door", lamp));
    // タグ
    CHECK(match("tag:enemy", orc));
    CHECK(match("tag:BOS", orc));
    CHECK(!match("tag:enemy", door));
    // コンポーネント名
    CHECK(match("c:light", lamp));
    CHECK(match("c:pointlight", lamp));
    CHECK(!match("c:light", door));
    CHECK(match("c:mesh", door) && match("c:mesh", orc));
    // 型
    CHECK(match("t:mesh", door));
    CHECK(match("t:light", lamp));
    CHECK(match("t:ライト", lamp));
    CHECK(match("t:cam", cam));       // 別名
    CHECK(match("t:mes", door));      // 前方一致
    CHECK(!match("t:mesh", lamp));
    // n: は名前だけ（タグに当たっても通らない）
    CHECK(match("n:orc", orc));
    CHECK(!match("n:enemy", orc));
    // 接頭辞なしの語は 名前 or タグ or コンポーネント名
    CHECK(match("enemy", orc));      // タグ
    CHECK(match("pointlight", lamp));// コンポーネント名
    CHECK(match("camera", cam));
    CHECK(!match("zzz", orc));
    // AND
    CHECK(match("orc tag:enemy", orc));
    CHECK(!match("orc tag:friend", orc));
    CHECK(match("  door   c:mesh ", door));   // 余分な空白
    // 型チップ（OR）
    CHECK(match("", door, kTMesh));
    CHECK(match("", lamp, kTMesh | kTLight));
    CHECK(!match("", cam, kTMesh | kTLight));
    CHECK(match("", cam, 0));   // チップ無しは絞らない
    // 空クエリ
    CHECK(ParseQuery("").Empty());
    CHECK(ParseQuery("   ").Empty());
    CHECK(!ParseQuery("a").Empty());
    // 状態
    reg.emplace<EditorHidden>(door);
    reg.emplace<EditorLocked>(lamp);
    CHECK(match("is:hidden", door) && !match("is:hidden", lamp));
    CHECK(match("is:locked", lamp) && !match("is:locked", door));
    // 壊れた接頭辞（値が空）は名前として扱う（何にも一致しない）
    CHECK(!match("tag:", door));
    // 署名が変わる
    CHECK(ParseQuery("a").Signature() != ParseQuery("b").Signature());
    CHECK(ParseQuery("tag:a").Signature() != ParseQuery("n:a").Signature());
}

void TestIndexAndRows()
{
    entt::registry reg;
    auto r1 = Make(reg, "R1");
    auto r2 = Make(reg, "R2");
    auto c1 = Make(reg, "C1", r1);
    auto c2 = Make(reg, "C2", r1);
    auto c3 = Make(reg, "C3", r1);
    auto g1 = Make(reg, "G1", c2);
    (void)r2; (void)c1; (void)c3; (void)g1;
    Index idx; idx.Build(reg);
    CHECK(idx.valid);
    CHECK(idx.roots.size() == 2);
    CHECK(Eq(ChildNames(reg, idx, r1), {"C1", "C2", "C3"}));   // 全員 0 → id 昇順
    CHECK(idx.ChildCount(c2) == 1 && idx.HasChildren(c2));
    CHECK(!idx.HasChildren(r2));

    // siblingOrder が効く
    reg.get<Transform>(c1).siblingOrder = 30;
    reg.get<Transform>(c2).siblingOrder = 10;
    reg.get<Transform>(c3).siblingOrder = 20;
    idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, r1), {"C2", "C3", "C1"}));
    // 同じ番号なら id 昇順
    reg.get<Transform>(c1).siblingOrder = 5;
    reg.get<Transform>(c2).siblingOrder = 5;
    reg.get<Transform>(c3).siblingOrder = 5;
    idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, r1), {"C1", "C2", "C3"}));

    // 行: 閉じていれば親だけ / 開けば子が 1 段
    std::unordered_set<entt::entity> open;
    std::vector<Row> rows;
    BuildRows(idx, open, rows);
    CHECK(rows.size() == 2);
    open.insert(r1);
    BuildRows(idx, open, rows);
    CHECK(rows.size() == 5);
    CHECK(rows[0].e == idx.roots[0] && rows[0].depth == 0);
    CHECK(rows[1].depth == 1 && rows[4].depth == 0);
    open.insert(c2);
    BuildRows(idx, open, rows);
    CHECK(rows.size() == 6);
    CHECK(reg.get<NameTag>(rows[3].e).name == "G1" && rows[3].depth == 2);

    // ルートの順序: 全員 0 なら registry の走査順のまま、非 0 があれば安定ソート
    const auto rootsBefore = idx.roots;
    reg.get<Transform>(r2).siblingOrder = -1;
    idx.Build(reg);
    CHECK(idx.roots.front() == r2);
    reg.get<Transform>(r2).siblingOrder = 0;
    idx.Build(reg);
    CHECK(idx.roots == rootsBefore);

    // 無効な親を指す子はルート扱い（壊れたデータで落ちない）
    auto orphan = Make(reg, "Orphan");
    const entt::entity dead = reg.create();
    reg.destroy(dead);
    reg.get<Transform>(orphan).parent = dead;
    idx.Build(reg);
    CHECK(std::find(idx.roots.begin(), idx.roots.end(), orphan) != idx.roots.end());

    // GridPlane は出さない
    auto grid = Make(reg, "Grid"); reg.emplace<GridPlane>(grid);
    idx.Build(reg);
    CHECK(std::find(idx.roots.begin(), idx.roots.end(), grid) == idx.roots.end());
}

void TestStructureKey()
{
    const uint64_t a = StructureKey(10, 10, 5, 3, 0, 0);
    CHECK(a == StructureKey(10, 10, 5, 3, 0, 0));
    CHECK(a != StructureKey(11, 10, 5, 3, 0, 0));
    CHECK(a != StructureKey(10, 11, 5, 3, 0, 0));
    CHECK(a != StructureKey(10, 10, 6, 3, 0, 0));
    CHECK(a != StructureKey(10, 10, 5, 2, 1, 0));   // Undo（EditSeq は進まないが深さが動く）
    CHECK(a != StructureKey(10, 10, 5, 3, 0, 1));
}

void TestRange()
{
    entt::registry reg;
    std::vector<Row> rows;
    std::vector<entt::entity> es;
    for (int i = 0; i < 6; ++i) { es.push_back(Make(reg, "x")); rows.push_back({es.back(), 0}); }
    std::vector<entt::entity> out;
    CHECK(RangeBetween(rows, es[1], es[3], out) && out.size() == 3 && out[0] == es[1] && out[2] == es[3]);
    CHECK(RangeBetween(rows, es[4], es[2], out) && out.size() == 3 && out[0] == es[2]);   // 逆向き
    CHECK(RangeBetween(rows, es[2], es[2], out) && out.size() == 1);
    CHECK(!RangeBetween(rows, es[0], entt::null, out));
    const entt::entity gone = reg.create();
    CHECK(!RangeBetween(rows, es[0], gone, out));   // 見えている行に無い（畳まれた）→ 呼び出し側が単独選択へ退避
}

void TestZones()
{
    CHECK(ZoneFromRatio(0.0f) == DropZone::Before);
    CHECK(ZoneFromRatio(0.24f) == DropZone::Before);
    CHECK(ZoneFromRatio(0.5f) == DropZone::Onto);
    CHECK(ZoneFromRatio(0.76f) == DropZone::After);
    CHECK(ZoneFromRatio(1.0f) == DropZone::After);
    CHECK(ZoneFromRatio(0.3f, false) == DropZone::Before);
    CHECK(ZoneFromRatio(0.7f, false) == DropZone::After);
}

void TestDrop()
{
    entt::registry reg;
    auto p = Make(reg, "P");
    auto a = Make(reg, "A", p), b = Make(reg, "B", p), c = Make(reg, "C", p), d = Make(reg, "D", p);
    Index idx; idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, p), {"A", "B", "C", "D"}));

    // D を B の前へ
    auto plan = PlanDrop(reg, {d}, b, DropZone::Before, idx);
    CHECK(plan.ok && plan.newParent == p && plan.moving.size() == 1);
    Apply(reg, plan); idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, p), {"A", "D", "B", "C"}));

    // A を C の後ろへ（番号が付いた状態からの隙間入れ）
    plan = PlanDrop(reg, {a}, c, DropZone::After, idx);
    CHECK(plan.ok);
    Apply(reg, plan); idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, p), {"D", "B", "C", "A"}));

    // 先頭 / 末尾へ
    plan = PlanDrop(reg, {a}, d, DropZone::Before, idx);
    Apply(reg, plan); idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, p), {"A", "D", "B", "C"}));
    plan = PlanDrop(reg, {a}, c, DropZone::After, idx);
    Apply(reg, plan); idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, p), {"D", "B", "C", "A"}));

    // 何度繰り返しても並びが正しい（隙間が尽きたら振り直す）
    for (int i = 0; i < 40; ++i)
    {
        const entt::entity last = idx.Children(p).first[idx.ChildCount(p) - 1];
        const entt::entity first = idx.Children(p).first[0];
        plan = PlanDrop(reg, {last}, first, DropZone::Before, idx);
        CHECK(plan.ok);
        Apply(reg, plan); idx.Build(reg);
        CHECK(idx.Children(p).first[0] == last && idx.ChildCount(p) == 4);
    }

    // 複数を一度に（選択順のまま挿入）
    plan = PlanDrop(reg, {b, c}, idx.Children(p).first[0], DropZone::After, idx);
    CHECK(plan.ok && plan.moving.size() == 2);
    Apply(reg, plan); idx.Build(reg);
    {
        const auto n = ChildNames(reg, idx, p);
        // 先頭の次に B, C が並ぶ
        CHECK(n.size() == 4 && n[1] == "B" && n[2] == "C");
    }

    // 別の親へ（Before/After で target の親が新しい親になる）
    auto q = Make(reg, "Q");
    auto qa = Make(reg, "QA", q);
    idx.Build(reg);
    plan = PlanDrop(reg, {d}, qa, DropZone::After, idx);
    CHECK(plan.ok && plan.newParent == q);
    Apply(reg, plan); idx.Build(reg);
    CHECK(Eq(ChildNames(reg, idx, q), {"QA", "D"}));
    CHECK(idx.ChildCount(p) == 3);

    // 中央 = 子にする（末尾）
    plan = PlanDrop(reg, {a}, q, DropZone::Onto, idx);
    CHECK(plan.ok && plan.newParent == q);
    Apply(reg, plan); idx.Build(reg);
    {
        const auto n = ChildNames(reg, idx, q);
        CHECK(n.size() == 3 && n.back() == "A");
    }

    // ルートへ（ルート同士の Before）
    idx.Build(reg);
    plan = PlanDrop(reg, {qa}, p, DropZone::Before, idx);
    CHECK(plan.ok && plan.newParent == entt::null);
    Apply(reg, plan); idx.Build(reg);
    CHECK(idx.roots.front() == qa);
    CHECK(idx.roots.size() == 3);

    // 循環は拒む（親を自分の子孫の下へ）
    plan = PlanDrop(reg, {q}, idx.Children(q).first[0], DropZone::Onto, idx);
    CHECK(!plan.ok);
    plan = PlanDrop(reg, {q}, idx.Children(q).first[0], DropZone::Before, idx);
    CHECK(!plan.ok);
    // 自分自身には落とせない
    plan = PlanDrop(reg, {q}, q, DropZone::Onto, idx);
    CHECK(!plan.ok);
    // 祖先も一緒に掴んでいる子は除く
    idx.Build(reg);
    auto x = Make(reg, "X");
    idx.Build(reg);
    const entt::entity qc = idx.Children(q).first[0];
    plan = PlanDrop(reg, {q, qc}, x, DropZone::Onto, idx);
    CHECK(plan.ok && plan.moving.size() == 1 && plan.moving[0] == q);
    // 無効な対象
    plan = PlanDrop(reg, {q}, entt::null, DropZone::Onto, idx);
    CHECK(!plan.ok);
    (void)b;
}

void TestFlags()
{
    entt::registry reg;
    auto p = Make(reg, "P");
    auto c = Make(reg, "C", p);
    auto g = Make(reg, "G", c);
    auto o = Make(reg, "Other");
    CHECK(!eflags::AnyOf<EditorHidden>(reg));
    CHECK(!eflags::IsHidden(reg, g));

    reg.emplace<EditorHidden>(p);
    CHECK(eflags::AnyOf<EditorHidden>(reg));
    CHECK(eflags::IsHidden(reg, p) && eflags::IsHidden(reg, c) && eflags::IsHidden(reg, g));   // 子孫へ伝播
    CHECK(!eflags::IsHidden(reg, o));
    CHECK(eflags::Self<EditorHidden>(reg, p) && !eflags::Self<EditorHidden>(reg, c));           // 継承と自分の区別
    reg.emplace<EditorLocked>(c);
    CHECK(!eflags::IsLocked(reg, p) && eflags::IsLocked(reg, c) && eflags::IsLocked(reg, g));
    reg.emplace<EntityDisabled>(g);
    CHECK(eflags::IsDisabled(reg, g) && !eflags::IsDisabled(reg, c));
    reg.remove<EditorHidden>(p);
    CHECK(!eflags::IsHidden(reg, g));

    // 壊れた親鎖（循環）でも止まる
    reg.get<Transform>(p).parent = g;
    CHECK(!eflags::IsHidden(reg, g));   // 立っていないので false（無限ループしない）
    reg.emplace<EditorHidden>(o);
    reg.get<Transform>(o).parent = o;   // 自己参照
    CHECK(eflags::IsHidden(reg, o));

    // 押した行が選択に含まれていれば選択ぜんぶ / 無ければその行だけ
    std::vector<entt::entity> sel{p, c};
    CHECK(ToggleTargets(sel, c).size() == 2);
    CHECK(ToggleTargets(sel, g).size() == 1 && ToggleTargets(sel, g)[0] == g);
}

void TestIsolate()
{
    entt::registry reg;
    //  R1
    //   ├ A
    //   │  └ A1
    //   └ B
    //  R2
    //   └ C
    //  R3
    auto r1 = Make(reg, "R1"); auto a = Make(reg, "A", r1); auto a1 = Make(reg, "A1", a); auto b = Make(reg, "B", r1);
    auto r2 = Make(reg, "R2"); auto c = Make(reg, "C", r2); auto r3 = Make(reg, "R3");
    Index idx; idx.Build(reg);

    CHECK(!IsIsolated<EditorHidden>(reg, idx, a));
    CHECK(AltClickAction<EditorHidden>(reg, idx, a) == AltAction::Isolate);
    auto plan = PlanIsolate<EditorHidden>(reg, idx, a);
    for (entt::entity e : plan.set) reg.emplace_or_replace<EditorHidden>(e);
    for (entt::entity e : plan.clear) reg.remove<EditorHidden>(e);
    // A（と祖先 R1・子孫 A1）は見える。B / R2 / C / R3 は隠れる。
    CHECK(!eflags::IsHidden(reg, a) && !eflags::IsHidden(reg, a1) && !eflags::IsHidden(reg, r1));
    CHECK(eflags::IsHidden(reg, b) && eflags::IsHidden(reg, r2) && eflags::IsHidden(reg, c) && eflags::IsHidden(reg, r3));
    // 最小限しか立てない（子孫の C は R2 の継承で足りる）
    CHECK(!eflags::Self<EditorHidden>(reg, c));
    CHECK(plan.set.size() == 3);   // B, R2, R3
    CHECK(IsIsolated<EditorHidden>(reg, idx, a));
    CHECK(AltClickAction<EditorHidden>(reg, idx, a) == AltAction::ClearAll);
    // 別の物では isolated ではない
    CHECK(!IsIsolated<EditorHidden>(reg, idx, b));
    // もう一度 Alt+クリック → 全部降ろす
    for (entt::entity e : PlanClearAll<EditorHidden>(reg)) reg.remove<EditorHidden>(e);
    CHECK(!eflags::AnyOf<EditorHidden>(reg));
    CHECK(!IsIsolated<EditorHidden>(reg, idx, a));

    // 自分が個別に隠れている物で isolate すると、その物は表示に戻る
    reg.emplace<EditorHidden>(a);
    plan = PlanIsolate<EditorHidden>(reg, idx, a);
    CHECK(std::find(plan.clear.begin(), plan.clear.end(), a) != plan.clear.end());
    for (entt::entity e : PlanClearAll<EditorHidden>(reg)) reg.remove<EditorHidden>(e);

    // ロックも同じ規則
    auto lp = PlanIsolate<EditorLocked>(reg, idx, c);
    for (entt::entity e : lp.set) reg.emplace_or_replace<EditorLocked>(e);
    CHECK(!eflags::IsLocked(reg, c) && !eflags::IsLocked(reg, r2));
    CHECK(eflags::IsLocked(reg, r1) && eflags::IsLocked(reg, a1) && eflags::IsLocked(reg, r3));
    CHECK(IsIsolated<EditorLocked>(reg, idx, c));

    // 1 体しか居ないシーンでは「isolated」にならない（切り替える意味が無い）
    entt::registry solo;
    auto only = Make(solo, "Only");
    Index si; si.Build(solo);
    CHECK(!IsIsolated<EditorHidden>(solo, si, only));

    CHECK(CountSelf<EditorLocked>(reg) == lp.set.size());
}

void TestFlagCommand()
{
    entt::registry reg;
    auto a = Make(reg, "A"); auto b = Make(reg, "B");
    reg.emplace<EditorHidden>(b);
    using Cmd = EditorFlagCommand<EditorHidden>;
    Cmd cmd(&reg, {{a, false, true}, {b, true, false}}, "Visibility");
    cmd.Redo();
    CHECK(eflags::Self<EditorHidden>(reg, a) && !eflags::Self<EditorHidden>(reg, b));
    cmd.Undo();
    CHECK(!eflags::Self<EditorHidden>(reg, a) && eflags::Self<EditorHidden>(reg, b));
    cmd.Redo();
    CHECK(eflags::Self<EditorHidden>(reg, a) && !eflags::Self<EditorHidden>(reg, b));
    // 消えたエンティティでも落ちない
    reg.destroy(a);
    cmd.Undo();
    cmd.Redo();
    CHECK(true);

    // UndoSystem を通す（1 エントリ）
    UndoSystem us;
    entt::registry r2;
    auto x = Make(r2, "X");
    us.PushCommand(std::make_unique<EditorFlagCommand<EditorLocked>>(
        &r2, std::vector<EditorFlagCommand<EditorLocked>::Change>{{x, false, true}}, "Lock"));
    r2.emplace<EditorLocked>(x);
    CHECK(us.UndoDepth() == 1);
    us.Undo();
    CHECK(!eflags::Self<EditorLocked>(r2, x));
    us.Redo();
    CHECK(eflags::Self<EditorLocked>(r2, x));
}

// ---------------------------------------------------------------------------
// シーン JSON
void TestSerialization()
{
    Scene src, dst;
    auto& sr = src.GetRegistry();
    auto plain = Make(sr, "Plain");
    auto flagged = Make(sr, "Flagged");
    sr.emplace<EditorHidden>(flagged);
    sr.emplace<EditorLocked>(flagged);
    sr.emplace<EditorFolder>(flagged);
    sr.get<Transform>(flagged).siblingOrder = 48;

    // フラグの無いエンティティの JSON にはキーが 1 つも増えない（後方互換）
    const std::string jp = SceneSerializer::SerializeEntity(src, plain, "");
    const auto pj = nlohmann::json::parse(jp);
    CHECK(!pj.contains("editorHidden") && !pj.contains("editorLocked") && !pj.contains("editorFolder") && !pj.contains("siblingOrder"));

    const std::string jf = SceneSerializer::SerializeEntity(src, flagged, "");
    const auto fj = nlohmann::json::parse(jf);
    CHECK(fj.value("editorHidden", false) && fj.value("editorLocked", false) && fj.value("editorFolder", false));
    CHECK(fj.value("siblingOrder", 0) == 48);

    // 往復
    const entt::entity back = SceneSerializer::InstantiateEntity(dst, jf, "");
    CHECK(back != entt::null);
    auto& dr = dst.GetRegistry();
    if (back != entt::null)
    {
        CHECK(dr.all_of<EditorHidden>(back) && dr.all_of<EditorLocked>(back) && dr.all_of<EditorFolder>(back));
        CHECK(dr.get<Transform>(back).siblingOrder == 48);
    }
    const entt::entity backPlain = SceneSerializer::InstantiateEntity(dst, jp, "");
    CHECK(backPlain != entt::null);
    if (backPlain != entt::null)
    {
        CHECK(!dr.all_of<EditorHidden>(backPlain) && !dr.all_of<EditorLocked>(backPlain) && !dr.all_of<EditorFolder>(backPlain));
        CHECK(dr.get<Transform>(backPlain).siblingOrder == 0);
    }

    // 旧 JSON（キーの無い手書き）でも従来どおり読める。型が違う値（文字列 / false）は無視される
    const std::string old = R"({"name":"Old","transform":{"position":[1,2,3],"rotation":[0,0,0],"scale":[1,1,1]}})";
    const entt::entity o = SceneSerializer::InstantiateEntity(dst, old, "");
    CHECK(o != entt::null);
    if (o != entt::null)
        CHECK(!dr.all_of<EditorHidden>(o) && dr.get<Transform>(o).siblingOrder == 0);
    const std::string weird = R"({"name":"Weird","editorHidden":"yes","editorLocked":false,"siblingOrder":"x","transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}})";
    const entt::entity w = SceneSerializer::InstantiateEntity(dst, weird, "");
    CHECK(w != entt::null);
    if (w != entt::null)
        CHECK(!dr.all_of<EditorHidden>(w) && !dr.all_of<EditorLocked>(w) && dr.get<Transform>(w).siblingOrder == 0);
}

// 複数のエンティティを持つシーンの保存 → 読み込みで、親子と兄弟順とフラグが保たれる
void TestSceneRoundTrip()
{
    Scene src;
    auto& sr = src.GetRegistry();
    auto root = Make(sr, "Root");
    auto k1 = Make(sr, "K1", root), k2 = Make(sr, "K2", root), k3 = Make(sr, "K3", root);
    sr.get<Transform>(k1).siblingOrder = 30;
    sr.get<Transform>(k2).siblingOrder = 10;
    sr.get<Transform>(k3).siblingOrder = 20;
    sr.emplace<EditorHidden>(k2);
    sr.emplace<EditorLocked>(root);
    const std::string path = "hier_logic_test_scene.json";
    CHECK(SceneSerializer::Save(src, path, ""));
    Scene dst;
    CHECK(SceneSerializer::Load(dst, path, ""));
    auto& dr = dst.GetRegistry();
    entt::entity dRoot = entt::null;
    for (auto [e, n] : dr.view<NameTag>().each()) if (n.name == "Root") dRoot = e;
    CHECK(dRoot != entt::null);
    if (dRoot != entt::null)
    {
        Index idx; idx.Build(dr);
        CHECK(Eq(ChildNames(dr, idx, dRoot), {"K2", "K3", "K1"}));   // 保存した兄弟順が読み込みで保たれる
        CHECK(eflags::Self<EditorLocked>(dr, dRoot));
        for (auto [e, n] : dr.view<NameTag>().each())
            if (n.name == "K2") CHECK(eflags::Self<EditorHidden>(dr, e));
    }
    std::remove(path.c_str());
}

} // namespace

int main()
{
    TestTypes();
    TestQuery();
    TestIndexAndRows();
    TestStructureKey();
    TestRange();
    TestZones();
    TestDrop();
    TestFlags();
    TestIsolate();
    TestFlagCommand();
    TestSerialization();
    TestSceneRoundTrip();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
