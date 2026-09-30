#include "editor/EditorCommandMcp.h"
#include "editor/EditorContext.h"
#include "editor/FuzzyMatch.h"
#include "editor/ToolWindows.h"
#include "core/mcp/McpMeta.h"   // McpSuggest（依存ゼロのヘッダ）

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e::cmd::mcp
{

using json = nlohmann::json;

namespace
{

// ---------------------------------------------------------------------------
// 上書き表（id 指定の分類）。★表に載っていないコマンドは既定（通常・引数なし）で自動的に公開される。
//   ここに書くのは「危険」「OS ダイアログ」「モーダルを開く」「選択が要る」「トグル」「副作用の種類」だけ。
//   id が表から消えたら tests/mcp_editor_command_test.cpp が落とす（分類が黙って外れない）。
// ---------------------------------------------------------------------------
struct Override
{
    const char* id;
    const char* effect;        // "" = 既定（write_setting）
    bool        guarded;
    const char* reason;        // guarded の理由
    bool        osDialog;
    bool        opensModal;
    bool        needsSelection;
    bool        toggle;
};

const Override kOverrides[] = {
    // ---- ファイル ----
    {"file.new",          "write_scene",  false, "",  false, true,  false, false},   // 名前入力ダイアログ（アプリ内）を開く。確定すると新規シーン
    {"file.open",         "write_scene",  true,  "OS のファイル選択ダイアログを開く（人の画面に出る）。AI からは dx12_open_scene {path} を使う", true, false, false, false},
    {"file.save",         "write_file",   true,  "シーンのファイルを上書き保存する。AI の編集は自動保存と journal で守られているので、通常は dx12_save_scene で足りる", false, false, false, false},
    {"file.saveAs",       "write_file",   true,  "名前を付けてシーンのファイルを書く（アプリ内のダイアログが開く）", false, true, false, false},
    {"file.newScript",    "write_file",   false, "",  false, true,  false, false},
    {"file.newShader",    "write_file",   false, "",  false, true,  false, false},
    {"file.closeProject", "runtime",      true,  "プロジェクトを閉じてランチャーへ戻る（このエディタの作業状態を手放す。MCP の対象シーンも無くなる）", false, false, false, false},
    // ---- 編集 ----
    {"edit.undo",         "write_scene",  false, "",  false, false, false, false},
    {"edit.redo",         "write_scene",  false, "",  false, false, false, false},
    {"edit.copy",         "write_setting", false, "", false, false, true,  false},
    {"edit.paste",        "write_scene",  false, "",  false, false, false, false},
    {"edit.duplicate",    "write_scene",  false, "",  false, false, true,  false},
    {"edit.delete",       "write_scene",  true,  "選択中のエンティティ（と子）を丸ごと削除する。呼び出し側から何が消えるか見えないため確認が要る（名前を指定する dx12_delete_entity は確認不要）", false, false, true, false},
    {"edit.rename",       "write_setting", false, "", false, false, true,  false},   // ヒエラルキーのインライン編集を開くだけ
    {"edit.group",        "write_scene",  false, "",  false, false, true,  false},
    {"edit.selectNone",   "write_setting", false, "", false, false, false, false},
    {"edit.focus",        "write_setting", false, "", false, false, true,  false},
    {"edit.toggleHidden", "write_scene",  false, "",  false, false, true,  false},
    {"edit.isolate",      "write_scene",  false, "",  false, false, true,  false},
    {"edit.showAll",      "write_scene",  false, "",  false, false, false, false},
    {"edit.toggleLocked", "write_scene",  false, "",  false, false, true,  false},
    {"edit.newFolder",    "write_scene",  false, "",  false, false, false, false},
    // ---- 表示 ----
    {"view.fill",         "",             false, "",  false, false, false, true},
    {"view.flyMode",      "",             false, "",  false, false, false, true},
    {"view.toggle2D",     "",             false, "",  false, false, false, true},
    {"view.viewportBar",  "",             false, "",  false, false, false, true},
    {"view.outline",      "",             false, "",  false, false, false, true},
    {"layout.bottomMaximize", "",         false, "",  false, false, false, true},
    {"layout.save",       "",             false, "",  false, true,  false, false},
    {"layout.slots",      "",             false, "",  false, true,  false, false},
    // ---- 再生 ----
    {"play.toggle",       "runtime",      false, "",  false, false, false, false},
    {"play.stop",         "runtime",      false, "",  false, false, false, false},
    {"play.pause",        "runtime",      false, "",  false, false, false, false},
    // ---- コマンド ----
    {"palette.commands",  "",             false, "",  false, true,  false, false},
    {"palette.quickOpen", "",             false, "",  false, true,  false, false},
    // ---- マテリアルグラフ ----
    {"matgraph.save",     "write_file",   true,  "マテリアルグラフのファイル(.dxmg)を上書き保存する", false, false, false, false},
    {"matgraph.saveAs",   "write_file",   true,  "名前を付けてマテリアルグラフを書く（アプリ内のダイアログが開く）", false, true, false, false},
    {"matgraph.open",     "",             false, "",  false, true,  false, false},
};

const Override* FindOverride(std::string_view id)
{
    for (const Override& o : kOverrides)
        if (id == o.id) return &o;
    return nullptr;
}

bool StartsWith(std::string_view s, std::string_view p) { return s.rfind(p, 0) == 0; }

const CreateItem* FindCreate(std::string_view id)
{
    for (const CreateItem& c : kCreateItems)
        if (id == c.id) return &c;
    return nullptr;
}

} // namespace

std::vector<std::string> OverrideIds()
{
    std::vector<std::string> v;
    for (const Override& o : kOverrides) v.push_back(o.id);
    return v;
}

std::string KindOf(std::string_view id)
{
    if (StartsWith(id, "window."))
        return tools::Find(std::string(id.substr(7)).c_str()) ? "window" : "";
    if (StartsWith(id, "create."))
        return FindCreate(id) ? "create" : "";
    return FindCommand(id) ? "command" : "";
}

Traits TraitsFor(std::string_view id)
{
    Traits t;
    t.kind = KindOf(id);
    if (t.kind.empty()) t.kind = "command";

    if (t.kind == "window")
    {
        t.effect = "write_setting";
        t.windowState = true;
        t.args.push_back({"state", "enum", false, "open（既定）/ close / toggle。表の実行はトグルなので、既に目的の状態なら何もしない",
                          {"open", "close", "toggle"}});
        return t;
    }
    if (t.kind == "create")
    {
        const CreateItem* c = FindCreate(id);
        t.effect = "write_scene";
        if (c && (!c->marker || !c->marker[0]))
        {
            t.effect = "write_setting";   // 地形 / スカルプト: 専用の作成窓が開くだけ
            return t;
        }
        if (c && !IsUiMarker(c->marker))
            t.args.push_back({"position", "vec3", false, "置く位置 [x,y,z]。省略はカメラの前（下を向いていれば床との交点）", {}});
        t.args.push_back({"name", "string", false, "エンティティ名（省略は既定名）", {}});
        return t;
    }

    // ---- 表のコマンド ----
    if (StartsWith(id, "play."))                                   t.effect = "runtime";
    if (StartsWith(id, "build.") || StartsWith(id, "git.") || StartsWith(id, "shell.") || StartsWith(id, "external."))
    {
        t.guarded = true;
        t.guardReason = "ビルド / 外部プロセス / リポジトリ操作など、エディタの外へ影響が及ぶコマンド";
        t.effect = "runtime";
    }
    if (const Override* o = FindOverride(id))
    {
        if (o->effect && o->effect[0]) t.effect = o->effect;
        t.guarded = t.guarded || o->guarded;
        if (o->reason && o->reason[0]) t.guardReason = o->reason;
        t.osDialog = o->osDialog;
        t.opensModal = o->opensModal;
        t.needsSelection = o->needsSelection;
        t.toggleState = o->toggle;
    }
    if (t.toggleState)
        t.args.push_back({"state", "enum", false, "on / off / toggle（既定 toggle）", {"on", "off", "toggle"}});
    return t;
}

// ---------------------------------------------------------------------------
// 実行できない理由
// ---------------------------------------------------------------------------
std::string DisabledReason(const EditorContext& ctx, std::string_view id)
{
    if (IsEnabled(ctx, id)) return {};
    if (StartsWith(id, "graph."))
        return "ノードグラフの窓（ノードグラフ サンドボックス / マテリアルグラフ）が開いていない。window.materialGraph で開く";
    if (StartsWith(id, "matgraph."))
        return "マテリアルグラフの窓が開いていない。先に window.materialGraph を実行する";
    if (StartsWith(id, "create."))
        return "Play 中はエンティティを作れない（play.stop で Editor に戻す）";
    const Def* d = FindCommand(id);
    if (!d) return "表に無いコマンド";
    if (d->scope == Scope::Editor && ctx.isPlaying)
        return "Play 中は実行できない（シーンを触る Editor 専用コマンド。play.stop で止めてから）";
    if (id == "edit.undo")  return "元に戻す履歴が無い";
    if (id == "edit.redo")  return "やり直す履歴が無い";
    if (id == "edit.paste") return "クリップボードが空（先に edit.copy）";
    if (id == "edit.copy" || id == "edit.duplicate" || id == "edit.delete" || id == "edit.rename" || id == "edit.group"
        || id == "edit.focus" || id == "edit.toggleHidden" || id == "edit.isolate" || id == "edit.toggleLocked")
        return "エンティティが選択されていない（dx12_editor_select で選ぶ）";
    if (id == "view.gizmoMove" || id == "view.gizmoRotate" || id == "view.gizmoScale" || id == "view.gizmoSpace")
        return ctx.flyMode ? "キーボードフライ中はギズモを切り替えられない（view.flyMode で解除）"
                           : "2D ビュー中はギズモを切り替えられない（view.toggle2D で 3D に戻す）";
    if (id == "play.stop" || id == "play.pause") return "Play 中ではない（play.toggle で開始）";
    return "現在の状態では実行できない";
}

namespace
{
std::string BlockedNow(const Traits& t, const RunEnv& env, std::string_view id)
{
    if (t.osDialog && env.virtualInput)
        return "仮想入力（--background）中は OS のダイアログを出せない。dx12_open_scene {path} を使う";
    if (env.aiTxOpen && (id == "edit.undo" || id == "edit.redo"))
        return "AI のトランザクションが開いている。transaction_commit / transaction_rollback で閉じてから実行する";
    return {};
}

const char* ScopeName(Scope s) { return s == Scope::Always ? "always" : "editor"; }
const char* KeyModeName(KeyMode m)
{
    switch (m)
    {
    case KeyMode::Global:   return "global";
    case KeyMode::Typing:   return "typing";
    case KeyMode::Panel:    return "panel";
    case KeyMode::External: return "external";
    }
    return "global";
}

void FillState(const EditorContext& ctx, const RunEnv& env, Row& r)
{
    r.traits = TraitsFor(r.id);
    r.enabled = IsEnabled(ctx, r.id);
    if (!r.enabled) r.disabledReason = DisabledReason(ctx, r.id);
    r.blockedNow = BlockedNow(r.traits, env, r.id);
    if (r.kind == "window")
    {
        if (const tools::Desc* d = tools::Find(r.id.substr(7).c_str()))
        {
            r.hasOpen = true;
            r.open = tools::IsOpen(ctx, *d);
        }
    }
    else if (r.traits.toggleState)
    {
        r.hasChecked = true;
        r.checked = IsChecked(ctx, r.id);
    }
    else if (r.kind == "command")
    {
        // トグルではないが checked が意味を持つもの（ワークスペース / テーマ）
        if (StartsWith(r.id, "workspace.") || StartsWith(r.id, "view.theme."))
        {
            r.hasChecked = true;
            r.checked = IsChecked(ctx, r.id);
        }
    }
}
} // namespace

std::vector<Row> BuildRows(const EditorContext& ctx, const RunEnv& env)
{
    std::vector<Row> rows;
    rows.reserve(kCommandCount + tools::kCount + kCreateItemCount);
    for (const Def& d : kCommands)
    {
        Row r;
        r.id = d.id; r.label = d.label; r.labelEn = d.labelEn ? d.labelEn : "";
        r.kind = "command"; r.category = d.category;
        r.chord = d.chord ? d.chord : ""; r.chord2 = d.chord2 ? d.chord2 : "";
        r.help = (d.help && d.help[0]) ? d.help : d.label;
        r.scope = ScopeName(d.scope); r.keyMode = KeyModeName(d.mode);
        FillState(ctx, env, r);
        rows.push_back(std::move(r));
    }
    for (const tools::Desc& t : tools::kAll)
    {
        Row r;
        r.id = std::string("window.") + t.id;
        r.label = t.title;
        r.labelEn = std::string("window ") + (t.keywords ? t.keywords : "");
        r.kind = "window"; r.category = "ウィンドウ";
        r.help = std::string("ツール窓「") + t.title + "」を開く / 閉じる（" + t.category + "）";
        r.scope = "always"; r.keyMode = "global";
        FillState(ctx, env, r);
        rows.push_back(std::move(r));
    }
    for (const CreateItem& c : kCreateItems)
    {
        Row r;
        r.id = c.id;
        r.label = std::string("作成: ") + c.label;
        r.labelEn = std::string("create add spawn ") + c.labelEn;
        r.kind = "create"; r.category = "作成";
        r.help = std::string("エンティティ「") + c.label + "」を作る（" + c.group + "）";
        r.scope = "editor"; r.keyMode = "global";
        FillState(ctx, env, r);
        rows.push_back(std::move(r));
    }
    return rows;
}

Row BuildRow(const EditorContext& ctx, const RunEnv& env, std::string_view id)
{
    for (Row& r : BuildRows(ctx, env))
        if (r.id == id) return std::move(r);
    return Row{};
}

json RowToJson(const Row& r, bool detail)
{
    json j = {{"id", r.id}, {"label", r.label}, {"category", r.category}, {"kind", r.kind}, {"enabled", r.enabled}};
    if (!r.chord.empty()) j["chord"] = r.chord;
    if (!r.enabled) j["disabledReason"] = r.disabledReason;
    if (r.traits.guarded)    j["guarded"] = true;
    if (r.traits.osDialog)   j["osDialog"] = true;
    if (r.traits.opensModal) j["opensModal"] = true;
    if (!r.blockedNow.empty()) j["blockedNow"] = r.blockedNow;
    if (!r.traits.args.empty()) j["hasArgs"] = true;
    if (!detail) return j;

    j["labelEn"] = r.labelEn;
    j["help"] = r.help;
    j["scope"] = r.scope;
    j["keyMode"] = r.keyMode;
    if (!r.chord2.empty()) j["chord2"] = r.chord2;
    if (r.hasChecked) j["checked"] = r.checked;
    if (r.hasOpen) j["open"] = r.open;
    j["effect"] = r.traits.effect;
    if (r.traits.guarded) j["guardReason"] = r.traits.guardReason;
    if (r.traits.needsSelection) j["needsSelection"] = true;
    if (!r.traits.args.empty())
    {
        json args = json::array();
        for (const ArgSpec& a : r.traits.args)
        {
            json aj = {{"name", a.name}, {"type", a.type}, {"required", a.required}, {"desc", a.desc}};
            if (!a.enumValues.empty()) aj["enum"] = a.enumValues;
            args.push_back(std::move(aj));
        }
        j["args"] = std::move(args);
    }
    json ex = {{"id", r.id}};
    if (r.kind == "window") ex["args"] = {{"state", "open"}};
    else if (r.kind == "create" && !r.traits.args.empty())
    {
        json a = json::object();
        for (const ArgSpec& s : r.traits.args)
        {
            if (s.name == "position") a["position"] = {0.0, 0.5, 0.0};
            else if (s.name == "name") a["name"] = "MyObject";
        }
        ex["args"] = a;
    }
    else if (r.traits.toggleState) ex["args"] = {{"state", "on"}};
    j["example"] = std::move(ex);
    return j;
}

// ---------------------------------------------------------------------------
// フィルタ / 提案
// ---------------------------------------------------------------------------
namespace
{
std::string LowerAscii(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
}

std::vector<Row> Filter(std::vector<Row> rows, const ListFilter& f)
{
    std::vector<std::pair<int, Row>> scored;
    const std::string cat = LowerAscii(f.category);
    for (Row& r : rows)
    {
        if (!f.id.empty() && r.id != f.id) continue;
        if (!f.kind.empty() && r.kind != f.kind) continue;
        if (!cat.empty() && LowerAscii(r.category) != cat) continue;
        if (f.enabledOnly && (!r.enabled || !r.blockedNow.empty())) continue;
        if (f.guardedOnly && !r.traits.guarded) continue;
        int score = 0;
        if (!f.query.empty())
        {
            int best = fuzzy::Score(f.query, r.id);
            const int sl = fuzzy::Score(f.query, r.label);
            if (sl >= 0) best = (std::max)(best, sl + 30);
            best = (std::max)(best, fuzzy::Score(f.query, r.labelEn));
            const int sc = fuzzy::Score(f.query, r.category);
            if (sc >= 0) best = (std::max)(best, sc - 40);
            if (best < 0) continue;
            score = best;
        }
        scored.emplace_back(score, std::move(r));
    }
    if (!f.query.empty())
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Row> out;
    out.reserve(scored.size());
    for (auto& p : scored) out.push_back(std::move(p.second));
    return out;
}

std::vector<std::string> SuggestIds(const std::vector<Row>& rows, std::string_view id, size_t n)
{
    std::vector<std::string> cands;
    cands.reserve(rows.size());
    for (const Row& r : rows) cands.push_back(r.id);
    std::vector<std::string> out = McpSuggest(std::string(id), cands, n);
    if (out.size() < n)
    {
        // 「window.postprocess」「postProcess」のように接頭辞や大小が違うだけの入力は、部分一致でも拾う
        ListFilter lf;
        lf.query = std::string(id);
        for (const Row& r : Filter(rows, lf))
        {
            if (out.size() >= n) break;
            if (std::find(out.begin(), out.end(), r.id) == out.end() && r.id != id) out.push_back(r.id);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 引数
// ---------------------------------------------------------------------------
bool ParseArgs(std::string_view id, const json& args, RunArgs& out, std::string& error)
{
    const Traits t = TraitsFor(id);
    if (args.is_null() || (args.is_object() && args.empty())) return true;
    if (!args.is_object()) { error = "args はオブジェクトで渡す"; return false; }

    auto accepted = [&]() {
        std::string s;
        for (const ArgSpec& a : t.args) { if (!s.empty()) s += ", "; s += a.name; }
        return s.empty() ? std::string("（このコマンドは引数を取らない）") : s;
    };
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        const std::string& k = it.key();
        const ArgSpec* spec = nullptr;
        for (const ArgSpec& a : t.args) if (a.name == k) spec = &a;
        if (!spec) { error = "未知の引数 '" + k + "'。受け付ける引数: " + accepted(); return false; }
        const json& v = it.value();
        if (spec->type == "enum")
        {
            if (!v.is_string() || std::find(spec->enumValues.begin(), spec->enumValues.end(), v.get<std::string>()) == spec->enumValues.end())
            {
                std::string vals;
                for (const auto& e : spec->enumValues) { if (!vals.empty()) vals += " / "; vals += e; }
                error = "引数 '" + k + "' は " + vals + " のどれか";
                return false;
            }
            out.state = v.get<std::string>();
        }
        else if (spec->type == "string")
        {
            if (!v.is_string() || v.get<std::string>().empty() || v.get<std::string>().size() > 128)
            {
                error = "引数 '" + k + "' は 1〜128 バイトの文字列";
                return false;
            }
            out.name = v.get<std::string>();
        }
        else if (spec->type == "vec3")
        {
            if (!v.is_array() || v.size() != 3 || !std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); }))
            {
                error = "引数 '" + k + "' は数値 3 つの配列 [x,y,z]";
                return false;
            }
            out.hasPosition = true;
            for (size_t i = 0; i < 3; ++i)
            {
                out.position[i] = v[i].get<float>();
                if (!std::isfinite(out.position[i])) { error = "引数 '" + k + "' に有限でない数がある"; return false; }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 実行
// ---------------------------------------------------------------------------
RunOutcome Run(EditorContext& ctx, const Env& env, std::string_view id, const RunArgs& args)
{
    RunOutcome o;
    const Traits t = TraitsFor(id);

    if (t.windowState)
    {
        const tools::Desc* d = tools::Find(std::string(id.substr(7)).c_str());
        if (!d) return o;
        const bool cur = tools::IsOpen(ctx, *d);
        const std::string st = args.state.empty() ? "open" : args.state;
        const bool want = st == "open" ? true : st == "close" ? false : !cur;
        if (want == cur)
        {
            o.executed = true; o.changed = false;
            o.note = std::string("ツール窓は既に") + (cur ? "開いている" : "閉じている");
            return o;
        }
        o.executed = Execute(ctx, env, id);
        return o;
    }
    if (t.toggleState)
    {
        const bool cur = IsChecked(ctx, id);
        const std::string st = args.state.empty() ? "toggle" : args.state;
        const bool want = st == "on" ? true : st == "off" ? false : !cur;
        if (want == cur)
        {
            o.executed = true; o.changed = false;
            o.note = std::string("既に") + (cur ? "ON" : "OFF");
            return o;
        }
        o.executed = Execute(ctx, env, id);
        return o;
    }
    if (t.kind == "create")
    {
        const size_t before = ctx.pendingSpawns.size();
        o.executed = Execute(ctx, env, id);
        if (o.executed && ctx.pendingSpawns.size() > before)
        {
            PendingSpawnRequest& r = ctx.pendingSpawns.back();
            if (args.hasPosition && !IsUiMarker(r.modelPath.c_str()))
                r.position = {args.position[0], args.position[1], args.position[2]};
            if (!args.name.empty()) r.name = args.name;
        }
        return o;
    }
    o.executed = Execute(ctx, env, id);
    return o;
}

} // namespace dx12e::cmd::mcp
