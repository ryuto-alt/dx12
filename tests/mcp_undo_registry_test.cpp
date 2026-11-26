// McpUndoTracker のレジストリ経由のフォールバック（McpRegistrySlot。W05-15）の単体テスト。GPU 不要。
//   TrackByJsonKey で Components.h に無い型を追跡し、編集 / 追加 / 削除のそれぞれで
//   Finish() のコマンドの Undo / Redo が JSON までそのまま戻すこと。追加の Undo は remove で消えること。
#include "core/mcp/McpUndoTrack.h"
#include "scene/ComponentSerializerRegistry.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

using namespace dx12e;
using nlohmann::json;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

struct DummyComp { int a = 0; std::string s; };
bool DummyHas(const entt::registry& r, entt::entity e) { return r.all_of<DummyComp>(e); }
void DummySave(const entt::registry& r, entt::entity e, json& out)
{
    const auto& d = r.get<DummyComp>(e);
    out = { {"a", d.a}, {"s", d.s} };
}
void DummyLoad(entt::registry& r, entt::entity e, const json& in)
{
    DummyComp d;
    d.a = in.value("a", 0);
    d.s = in.value("s", std::string());
    r.emplace_or_replace<DummyComp>(e, std::move(d));
}
void DummyRemove(entt::registry& r, entt::entity e) { r.remove<DummyComp>(e); }

// 比較は保存形式（JSON）で。無ければ null
json Snap(entt::registry& r, entt::entity e)
{
    json j;
    if (DummyHas(r, e)) DummySave(r, e, j);
    return j;
}

entt::entity Add(Scene& s)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ "E" });
    reg.emplace<Transform>(e);
    return e;
}

// 1 回の「MCP 呼び出し」: Begin → Track → edit → Finish。コマンドの数を返し、cmds へ渡す
template <class F>
std::vector<std::unique_ptr<IUndoCommand>> Run(Scene& scene, entt::entity e, F&& edit)
{
    McpUndoTracker tr;
    tr.Begin(&scene, "");
    CHECK(tr.TrackByJsonKey(e, "dummyTest"));
    edit();
    return tr.Finish();
}
} // namespace

int main()
{
    ComponentSerializer d; d.typeName = "dummyTest"; d.has = DummyHas; d.save = DummySave; d.load = DummyLoad; d.remove = DummyRemove;
    TestOnlyAppendComponentSerializer(d);

    // ── 編集 ──
    {
        Scene scene; auto& reg = scene.GetRegistry();
        const entt::entity e = Add(scene);
        reg.emplace<DummyComp>(e, DummyComp{ 1, "x" });
        const json before = Snap(reg, e);
        auto cmds = Run(scene, e, [&] { reg.get<DummyComp>(e) = DummyComp{ 2, "y" }; });
        const json after = Snap(reg, e);
        CHECK(cmds.size() == 1);
        CHECK(before != after);
        if (cmds.size() == 1)
        {
            cmds[0]->Undo(); CHECK(Snap(reg, e) == before);
            cmds[0]->Redo(); CHECK(Snap(reg, e) == after);
            cmds[0]->Undo(); CHECK(Snap(reg, e) == before);
        }
    }
    // ── 追加（Undo は remove で消える）──
    {
        Scene scene; auto& reg = scene.GetRegistry();
        const entt::entity e = Add(scene);
        auto cmds = Run(scene, e, [&] { reg.emplace<DummyComp>(e, DummyComp{ 7, "new" }); });
        const json after = Snap(reg, e);
        CHECK(cmds.size() == 1);
        if (cmds.size() == 1)
        {
            cmds[0]->Undo(); CHECK(!reg.all_of<DummyComp>(e));
            cmds[0]->Redo(); CHECK(Snap(reg, e) == after);
            cmds[0]->Undo(); CHECK(!reg.all_of<DummyComp>(e));
        }
    }
    // ── 削除 ──
    {
        Scene scene; auto& reg = scene.GetRegistry();
        const entt::entity e = Add(scene);
        reg.emplace<DummyComp>(e, DummyComp{ 3, "z" });
        const json before = Snap(reg, e);
        auto cmds = Run(scene, e, [&] { reg.remove<DummyComp>(e); });
        CHECK(cmds.size() == 1);
        if (cmds.size() == 1)
        {
            cmds[0]->Undo(); CHECK(Snap(reg, e) == before);
            cmds[0]->Redo(); CHECK(!reg.all_of<DummyComp>(e));
        }
    }
    // ── 変更なし: コマンドを積まない ──
    {
        Scene scene; auto& reg = scene.GetRegistry();
        const entt::entity e = Add(scene);
        reg.emplace<DummyComp>(e, DummyComp{ 1, "x" });
        auto cmds = Run(scene, e, [&] {});
        CHECK(cmds.empty());
    }

    TestOnlyResetComponentSerializers();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
