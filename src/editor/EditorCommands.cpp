#include "editor/EditorCommands.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/EditorIcons.h"
#include "editor/ToolWindows.h"
#include "editor/UiWidgets.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // GetTopMostAndVisiblePopupModal
#pragma warning(pop)

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace dx12e::cmd
{

namespace
{

// ---------------- キー名 → ImGuiKey ----------------
ImGuiKey KeyFromName(const std::string& n)
{
    if (n.size() == 1)
    {
        const char c = n[0];
        if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
        if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
    }
    if (n.size() >= 2 && n[0] == 'F')
    {
        const int k = std::atoi(n.c_str() + 1);
        if (k >= 1 && k <= 12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (k - 1));
    }
    if (n == "ESC")   return ImGuiKey_Escape;
    if (n == "DEL")   return ImGuiKey_Delete;
    if (n == "ENTER") return ImGuiKey_Enter;
    if (n == "TAB")   return ImGuiKey_Tab;
    if (n == "SPACE") return ImGuiKey_Space;
    if (n == "GRAVE") return ImGuiKey_GraveAccent;
    if (n == "UP")    return ImGuiKey_UpArrow;
    if (n == "DOWN")  return ImGuiKey_DownArrow;
    if (n == "LEFT")  return ImGuiKey_LeftArrow;
    if (n == "RIGHT") return ImGuiKey_RightArrow;
    return ImGuiKey_None;
}

struct BoundChord { bool ctrl = false, shift = false, alt = false; ImGuiKey key = ImGuiKey_None; };
struct Bound { const Def* def = nullptr; BoundChord c[2]; int n = 0; };

const std::vector<Bound>& Bindings()
{
    static const std::vector<Bound> s = []
    {
        std::vector<Bound> v;
        for (const Def& d : kCommands)
        {
            Bound b;
            b.def = &d;
            for (const char* text : {d.chord, d.chord2})
            {
                if (!text || !text[0]) continue;
                const ChordSpec spec = ParseChordSpec(text);
                if (!spec.valid) continue;
                const ImGuiKey k = KeyFromName(spec.key);
                if (k == ImGuiKey_None) continue;
                BoundChord& c = b.c[b.n++];
                c.ctrl = spec.ctrl; c.shift = spec.shift; c.alt = spec.alt; c.key = k;
            }
            if (b.n > 0) v.push_back(b);
        }
        return v;
    }();
    return s;
}

bool ChordPressed(const BoundChord& c)
{
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl == c.ctrl && io.KeyShift == c.shift && io.KeyAlt == c.alt
        && ImGui::IsKeyPressed(c.key, /*repeat=*/false);
}

void OpenNewSceneDialog(EditorContext& ctx, bool create, const char* defaultName)
{
    ctx.showNewSceneDialog = true;
    ctx.newSceneDialogIsCreate = create;
    std::memset(ctx.newSceneNameBuf, 0, sizeof(ctx.newSceneNameBuf));
    strncpy_s(ctx.newSceneNameBuf, defaultName, _TRUNCATE);
}

// 現在のシーン名（拡張子なし）。無ければ既定名。
std::string CurrentSceneStem(const EditorContext& ctx, const char* fallback)
{
    if (ctx.currentScenePath.empty()) return fallback;
    const std::string stem = std::filesystem::path(ctx.currentScenePath).stem().string();
    return stem.empty() ? std::string(fallback) : stem;
}

} // namespace

// ---------------- 公開: 表の引き当て ----------------

const char* IconFor(std::string_view id)
{
    struct P { const char* id; const char* icon; };
    static const P kIcons[] = {
        {"file.new", ICON_FILE_PLUS}, {"file.open", ICON_FOLDER_OPEN}, {"file.save", ICON_SAVE},
        {"file.saveAs", ICON_SAVE}, {"file.newScript", ICON_FILE_CODE}, {"file.newShader", ICON_T_SHADER},
        {"file.closeProject", ICON_POWER},
        {"edit.undo", ICON_UNDO}, {"edit.redo", ICON_REDO}, {"edit.copy", ICON_COPY}, {"edit.paste", ICON_PASTE},
        {"edit.duplicate", ICON_T_LAYERS}, {"edit.delete", ICON_TRASH}, {"edit.rename", ICON_FILE_TEXT},
        {"edit.group", ICON_T_GROUP}, {"edit.selectNone", ICON_POINTER}, {"edit.focus", ICON_EYE},
        {"view.gizmoMove", ICON_MOVE}, {"view.gizmoRotate", ICON_ROTATE}, {"view.gizmoScale", ICON_SCALE},
        {"view.gizmoSpace", ICON_SPACE_WORLD}, {"view.fill", ICON_T_SUN}, {"view.flyMode", ICON_KEYBOARD},
        {"view.toggle2D", ICON_VIEW_2D}, {"view.resetLayout", ICON_REFRESH}, {"view.closeTools", ICON_CLOSE},
        {"view.fullscreen", ICON_T_MONITOR},
        {"view.theme.default", ICON_T_SUN}, {"view.theme.a", ICON_T_SUN}, {"view.theme.b", ICON_T_SUN}, {"view.theme.c", ICON_T_SUN},
        {"play.toggle", ICON_PLAY}, {"play.stop", ICON_STOP}, {"play.pause", ICON_PAUSE},
        {"palette.commands", ICON_SEARCH}, {"palette.quickOpen", ICON_SEARCH},
    };
    for (const P& p : kIcons)
        if (id == p.id) return p.icon;
    if (id.rfind("window.", 0) == 0)
    {
        const std::string tid(id.substr(7));
        if (const tools::Desc* d = tools::Find(tid.c_str())) return d->icon;
    }
    if (id.rfind("create.", 0) == 0) return ICON_PLUS;
    return ICON_BLANK;
}

const char* ShortcutText(std::string_view id)
{
    // 表示文字列は毎回組み立てるので id ごとに静的にキャッシュする（メニューは毎フレーム呼ぶ）。
    static std::vector<std::pair<const Def*, std::string>> cache;
    const Def* d = FindCommand(id);
    if (!d || ((!d->chord || !d->chord[0]) && (!d->chord2 || !d->chord2[0]))) return nullptr;
    for (auto& p : cache)
        if (p.first == d) return p.second.c_str();
    cache.emplace_back(d, ChordLabel(*d));
    return cache.back().second.c_str();
}

bool IsChecked(const EditorContext& ctx, std::string_view id)
{
    if (id == "view.fill")    return ctx.viewportFill > 0.0f;
    if (id == "view.flyMode") return ctx.flyMode;
    if (id == "view.toggle2D") return ctx.view2D;
    if (id == "view.theme.default") return theme::CurrentVariant() == theme::Variant::Default;
    if (id == "view.theme.a") return theme::CurrentVariant() == theme::Variant::A;
    if (id == "view.theme.b") return theme::CurrentVariant() == theme::Variant::B;
    if (id == "view.theme.c") return theme::CurrentVariant() == theme::Variant::C;
    return false;
}

bool IsEnabled(const EditorContext& ctx, std::string_view id)
{
    if (id.rfind("window.", 0) == 0 || id.rfind("create.", 0) == 0)
        return id.rfind("create.", 0) == 0 ? !ctx.isPlaying : true;

    const Def* d = FindCommand(id);
    if (!d) return false;
    if (d->scope == Scope::Editor && ctx.isPlaying) return false;

    if (id == "edit.undo")      return ctx.undoSystem.CanUndo();
    if (id == "edit.redo")      return ctx.undoSystem.CanRedo();
    if (id == "edit.paste")     return !ctx.clipboard.empty();
    if (id == "edit.copy" || id == "edit.duplicate" || id == "edit.delete" || id == "edit.rename"
        || id == "edit.group" || id == "edit.focus")
        return ctx.HasSelection();
    if (id == "view.gizmoMove" || id == "view.gizmoRotate" || id == "view.gizmoScale" || id == "view.gizmoSpace")
        return !ctx.flyMode && !ctx.view2D;   // 2D / フライ中は W/E/R/T をカメラ移動に使う
    if (id == "play.stop" || id == "play.pause") return ctx.isPlaying;
    return true;
}

// ---------------- 公開: 実行 ----------------

bool Execute(EditorContext& ctx, const Env& env, std::string_view id)
{
    // ウィンドウの開閉
    if (id.rfind("window.", 0) == 0)
    {
        const std::string tid(id.substr(7));
        if (const tools::Desc* d = tools::Find(tid.c_str()))
        {
            ctx.*(d->flag) = !(ctx.*(d->flag));
            return true;
        }
        return false;
    }
    // エンティティ作成
    if (id.rfind("create.", 0) == 0)
    {
        for (const CreateItem& it : kCreateItems)
            if (id == it.id) { SpawnItem(ctx, it); return true; }
        return false;
    }

    if (!FindCommand(id)) return false;
    if (!IsEnabled(ctx, id)) return false;

    // ---- ファイル ----
    if (id == "file.new")        { OpenNewSceneDialog(ctx, true, "NewScene"); return true; }
    if (id == "file.open")       { ctx.pendingOpenSceneDialog = true; return true; }
    if (id == "file.save")       { ctx.pendingSaveScene = true; return true; }
    if (id == "file.saveAs")
    {
        // アプリ内の名前入力ダイアログ（assets/scenes/<名前>.json）。OS のダイアログは仮想入力中に出せず、
        // Ctrl+S（保存先未設定）と同じ入口にそろえる。
        OpenNewSceneDialog(ctx, false, CurrentSceneStem(ctx, "Untitled").c_str());
        return true;
    }
    if (id == "file.newScript")
    {
        ctx.showNewScriptDialog = true;
        std::memset(ctx.newScriptNameBuf, 0, sizeof(ctx.newScriptNameBuf));
        strncpy_s(ctx.newScriptNameBuf, "NewScript", _TRUNCATE);
        return true;
    }
    if (id == "file.newShader")
    {
        ctx.showNewShaderDialog = true;
        std::memset(ctx.newShaderNameBuf, 0, sizeof(ctx.newShaderNameBuf));
        strncpy_s(ctx.newShaderNameBuf, "NewShader", _TRUNCATE);
        return true;
    }
    if (id == "file.closeProject") { ctx.pendingCloseProject = true; return true; }

    // ---- 編集 ----
    if (id == "edit.undo")   { ctx.pendingUndo = true; return true; }
    if (id == "edit.redo")   { ctx.pendingRedo = true; return true; }
    if (id == "edit.copy")
    {
        if (!env.scene) return false;
        ctx.clipboard.clear();
        for (auto e : SceneSerializer::TopmostRoots(*env.scene, ctx.selectedEntities))
        {
            std::string snap = SceneSerializer::SerializeSubtree(*env.scene, e, env.assetsDir);
            if (!snap.empty()) ctx.clipboard.push_back(std::move(snap));
        }
        if (!ctx.clipboard.empty()) ctx.Notify(ui::ToastKind::Info, "コピーしました");
        return true;
    }
    if (id == "edit.paste")  { ctx.pendingPastes = ctx.clipboard; return true; }
    if (id == "edit.duplicate")
    {
        for (auto e : ctx.selectedEntities) ctx.pendingDuplications.push_back(e);
        return true;
    }
    if (id == "edit.delete")
    {
        for (auto e : ctx.selectedEntities) ctx.pendingDeletions.push_back(e);
        ctx.ClearSelection();
        return true;
    }
    if (id == "edit.rename")
    {
        ctx.requestRenameEntity = ctx.selectedEntity;   // ヒエラルキーがインライン編集を開く
        return true;
    }
    if (id == "edit.group")  { ctx.pendingGroupSelection = true; return true; }
    if (id == "edit.selectNone")
    {
        // 優先順: キーボードフライ中 → 解除 / Play 中 → 一時停止中だけ停止（ゲームが Esc を使うので
        // 通常の Play 中は素通し）/ Editor → 選択解除。
        if (ctx.flyMode)   { ctx.flyMode = false; return true; }
        if (ctx.isPlaying) { if (ctx.paused) ctx.pendingPlayRequest = 2; return true; }
        ctx.ClearSelection();
        return true;
    }
    if (id == "edit.focus")  { ctx.pendingFocusSelection = true; return true; }

    // ---- 表示 ----
    if (id == "view.gizmoMove")   { ctx.gizmoMode = GizmoMode::Translate; return true; }
    if (id == "view.gizmoRotate") { ctx.gizmoMode = GizmoMode::Rotate;    return true; }
    if (id == "view.gizmoScale")  { ctx.gizmoMode = GizmoMode::Scale;     return true; }
    if (id == "view.gizmoSpace")  { ctx.gizmoLocalSpace = !ctx.gizmoLocalSpace; return true; }
    if (id == "view.fill")        { ctx.viewportFill = (ctx.viewportFill > 0.0f) ? 0.0f : 0.35f; return true; }
    if (id == "view.flyMode")     { ctx.flyMode = !ctx.flyMode; return true; }
    if (id == "view.toggle2D")    { ctx.view2D = !ctx.view2D; return true; }
    if (id == "view.resetLayout") { ctx.resetLayout = true; return true; }
    if (id == "view.closeTools")  { tools::CloseAll(ctx); return true; }
    if (id == "view.fullscreen")  { ctx.pendingToggleFullscreen = true; return true; }
    if (id == "view.theme.default") { theme::RequestVariant(theme::Variant::Default); return true; }
    if (id == "view.theme.a")       { theme::RequestVariant(theme::Variant::A);       return true; }
    if (id == "view.theme.b")       { theme::RequestVariant(theme::Variant::B);       return true; }
    if (id == "view.theme.c")       { theme::RequestVariant(theme::Variant::C);       return true; }

    // ---- 再生 ----
    if (id == "play.toggle") { ctx.pendingPlayRequest = ctx.isPlaying ? 2 : 1; return true; }
    if (id == "play.stop")   { ctx.pendingPlayRequest = 2; return true; }
    if (id == "play.pause")  { ctx.paused = !ctx.paused; return true; }

    // ---- パレット ----
    if (id == "palette.commands")  { ctx.paletteRequest = 1; return true; }
    if (id == "palette.quickOpen") { ctx.paletteRequest = 2; return true; }

    return false;
}

// ---------------- 公開: ショートカット ----------------

void ProcessShortcuts(EditorContext& ctx, const Env& env)
{
    // 直前のフレームでポップアップ（右クリックメニュー / コンボ / ツールチップ以外）が開いていたか。
    // ImGui は Esc でポップアップを閉じるが、閉じたのは NewFrame 時点なので、その同じ Esc を
    // こちらが「選択解除」としても拾ってしまう。1 フレーム遅れて見て、閉じたばかりの Esc は無視する。
    static bool s_popupWasOpen = false;
    const bool popupOpenNow = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool popupJustClosed = s_popupWasOpen && !popupOpenNow;
    s_popupWasOpen = popupOpenNow;

    if (!ctx.appForeground) return;
    if (ctx.paletteOpen) return;                          // パレットが自分でキーを処理する
    if (ImGui::GetTopMostAndVisiblePopupModal()) return;  // 確認モーダル中は効かせない

    const ImGuiIO& io = ImGui::GetIO();
    for (const Bound& b : Bindings())
    {
        const Def& d = *b.def;
        if (d.mode == KeyMode::Panel || d.mode == KeyMode::External) continue;
        if (d.scope == Scope::Editor && ctx.isPlaying) continue;
        // ★Typing のコマンドは「文字を打っている最中」と「マウスを掴んでいる最中」だけ止める。
        //   io.WantCaptureKeyboard で止めてはいけない: NavEnableKeyboard が有効な ImGui では、どこかの窓に
        //   フォーカスがある（NavWindow がある）だけで WantCaptureKeyboard が常に true になる。
        //   つまりパネルを 1 回クリックしただけで W/E/R・Ctrl+Z・Esc・F2 が全部死ぬ（旧実装の実態）。
        //   WantTextInput（文字入力欄が有効）と IsAnyItemActive（スライダーのドラッグ中など）で十分。
        if (d.mode == KeyMode::Typing && (io.WantTextInput || ImGui::IsAnyItemActive() || ctx.mouseCaptured)) continue;
        // 右ドラッグ中のフライで誤発火させない。ただし Play / 停止（Scope::Always）はゲームがマウスを
        // 掴んでいる間こそ効かなければならない（掴まれると UI の停止ボタンは押せない）。
        if (d.mode == KeyMode::Global && d.scope == Scope::Editor && ctx.mouseCaptured) continue;
        if (popupJustClosed && std::strcmp(d.id, "edit.selectNone") == 0) continue;

        for (int i = 0; i < b.n; ++i)
        {
            if (!ChordPressed(b.c[i])) continue;
            // 実行できない状態（Play 中の Undo 等）は無視。同じキーを別のコマンドが持つことはない
            // （tests/editor_ux_test.cpp が重複を落とす）ので、押せたらここで 1 コマンドだけ実行して抜ける。
            Execute(ctx, env, d.id);
            return;
        }
    }
}

// ---------------- 公開: メニュー ----------------

bool MenuItem(EditorContext& ctx, const Env& env, const char* id)
{
    const Def* d = FindCommand(id);
    if (!d) return false;
    const bool enabled = IsEnabled(ctx, id);
    const bool checked = IsChecked(ctx, id);
    if (ui::MenuItem(IconFor(id), d->label, ShortcutText(id), checked, enabled))
    {
        Execute(ctx, env, id);
        return true;
    }
    return false;
}

// ---------------- 公開: エンティティ作成 ----------------

DirectX::XMFLOAT3 SpawnPosition(const EditorContext& ctx, float defaultY)
{
    if (!ctx.camValid) return {0.0f, defaultY, 0.0f};
    const DirectX::XMFLOAT3 c = ctx.camPos, f = ctx.camFwd;
    // 下を向いているなら床（Y=0）との交点。遠すぎる交点（ほぼ水平）は採らない。
    if (f.y < -0.05f)
    {
        const float t = -c.y / f.y;
        if (t > 0.5f && t <= 60.0f)
            return {c.x + f.x * t, defaultY, c.z + f.z * t};
    }
    // それ以外はカメラの正面 8m。
    constexpr float kDist = 8.0f;
    return {c.x + f.x * kDist, c.y + f.y * kDist, c.z + f.z * kDist};
}

void SpawnItem(EditorContext& ctx, const CreateItem& item)
{
    if (!item.marker || !item.marker[0])
    {
        // 専用の作成窓を開くもの（地形 / スカルプト）
        if (std::strcmp(item.openTool, "terrain") == 0) ctx.showTerrainEditor = true;
        else if (std::strcmp(item.openTool, "sculpt") == 0) ctx.showSculptEditor = true;
        return;
    }
    PendingSpawnRequest req;
    req.modelPath = item.marker;
    req.position = IsUiMarker(item.marker) ? DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f}
                                           : SpawnPosition(ctx, item.defaultY);
    ctx.pendingSpawns.push_back(std::move(req));
}

void DrawCreateMenu(EditorContext& ctx)
{
    const char* prevGroup = nullptr;
    const char* openSub = nullptr;   // 今開いているサブメニュー（Gimmick / UI）
    bool subOpen = false;
    auto closeSub = [&]() { if (openSub && subOpen) ImGui::EndMenu(); openSub = nullptr; subOpen = false; };

    for (const CreateItem& it : kCreateItems)
    {
        const bool isSub = std::strcmp(it.group, "Gimmick") == 0 || std::strcmp(it.group, "UI") == 0;
        if (!prevGroup || std::strcmp(prevGroup, it.group) != 0)
        {
            closeSub();
            // グループの切れ目に区切り線（先頭グループの前には出さない）
            if (prevGroup && !isSub) ImGui::Separator();
            else if (prevGroup && isSub) ImGui::Separator();
            if (isSub)
            {
                openSub = it.group;
                const char* title = std::strcmp(it.group, "Gimmick") == 0 ? "Gimmick（ステージ部品）"
                                                                          : "UI（ゲーム内UI）";
                subOpen = ImGui::BeginMenu(title);
            }
            prevGroup = it.group;
        }
        if (isSub && !subOpen) continue;   // 閉じているサブメニューの中身は描かない
        if (ImGui::MenuItem(it.label))
            SpawnItem(ctx, it);
    }
    closeSub();
}

} // namespace dx12e::cmd
