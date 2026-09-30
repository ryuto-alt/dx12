#include "editor/EditorStateMcp.h"
#include "editor/EditorContext.h"
#include "editor/Toast.h"
#include "editor/ToolWindows.h"
#include "ecs/Components.h"
#include "gui/VirtualInputImGui.h"
#include "core/mcp/McpMeta.h"   // McpFnv1a64 / McpHex16（依存ゼロのヘッダ）

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // OpenPopupStack / DockContext（読むだけ）
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <functional>

namespace dx12e::edstate
{

using json = nlohmann::json;

namespace
{

std::string HexId(unsigned id)
{
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08x", id);
    return b;
}

// "タイトル##ID" / "タイトル###ID" → {タイトル, ID}
void SplitName(const std::string& name, std::string& title, std::string& idPart)
{
    const size_t p = name.find("##");
    if (p == std::string::npos) { title = name; idPart.clear(); return; }
    title = name.substr(0, p);
    size_t q = p;
    while (q < name.size() && name[q] == '#') ++q;
    idPart = name.substr(q);
}

const char* kEscHint = "Esc で閉じる（dx12_imgui {op:\"key\", key:\"Esc\"}）";
const char* kDismissHint = "editor_modal {action:\"dismiss\"}（dx12_call {name:\"editor_modal\", args:{action:\"dismiss\"}}）でキャンセルと同じに閉じる。ImGui のモーダルは Esc では閉じない";
const char* kButtonHint = "ボタンで選んで閉じる（dx12_imgui {op:\"find\"} → {op:\"pointer\"}。dx12_imgui {op:\"screenshot\"} で見て座標を決める）。ImGui のモーダルは Esc では閉じない";
const char* kUnsavedHint = "「保存」「破棄」「キャンセル」のどれかのボタンを押す（dx12_imgui {op:\"find\"} → {op:\"pointer\"}）。内容の判断が要るので editor_modal では閉じない";
const char* kRecoveryHint = "「復元」か「破棄」のボタンを押す（dx12_imgui {op:\"find\"} → {op:\"pointer\"}）。内容の判断が要るので editor_modal では閉じない";

// キャンセルしても副作用が無く、editor_modal {action:"dismiss"} で閉じてよいダイアログ。
struct KnownPopup { const char* suffix; const char* id; const char* hint; bool dismissSafe; };
const KnownPopup kKnown[] = {
    {"NewScenePopup",     "new_scene",         nullptr,        true},   // newSceneDialogIsCreate で save_as と出し分ける
    {"NewScriptPopup",    "new_script",        nullptr,        true},
    {"NewShaderPopup",    "new_shader",        nullptr,        true},
    {"UnsavedConfirm",    "unsaved_confirm",   kUnsavedHint,   false},
    {"AutosaveRecovery",  "autosave_recovery", kRecoveryHint,  false},
    {"ShortcutsPopup",    "shortcuts",         nullptr,        true},
    {"AboutPopup",        "about",             nullptr,        true},
    {"mgConfirm",         "matgraph_confirm",  kButtonHint,    false},   // マテリアルグラフ窓の状態機械が持つ。閉じても再び開くのでボタンで
    {"mgOpen",            "matgraph_open",     kButtonHint,    false},
    {"mgSaveAs",          "matgraph_save_as",  kButtonHint,    false},
};
const KnownPopup* FindKnown(const std::string& suffix)
{
    for (const KnownPopup& k : kKnown) if (suffix == k.suffix) return &k;
    return nullptr;
}

bool HasId(const std::vector<ModalEntry>& v, const std::string& id)
{
    return std::any_of(v.begin(), v.end(), [&](const ModalEntry& m) { return m.id == id; });
}

} // namespace

std::vector<ModalEntry> CollectModals(const EditorContext& ctx)
{
    std::vector<ModalEntry> out;

    if (ImGuiContext* g = ImGui::GetCurrentContext())
    {
        for (const ImGuiPopupData& pd : g->OpenPopupStack)
        {
            const ImGuiWindow* w = pd.Window;
            if (!w || !w->Name) continue;
            const bool modal = (w->Flags & ImGuiWindowFlags_Modal) != 0;
            ModalEntry m;
            const std::string name = w->Name;
            std::string title, idPart;
            SplitName(name, title, idPart);
            m.kind   = modal ? "imgui-modal" : "popup";
            m.source = "imgui";
            m.blocking = modal;
            m.title = title.empty() ? name : title;
            m.id = idPart.empty() ? m.title : idPart;
            m.canDismiss = false;
            m.dismissHint = kButtonHint;
            if (const KnownPopup* k = FindKnown(idPart))
            {
                m.id = k->id;
                if (idPart == "NewScenePopup" && !ctx.newSceneDialogIsCreate) m.id = "save_as";
                m.canDismiss = k->dismissSafe;
                m.dismissHint = k->dismissSafe ? kDismissHint : (k->hint ? k->hint : kButtonHint);
            }
            if (!modal) { m.canDismiss = true; m.dismissHint = "メニュー / コンボ等の非モーダルなポップアップ。Esc か外側のクリックで閉じる（入力は塞がない）"; }
            out.push_back(std::move(m));
        }
    }

    // ImGui のポップアップになる前の要求（1 フレームで消えるフラグ）と、ラッチされるダイアログ
    auto addEditor = [&](bool on, const char* id, const char* title, bool dismiss, const char* hint)
    {
        if (!on || HasId(out, id)) return;
        ModalEntry m;
        m.kind = "editor-dialog"; m.id = id; m.title = title; m.source = "editor";
        m.canDismiss = dismiss; m.dismissHint = hint ? hint : (dismiss ? kDismissHint : kButtonHint);
        out.push_back(std::move(m));
    };
    addEditor(ctx.showNewSceneDialog, ctx.newSceneDialogIsCreate ? "new_scene" : "save_as",
              ctx.newSceneDialogIsCreate ? "新規シーン" : "名前を付けて保存", true, nullptr);
    addEditor(ctx.showNewScriptDialog, "new_script", "新規スクリプト", true, nullptr);
    addEditor(ctx.showNewShaderDialog, "new_shader", "新規シェーダー", true, nullptr);
    addEditor(ctx.showUnsavedConfirm, "unsaved_confirm", "未保存の変更", false, kUnsavedHint);
    addEditor(ctx.showAutosaveRecovery, "autosave_recovery", "オートセーブからの復旧", false, kRecoveryHint);
    if (ctx.paletteOpen && !HasId(out, "command_palette"))
    {
        ModalEntry m;
        m.kind = "palette"; m.id = "command_palette"; m.title = "コマンドパレット / クイックオープン"; m.source = "editor";
        m.canDismiss = true; m.dismissHint = kEscHint;
        out.push_back(std::move(m));
    }
    return out;
}

bool IsBlocking(const std::vector<ModalEntry>& modals)
{
    return std::any_of(modals.begin(), modals.end(), [](const ModalEntry& m) { return m.blocking; });
}
bool IsBlocking(const EditorContext& ctx) { return IsBlocking(CollectModals(ctx)); }

json ModalJson(const EditorContext& ctx)
{
    const std::vector<ModalEntry> ms = CollectModals(ctx);
    json arr = json::array();
    bool popup = false;
    size_t blockingCount = 0;
    for (const ModalEntry& m : ms)
    {
        if (m.kind == "popup") popup = true;
        if (m.blocking) ++blockingCount;
        arr.push_back({{"kind", m.kind}, {"id", m.id}, {"title", m.title}, {"source", m.source},
                       {"blocking", m.blocking}, {"canDismiss", m.canDismiss}, {"dismissHint", m.dismissHint}});
    }
    const bool blocking = blockingCount > 0;
    json j = {{"blocking", blocking}, {"count", blockingCount}, {"modals", std::move(arr)}, {"popupOpen", popup}};
    j["note"] = blocking
        ? "モーダルが開いている間は dx12_editor_command の run が E_MODAL_OPEN で拒否される。閉じてから続ける"
        : "入力を塞いでいるモーダルは無い";
    return j;
}

DismissResult DismissTopModal(EditorContext& ctx)
{
    DismissResult r;
    // 1) ImGui のポップアップスタックの一番上の「モーダル」（スタックの深さ = level）
    int topLevel = -1;
    std::string topId, topTitle;
    bool topSafe = false;
    if (ImGuiContext* g = ImGui::GetCurrentContext())
    {
        for (int i = 0; i < g->OpenPopupStack.Size; ++i)
        {
            const ImGuiWindow* w = g->OpenPopupStack[i].Window;
            if (!w || !w->Name || !(w->Flags & ImGuiWindowFlags_Modal)) continue;
            std::string title, idPart;
            SplitName(w->Name, title, idPart);
            topLevel = i;
            topTitle = title.empty() ? std::string(w->Name) : title;
            topId = idPart;
            topSafe = false;
            if (const KnownPopup* k = FindKnown(idPart))
            {
                topId = (idPart == "NewScenePopup" && !ctx.newSceneDialogIsCreate) ? "save_as" : k->id;
                topSafe = k->dismissSafe;
            }
        }
    }
    if (topLevel >= 0)
    {
        r.id = topId; r.title = topTitle;
        if (!topSafe)
        {
            r.reason = "このダイアログは内容の判断（保存 / 破棄 / 復元 など）が要る、または窓が状態を持つので editor_modal では閉じない。ボタンで選ぶ（dx12_imgui {op:\"find\"} → {op:\"pointer\"}）";
            return r;
        }
        // level のポップアップとその上を閉じる（キャンセルボタンと同じ。ImGui::CloseCurrentPopup の外から呼べる形）
        ImGui::ClosePopupToLevel(topLevel, /*restore_focus_to_window_under_popup=*/true);
        r.ok = true;
        return r;
    }
    // 2) まだ ImGui のポップアップになっていない 1 フレームの要求
    auto clear = [&](bool& flag, const char* id, const char* title) { flag = false; r.ok = true; r.id = id; r.title = title; };
    if (ctx.showNewSceneDialog)   { clear(ctx.showNewSceneDialog, ctx.newSceneDialogIsCreate ? "new_scene" : "save_as", "新規シーン"); return r; }
    if (ctx.showNewScriptDialog)  { clear(ctx.showNewScriptDialog, "new_script", "新規スクリプト"); return r; }
    if (ctx.showNewShaderDialog)  { clear(ctx.showNewShaderDialog, "new_shader", "新規シェーダー"); return r; }
    // 3) それ以外（パレット / 未保存の確認 / 復旧）
    if (ctx.paletteOpen) { r.id = "command_palette"; r.title = "コマンドパレット"; r.reason = "コマンドパレットは Esc で閉じる（dx12_imgui {op:\"key\", key:\"Esc\"}）"; return r; }
    if (ctx.showUnsavedConfirm || ctx.showAutosaveRecovery)
    {
        r.id = ctx.showUnsavedConfirm ? "unsaved_confirm" : "autosave_recovery";
        r.reason = "内容の判断（保存 / 破棄 / 復元）が要るので editor_modal では閉じない。ボタンで選ぶ（dx12_imgui {op:\"find\"} → {op:\"pointer\"}）";
        return r;
    }
    r.reason = "閉じるモーダルが無い";
    return r;
}

json SelectionJson(const EditorContext& ctx, const entt::registry& reg)
{
    auto describe = [&](entt::entity e) -> json
    {
        json j = {{"entityId", static_cast<uint32_t>(e)}};
        if (const auto* n = reg.try_get<NameTag>(e)) j["name"] = n->name; else j["name"] = "";
        if (const auto* g = reg.try_get<EntityGuid>(e); g && g->value != 0) j["guid"] = FormatEntityGuidHex(g->value);
        return j;
    };
    json list = json::array();
    size_t count = 0;
    for (entt::entity e : ctx.selectedEntities)
    {
        if (!reg.valid(e)) continue;
        ++count;
        if (list.size() < 50) list.push_back(describe(e));
    }
    json j = {{"count", count}, {"entities", std::move(list)}};
    j["primary"] = (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)) ? describe(ctx.selectedEntity) : json(nullptr);
    if (count > 50) j["truncated"] = true;
    if (ctx.hoveredEntity != entt::null && reg.valid(ctx.hoveredEntity)) j["hovered"] = describe(ctx.hoveredEntity);
    return j;
}

json WindowsJson(const EditorContext& ctx)
{
    const std::vector<vinput_gui::WindowInfo> wins = vinput_gui::CollectWindows(false);
    float vx = 0.0f, vy = 0.0f;
    if (ImGui::GetCurrentContext()) { const ImGuiViewport* vp = ImGui::GetMainViewport(); vx = vp->Pos.x; vy = vp->Pos.y; }

    static const char* const kSlot[] = {"right_tab", "floating", "right_split", "bottom_tab"};
    json tools = json::array();
    json open = json::array();
    for (const tools::Desc& t : tools::kAll)
    {
        const bool isOpen = tools::IsOpen(ctx, t);
        json j = {{"id", t.id}, {"title", t.title}, {"open", isOpen},
                  {"slot", kSlot[static_cast<int>(t.slot) < 4 ? static_cast<int>(t.slot) : 0]},
                  {"menu", t.home == tools::MenuHome::View ? "view" : "tools"}, {"category", t.category}};
        const std::string imName = (t.imguiName && t.imguiName[0]) ? t.imguiName : t.title;
        j["imguiName"] = imName;
        for (const auto& w : wins)
        {
            if (w.name != imName && w.title != imName) continue;
            j["visible"] = w.visible;
            j["docked"] = w.docked;
            j["focused"] = w.focused;
            j["collapsed"] = w.collapsed;
            if (w.visible) j["rect"] = {{"x", w.x - vx}, {"y", w.y - vy}, {"w", w.w}, {"h", w.h}};
            break;
        }
        if (isOpen) open.push_back(t.id);
        tools.push_back(std::move(j));
    }
    const vinput_gui::HoverInfo hv = vinput_gui::QueryHoverInfo();
    const size_t openCount = open.size();
    return {{"open", std::move(open)}, {"openCount", openCount}, {"tools", std::move(tools)},
            {"focusedWindow", hv.focusedWindow}, {"hoveredWindow", hv.hoveredWindow}};
}

json LayoutJson(const EditorContext& ctx, float dpiScale)
{
    json j;
    j["workspace"] = ctx.currentWorkspace;
    j["savedLayouts"] = ctx.savedLayoutNames;
    j["bottomDockMaximized"] = ctx.bottomDockMaximized;
    j["dockRatios"] = {{"left", ctx.dockRatioLeft}, {"right", ctx.dockRatioRight}, {"bottom", ctx.dockRatioBottom},
                       {"rightSplit", ctx.dockRatioRightSplit}};
    json nodes = json::array();
    std::string hashSrc;
    if (ImGuiContext* g = ImGui::GetCurrentContext())
    {
        j["display"] = {{"width", g->IO.DisplaySize.x}, {"height", g->IO.DisplaySize.y}, {"dpiScale", dpiScale}};

        // ドックノード（このフレームに生きているものだけ）。id 昇順で決定的に並べる。
        std::vector<ImGuiDockNode*> alive;
        for (const ImGuiStoragePair& p : g->DockContext.Nodes.Data)
        {
            ImGuiDockNode* n = static_cast<ImGuiDockNode*>(p.val_p);
            if (!n) continue;
            if (n->LastFrameAlive < g->FrameCount - 2) continue;
            alive.push_back(n);
        }
        std::sort(alive.begin(), alive.end(), [](const ImGuiDockNode* a, const ImGuiDockNode* b) { return a->ID < b->ID; });

        auto tabsOf = [](const ImGuiDockNode* n)
        {
            std::vector<std::string> v;
            for (const ImGuiWindow* w : n->Windows) if (w && w->Name) v.emplace_back(w->Name);
            std::sort(v.begin(), v.end());
            return v;
        };
        // 構造ハッシュ用の正規形（ノード ID・サイズを含めない = リセット後に同じ配置なら同じ値）
        std::function<std::string(const ImGuiDockNode*)> canon = [&](const ImGuiDockNode* n) -> std::string
        {
            if (n->IsSplitNode())
            {
                const std::string a = n->ChildNodes[0] ? canon(n->ChildNodes[0]) : "";
                const std::string b = n->ChildNodes[1] ? canon(n->ChildNodes[1]) : "";
                return std::string(n->SplitAxis == ImGuiAxis_X ? "H(" : "V(") + a + "," + b + ")";
            }
            std::string s = "[";
            for (const std::string& t : tabsOf(n)) { s += t; s += ","; }
            return s + "]";
        };
        std::vector<std::string> roots;
        for (const ImGuiDockNode* n : alive)
        {
            json nj = {{"id", HexId(n->ID)}, {"parent", n->ParentNode ? HexId(n->ParentNode->ID) : std::string("0x00000000")},
                       {"axis", n->IsSplitNode() ? (n->SplitAxis == ImGuiAxis_X ? "h" : "v") : "none"},
                       {"size", {{"w", n->Size.x}, {"h", n->Size.y}}}};
            json tabs = json::array();
            if (n->IsLeafNode())
            {
                if (n->TabBar)
                {
                    for (int i = 0; i < n->TabBar->Tabs.Size; ++i)
                        for (const ImGuiWindow* w : n->Windows)
                            if (w && w->TabId == n->TabBar->Tabs[i].ID && w->Name) { tabs.push_back(std::string(w->Name)); break; }
                }
                else
                    for (const ImGuiWindow* w : n->Windows) if (w && w->Name) tabs.push_back(std::string(w->Name));
            }
            nj["tabs"] = std::move(tabs);
            if (n->IsCentralNode()) nj["central"] = true;
            nodes.push_back(std::move(nj));
            if (n->IsRootNode() && (n->IsDockSpace() || n->IsSplitNode() || !n->Windows.empty())) roots.push_back(canon(n));
        }
        std::sort(roots.begin(), roots.end());
        for (const std::string& r : roots) { hashSrc += r; hashSrc += ";"; }
    }
    j["dockNodes"] = std::move(nodes);
    j["structureHash"] = McpHex16(McpFnv1a64(hashSrc));
    return j;
}

json UndoJson(const EditorContext& ctx)
{
    const UndoSystem& u = ctx.undoSystem;
    json j = {{"canUndo", u.CanUndo()}, {"canRedo", u.CanRedo()},
              {"undoDepth", u.UndoDepth()}, {"redoDepth", u.RedoDepth()},
              {"recentUndo", u.RecentUndoNames(10)}, {"recentRedo", u.RecentRedoNames(10)},
              {"dirty", ctx.IsSceneDirty()}, {"editSeq", u.EditSeq()}, {"savedSeq", ctx.sceneSavedSeq}};
    if (const char* n = u.PeekUndoName()) { j["nextUndo"] = n; j["nextUndoIsAi"] = u.PeekUndoIsAi(); }
    if (const char* n = u.PeekRedoName()) j["nextRedo"] = n;
    return j;
}

json ToastsJson(size_t limit)
{
    json recent = json::array();
    size_t live = 0;
    for (const ui::ToastRecord& r : ui::ToastHistory(limit))
        recent.push_back({{"seq", r.seq}, {"kind", ui::ToastKindName(r.kind)}, {"text", r.text}, {"count", r.count},
                          {"ageSec", static_cast<double>(static_cast<long long>(r.ageSec * 10.0)) / 10.0}, {"live", r.live}});
    live = ui::ToastLiveCount();
    return {{"live", live}, {"recent", std::move(recent)}};
}

} // namespace dx12e::edstate
