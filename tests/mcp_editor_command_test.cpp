// M7: エディタ操作の全面公開（dx12_editor_command / dx12_editor_state）の土台の単体テスト。
//   ・editor/EditorCommandMcp.{h,cpp}  … コマンド表 → 公開用の一覧（kCommands / window.* / create.*）・分類・可否・引数・実行
//   ・editor/EditorStateMcp.{h,cpp}    … 選択 / 窓 / レイアウト / モーダル / undo / トーストの JSON
//   ・editor/Toast.cpp（履歴）/ editor/UndoCore.h（履歴の名前）
// 窓も GPU も無し。Editor をリンクして、モックの EditorContext と ImGui コンテキストだけで動かす。
//
// ★何を守っているか
//   (1) 表に増えたコマンドが「手書きの二重管理なしで」公開されること（全 id が list に出て、実行できる）。
//   (2) 危険なコマンド（guarded）の分類が黙って外れないこと。上書き表が実在の id を指すこと。
//   (3) 実行できないときは必ず理由が付くこと（AI が理由を読んで直せる）。
//   (4) モーダル / ダイアログ / パレットを AI が検知できること（詰まらない）。
//
// 実行: ctest --output-on-failure -R McpEditorCommand

#include "editor/EditorCommandMcp.h"
#include "editor/EditorContext.h"
#include "editor/EditorStateMcp.h"
#include "editor/Toast.h"
#include "editor/ToolWindows.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace dx12e;
namespace mc = dx12e::cmd::mcp;
using json = nlohmann::json;

namespace
{
int g_checks = 0, g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

const mc::Row* Find(const std::vector<mc::Row>& rows, const std::string& id)
{
    for (const mc::Row& r : rows) if (r.id == id) return &r;
    return nullptr;
}

std::vector<std::string> ExpectedIds()
{
    std::vector<std::string> ids;
    for (const cmd::Def& d : cmd::kCommands) ids.push_back(d.id);
    for (const tools::Desc& t : tools::kAll) ids.push_back(std::string("window.") + t.id);
    for (const cmd::CreateItem& c : cmd::kCreateItems) ids.push_back(c.id);
    return ids;
}

// ---------------------------------------------------------------------------
// (1) 全 id が list に出る / 種別 / 重複無し / ラベル
// ---------------------------------------------------------------------------
void TestCoverage()
{
    EditorContext ctx;
    const mc::RunEnv env;
    const std::vector<mc::Row> rows = mc::BuildRows(ctx, env);
    const std::vector<std::string> expect = ExpectedIds();
    std::printf("  表のコマンド %zu 件（kCommands %zu + window.* %zu + create.* %zu）\n",
                rows.size(), cmd::kCommandCount, tools::kCount, cmd::kCreateItemCount);
    CHECK(rows.size() == expect.size(), "rows=%zu expect=%zu", rows.size(), expect.size());
    CHECK(rows.size() == cmd::kCommandCount + tools::kCount + cmd::kCreateItemCount, "合計が 3 表の和と一致");

    std::set<std::string> seen;
    for (const mc::Row& r : rows)
    {
        CHECK(seen.insert(r.id).second, "id が重複: %s", r.id.c_str());
        CHECK(!r.label.empty(), "%s: ラベルが空", r.id.c_str());
        CHECK(!r.category.empty(), "%s: カテゴリが空", r.id.c_str());
        CHECK(mc::KindOf(r.id) == r.kind, "%s: KindOf(%s) != row.kind(%s)", r.id.c_str(), mc::KindOf(r.id).c_str(), r.kind.c_str());
        CHECK(r.traits.kind == r.kind, "%s: traits.kind が違う", r.id.c_str());
        CHECK(!r.traits.effect.empty(), "%s: effect が空", r.id.c_str());
    }
    for (const std::string& id : expect) CHECK(Find(rows, id) != nullptr, "表の id が list に無い: %s", id.c_str());
    CHECK(mc::KindOf("nope.nothing").empty(), "未知の id の種別は空");
    CHECK(mc::KindOf("window.doesNotExist").empty(), "未知の window の種別は空");
    CHECK(mc::KindOf("create.doesNotExist").empty(), "未知の create の種別は空");

    // JSON（compact / detail）が最低限のキーを持つ
    for (const mc::Row& r : rows)
    {
        const json c = mc::RowToJson(r, false);
        const json d = mc::RowToJson(r, true);
        for (const char* k : {"id", "label", "category", "kind", "enabled"})
            CHECK(c.contains(k), "%s: compact に %s が無い", r.id.c_str(), k);
        for (const char* k : {"labelEn", "help", "scope", "effect", "example"})
            CHECK(d.contains(k), "%s: detail に %s が無い", r.id.c_str(), k);
        CHECK(c.value("enabled", true) || c.contains("disabledReason"), "%s: 無効なのに理由が無い", r.id.c_str());
        CHECK(!c.contains("hasArgs") || d.contains("args"), "%s: hasArgs なのに args が無い", r.id.c_str());
    }
}

// ---------------------------------------------------------------------------
// (2) 分類（guarded / osDialog / opensModal）が黙って外れない
// ---------------------------------------------------------------------------
void TestClassification()
{
    EditorContext ctx;
    const std::vector<mc::Row> rows = mc::BuildRows(ctx, mc::RunEnv{});

    std::set<std::string> guarded, os, modal;
    for (const mc::Row& r : rows)
    {
        if (r.traits.guarded) guarded.insert(r.id);
        if (r.traits.osDialog) os.insert(r.id);
        if (r.traits.opensModal) modal.insert(r.id);
        // guarded には必ず理由がある
        CHECK(!r.traits.guarded || !r.traits.guardReason.empty(), "%s: guarded なのに理由が無い", r.id.c_str());
        // window.* / create.* は guarded にならない（開閉と作成）
        CHECK(!(r.traits.guarded && r.kind != "command"), "%s: window / create が guarded", r.id.c_str());
    }
    const std::set<std::string> wantGuarded = {"edit.delete", "file.save", "file.saveAs", "file.closeProject", "file.open", "matgraph.save", "matgraph.saveAs"};
    CHECK(guarded == wantGuarded, "guarded の集合が変わった（意図した変更なら test の期待値と docs/MCP.md を更新する）");
    const std::set<std::string> wantOs = {"file.open"};
    CHECK(os == wantOs, "osDialog の集合が変わった");
    for (const char* id : {"file.new", "file.saveAs", "file.newScript", "file.newShader", "palette.commands", "palette.quickOpen", "layout.save", "layout.slots"})
        CHECK(modal.count(id) == 1, "%s は opensModal のはず", id);

    // 上書き表の id は全部、表に実在する（id を変えた / 消したときに分類が黙って外れない）
    for (const std::string& id : mc::OverrideIds())
        CHECK(mc::KindOf(id) == "command", "上書き表の id が表に無い: %s", id.c_str());

    // 新しいコマンド（表に無い id）の既定は「通常・引数なし」= 何も書かなくても公開される
    const mc::Traits t = mc::TraitsFor("edit.somethingNew");
    CHECK(!t.guarded && !t.osDialog && !t.opensModal && t.args.empty(), "未知の id の既定が通常でない");
    // 接頭辞で自動 guarded（ビルド / git / 外部プロセス）
    CHECK(mc::TraitsFor("build.package").guarded, "build.* は自動で guarded");
    CHECK(mc::TraitsFor("git.push").guarded, "git.* は自動で guarded");
    CHECK(mc::TraitsFor("external.openBrowser").guarded, "external.* は自動で guarded");

    // 引数の宣言
    const mc::Traits w = mc::TraitsFor("window.postProcess");
    CHECK(w.windowState && w.args.size() == 1 && w.args[0].name == "state", "window.* の args");
    const mc::Traits cb = mc::TraitsFor("create.box");
    CHECK(cb.args.size() == 2 && cb.args[0].name == "position", "create.box の args = position, name");
    const mc::Traits cu = mc::TraitsFor("create.uiText");
    CHECK(cu.args.size() == 1 && cu.args[0].name == "name", "create.ui* は name だけ");
    CHECK(mc::TraitsFor("create.terrain").args.empty(), "create.terrain（専用窓を開くだけ）は引数なし");
    CHECK(mc::TraitsFor("view.flyMode").toggleState, "view.flyMode はトグル");
    CHECK(mc::TraitsFor("edit.undo").args.empty(), "edit.undo は引数なし");
}

// ---------------------------------------------------------------------------
// (3) 実行できるか / 理由
// ---------------------------------------------------------------------------
void TestEnabledAndReasons()
{
    struct Scenario { const char* name; void (*setup)(EditorContext&); };
    const Scenario scenarios[] = {
        {"既定（Editor・選択なし）", [](EditorContext&) {}},
        {"Play 中", [](EditorContext& c) { c.isPlaying = true; }},
        {"フライ中", [](EditorContext& c) { c.flyMode = true; }},
        {"2D ビュー", [](EditorContext& c) { c.view2D = true; }},
        {"選択あり", [](EditorContext& c) { c.selectedEntities.push_back(static_cast<entt::entity>(1)); c.selectedEntity = static_cast<entt::entity>(1); }},
        {"マテリアルグラフ窓が開いている", [](EditorContext& c) { c.showMaterialGraph = true; }},
        {"クリップボードあり", [](EditorContext& c) { c.clipboard.push_back("{}"); }},
    };
    for (const Scenario& s : scenarios)
    {
        EditorContext ctx;
        s.setup(ctx);
        size_t enabled = 0, disabled = 0;
        for (const mc::Row& r : mc::BuildRows(ctx, mc::RunEnv{}))
        {
            if (r.enabled) { ++enabled; CHECK(r.disabledReason.empty(), "[%s] %s: 有効なのに理由がある", s.name, r.id.c_str()); }
            else
            {
                ++disabled;
                CHECK(!r.disabledReason.empty(), "[%s] %s: 無効なのに理由が無い", s.name, r.id.c_str());
                CHECK(r.disabledReason != "現在の状態では実行できない", "[%s] %s: 理由が汎用のまま（個別の理由を足す）", s.name, r.id.c_str());
            }
        }
        std::printf("  [%s] 有効 %zu / 無効 %zu\n", s.name, enabled, disabled);
    }

    // Play 中は Editor 専用コマンドと create.* が無効（理由に play.stop）、window.* は開閉できる
    {
        EditorContext ctx;
        ctx.isPlaying = true;
        const auto rows = mc::BuildRows(ctx, mc::RunEnv{});
        CHECK(!Find(rows, "create.box")->enabled && Find(rows, "create.box")->disabledReason.find("play.stop") != std::string::npos, "Play 中の create.box");
        CHECK(!Find(rows, "edit.undo")->enabled, "Play 中の edit.undo");
        CHECK(Find(rows, "window.postProcess")->enabled, "Play 中でも window.* は開閉できる");
        CHECK(Find(rows, "play.stop")->enabled, "Play 中の play.stop は有効");
    }
    // 環境による拒否（blockedNow）
    {
        EditorContext ctx;
        mc::RunEnv env;
        env.virtualInput = true;
        const auto rows = mc::BuildRows(ctx, env);
        CHECK(!Find(rows, "file.open")->blockedNow.empty(), "仮想入力中の file.open は blockedNow");
        CHECK(Find(rows, "file.save")->blockedNow.empty(), "file.save は OS ダイアログではない");
        mc::RunEnv env2;
        env2.aiTxOpen = true;
        const auto rows2 = mc::BuildRows(ctx, env2);
        CHECK(!Find(rows2, "edit.undo")->blockedNow.empty(), "AI トランザクション中の edit.undo は blockedNow");
    }
}

// ---------------------------------------------------------------------------
// 引数
// ---------------------------------------------------------------------------
void TestArgs()
{
    mc::RunArgs a;
    std::string err;
    CHECK(mc::ParseArgs("window.postProcess", json::object(), a, err), "空の args は通る");
    CHECK(mc::ParseArgs("window.postProcess", json{{"state", "close"}}, a, err) && a.state == "close", "window state=close");
    a = {};
    CHECK(!mc::ParseArgs("window.postProcess", json{{"state", "maybe"}}, a, err) && err.find("open") != std::string::npos, "window state の不正値");
    a = {}; err.clear();
    CHECK(!mc::ParseArgs("window.postProcess", json{{"pos", 1}}, a, err) && err.find("state") != std::string::npos, "未知の引数 → 受け付ける引数を案内");
    a = {};
    CHECK(mc::ParseArgs("create.box", json{{"position", {1, 2, 3}}, {"name", "Crate"}}, a, err) && a.hasPosition && a.position[1] == 2.0f && a.name == "Crate", "create.box の args");
    CHECK(!mc::ParseArgs("create.box", json{{"position", {1, 2}}}, a, err), "position は 3 要素");
    CHECK(!mc::ParseArgs("create.box", json{{"name", ""}}, a, err), "name は空にできない");
    CHECK(!mc::ParseArgs("create.uiText", json{{"position", {1, 2, 3}}}, a, err), "UI 系は position を受け付けない");
    CHECK(!mc::ParseArgs("edit.undo", json{{"state", "on"}}, a, err) && err.find("引数を取らない") != std::string::npos, "引数なしのコマンドに args");
    CHECK(mc::ParseArgs("edit.undo", nullptr, a, err), "null の args は通る");
    CHECK(!mc::ParseArgs("edit.undo", json::array(), a, err), "配列の args は不可");
}

// ---------------------------------------------------------------------------
// (1b) 表の全コマンドを実行できる（モック ctx）
// ---------------------------------------------------------------------------
void TestRunAll()
{
    const cmd::Env env{nullptr, ""};
    // 有効な全コマンドを既定の引数で実行 → 例外なく executed。副作用はフラグ / キューに積まれるだけ
    {
        EditorContext ctx;
        size_t ran = 0;
        for (const mc::Row& r : mc::BuildRows(ctx, mc::RunEnv{}))
        {
            if (!r.enabled) continue;
            EditorContext c2;   // コマンドごとに新しい ctx（前のコマンドの状態を引きずらない）
            const mc::RunOutcome o = mc::Run(c2, env, r.id, mc::RunArgs{});
            // Scene（レジストリ）が要るコマンドは、Scene の無いモックでは false を返す（実機は cmd::Env に scene が入る）
            if (r.id == "edit.showAll" || r.id == "edit.newFolder") continue;
            CHECK(o.executed, "%s: 有効なのに実行できなかった", r.id.c_str());
            ++ran;
        }
        std::printf("  既定 ctx で有効な %zu コマンドを実行\n", ran);
        CHECK(ran > 70, "実行できたコマンドが少なすぎる: %zu", ran);
    }
    // 選択ありで有効になるもの（edit.* の選択依存）は scene が無いと copy だけ false を返しうる。それ以外は実行できる
    {
        EditorContext base;
        base.selectedEntities.push_back(static_cast<entt::entity>(3));
        base.selectedEntity = static_cast<entt::entity>(3);
        base.clipboard.push_back("{}");
        for (const mc::Row& r : mc::BuildRows(base, mc::RunEnv{}))
        {
            if (!r.enabled || !r.traits.needsSelection) continue;
            if (r.id == "edit.copy" || r.id.rfind("edit.toggle", 0) == 0 || r.id == "edit.isolate" || r.id == "edit.showAll" || r.id == "edit.newFolder") continue;   // scene が要る
            EditorContext c2 = EditorContext();
            c2.selectedEntities = base.selectedEntities; c2.selectedEntity = base.selectedEntity;
            const mc::RunOutcome o = mc::Run(c2, env, r.id, mc::RunArgs{});
            CHECK(o.executed, "%s（選択あり）: 実行できなかった", r.id.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// 実行の意味（window / トグル / create の引数 / edit.undo など）
// ---------------------------------------------------------------------------
void TestRunSemantics()
{
    const cmd::Env env{nullptr, ""};
    {
        EditorContext ctx;
        mc::RunArgs a;
        mc::RunOutcome o = mc::Run(ctx, env, "window.postProcess", a);
        CHECK(o.executed && o.changed && ctx.showPostProcess, "window.postProcess 既定 = open");
        o = mc::Run(ctx, env, "window.postProcess", a);
        CHECK(o.executed && !o.changed && ctx.showPostProcess, "既に開いている → 何もしない（トグルで閉じない）");
        a.state = "close";
        o = mc::Run(ctx, env, "window.postProcess", a);
        CHECK(o.executed && o.changed && !ctx.showPostProcess, "state=close で閉じる");
        o = mc::Run(ctx, env, "window.postProcess", a);
        CHECK(o.executed && !o.changed, "既に閉じている → 何もしない");
        a.state = "toggle";
        mc::Run(ctx, env, "window.postProcess", a);
        CHECK(ctx.showPostProcess, "state=toggle");
        // 開いた窓が windows JSON の open に出る
        const json w = edstate::WindowsJson(ctx);
        bool found = false;
        for (const auto& id : w["open"]) if (id == "postProcess") found = true;
        CHECK(found, "WindowsJson.open に postProcess");
        CHECK(w["openCount"].get<size_t>() == 1, "openCount");
    }
    {
        EditorContext ctx;
        mc::RunArgs a;
        CHECK(mc::Run(ctx, env, "view.flyMode", a).changed && ctx.flyMode, "view.flyMode 既定 = toggle");
        a.state = "on";
        CHECK(!mc::Run(ctx, env, "view.flyMode", a).changed && ctx.flyMode, "state=on で既に ON → 変わらない");
        a.state = "off";
        CHECK(mc::Run(ctx, env, "view.flyMode", a).changed && !ctx.flyMode, "state=off");
    }
    {
        EditorContext ctx;
        mc::RunArgs a;
        a.hasPosition = true; a.position[0] = 1.0f; a.position[1] = 2.0f; a.position[2] = 3.0f;
        a.name = "Crate";
        const mc::RunOutcome o = mc::Run(ctx, env, "create.box", a);
        CHECK(o.executed && ctx.pendingSpawns.size() == 1, "create.box が pendingSpawns へ積まれた");
        if (!ctx.pendingSpawns.empty())
        {
            const auto& s = ctx.pendingSpawns.back();
            CHECK(s.position.x == 1.0f && s.position.y == 2.0f && s.position.z == 3.0f, "position が反映された");
            CHECK(s.name == "Crate", "name が反映された");
            CHECK(s.modelPath == "__primitive_box__", "marker");
        }
        // UI 系は position を無視して Canvas の子へ（name だけ）
        EditorContext c2;
        mc::RunArgs b; b.name = "Title";
        mc::Run(c2, env, "create.uiText", b);
        CHECK(c2.pendingSpawns.size() == 1 && c2.pendingSpawns.back().name == "Title", "create.uiText の name");
        // 専用窓を開くだけの create
        EditorContext c3;
        mc::Run(c3, env, "create.terrain", mc::RunArgs{});
        CHECK(c3.showTerrainEditor && c3.pendingSpawns.empty(), "create.terrain は窓を開くだけ");
    }
    {
        EditorContext ctx;
        CHECK(mc::Run(ctx, env, "edit.selectNone", mc::RunArgs{}).executed, "edit.selectNone");
        CHECK(mc::Run(ctx, env, "play.toggle", mc::RunArgs{}).executed && ctx.pendingPlayRequest == 1, "play.toggle → Play 要求");
        EditorContext c2;
        c2.isPlaying = true;
        mc::Run(c2, env, "play.stop", mc::RunArgs{});
        CHECK(c2.pendingPlayRequest == 2, "play.stop → Stop 要求");
        EditorContext c3;
        mc::Run(c3, env, "file.save", mc::RunArgs{});
        CHECK(c3.pendingSaveScene, "file.save → pendingSaveScene（実際の保存は Application）");
        mc::Run(c3, env, "file.new", mc::RunArgs{});
        CHECK(c3.showNewSceneDialog && c3.newSceneDialogIsCreate, "file.new → 新規シーンダイアログ要求");
    }
}

// ---------------------------------------------------------------------------
// フィルタ / 提案
// ---------------------------------------------------------------------------
void TestFilter()
{
    EditorContext ctx;
    const std::vector<mc::Row> rows = mc::BuildRows(ctx, mc::RunEnv{});
    auto top = [&](const char* q, const char* id)
    {
        mc::ListFilter f; f.query = q;
        const auto hits = mc::Filter(rows, f);
        bool inTop3 = false;
        for (size_t i = 0; i < hits.size() && i < 3; ++i) if (hits[i].id == id) inTop3 = true;
        CHECK(inTop3, "query \"%s\" の上位 3 件に %s が無い（%zu 件）", q, id, hits.size());
    };
    top("ポスト", "window.postProcess");
    top("post process", "window.postProcess");
    top("元に戻す", "edit.undo");
    top("undo", "edit.undo");
    top("box", "create.box");
    top("保存", "file.save");
    top("再生", "play.toggle");
    top("Lighting", "window.lighting");

    mc::ListFilter f;
    f.id = "edit.undo";
    CHECK(mc::Filter(rows, f).size() == 1, "id 完全一致は 1 件");
    f = {}; f.kind = "window";
    CHECK(mc::Filter(rows, f).size() == tools::kCount, "kind=window は kAll と同数");
    f = {}; f.kind = "create";
    CHECK(mc::Filter(rows, f).size() == cmd::kCreateItemCount, "kind=create は表と同数");
    f = {}; f.guardedOnly = true;
    CHECK(mc::Filter(rows, f).size() == 7, "guardedOnly は 7 件");
    f = {}; f.enabledOnly = true;
    const size_t en = mc::Filter(rows, f).size();
    CHECK(en > 0 && en < rows.size(), "enabledOnly は一部");
    f = {}; f.category = "編集";
    CHECK(!mc::Filter(rows, f).empty(), "category=編集");

    const auto s = mc::SuggestIds(rows, "window.postprocess");
    CHECK(!s.empty() && s[0] == "window.postProcess", "大文字小文字違いの打ち間違いは先頭に出る");
    const auto s2 = mc::SuggestIds(rows, "edit.undoo");
    CHECK(!s2.empty() && s2[0] == "edit.undo", "1 文字違い");
}

// ---------------------------------------------------------------------------
// undo 履歴 / トースト / 選択（entt なし）
// ---------------------------------------------------------------------------
void TestStateSections()
{
    // undo
    {
        EditorContext ctx;
        json u = edstate::UndoJson(ctx);
        CHECK(!u["canUndo"].get<bool>() && !u["canRedo"].get<bool>() && u["undoDepth"] == 0, "空の undo");
        ctx.undoSystem.PushCommand(std::make_unique<AiUndoEntry>("AI: create_entity"));
        ctx.undoSystem.PushCommand(std::make_unique<AiUndoEntry>("AI: set_transform"));
        u = edstate::UndoJson(ctx);
        CHECK(u["canUndo"].get<bool>() && u["undoDepth"] == 2, "積んだ undo");
        CHECK(u["nextUndo"] == "AI: set_transform" && u["nextUndoIsAi"].get<bool>(), "次の undo");
        CHECK(u["recentUndo"][0] == "AI: set_transform" && u["recentUndo"][1] == "AI: create_entity", "履歴は新しい順");
        CHECK(u["dirty"].get<bool>(), "積んだら dirty");
        ctx.undoSystem.Undo();
        u = edstate::UndoJson(ctx);
        CHECK(u["canRedo"].get<bool>() && u["nextRedo"] == "AI: set_transform", "undo 後の redo");
        ctx.MarkSceneSaved(0);
        CHECK(!edstate::UndoJson(ctx)["dirty"].get<bool>(), "保存したら dirty が消える");
    }
    // トースト履歴
    {
        ui::ToastClearAll();
        const uint32_t base = ui::ToastHistory(1).empty() ? 0 : ui::ToastHistory(1).front().seq;
        ui::Toast(ui::ToastKind::Info, "履歴テスト A");
        ui::Toast(ui::ToastKind::Warn, "履歴テスト B", 5.0f);
        ui::Toast(ui::ToastKind::Warn, "履歴テスト B", 5.0f);   // 畳み込み
        const auto h = ui::ToastHistory(10);
        CHECK(h.size() >= 2, "履歴に残る");
        if (h.size() >= 2)
        {
            CHECK(h[0].text == "履歴テスト B" && h[0].count == 2 && h[0].kind == ui::ToastKind::Warn, "新しい順・連打は count に畳む");
            CHECK(h[1].text == "履歴テスト A" && h[1].count == 1, "A");
            CHECK(h[0].seq > base && h[0].seq > h[1].seq, "seq は単調増加");
            CHECK(h[0].live, "表示中");
        }
        const json t = edstate::ToastsJson(5);
        CHECK(t["live"].get<size_t>() >= 1 && t["recent"].is_array() && !t["recent"].empty(), "ToastsJson");
        ui::ToastClearAll();
        CHECK(!ui::ToastHistory(1).empty(), "表示を消しても履歴は残る");
        CHECK(!ui::ToastHistory(1).front().live, "消えたら live=false");
    }
    // 選択（レジストリ）
    {
        EditorContext ctx;
        entt::registry reg;
        const auto e = reg.create();
        reg.emplace<NameTag>(e, NameTag{"Crate"});
        reg.emplace<EntityGuid>(e, EntityGuid{0x1234abcdULL});
        ctx.Select(e);
        const json s = edstate::SelectionJson(ctx, reg);
        CHECK(s["count"] == 1 && s["primary"]["name"] == "Crate" && s["primary"]["guid"] == FormatEntityGuidHex(0x1234abcdULL), "選択の JSON（guid は 16 桁 hex）");
        CHECK(s["entities"].size() == 1, "entities");
        // 消えたエンティティは数えない
        reg.destroy(e);
        const json s2 = edstate::SelectionJson(ctx, reg);
        CHECK(s2["count"] == 0 && s2["primary"].is_null(), "消えたエンティティは選択に出ない");
    }
}

// ---------------------------------------------------------------------------
// (4) モーダル検知（ImGui のポップアップスタック + EditorContext のダイアログ要求）
// ---------------------------------------------------------------------------
void AckTextures()
{
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
    {
        if (tex->Status == ImTextureStatus_WantCreate) { tex->SetTexID(static_cast<ImTextureID>(1)); tex->SetStatus(ImTextureStatus_OK); }
        else if (tex->Status == ImTextureStatus_WantUpdates) tex->SetStatus(ImTextureStatus_OK);
        else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) { tex->SetTexID(ImTextureID_Invalid); tex->SetStatus(ImTextureStatus_Destroyed); }
    }
}

struct ModalHarness
{
    bool open = false;      // OpenPopup を要求
    bool nonModalPopup = false;
    void Frame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(1280, 720);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1280, 720));
        ImGui::Begin("host", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        if (open) { ImGui::OpenPopup("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3##NewScenePopup"); open = false; }   // 新規シーン##NewScenePopup
        if (ImGui::BeginPopupModal("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3##NewScenePopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("scene name");
            if (ImGui::Button("cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (nonModalPopup) { ImGui::OpenPopup("ctxmenu"); nonModalPopup = false; }
        if (ImGui::BeginPopup("ctxmenu")) { ImGui::MenuItem("item"); ImGui::EndPopup(); }
        ImGui::End();
        ImGui::Render();
        AckTextures();
    }
    void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }
};

void TestModal()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();

    {
        EditorContext ctx;
        ModalHarness H;
        H.Frames(2);
        CHECK(!edstate::IsBlocking(ctx) && edstate::CollectModals(ctx).empty(), "モーダル無し");
        json mj = edstate::ModalJson(ctx);
        CHECK(!mj["blocking"].get<bool>() && mj["count"] == 0 && !mj["popupOpen"].get<bool>(), "ModalJson（無し）");

        // ImGui のモーダルを開く → 検知 → キャンセルで閉じる
        ctx.newSceneDialogIsCreate = true;
        H.open = true;
        H.Frames(3);
        auto ms = edstate::CollectModals(ctx);
        CHECK(ms.size() == 1 && ms[0].kind == "imgui-modal" && ms[0].id == "new_scene" && ms[0].blocking, "新規シーンのモーダルを検知");
        if (!ms.empty())
        {
            CHECK(ms[0].title == "\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3", "タイトル（## より前）");
            CHECK(ms[0].canDismiss && ms[0].dismissHint.find("editor_modal") != std::string::npos, "editor_modal dismiss で閉じられる案内（ImGui のモーダルは Esc では閉じない）");
        }
        CHECK(edstate::IsBlocking(ctx), "IsBlocking");
        mj = edstate::ModalJson(ctx);
        CHECK(mj["blocking"].get<bool>() && mj["count"] == 1 && mj["modals"][0]["id"] == "new_scene", "ModalJson（あり）");

        // 名前を付けて保存として開いたものは save_as
        ctx.newSceneDialogIsCreate = false;
        ms = edstate::CollectModals(ctx);
        CHECK(!ms.empty() && ms[0].id == "save_as", "保存ダイアログは save_as");
        ctx.newSceneDialogIsCreate = true;

        // ImGui のモーダルは Esc では閉じない（ImGui の仕様。dismissHint に嘘を書かないための確認）
        io.AddKeyEvent(ImGuiKey_Escape, true);
        H.Frames(2);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        H.Frames(2);
        CHECK(edstate::IsBlocking(ctx), "Esc ではモーダルは閉じない");
        // DismissTopModal（キャンセルと同じ）で閉じる
        const edstate::DismissResult dr = edstate::DismissTopModal(ctx);
        CHECK(dr.ok && dr.id == "new_scene", "DismissTopModal が新規シーンを閉じた");
        H.Frames(2);
        CHECK(!edstate::IsBlocking(ctx), "閉じたら検知も消える");
        const edstate::DismissResult dr2 = edstate::DismissTopModal(ctx);
        CHECK(!dr2.ok && dr2.id.empty() && !dr2.reason.empty(), "閉じるものが無いときは理由つきで false");

        // 非モーダルのポップアップは blocking にならない
        H.nonModalPopup = true;
        H.Frames(3);
        ms = edstate::CollectModals(ctx);
        CHECK(!ms.empty() && ms[0].kind == "popup" && !ms[0].blocking, "非モーダルは popup / blocking=false");
        CHECK(!edstate::IsBlocking(ctx) && edstate::ModalJson(ctx)["popupOpen"].get<bool>(), "popupOpen だけ true");
        io.AddKeyEvent(ImGuiKey_Escape, true); H.Frames(2); io.AddKeyEvent(ImGuiKey_Escape, false); H.Frames(2);
    }
    // 窓 JSON / レイアウト JSON が ImGui コンテキスト付きで落ちない
    {
        EditorContext ctx;
        ctx.showPostProcess = true;
        const json w = edstate::WindowsJson(ctx);
        CHECK(w["tools"].size() == tools::kCount, "tools の件数");
        CHECK(w["tools"][0].contains("slot") && w["tools"][0].contains("imguiName"), "tools の項目");
        const json l = edstate::LayoutJson(ctx, 1.5f);
        CHECK(l.contains("dockNodes") && l.contains("structureHash") && l["display"]["dpiScale"] == 1.5f, "LayoutJson");
        CHECK(l["structureHash"].get<std::string>().size() == 16, "structureHash は 16 桁 hex");
        const json l2 = edstate::LayoutJson(ctx, 1.0f);
        CHECK(l["structureHash"] == l2["structureHash"], "同じ構造は同じハッシュ");
    }
    ImGui::DestroyContext();

    // EditorContext のダイアログ要求 / パレット（ImGui のポップアップになる前でも検知する）
    {
        EditorContext ctx;
        ctx.showNewScriptDialog = true;
        auto ms = edstate::CollectModals(ctx);
        CHECK(ms.size() == 1 && ms[0].kind == "editor-dialog" && ms[0].id == "new_script", "new_script 要求");
        const edstate::DismissResult fd = edstate::DismissTopModal(ctx);
        CHECK(fd.ok && fd.id == "new_script" && !ctx.showNewScriptDialog, "ダイアログ要求のフラグを取り下げる");
        ctx.showUnsavedConfirm = true;
        ms = edstate::CollectModals(ctx);
        CHECK(ms.size() == 1 && ms[0].id == "unsaved_confirm" && !ms[0].canDismiss, "unsaved_confirm は editor_modal では閉じない");
        const edstate::DismissResult ud = edstate::DismissTopModal(ctx);
        CHECK(!ud.ok && ud.id == "unsaved_confirm" && !ud.reason.empty(), "unsaved_confirm の dismiss は理由つきで拒否");
        ctx.showUnsavedConfirm = false;
        ctx.showAutosaveRecovery = true;
        CHECK(edstate::CollectModals(ctx)[0].id == "autosave_recovery", "autosave_recovery");
        ctx.showAutosaveRecovery = false;
        ctx.paletteOpen = true;
        ms = edstate::CollectModals(ctx);
        CHECK(ms.size() == 1 && ms[0].kind == "palette" && ms[0].blocking, "コマンドパレット");
    }
}

} // namespace

int main()
{
    std::printf("== McpEditorCommandTests ==\n");
    TestCoverage();
    TestClassification();
    TestEnabledAndReasons();
    TestArgs();
    TestRunAll();
    TestRunSemantics();
    TestFilter();
    TestStateSections();
    TestModal();
    std::printf("checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
