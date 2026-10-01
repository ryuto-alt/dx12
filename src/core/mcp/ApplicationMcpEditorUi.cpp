// ===========================================================================
// MCP: エディタ操作の全面公開（M7）
// ---------------------------------------------------------------------------
// docs/MCP_ENHANCEMENT_DESIGN.md §4.6 / tools/mcp-server の dx12_editor_command・dx12_editor_state。
//
//   editor_command_list        コマンド表（kCommands / window.* / create.*）の一覧 + いま実行できるか + 危険度
//   editor_command_run         コマンドを実行（メニュー・ショートカット・パレットと同じ cmd::Execute の経路）。遅延応答
//   editor_command_run_guarded 危険（guarded）なコマンドを実行する口。ディスパッチャの confirm_token ゲートが掛かる
//   editor_state               選択 / 窓 / レイアウト / モーダル / モード / undo / トースト / perf を読む
//   editor_notify              エディタのトーストに出す（AI → 人）
//   editor_select              複数選択・追加 / 解除・名前 / guid / クエリ
//
// ★一覧は表から機械的に作る（editor/EditorCommandMcp.cpp）。コマンドを表に足せばここは何も書かなくても公開される。
// ★OS のカーソル / 前面ウィンドウには触れない。実行は EditorContext のフラグを立てるだけ（フレーム境界で UI 側が消化する）。
// ★モーダル / ダイアログ / パレットが開いている間の run は E_MODAL_OPEN（キー操作が効かないのと同じ規則）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"
#include "core/mcp/McpSafety.h"
#include "editor/EditorCommandMcp.h"
#include "editor/EditorStateMcp.h"
#include "editor/Toast.h"
#include "editor/ToolWindows.h"
#include "gui/VirtualInputImGui.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <map>
#include <set>

// DX12E_MCP_HANDLER は [this] だけ捕まえる。このファイルのハンドラは登録関数の中のラムダ（requireEditor / makeRunEnv）も使うので捕まえ直す版。
#define EDITORUI_HANDLER                                                     \
    [this, requireEditor, makeRunEnv]([[maybe_unused]] const nlohmann::json& params, \
           [[maybe_unused]] nlohmann::json&       resp,                      \
           [[maybe_unused]] const std::string&    method,                    \
           [[maybe_unused]] McpDeferred&          deferred,                  \
           [[maybe_unused]] bool&                 isDeferred,                \
           [[maybe_unused]] bool                  busyPlaying) -> void

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;

namespace
{
using json = nlohmann::json;
namespace mc = dx12e::cmd::mcp;

// ---------------------------------------------------------------------------
// run の前後スナップショットと待ち
// ---------------------------------------------------------------------------
struct Snap
{
    std::string mode;
    bool paused = false;
    std::vector<uint32_t> selection;
    size_t entities = 0;
    bool dirty = false;
    std::set<std::string> openWindows;
    std::set<std::string> modalIds;
};

struct EditorRunWait
{
    McpDeferred reply;
    std::string id;
    uint64_t    startFrame = 0;
    std::chrono::steady_clock::time_point startTime;
    bool        changed = true;
    std::string note;
    Snap        before;
    uint32_t    toastSeq0 = 0;
};

// 単一の Application しか無い（MCP ブリッジも 1 本）のでファイル内の静的でよい。
std::vector<EditorRunWait>&  Waits()    { static std::vector<EditorRunWait> w; return w; }
std::function<Snap()>&       SnapFn()   { static std::function<Snap()> f; return f; }
std::function<bool()>&       QuietFn()  { static std::function<bool()> f; return f; }
std::function<uint64_t()>&   FrameFn()  { static std::function<uint64_t()> f; return f; }
std::function<json(const Snap&, const Snap&, uint32_t)>& EffectsFn()
{
    static std::function<json(const Snap&, const Snap&, uint32_t)> f;
    return f;
}

constexpr uint64_t kMinFrames = 3;      // 表のコマンドが立てたフラグを UI / Application が消化するまでの最小待ち
constexpr uint64_t kMaxFrames = 180;
constexpr double   kMaxSeconds = 6.0;

// `*` `?` の簡易ワイルドカード（大文字小文字は ASCII だけ無視）。ワイルドカードが無ければ部分一致。
bool WildMatch(const std::string& pat, const std::string& text)
{
    auto low = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string p, t;
    for (unsigned char c : pat) p.push_back(low(c));
    for (unsigned char c : text) t.push_back(low(c));
    if (p.find_first_of("*?") == std::string::npos) return t.find(p) != std::string::npos;
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size())
    {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { ++pi; ++ti; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

json ArgsSchemaJson(const mc::Traits& t)
{
    json a = json::array();
    for (const mc::ArgSpec& s : t.args)
    {
        json j = {{"name", s.name}, {"type", s.type}, {"required", s.required}, {"desc", s.desc}};
        if (!s.enumValues.empty()) j["enum"] = s.enumValues;
        a.push_back(std::move(j));
    }
    return a;
}

} // namespace

// ---------------------------------------------------------------------------
// 登録
// （McpDefine の第 2 引数は McpMeta(m) と書く: tests/mcp_manifest_test.cpp は "McpMeta" で始まる引数を「meta を直接渡す多重定義」と数える）
// ---------------------------------------------------------------------------
void Application::RegisterMcpEditorUiMethods()
{
    // ---- 共通の部品（this を掴むラムダ。Waits の Service 側からも使う）----
    auto requireEditor = [this]()
    {
        if (m_isGameMode || !m_editorCtx || !m_scene || !ImGui::GetCurrentContext())
        {
            McpError err(McpErr::ModeConflict, "editor UI is not available",
                         "エディタとして起動していること（配布ゲーム / ランチャー画面 / ImGui 未初期化では使えない）。dx12_ping が通ってから呼ぶ");
            err.name  = "E_MODE_CONFLICT";
            err.cause = "エディタ UI（EditorContext / ImGui）がまだ無い、または配布ゲームとして動いている";
            err.details = {{"reason", "editor-ui-unavailable"}};
            throw err;
        }
    };
    auto makeRunEnv = [this]()
    {
        mc::RunEnv env;
        env.virtualInput = vinput::Enabled();
        env.aiTxOpen = m_editorCtx && m_editorCtx->mcpUndo.TxOpen();
        return env;
    };
    auto cmdEnv = [this]() { return cmd::Env{m_scene.get(), PathResolver::AssetsDir()}; };
    auto entityCount = [this]() -> size_t
    {
        return m_scene ? m_scene->GetRegistry().view<NameTag>().size() : 0;
    };

    SnapFn() = [this, entityCount]() -> Snap
    {
        Snap s;
        if (!m_editorCtx) return s;
        const EditorContext& c = *m_editorCtx;
        s.mode = m_engineMode == EngineMode::Playing ? "playing" : "editor";
        s.paused = c.paused;
        if (m_scene)
            for (entt::entity e : c.selectedEntities)
                if (m_scene->GetRegistry().valid(e)) s.selection.push_back(static_cast<uint32_t>(e));
        s.entities = entityCount();
        s.dirty = c.IsSceneDirty();
        for (const tools::Desc& d : tools::kAll)
            if (tools::IsOpen(c, d)) s.openWindows.insert(d.id);
        for (const auto& m : edstate::CollectModals(c)) s.modalIds.insert(m.id);
        return s;
    };
    FrameFn() = [this]() -> uint64_t { return m_perfTotalFrames; };
    QuietFn() = [this]() -> bool
    {
        if (!m_editorCtx) return true;
        const EditorContext& c = *m_editorCtx;
        return c.pendingSpawns.empty() && c.pendingDuplications.empty() && c.pendingPastes.empty()
            && c.pendingDeletions.empty() && c.mcpDeletions.empty() && c.mcpDuplications.empty()
            && !c.pendingUndo && !c.pendingRedo && !c.pendingSaveScene && c.pendingPlayRequest == 0
            && !m_modeChangeRequested && !c.resetLayout && c.pendingWorkspace.empty()
            && c.pendingBookmarkJump == 0 && c.pendingBookmarkSet == 0 && !c.pendingGroupSelection
            && !c.pendingFocusSelection && c.pendingLoadPath.empty() && !c.pendingCloseProject
            && c.paletteRequest == 0 && !c.whatsNewRequest && !c.pendingToggleFullscreen && !c.bottomDockMaximizeToggle
            && !c.layoutSaveWindowRequest && !c.layoutSlotsWindowRequest && c.pendingLayoutSaveName.empty()
            && c.pendingLayoutRestore.empty() && c.pendingToolSlots.empty();
    };
    EffectsFn() = [this](const Snap& b, const Snap& a, uint32_t toastSeq0) -> json
    {
        json fx = json::object();
        json opened = json::array(), closed = json::array();
        for (const auto& w : a.openWindows) if (!b.openWindows.count(w)) opened.push_back(w);
        for (const auto& w : b.openWindows) if (!a.openWindows.count(w)) closed.push_back(w);
        fx["windowsOpened"] = std::move(opened);
        fx["windowsClosed"] = std::move(closed);

        json toasts = json::array();
        auto hist = ui::ToastHistory(30);   // 新しい順
        for (auto it = hist.rbegin(); it != hist.rend(); ++it)
            if (it->seq > toastSeq0) toasts.push_back({{"kind", ui::ToastKindName(it->kind)}, {"text", it->text}});
        fx["toasts"] = std::move(toasts);

        fx["mode"] = {{"before", b.mode}, {"after", a.mode}};
        fx["paused"] = {{"before", b.paused}, {"after", a.paused}};
        fx["selection"] = {{"before", b.selection}, {"after", a.selection}};
        fx["entityCount"] = {{"before", b.entities}, {"after", a.entities},
                             {"delta", static_cast<long long>(a.entities) - static_cast<long long>(b.entities)}};
        fx["dirty"] = {{"before", b.dirty}, {"after", a.dirty}};
        if (m_editorCtx)
        {
            const UndoSystem& u = m_editorCtx->undoSystem;
            json uj = {{"canUndo", u.CanUndo()}, {"canRedo", u.CanRedo()}};
            if (const char* n = u.PeekUndoName()) uj["nextUndo"] = n;
            fx["undo"] = std::move(uj);
        }
        json modals = json::array();
        if (m_editorCtx)
            for (const auto& m : edstate::CollectModals(*m_editorCtx))
                if (!b.modalIds.count(m.id)) modals.push_back({{"kind", m.kind}, {"id", m.id}, {"title", m.title}});
        fx["modalsOpened"] = std::move(modals);

        // 増えたエンティティ（create.* / 複製 / 貼り付け）。NameTag の pool は挿入順なので、増えた分は末尾の N 個
        //（生成だけが起きた区間の話。削除が混ざる操作では推定しない）。
        if (a.entities > b.entities && m_scene)
        {
            auto& reg = m_scene->GetRegistry();
            auto& pool = reg.storage<NameTag>();
            const size_t n = (std::min<size_t>)({a.entities - b.entities, static_cast<size_t>(10), pool.size()});
            json created = json::array();
            for (size_t i = pool.size() - n; i < pool.size(); ++i)
            {
                const entt::entity e = pool.data()[i];
                if (!reg.valid(e)) continue;
                created.push_back({{"entityId", static_cast<uint32_t>(e)}, {"name", pool.get(e).name}});
            }
            if (!created.empty()) fx["created"] = std::move(created);
        }
        return fx;
    };

    // =======================================================================
    // editor_command_list
    // =======================================================================
    {
        McpMeta m;
        m.summary    = "エディタのコマンド表（メニュー / ショートカット / パレットと同じ表）を返す。id・表示名・キー・カテゴリ・いま実行できるか(理由)・危険度・引数の有無。"
                       "新しいコマンドが表に増えれば自動で出る";
        m.keywords   = "editor command list コマンド 一覧 メニュー ショートカット パレット ウィンドウを開く window create 作成 undo redo 元に戻す エディタ操作 一覧表";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "command_list";
        m.effect     = McpEffect::Read;
        m.timeoutMs  = 8000;
        m.idempotent = true;
        m.params     = {P("id", "string", false, nullptr, nullptr, nullptr, nullptr, "このコマンド 1 件だけ詳しく返す（例 window.postProcess）。無ければ E_NOT_FOUND_COMMAND + didYouMean"),
                        P("query", "string", false, nullptr, nullptr, nullptr, nullptr, "曖昧検索（id・表示名・英名・カテゴリ。日本語 / 英語 / かな）"),
                        P("category", "string", false, nullptr, nullptr, nullptr, nullptr, "カテゴリで絞る（ファイル / 編集 / 表示 / 再生 / ウィンドウ / 作成 / ワークスペース …）"),
                        P("kind", "enum", false, "command|window|create", nullptr, nullptr, nullptr, "種別で絞る: command=表のコマンド / window=ツール窓の開閉 / create=エンティティ作成"),
                        P("enabledOnly", "bool", false, nullptr, nullptr, nullptr, "false", "true でいま実行できるものだけ"),
                        P("guardedOnly", "bool", false, nullptr, nullptr, nullptr, "false", "true で危険（guarded）なものだけ"),
                        P("detail", "bool", false, nullptr, nullptr, nullptr, "false", "true で英名・説明・引数の仕様・例まで返す（id 指定時は常に true）")};
        m.next       = {{"editor_command_run", "実行する。guarded なものは editor_command_run_guarded（承認が要る）"},
                        {"editor_state", "モーダルが開いていないか / いまの状態を確かめる"}};
        m.examples   = {{"{\"query\":\"ポスト\"}", "ポストプロセスの窓を探す"},
                        {"{\"kind\":\"window\",\"detail\":true}", "開閉できる全ツール窓と開閉状態"},
                        {"{\"id\":\"create.box\"}", "1 件の引数仕様まで"}};
        McpDefine("editor_command_list", McpMeta(m), EDITORUI_HANDLER
            {
                requireEditor();
                const mc::RunEnv renv = makeRunEnv();
                std::vector<mc::Row> all = mc::BuildRows(*m_editorCtx, renv);
                const size_t total = all.size();

                mc::ListFilter f;
                f.id = params.value("id", std::string());
                f.query = params.value("query", std::string());
                f.category = params.value("category", std::string());
                f.kind = params.value("kind", std::string());
                f.enabledOnly = params.value("enabledOnly", false);
                f.guardedOnly = params.value("guardedOnly", false);
                const bool detail = params.value("detail", false) || !f.id.empty();

                if (!f.id.empty() && std::none_of(all.begin(), all.end(), [&](const mc::Row& r) { return r.id == f.id; }))
                {
                    McpError err(McpErr::NotFound, "no editor command '" + f.id + "'",
                                 "editor_command_list {query} で探す。id は表の id（window.<ツール名> / create.<種類> / file.save など）");
                    err.name = "E_NOT_FOUND_COMMAND";
                    err.cause = "この id のコマンドは表に無い";
                    err.didYouMean = mc::SuggestIds(all, f.id);
                    err.fix.push_back(MakeMcpFix("editor_command_list", json{{"query", f.id}}, "曖昧検索で正しい id を探す"));
                    throw err;
                }

                std::map<std::string, int> cats;
                for (const mc::Row& r : all) ++cats[r.category];
                std::vector<mc::Row> rows = mc::Filter(std::move(all), f);

                json commands = json::array();
                for (const mc::Row& r : rows) commands.push_back(mc::RowToJson(r, detail));
                json categories = json::array();
                for (const auto& [id, n] : cats) categories.push_back({{"id", id}, {"count", n}});

                const bool modal = edstate::IsBlocking(*m_editorCtx);
                resp["ok"] = true;
                resp["result"] = {{"total", total}, {"count", commands.size()}, {"categories", std::move(categories)},
                                  {"context", {{"playing", m_engineMode == EngineMode::Playing}, {"paused", m_editorCtx->paused},
                                               {"virtualInput", renv.virtualInput}, {"background", IsBackground()}, {"modalOpen", modal}}},
                                  {"commands", std::move(commands)}};
            });
    }

    // =======================================================================
    // editor_command_run / editor_command_run_guarded
    // =======================================================================
    // 実行の本体（2 つの method が共有）。guardedRoute=true は confirm_token ゲートを通ってきた口。
    auto runCommand = [this, requireEditor, makeRunEnv, cmdEnv](const json& params, json& resp, McpDeferred& deferred, bool& isDeferred, bool guardedRoute)
    {
        requireEditor();
        EditorContext& ctx = *m_editorCtx;
        const std::string id = params.value("id", std::string());
        const mc::RunEnv renv = makeRunEnv();
        const std::vector<mc::Row> all = mc::BuildRows(ctx, renv);
        const auto rowIt = std::find_if(all.begin(), all.end(), [&](const mc::Row& r) { return r.id == id; });
        if (rowIt == all.end())
        {
            McpError err(McpErr::NotFound, "no editor command '" + id + "'",
                         "editor_command_list {query} で探す。id は表の id（window.<ツール名> / create.<種類> / file.save など）");
            err.name = "E_NOT_FOUND_COMMAND";
            err.cause = "この id のコマンドは表に無い";
            err.didYouMean = mc::SuggestIds(all, id);
            err.fix.push_back(MakeMcpFix("editor_command_list", json{{"query", id}}, "曖昧検索で正しい id を探す"));
            throw err;
        }
        const mc::Row& row = *rowIt;

        // ---- 引数 ----
        mc::RunArgs ra;
        std::string argErr;
        const json argsIn = params.contains("args") ? params["args"] : json::object();
        if (!mc::ParseArgs(id, argsIn, ra, argErr))
        {
            McpError err(McpErr::InvalidParam, "editor command '" + id + "': " + argErr,
                         "受け付ける引数は details.args。editor_command_list {id, detail:true} でも確かめられる");
            err.name = "E_INVALID_PARAM";
            err.cause = argErr;
            err.details = {{"id", id}, {"args", ArgsSchemaJson(row.traits)}};
            err.fix.push_back(MakeMcpFix("editor_command_list", json{{"id", id}}, "このコマンドの引数の仕様"));
            throw err;
        }

        // ---- OS ダイアログ（仮想入力中は出せない）----
        if (row.traits.osDialog && renv.virtualInput)
        {
            McpError err(McpErr::Unsupported, "editor command '" + id + "' opens an OS dialog, which is blocked in virtual-input (background) mode",
                         "OS のファイル選択ダイアログは人の画面に出てフォーカスを奪うので、仮想入力 / --background では出さない。dx12_open_scene {path} を使う");
            err.name = "E_UNSUPPORTED";
            err.cause = "このコマンドは OS ネイティブのダイアログを開く。仮想入力モード中は実行しない";
            err.details = {{"reason", "os-dialog"}, {"id", id}, {"virtualInput", true}};
            err.fix.push_back(MakeMcpFix("open_scene", json{{"path", "scenes/<name>.json"}}, "パスを指定してシーンを開く"));
            throw err;
        }

        // ---- guarded は確認トークンを通った口でだけ実行する ----
        if (row.traits.guarded && !guardedRoute)
        {
            McpError err(McpErr::Guarded, "editor command '" + id + "' is guarded: run it with editor_command_run_guarded",
                         "危険なコマンド（" + row.traits.guardReason + "）。editor_command_run_guarded で実行する（確認が要る）。実行せず影響だけ見るなら dryRun:true");
            err.name = "E_GUARDED";
            err.cause = row.traits.guardReason;
            err.fix.push_back(MakeMcpFix("editor_command_run_guarded", json{{"id", id}, {"args", argsIn}},
                                         "確認トークン付きで実行する（TS の dx12_call_guarded / confirm 経路が自動で付ける）"));
            err.details = {{"id", id}, {"gate", "editor-command"}, {"command", id}, {"reason", row.traits.guardReason}};
            throw err;
        }

        // ---- モーダル / ダイアログ / パレットが開いている間は断る（キー操作が効かないのと同じ規則）----
        {
            const auto modals = edstate::CollectModals(ctx);
            if (edstate::IsBlocking(modals))
            {
                json mj = json::array();
                for (const auto& mm : modals)
                    if (mm.blocking) mj.push_back({{"kind", mm.kind}, {"id", mm.id}, {"title", mm.title}, {"dismissHint", mm.dismissHint}});
                McpError err(McpErr::ModalOpen, "a modal dialog is open; editor command '" + id + "' was not run",
                             "先にモーダルを閉じる（editor_state {scope:\"modal\"} で種類を確かめ、editor_modal {action:\"dismiss\"} かボタンで閉じる）");
                err.name = "E_MODAL_OPEN";
                err.cause = "モーダル / ダイアログ / コマンドパレットが開いていて、エディタの操作が塞がれている";
                err.details = {{"modals", std::move(mj)}};
                err.fix.push_back(MakeMcpFix("editor_state", json{{"scope", "modal"}}, "開いているモーダルの種類と閉じ方"));
                err.fix.push_back(MakeMcpFix("editor_modal", json{{"action", "dismiss"}}, "キャンセルしても害の無いダイアログ（新規シーン / 名前を付けて保存 / 新規スクリプト等）を閉じる。ImGui のモーダルは Esc では閉じない。判断が要るダイアログ（未保存の確認 / 復旧）は imgui_find + imgui_pointer でボタンを押す"));
                throw err;
            }
        }

        // ---- 実行できる状態か ----
        if (!row.enabled || !row.blockedNow.empty())
        {
            const std::string why = !row.enabled ? row.disabledReason : row.blockedNow;
            McpError err(McpErr::ModeConflict, "editor command '" + id + "' cannot run now: " + why,
                         "理由を解消してから実行する。editor_state で状況を確かめる");
            err.name = "E_MODE_CONFLICT";
            err.cause = why;
            err.details = {{"id", id}, {"reason", why}, {"playing", m_engineMode == EngineMode::Playing}};
            if (row.traits.needsSelection)
                err.fix.push_back(MakeMcpFix("editor_select", json{{"names", json::array({"<entity name>"})}}, "対象のエンティティを選ぶ"));
            if (why.find("Play") != std::string::npos)
                err.fix.push_back(MakeMcpFix("stop", json::object(), "Editor に戻す"));
            err.fix.push_back(MakeMcpFix("editor_state", json{{"scope", "mode"}}, "いまのモードと状態"));
            throw err;
        }

        // ---- 実行（cmd::Execute の経路。フラグを立てるだけで、消化は次フレーム以降）----
        const Snap before = SnapFn()();
        uint32_t toastSeq0 = 0;
        {
            const auto h = ui::ToastHistory(1);
            if (!h.empty()) toastSeq0 = h.front().seq;
        }
        const mc::RunOutcome oc = mc::Run(ctx, cmdEnv(), id, ra);
        if (!oc.executed)
        {
            McpError err(McpErr::Internal, "editor command '" + id + "' did not run",
                         "表のコマンドが false を返した（対象が無い / 実装が受け付けなかった）。editor_state で状況を確かめる");
            err.name = "E_INTERNAL";
            err.cause = "cmd::Execute が false を返した";
            throw err;
        }
        if (!oc.changed)
        {
            // 目的の状態に既になっていた（何もしなかった）: 待たずに返す
            resp["ok"] = true;
            resp["result"] = {{"id", id}, {"executed", true}, {"changed", false}, {"frames", 0},
                              {"effects", EffectsFn()(before, before, toastSeq0)}, {"note", oc.note}};
            return;
        }

        EditorRunWait w;
        w.reply = deferred;
        w.id = id;
        w.startFrame = FrameFn()();
        w.startTime = std::chrono::steady_clock::now();
        w.changed = oc.changed;
        w.note = oc.note;
        w.before = before;
        w.toastSeq0 = toastSeq0;
        Waits().push_back(std::move(w));
        isDeferred = true;
    };

    // dryRun のプレビュー（実行せず、何が起こるか）。ディスパッチャの PreviewTable から呼ばれる。
    auto previewCommand = [this, makeRunEnv](const json& p) -> json
    {
        json out = {{"summary", ""}, {"willFail", false}, {"targets", json::array()}, {"count", 0}, {"destructive", false},
                    {"undoable", ""}, {"files", json::array()}, {"notes", json::array()}, {"enabled", false}, {"guarded", false}};
        auto fail = [&](const std::string& why)
        {
            out["willFail"] = true; out["reason"] = why; out["summary"] = "実行すると失敗する: " + why;
            out["undoable"] = "変更なし(失敗する)";
        };
        if (m_isGameMode || !m_editorCtx || !m_scene || !ImGui::GetCurrentContext()) { fail("エディタ UI が無い"); return out; }
        const std::string id = p.value("id", std::string());
        const std::vector<mc::Row> all = mc::BuildRows(*m_editorCtx, makeRunEnv());
        const auto it = std::find_if(all.begin(), all.end(), [&](const mc::Row& r) { return r.id == id; });
        if (it == all.end()) { fail("表に無いコマンド '" + id + "'"); out["didYouMean"] = mc::SuggestIds(all, id); return out; }
        const mc::Row& r = *it;
        mc::RunArgs ra; std::string err;
        if (!mc::ParseArgs(id, p.contains("args") ? p["args"] : json::object(), ra, err)) { fail(err); return out; }
        out["enabled"] = r.enabled;
        out["guarded"] = r.traits.guarded;
        out["command"] = {{"id", r.id}, {"label", r.label}, {"kind", r.kind}, {"effect", r.traits.effect}};
        if (!r.blockedNow.empty()) { fail(r.blockedNow); return out; }
        if (edstate::IsBlocking(*m_editorCtx)) { fail("モーダル / ダイアログが開いている（E_MODAL_OPEN）"); return out; }
        if (!r.enabled) { fail(r.disabledReason); return out; }
        out["summary"] = r.help;
        if (r.traits.needsSelection && m_scene)
        {
            auto& reg = m_scene->GetRegistry();
            json targets = json::array();
            for (entt::entity e : m_editorCtx->selectedEntities)
            {
                if (!reg.valid(e)) continue;
                if (targets.size() < 20)
                    targets.push_back({{"kind", "entity"}, {"id", static_cast<uint32_t>(e)}, {"name", reg.all_of<NameTag>(e) ? reg.get<NameTag>(e).name : std::string()}});
            }
            out["count"] = m_editorCtx->selectedEntities.size();
            out["targets"] = std::move(targets);
        }
        if (id == "edit.delete") { out["destructive"] = true; out["notes"].push_back("選択中のエンティティ（と子）が消える。edit.undo で戻せる"); }
        if (r.traits.guarded) out["notes"].push_back("実行には editor_command_run_guarded が要る（確認トークン）: " + r.traits.guardReason);
        if (r.traits.opensModal) out["notes"].push_back("アプリ内のダイアログ / モーダルが開く。開いている間は次の run が E_MODAL_OPEN になる（Esc かボタンで閉じる）");
        if (r.traits.effect == "write_file") out["files"].push_back({{"action", "write"}, {"note", "ファイルを書く（Undo では戻らない）"}});
        out["undoable"] = r.traits.effect == "write_scene" ? "edit.undo で戻せる（Editor モードのみ）"
                        : r.traits.effect == "write_file" ? "ファイルを書く。Undo では戻らない"
                        : "エディタの表示 / 状態だけで、シーンのデータは変わらない";
        return out;
    };

    {
        McpMeta m;
        m.summary    = "エディタのコマンドを名前（id）で実行する。メニュー・ショートカット・パレットと同じ経路。window.<ツール名>（窓の開閉）/ create.<種類>（エンティティ作成）/ "
                       "edit.undo・edit.redo・edit.duplicate・view.*・play.* など。危険なコマンドは editor_command_run_guarded";
        m.keywords   = "editor command run 実行 コマンド ウィンドウを開く 窓 開閉 ポストプロセス create 作成 undo redo 元に戻す やり直す 複製 グループ フォーカス ビュー ギズモ play stop 再生 メニュー ショートカット";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "command_run";
        m.effect     = McpEffect::WriteSetting;
        m.timeoutMs  = 15000;
        m.deferred   = true;
        m.dryRun     = "preview";
        m.params     = {P("id", "string", true, nullptr, nullptr, nullptr, nullptr, "コマンド id（editor_command_list で探す。例 window.postProcess / create.box / edit.undo）"),
                        P("args", "object", false, nullptr, nullptr, nullptr, nullptr,
                          "コマンドごとの引数。window.*: {state:open|close|toggle（既定 open）} / トグル系: {state:on|off|toggle} / create.*: {position:[x,y,z], name}。その他は無し")};
        m.next       = {{"editor_state", "実行の結果（窓が開いたか・選択・モーダル）を確かめる"},
                        {"editor_command_run_guarded", "guarded なコマンド（削除・保存・ファイルダイアログなど）はこちら"}};
        m.examples   = {{"{\"id\":\"window.postProcess\"}", "Post Process 窓を開く"},
                        {"{\"id\":\"create.box\",\"args\":{\"position\":[0,0.5,0],\"name\":\"Crate\"}}", "Box を作る"},
                        {"{\"id\":\"edit.undo\"}", "元に戻す"}};
        McpDefine("editor_command_run", McpMeta(m), [runCommand](const json& params, json& resp, const std::string&, McpDeferred& deferred, bool& isDeferred, bool)
            { runCommand(params, resp, deferred, isDeferred, /*guardedRoute=*/false); });
    }
    {
        McpMeta m;
        m.summary    = "危険（guarded）なエディタコマンドを実行する口（選択の削除 edit.delete・シーンの上書き保存 file.save・名前を付けて保存・プロジェクトを閉じる・OS のファイルダイアログ file.open など）。"
                       "確認トークン（confirm_token）が要る";
        m.keywords   = "editor command run guarded 危険 削除 保存 上書き プロジェクトを閉じる ダイアログ 確認 承認";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "command_run_guarded";
        m.effect     = McpEffect::Guarded;
        m.timeoutMs  = 15000;
        m.deferred   = true;
        m.dryRun     = "preview";
        m.params     = {P("id", "string", true, nullptr, nullptr, nullptr, nullptr, "コマンド id（editor_command_list {guardedOnly:true} で一覧）"),
                        P("args", "object", false, nullptr, nullptr, nullptr, nullptr, "コマンドごとの引数（editor_command_run と同じ）")};
        m.next       = {{"editor_state", "結果（エンティティ数・dirty・モーダル）を確かめる"}};
        m.examples   = {{"{\"id\":\"edit.delete\"}", "選択中のエンティティを削除する（edit.undo で戻せる）"}};
        McpDefine("editor_command_run_guarded", McpMeta(m), [runCommand](const json& params, json& resp, const std::string&, McpDeferred& deferred, bool& isDeferred, bool)
            { runCommand(params, resp, deferred, isDeferred, /*guardedRoute=*/true); });
    }
    // dryRun プレビュー表へ（run / run_guarded とも同じ関数）
    {
        auto reg = [&](const char* name)
        {
            mcpsafety::PreviewTable()[name] = [previewCommand](const std::string& paramsJson) -> std::string
            {
                json p = json::parse(paramsJson, nullptr, /*allow_exceptions=*/false);
                if (p.is_discarded() || !p.is_object()) p = json::object();
                json out;
                try { out = previewCommand(p); }
                catch (const std::exception& e) { out = {{"summary", std::string("実行すると失敗する: ") + e.what()}, {"willFail", true}, {"reason", e.what()},
                                                          {"targets", json::array()}, {"count", 0}, {"destructive", false}, {"undoable", "変更なし(失敗する)"},
                                                          {"files", json::array()}, {"notes", json::array()}}; }
                return out.dump(-1, ' ', false, json::error_handler_t::replace);
            };
        };
        reg("editor_command_run");
        reg("editor_command_run_guarded");
    }

    // =======================================================================
    // editor_state
    // =======================================================================
    {
        McpMeta m;
        m.summary    = "エディタの状態を読む（読み取り専用）。選択・開いているツール窓とドック構成・モーダル / ポップアップ（AI が詰まらない）・Play / エディタモード・"
                       "undo 履歴と未保存(dirty)・直近のトースト・ビューポートのカメラ / ビューモード・DPI・背景 / 仮想入力・perf";
        m.keywords   = "editor state 状態 エディタの状態 選択 selection ウィンドウ windows ドック layout レイアウト モーダル modal ダイアログ ポップアップ undo dirty 未保存 トースト toast 通知 モード play カメラ perf fps";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "state";
        m.effect     = McpEffect::Read;
        m.timeoutMs  = 8000;
        m.idempotent = true;
        m.aliases    = {"dx12_editor_state"};
        m.params     = {P("scope", "enum", false, "all|selection|windows|layout|modal|mode|undo|toasts|perf", nullptr, nullptr, "\"all\"", "返すセクション（既定 all）"),
                        P("limit", "int", false, nullptr, "1", "50", "20", "toasts の件数（新しい順）")};
        m.next       = {{"editor_command_run", "モーダルが無ければコマンドを実行する"},
                        {"editor_modal", "モーダルが開いているとき閉じる（キャンセルと同じ）"}};
        m.examples   = {{"{\"scope\":\"modal\"}", "モーダルが開いていないか（コマンド実行の前に）"},
                        {"{\"scope\":\"windows\"}", "開いているツール窓とドックのタブ"},
                        {"{}", "全部"}};
        McpDefine("editor_state", McpMeta(m), EDITORUI_HANDLER
            {
                requireEditor();
                const std::string scope = params.value("scope", std::string("all"));
                const size_t limit = static_cast<size_t>(std::clamp(params.value("limit", 20), 1, 50));
                const bool all = scope == "all";
                EditorContext& ctx = *m_editorCtx;
                json r = {{"scope", scope}, {"frame", m_perfTotalFrames}};

                if (all || scope == "selection") r["selection"] = edstate::SelectionJson(ctx, m_scene->GetRegistry());
                if (all || scope == "windows")
                {
                    r["windows"] = edstate::WindowsJson(ctx);
                }
                if (all || scope == "layout")
                    r["layout"] = edstate::LayoutJson(ctx, m_imguiManager ? m_imguiManager->GetUiScale() : 1.0f);
                if (all || scope == "modal") r["modal"] = edstate::ModalJson(ctx);
                if (all || scope == "mode")
                {
                    static const char* const kViewMode[] = {"lit", "depth", "normal", "roughness", "metallic", "ao", "wire"};
                    const int vm = std::clamp(ctx.vpPrefs.viewMode, 0, 6);
                    json viewport = {{"viewMode", kViewMode[vm]}, {"view2D", ctx.view2D}, {"flyMode", ctx.flyMode},
                                     {"gizmo", {{"mode", ctx.gizmoMode == GizmoMode::Translate ? "translate" : ctx.gizmoMode == GizmoMode::Rotate ? "rotate" : "scale"},
                                                {"space", ctx.gizmoLocalSpace ? "local" : "world"}}},
                                     {"fill", ctx.viewportFill}, {"workspace", ctx.currentWorkspace}};
                    if (m_camera)
                    {
                        const auto p = m_camera->GetPosition();
                        const auto f = m_camera->GetForward();
                        viewport["camera"] = {{"position", {p.x, p.y, p.z}}, {"forward", {f.x, f.y, f.z}}, {"fovY", m_camera->GetFovY()}};
                    }
                    json tx = {{"open", ctx.mcpUndo.TxOpen()}};
                    if (ctx.mcpUndo.TxOpen()) tx["label"] = ctx.mcpUndo.TxLabel();
                    r["mode"] = {{"engineMode", m_engineMode == EngineMode::Playing ? "playing" : "editor"},
                                 {"playing", m_engineMode == EngineMode::Playing}, {"paused", ctx.paused},
                                 {"headless", m_headless},
                                 {"background", {{"mode", BackgroundModeName(m_bgOptions.mode)}, {"toolWindow", m_bgOptions.toolWindow}}},
                                 {"virtualInput", vinput::Enabled()},
                                 {"dpiScale", m_imguiManager ? m_imguiManager->GetUiScale() : 1.0f},
                                 {"scene", {{"path", ctx.currentScenePath}, {"dirty", ctx.IsSceneDirty()}, {"generation", m_sceneGeneration},
                                            {"entityCount", m_scene->GetRegistry().view<NameTag>().size()}}},
                                 {"aiTransaction", std::move(tx)},
                                 {"viewport", std::move(viewport)}};
                }
                if (all || scope == "undo") r["undo"] = edstate::UndoJson(ctx);
                if (all || scope == "toasts") r["toasts"] = edstate::ToastsJson(limit);
                if (all || scope == "perf")
                {
                    const u32 have = static_cast<u32>((std::min<u64>)(m_perfTotalFrames, kPerfHistory));
                    const u32 n = (std::min)(have, 30u);
                    double frameMs = 0, workMs = 0, draws = 0;
                    u32 counted = 0;
                    for (u32 i = 0; i < n; ++i)
                    {
                        const PerfFrame& f = m_perfHistory[static_cast<size_t>((m_perfTotalFrames - 1 - i) % kPerfHistory)];
                        if (f.frameMs <= 0.0f) continue;
                        frameMs += f.frameMs; workMs += f.workMs; draws += f.draws; ++counted;
                    }
                    json p = {{"samples", counted}, {"entityCount", m_scene->GetRegistry().view<NameTag>().size()}};
                    if (counted > 0)
                    {
                        p["frameMs"] = frameMs / counted;
                        p["fps"] = frameMs > 0 ? 1000.0 * counted / frameMs : 0.0;
                        p["cpuMs"] = workMs / counted;
                        p["drawCalls"] = draws / counted;
                    }
                    r["perf"] = std::move(p);
                }
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }

    // =======================================================================
    // editor_modal（M7 の最小版。M8 で respond（ボタン名指定）を足す）
    // =======================================================================
    {
        McpMeta m;
        m.summary    = "開いているモーダル / ダイアログを読む（get）/ キャンセルと同じに閉じる（dismiss）。ImGui のモーダルは Esc では閉じないので、AI が詰まったときの脱出口。"
                       "閉じてよいのは新規シーン・名前を付けて保存・新規スクリプト / シェーダー・ショートカット一覧など、キャンセルしても副作用が無いものだけ";
        m.keywords   = "editor modal dialog モーダル ダイアログ 閉じる dismiss キャンセル cancel 詰まった 固まった 入力できない ポップアップ 新規シーン";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "modal";
        m.effect     = McpEffect::WriteSetting;
        m.timeoutMs  = 8000;
        m.idempotent = true;
        m.params     = {P("action", "enum", false, "get|dismiss", nullptr, nullptr, "\"get\"", "get=いまのモーダルを読む / dismiss=一番上の安全に閉じられるモーダルを閉じる")};
        m.next       = {{"editor_state", "閉じたか確かめる（scope:\"modal\"）"},
                        {"imgui_find", "判断が要るダイアログ（未保存の確認など）はボタンを探して imgui_pointer で押す"}};
        m.examples   = {{"{\"action\":\"dismiss\"}", "新規シーンのダイアログをキャンセルして閉じる"}};
        McpDefine("editor_modal", McpMeta(m), EDITORUI_HANDLER
            {
                requireEditor();
                const std::string action = params.value("action", std::string("get"));
                if (action == "get")
                {
                    resp["ok"] = true;
                    resp["result"] = edstate::ModalJson(*m_editorCtx);
                    return;
                }
                const edstate::DismissResult d = edstate::DismissTopModal(*m_editorCtx);
                if (!d.ok)
                {
                    McpError err(McpErr::ModeConflict, "editor_modal: " + d.reason,
                                 d.reason);
                    err.name = d.id.empty() ? "E_NOT_FOUND" : "E_UNSUPPORTED";
                    err.code = d.id.empty() ? McpErr::NotFound : McpErr::Unsupported;
                    err.cause = d.reason;
                    err.details = {{"id", d.id}, {"title", d.title}};
                    err.fix.push_back(MakeMcpFix("editor_state", json{{"scope", "modal"}}, "開いているモーダルと閉じ方"));
                    throw err;
                }
                resp["ok"] = true;
                resp["result"] = {{"dismissed", true}, {"id", d.id}, {"title", d.title},
                                  {"modal", edstate::ModalJson(*m_editorCtx)},
                                  {"note", "ImGui のポップアップを閉じた。まだ開いているモーダルがあれば modal に出る（もう一度 dismiss）"}};
            });
    }

    // =======================================================================
    // editor_notify
    // =======================================================================
    {
        McpMeta m;
        m.summary    = "エディタの右下にトースト通知を出す（AI から人へ。「生成完了」「要確認」を静かに伝える）。OS の通知は使わない";
        m.keywords   = "editor notify 通知 トースト toast メッセージ 知らせる 人に伝える 完了 警告 エラー 表示";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "notify";
        m.effect     = McpEffect::WriteSetting;
        m.timeoutMs  = 5000;
        m.aliases    = {"dx12_editor_notify"};
        m.params     = {P("message", "string", true, nullptr, nullptr, nullptr, nullptr, "本文（1〜400 文字）"),
                        P("level", "enum", false, "info|success|warn|error", nullptr, nullptr, "\"info\"", "種別（色とアイコン。error は表示が長め）"),
                        P("seconds", "number", false, nullptr, "0.5", "30", nullptr, "表示秒数（省略は種別ごとの既定: 3 秒 / error 6 秒）")};
        m.next       = {{"editor_state", "scope:\"toasts\" で出たトーストを読み返す"}};
        m.examples   = {{"{\"message\":\"部屋の生成が終わりました\",\"level\":\"success\"}", "完了を人に知らせる"}};
        McpDefine("editor_notify", McpMeta(m), EDITORUI_HANDLER
            {
                requireEditor();
                const std::string message = params.value("message", std::string());
                const size_t chars = McpDecodeUtf8(message).size();
                if (chars < 1 || chars > 400)
                {
                    McpError err(McpErr::InvalidParam, "editor_notify: message must be 1-400 characters (got " + std::to_string(chars) + ")",
                                 "message は 1〜400 文字。長い内容はファイルやログへ書いて、トーストは要点だけにする");
                    err.name = "E_OUT_OF_RANGE";
                    err.cause = "message の長さが範囲外";
                    err.details = {{"param", "message"}, {"length", chars}, {"range", {{"min", 1}, {"max", 400}}}};
                    throw err;
                }
                ui::ToastKind kind = ui::ToastKind::Info;
                ui::ParseToastKind(params.value("level", std::string("info")), kind);
                const float secs = params.contains("seconds") ? params["seconds"].get<float>() : -1.0f;
                const uint32_t tid = ui::Toast(kind, message, secs);
                resp["ok"] = true;
                resp["result"] = {{"notified", true}, {"id", tid}, {"level", ui::ToastKindName(kind)},
                                  {"seconds", secs > 0.0f ? secs : ui::ToastQueue::DefaultLife(kind)},
                                  {"live", ui::ToastLiveCount()},
                                  {"note", "人のエディタ画面の右下に出る。--background ではウィンドウがオフスクリーンなので、人の画面には出ない（editor_state {scope:\"toasts\"} には残る）"}};
            });
    }

    // =======================================================================
    // editor_select
    // =======================================================================
    {
        McpMeta m;
        m.summary    = "エディタの選択を操作する（複数選択・追加 / 解除 / 全解除）。entity id・名前・guid・名前のクエリ（部分一致 / * ? ワイルドカード）・タグで指定。"
                       "選択は edit.* コマンド（複製・削除など）の対象になる。select_entity（1 体だけ）の上位互換";
        m.keywords   = "editor select 選択 複数選択 追加 解除 全解除 selection multi deselect クエリ 名前 guid タグ フォーカス select_entity";
        m.category   = "editor";
        m.group      = "editor";
        m.target     = "select";
        m.effect     = McpEffect::WriteSetting;
        m.timeoutMs  = 8000;
        m.idempotent = true;
        m.aliases    = {"dx12_editor_select"};
        m.params     = {P("mode", "enum", false, "set|add|remove|toggle|clear", nullptr, nullptr, "\"set\"", "set=置き換え / add=追加 / remove=解除 / toggle=反転 / clear=全解除（指定不要）"),
                        P("entities", "array", false, nullptr, nullptr, nullptr, nullptr, "entity id の配列"),
                        P("names", "array", false, nullptr, nullptr, nullptr, nullptr, "エンティティ名の配列（完全一致）"),
                        P("query", "string", false, nullptr, nullptr, nullptr, nullptr, "名前の部分一致（* ? のワイルドカード可。大文字小文字は無視）"),
                        P("tag", "string", false, nullptr, nullptr, nullptr, nullptr, "Tag コンポーネントで絞る"),
                        P("guids", "array", false, nullptr, nullptr, nullptr, nullptr, "EntityGuid（16 進文字列）の配列"),
                        P("limit", "int", false, nullptr, "1", "2000", "200", "query / tag で選ぶ最大件数"),
                        P("focus", "bool", false, nullptr, nullptr, nullptr, "false", "選択後にカメラを寄せる（Editor モード）")};
        m.next       = {{"editor_command_run", "選択に対して edit.duplicate / edit.group / edit.focus などを実行する"},
                        {"editor_state", "scope:\"selection\" で現在の選択を読む"}};
        m.examples   = {{"{\"names\":[\"Crate\",\"Lamp\"]}", "2 体を選ぶ"},
                        {"{\"query\":\"Wall*\",\"mode\":\"add\"}", "Wall で始まる名前を追加選択"},
                        {"{\"mode\":\"clear\"}", "全解除"}};
        McpDefine("editor_select", McpMeta(m), EDITORUI_HANDLER
            {
                requireEditor();
                EditorContext& ctx = *m_editorCtx;
                auto& reg = m_scene->GetRegistry();
                const std::string mode = params.value("mode", std::string("set"));
                const size_t limit = static_cast<size_t>(std::clamp(params.value("limit", 200), 1, 2000));

                std::vector<entt::entity> picked;
                json notFound = json::array();
                std::vector<std::string> missNames;
                auto push = [&](entt::entity e)
                {
                    if (e == entt::null || !reg.valid(e)) return;
                    if (std::find(picked.begin(), picked.end(), e) == picked.end()) picked.push_back(e);
                };
                bool anySpecifier = false;
                bool truncated = false;

                if (params.contains("entities") && params["entities"].is_array())
                {
                    anySpecifier = true;
                    for (const json& v : params["entities"])
                    {
                        if (!v.is_number_integer()) { notFound.push_back(v); continue; }
                        const auto e = static_cast<entt::entity>(v.get<uint32_t>());
                        if (reg.valid(e)) push(e); else notFound.push_back(v);
                    }
                }
                if (params.contains("names") && params["names"].is_array())
                {
                    anySpecifier = true;
                    for (const json& v : params["names"])
                    {
                        if (!v.is_string()) { notFound.push_back(v); continue; }
                        auto ent = m_scene->FindEntity(v.get<std::string>());
                        if (ent.IsValid()) push(ent.GetHandle());
                        else { notFound.push_back(v); missNames.push_back(v.get<std::string>()); }
                    }
                }
                if (params.contains("guids") && params["guids"].is_array())
                {
                    anySpecifier = true;
                    for (const json& v : params["guids"])
                    {
                        const uint64_t g = v.is_string() ? ParseEntityGuidHex(v.get<std::string>()) : 0;
                        const entt::entity e = g ? FindEntityByGuid(reg, g) : entt::null;
                        if (e != entt::null) push(e); else notFound.push_back(v);
                    }
                }
                if (params.contains("query") && params["query"].is_string() && !params["query"].get<std::string>().empty())
                {
                    anySpecifier = true;
                    const std::string q = params["query"].get<std::string>();
                    size_t hits = 0;
                    for (auto [e, tag] : reg.view<const NameTag>().each())
                    {
                        if (reg.all_of<GridPlane>(e) || !WildMatch(q, tag.name)) continue;
                        if (hits >= limit) { truncated = true; break; }
                        ++hits;
                        push(e);
                    }
                    if (hits == 0) notFound.push_back(q);
                }
                if (params.contains("tag") && params["tag"].is_string() && !params["tag"].get<std::string>().empty())
                {
                    anySpecifier = true;
                    size_t hits = 0;
                    for (entt::entity e : m_scene->QueryByTag(params["tag"].get<std::string>()))
                    {
                        if (hits >= limit) { truncated = true; break; }
                        ++hits;
                        push(e);
                    }
                    if (hits == 0) notFound.push_back("tag:" + params["tag"].get<std::string>());
                }

                if (mode != "clear" && !anySpecifier)
                    throw McpError(McpErr::InvalidParam, "editor_select: specify entities / names / guids / query / tag, or mode:\"clear\"",
                                   "選ぶ対象を entities（id）/ names（名前）/ guids / query（名前の部分一致）/ tag で指定する。全解除は mode:\"clear\"");
                if (mode != "clear" && picked.empty())
                {
                    McpError err(McpErr::NotFound, "editor_select: no entity matched the specifiers",
                                 "list_entities で名前を確かめるか、query（部分一致 / ワイルドカード）で探す");
                    err.name = "E_NOT_FOUND_ENTITY";
                    err.cause = "指定に合うエンティティが 1 つも無い";
                    err.details = {{"notFound", notFound}};
                    if (!missNames.empty()) err.didYouMean = McpSuggestEntityNames(*m_scene, missNames.front());
                    err.fix.push_back(MakeMcpFix("list_entities", json::object(), "実在のエンティティ名 / id を確かめる"));
                    throw err;
                }

                const size_t before = ctx.selectedEntities.size();
                if (mode == "clear") ctx.ClearSelection();
                else if (mode == "set")
                {
                    ctx.ClearSelection();
                    for (entt::entity e : picked) ctx.AddToSelection(e);
                    ctx.selectedEntity = picked.front();   // 主選択は最初に指定したもの
                }
                else if (mode == "add") { for (entt::entity e : picked) ctx.AddToSelection(e); }
                else if (mode == "toggle") { for (entt::entity e : picked) ctx.ToggleSelection(e); }
                else // remove
                {
                    for (entt::entity e : picked) if (ctx.IsSelected(e)) ctx.ToggleSelection(e);
                }
                if (params.value("focus", false) && ctx.HasSelection()) ctx.pendingFocusSelection = true;

                json r = {{"mode", mode}, {"selection", edstate::SelectionJson(ctx, reg)}, {"matched", picked.size()},
                          {"notFound", std::move(notFound)}, {"previousCount", before}};
                if (truncated) r["truncated"] = true;
                resp["ok"] = true;
                resp["result"] = std::move(r);
            });
    }
}

// ---------------------------------------------------------------------------
// 遅延応答（毎フレーム）。表のコマンドが立てたフラグが消化され、UI が反応した後に返す。
// ---------------------------------------------------------------------------
void Application::ServiceMcpEditorUi()
{
    auto& waits = Waits();
    if (waits.empty() || !FrameFn() || !QuietFn()) return;
    const uint64_t frame = FrameFn()();
    const auto now = std::chrono::steady_clock::now();

    for (auto it = waits.begin(); it != waits.end();)
    {
        const uint64_t frames = frame >= it->startFrame ? frame - it->startFrame : 0;
        const double secs = std::chrono::duration<double>(now - it->startTime).count();
        const bool quiet = frames >= kMinFrames && QuietFn()();
        const bool timedOut = !quiet && (frames >= kMaxFrames || secs >= kMaxSeconds);
        if (!quiet && !timedOut) { ++it; continue; }

        const Snap after = SnapFn()();
        json r = {{"id", it->id}, {"executed", true}, {"changed", it->changed}, {"frames", frames},
                  {"effects", EffectsFn()(it->before, after, it->toastSeq0)}};
        if (!it->note.empty()) r["note"] = it->note;
        if (timedOut)
        {
            r["timedOut"] = true;
            r["note"] = "処理がまだ終わっていない（フレーム境界の要求が残っている）。editor_state で状況を確かめる";
        }
        CompleteMcp(m_mcpBridge.get(), it->reply, std::move(r));
        it = waits.erase(it);
    }
}

} // namespace dx12e
