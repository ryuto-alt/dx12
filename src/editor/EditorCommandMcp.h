#pragma once

// ===========================================================================
// エディタのコマンド表 → MCP 公開用の「分類・引数・実行可否・実行」（M7）
// ---------------------------------------------------------------------------
// docs/MCP_ENHANCEMENT_DESIGN.md §4.6。MCP（dx12_editor_command）はメニュー / ショートカット / パレットに続く
// 4 本目の入口で、コマンドの一覧は**表から機械的に作る**（手書きの二重管理をしない）:
//   ・kCommands（EditorCommandTable.h）／ window.*（ToolWindows.h の kAll）／ create.*（EditorCreateTable.h）
//   ・表に新しいコマンドが増えれば、この層は何も書かなくても list に出て run で実行できる（既定 = 通常・引数なし）
// この層が持つのは「危険か」「OS ダイアログを開くか」「モーダルを開くか」「引数」だけの分類（TraitsFor）と、
// いま実行できるか（ctx を見る）・実行（cmd::Execute の 1 段ラッパ）。実行経路はメニュー・キーと同一（cmd::Execute）。
//
// ★分類（TraitsFor）は id だけで決まる純関数で、EditorContext にも ImGui にも依存しない
//   （tests/mcp_editor_command_test.cpp が「全 id に妥当な分類が付く / 上書き表が実在の id を指す」を検査する）。
// ===========================================================================

#include "editor/EditorCommandTable.h"
#include "editor/EditorCreateTable.h"
#include "editor/EditorCommands.h"   // cmd::Env

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace dx12e
{
class EditorContext;
}

namespace dx12e::cmd::mcp
{

struct ArgSpec
{
    std::string              name;
    std::string              type;          // "enum" | "string" | "vec3"
    bool                     required = false;
    std::string              desc;
    std::vector<std::string> enumValues;
};

struct Traits
{
    std::string kind = "command";     // "command" | "window" | "create"
    std::string effect = "write_setting";   // read | write_scene | write_setting | write_file | runtime
    bool        guarded = false;      // 危険: 実行に確認が要る（editor_command_run は拒否し editor_command_run_guarded だけが実行する）
    std::string guardReason;
    bool        osDialog = false;     // OS のネイティブダイアログを開く経路がある（仮想入力 / 背景モード中は実行拒否）
    bool        opensModal = false;   // アプリ内のダイアログ / モーダル / パレットを開く
    bool        needsSelection = false;
    bool        toggleState = false;  // args.state = on / off / toggle（トグル系）
    bool        windowState = false;  // args.state = open / close / toggle（window.*）
    std::vector<ArgSpec> args;
};

// id だけから決まる分類。未知の id / 新しいコマンドは既定（通常・引数なし）。
Traits TraitsFor(std::string_view id);

// 上書き表（id 指定の分類）に載っている id の一覧（ctest が「表に実在するか」を見る）。
std::vector<std::string> OverrideIds();

// id の種別: "command"（kCommands）/ "window"（window.<tool>）/ "create"（create.*）/ ""（表に無い）。
std::string KindOf(std::string_view id);

// 実行環境（MCP 側の状態。ctx だけでは分からないもの）。
struct RunEnv
{
    bool virtualInput = false;   // 仮想入力モード（= --background）。OS ダイアログは出せない
    bool aiTxOpen = false;       // AI のトランザクションが開いている（edit.undo / redo は閉じてしまうので断る）
};

// 表 1 行ぶん。
struct Row
{
    std::string id, label, labelEn, category, kind, chord, chord2, help, scope, keyMode;
    Traits      traits;
    bool        enabled = true;
    std::string disabledReason;
    bool        hasChecked = false;   // トグル系（checked が意味を持つ）
    bool        checked = false;
    bool        hasOpen = false;      // kind == "window"
    bool        open = false;
    std::string blockedNow;           // いまの環境で実行が拒否になる理由（空 = 実行できる）
};

// 表の全コマンド（kCommands → window.* → create.*）。ctx でその時点の可否を付ける。
std::vector<Row> BuildRows(const EditorContext& ctx, const RunEnv& env);
// 1 件だけ（無ければ id が空の Row）。
Row BuildRow(const EditorContext& ctx, const RunEnv& env, std::string_view id);

// 実行できない理由（IsEnabled が false のとき。true なら空文字）。
std::string DisabledReason(const EditorContext& ctx, std::string_view id);

// 行 → JSON。compact は list の既定（短いキーだけ）、detail は describe 用。
nlohmann::json RowToJson(const Row& r, bool detail);

// list のフィルタ。
struct ListFilter
{
    std::string id;         // 完全一致
    std::string query;      // 曖昧検索（id・ラベル・英名・カテゴリ）
    std::string category;
    std::string kind;
    bool        enabledOnly = false;
    bool        guardedOnly = false;
};
// フィルタしてスコア順（query 無しは表の順）に並べる。
std::vector<Row> Filter(std::vector<Row> rows, const ListFilter& f);
// 近い id（打ち間違い用。最大 n 件）。
std::vector<std::string> SuggestIds(const std::vector<Row>& rows, std::string_view id, size_t n = 5);

// run の引数（検証済みの値）。
struct RunArgs
{
    std::string state;                 // window: open/close/toggle、トグル: on/off/toggle（空 = 既定）
    bool        hasPosition = false;
    float       position[3] = {0.0f, 0.0f, 0.0f};
    std::string name;
};
struct RunOutcome
{
    bool        executed = false;
    bool        changed = true;     // 目的の状態に既になっていて何もしなかったら false
    std::string note;
};
// 引数の JSON を検証して RunArgs へ。不正なら false と理由（details 用の JSON も）。
bool ParseArgs(std::string_view id, const nlohmann::json& args, RunArgs& out, std::string& error);
// 実行（cmd::Execute の薄いラッパ。window / トグルは state、create は position / name を反映）。
RunOutcome Run(EditorContext& ctx, const Env& env, std::string_view id, const RunArgs& args);

} // namespace dx12e::cmd::mcp
