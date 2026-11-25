// Lua 公開プロパティの Undo（W05-14）の単体テスト。GPU 不要。
//   ・値の往復（Float / Bool / String / Vec3 / Entity）
//   ・GUID で引く（ハンドルが変わっても追従）。エンティティが消えたら何もしない
//   ・ホットリロードで名前 / 型が変わっても例外を出さず、無関係なプロパティを壊さない
//   ・「既定値に戻す」は複合 1 件で、Undo 1 回で全部戻る。差が無ければ積まない
#include "ecs/Components.h"
#include "editor/UndoCore.h"
#include "editor/UndoSystem.h"
#include "editor/undo/ScriptPropertyUndo.h"

#include <entt/entt.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace dx12e;

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

ScriptProp Prop(const char* name, ScriptPropType t)
{
    ScriptProp p; p.name = name; p.type = t; return p;
}

entt::entity MakeEntity(entt::registry& reg, uint64_t guid)
{
    const entt::entity e = reg.create();
    reg.emplace<EntityGuid>(e, EntityGuid{ guid });
    LuaScript ls;
    ls.scriptPath = "scripts/x.lua";
    ScriptProp speed = Prop("speed", ScriptPropType::Float); speed.num = 5.0;
    ScriptProp on    = Prop("on", ScriptPropType::Bool);     on.b = true;
    ScriptProp nm    = Prop("label", ScriptPropType::String); nm.str = "a";
    ScriptProp col   = Prop("tint", ScriptPropType::Vec3);    col.vec = { 1.0f, 2.0f, 3.0f };
    ls.props = { speed, on, nm, col };
    reg.emplace<LuaScript>(e, std::move(ls));
    return e;
}

LuaScript& LS(entt::registry& reg, entt::entity e) { return reg.get<LuaScript>(e); }

void TestRoundTrip()
{
    entt::registry reg;
    const entt::entity e = MakeEntity(reg, 0xABCDEFull);
    auto& ls = LS(reg, e);

    // ドラッグ: 5 -> 9（途中の値は積まない。before = 開始時 / after = 離した時）
    const ScriptProp before = ls.props[0];
    ls.props[0].num = 6.0; ls.props[0].num = 7.5; ls.props[0].num = 9.0;
    ScriptPropertyChange c;
    CHECK(MakeScriptPropertyChange(reg, e, "speed", before, ls.props[0], c));
    ScriptPropertyUndo cmd(&reg, c);
    cmd.Undo();
    CHECK(LS(reg, e).props[0].num == 5.0);
    cmd.Redo();
    CHECK(LS(reg, e).props[0].num == 9.0);
    CHECK(std::string(cmd.GetName()) == "Script Property");

    // 値が同じなら変更として作らない
    CHECK(!MakeScriptPropertyChange(reg, e, "speed", ls.props[0], ls.props[0], c));

    // Bool / String / Vec3
    for (int k = 1; k <= 3; ++k)
    {
        ScriptProp b = ls.props[k];
        ScriptProp a = b;
        a.b = !b.b; a.str = "zzz"; a.vec = { 9.0f, 8.0f, 7.0f };
        ScriptPropertyChange cc;
        CHECK(MakeScriptPropertyChange(reg, e, b.name, b, a, cc));
        ScriptPropertyUndo u(&reg, cc);
        u.Redo();
        CHECK(ScriptPropValueEquals(LS(reg, e).props[k], a));
        u.Undo();
        CHECK(ScriptPropValueEquals(LS(reg, e).props[k], b));
    }
}

void TestGuidResolveAndMissing()
{
    entt::registry reg;
    const entt::entity e = MakeEntity(reg, 0x1234ull);
    auto& ls = LS(reg, e);
    const ScriptProp before = ls.props[0];
    ls.props[0].num = 1.0;
    ScriptPropertyChange c;
    CHECK(MakeScriptPropertyChange(reg, e, "speed", before, ls.props[0], c));
    ScriptPropertyUndo cmd(&reg, c);

    // エンティティを作り直す（Play/Stop 相当）: ハンドルは別でも GUID で同じものを引く
    reg.destroy(e);
    const entt::entity e2 = MakeEntity(reg, 0x1234ull);
    LS(reg, e2).props[0].num = 1.0;
    cmd.Undo();
    CHECK(LS(reg, e2).props[0].num == 5.0);

    // GUID を持つエンティティが消えたら何もしない（別エンティティに当てない）
    const entt::entity other = MakeEntity(reg, 0x9999ull);
    reg.destroy(e2);
    cmd.Undo(); cmd.Redo();
    CHECK(LS(reg, other).props[0].num == 5.0);

    // GUID 0（保存前）はハンドルで引く
    entt::registry r2;
    const entt::entity z = MakeEntity(r2, 0);
    const ScriptProp zb = LS(r2, z).props[0];
    LS(r2, z).props[0].num = 2.0;
    ScriptPropertyChange zc;
    CHECK(MakeScriptPropertyChange(r2, z, "speed", zb, LS(r2, z).props[0], zc));
    ScriptPropertyUndo zu(&r2, zc);
    zu.Undo();
    CHECK(LS(r2, z).props[0].num == 5.0);
}

void TestHotReload()
{
    entt::registry reg;
    const entt::entity e = MakeEntity(reg, 0x77ull);
    auto& ls = LS(reg, e);
    const ScriptProp before = ls.props[0];
    ls.props[0].num = 3.0;
    ScriptPropertyChange c;
    CHECK(MakeScriptPropertyChange(reg, e, "speed", before, ls.props[0], c));
    ScriptPropertyUndo cmd(&reg, c);

    // 名前が消えた: 何もしない
    ls.props[0].name = "renamed";
    cmd.Undo(); cmd.Redo();
    CHECK(ls.props[0].num == 3.0);
    CHECK(ls.props[0].name == "renamed");

    // 型が変わった: 何もしない
    ls.props[0].name = "speed"; ls.props[0].type = ScriptPropType::String; ls.props[0].str = "q";
    cmd.Undo();
    CHECK(ls.props[0].str == "q" && ls.props[0].num == 3.0);

    // LuaScript 自体が外れた: 例外なし
    reg.remove<LuaScript>(e);
    cmd.Undo(); cmd.Redo();
    CHECK(!reg.all_of<LuaScript>(e));

    // before に無い名前は複合に入らない
    entt::registry r3;
    const entt::entity e3 = MakeEntity(r3, 0x5ull);
    std::vector<ScriptProp> snap = LS(r3, e3).props;
    snap.push_back(Prop("gone", ScriptPropType::Float));
    CHECK(MakeScriptPropertiesResetCommand(r3, e3, snap) == nullptr);   // 差なし
}

void TestResetComposite()
{
    entt::registry reg;
    const entt::entity e = MakeEntity(reg, 0x42ull);
    auto& ls = LS(reg, e);
    const std::vector<ScriptProp> before = ls.props;   // 「戻す」直前
    // 既定値へ
    ls.props[0].num = 0.0; ls.props[1].b = false; ls.props[2].str.clear(); ls.props[3].vec = { 0, 0, 0 };

    auto cmd = MakeScriptPropertiesResetCommand(reg, e, before);
    CHECK(cmd != nullptr);
    if (!cmd) return;
    CHECK(std::string(cmd->GetName()) == "Reset Script Properties");

    UndoSystem sys;
    sys.PushCommand(std::move(cmd));
    CHECK(sys.UndoDepth() == 1);            // 1 件
    sys.Undo();                              // 1 回で全部戻る
    CHECK(ls.props[0].num == 5.0 && ls.props[1].b == true && ls.props[2].str == "a" && ls.props[3].vec.y == 2.0f);
    CHECK(sys.RedoDepth() == 1);
    sys.Redo();
    CHECK(ls.props[0].num == 0.0 && ls.props[1].b == false && ls.props[2].str.empty() && ls.props[3].vec.y == 0.0f);

    // 既に既定値なら何も積まない
    CHECK(MakeScriptPropertiesResetCommand(reg, e, ls.props) == nullptr);
}

void TestEntityProp()
{
    entt::registry reg;
    const entt::entity e = MakeEntity(reg, 0x11ull);
    LS(reg, e).props.push_back(Prop("target", ScriptPropType::Entity));
    auto& p = LS(reg, e).props.back();
    const ScriptProp before = p;
    p.str = "Boss"; p.guid = 0xBEEFull;
    ScriptPropertyChange c;
    CHECK(MakeScriptPropertyChange(reg, e, "target", before, p, c));
    ScriptPropertyUndo u(&reg, c);
    u.Undo();
    CHECK(LS(reg, e).props.back().str.empty() && LS(reg, e).props.back().guid == 0);
    u.Redo();
    CHECK(LS(reg, e).props.back().str == "Boss" && LS(reg, e).props.back().guid == 0xBEEFull);
}
} // namespace

int main()
{
    TestRoundTrip();
    TestGuidResolveAndMissing();
    TestHotReload();
    TestResetComposite();
    TestEntityProp();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
