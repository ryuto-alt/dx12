#include "editor/panels/ToolbarPanel.h"
#include "core/VirtualGuard.h"
#include "gui/VirtualInputImGui.h"   // dx12_imgui_find 用アンカー   // 仮想入力モード中は ShellExecute / ダイアログを実行しない
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/EditorCommands.h"   // メニュー/ショートカットはコマンド表から描く
#include "editor/ToolWindows.h"      // ツール窓レジストリ（表示 / ツール / 窓▾ の共通の表）
#include "editor/Toast.h"
#include "editor/WorkspaceLogic.h"   // ワークスペースのプリセット（ツールバーの「ワークスペース ▾」）
#include "editor/BottomDock.h"        // 下部ドックの登録タブの開閉
#include "scripting/ScriptEngine.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneSettingsHash.h"   // 未保存判定に使う設定の指紋
#include "core/Window.h"
#include "core/Logger.h"
#include "core/AtomicFile.h"
#include "core/SceneBackup.h"      // 「以前の版に戻す」窓（世代の一覧）
#include "core/PathResolver.h"
#include "core/Version.h"          // ツールバーの版バッジ（文字列を直書きしない）
#include "project/ProjectManager.h"
#include "editor/panels/ShaderTemplates.h"

#include <commdlg.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // MenuBarRect(タイトルバー帯の実寸取得)
#pragma warning(pop)

namespace
{
void OpenInVSCode(const std::string& filePath)
{
    if (dx12e::guard::Blocked("Open in VS Code")) return;   // 仮想入力モード: 人の画面に窓を出さない
    static std::string cachedExe;
    static bool resolved = false;
    if (!resolved)
    {
        resolved = true;
        char appData[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData)))
        {
            namespace fs = std::filesystem;
            fs::path candidate = fs::path(appData) / "Programs" / "Microsoft VS Code" / "Code.exe";
            if (fs::exists(candidate))
                cachedExe = candidate.string();
        }
        if (cachedExe.empty())
        {
            namespace fs = std::filesystem;
            const char* dirs[] = {"C:\\Program Files\\Microsoft VS Code\\Code.exe",
                                  "C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe"};
            for (const char* p : dirs)
                if (fs::exists(p)) { cachedExe = p; break; }
        }
    }

    if (!cachedExe.empty())
    {
        std::string cmdLine = "\"" + cachedExe + "\" \"" + filePath + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr,
                           FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return;
        }
    }

    dx12e::guard::ShellExecuteGuarded(nullptr, "open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
} // anonymous namespace

namespace dx12e
{

// レジストリ（editor/ToolWindows.h）の窓をカテゴリ見出し付きのトグル項目として描く。
// home 指定なら「表示」「ツール」のどちらに置く窓だけ、allHomes=true なら全部（ツールバーの「窓▾」）。
static void DrawToolWindowItems(EditorContext& ctx, tools::MenuHome home, bool allHomes)
{
    const char* cat = nullptr;
    for (const tools::Desc& d : tools::kAll)
    {
        if (!allHomes && d.home != home) continue;
        if (!cat || std::strcmp(cat, d.category) != 0)
        {
            if (cat) ImGui::Separator();   // 最初の見出しの前には出さない（呼び出し側が区切る）
            ImGui::TextDisabled("%s", d.category);
            cat = d.category;
        }
        ui::MenuItem(d.icon, d.title, nullptr, &(ctx.*(d.flag)));
    }
}

// ワークスペースのプリセット一覧（チェック = 今のワークスペース）。実処理は WorkspaceManager（コマンド workspace.<id>）。
static void DrawWorkspaceItems(EditorContext& ctx, const cmd::Env& env)
{
    (void)env;
    for (const ws::Preset& p : ws::Presets())
        if (ui::MenuItem(ICON_WINDOWS, p.title, nullptr, ctx.currentWorkspace == p.id))
            ctx.pendingWorkspace = p.id;   // コマンド workspace.<id> と同じ要求（表のラベルは「ワークスペース: …」でパレット向け）
}

// レイアウトの保存 / 復元 / 削除 / ツール窓の配置先 / 下部ドック。表示メニューの「レイアウト」とツールバーの「ワークスペース ▾」が共用。
static void DrawLayoutItems(EditorContext& ctx, const cmd::Env& env)
{
    cmd::MenuItem(ctx, env, "layout.save");
    {
        const bool any = !ctx.savedLayoutNames.empty();
        if (ImGui::BeginMenu("保存済みレイアウトを復元", any))
        {
            for (const std::string& n : ctx.savedLayoutNames)
                if (ui::MenuItem(ICON_BLANK, n.c_str())) ctx.pendingLayoutRestore = n;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("保存済みレイアウトを削除", any))
        {
            for (const std::string& n : ctx.savedLayoutNames)
                if (ui::MenuItem(ICON_BLANK, n.c_str())) ctx.pendingLayoutDelete = n;
            ImGui::EndMenu();
        }
    }
    cmd::MenuItem(ctx, env, "layout.slots");
    ImGui::Separator();
    // 下部ドックの登録タブ（BottomDock.h。タイムラインなど）
    for (bottomdock::Tab& t : bottomdock::Tabs())
    {
        bool open = t.open;
        if (ui::MenuItem(t.icon.empty() ? ICON_BLANK : t.icon.c_str(), t.title.c_str(), nullptr, &open))
            t.open = open;
    }
    cmd::MenuItem(ctx, env, "layout.bottomMaximize");
}

void ToolbarPanel::Render(bool isPlaying,
                          EditorContext& ctx,
                          bool& outModeChangeRequested,
                          bool& outPendingPlayMode,
                          ScriptEngine* scriptEngine,
                          GameClock* /*clock*/,
                          Scene* scene,
                          Window* window,
                          AudioSystem* /*audioSystem*/,
                          const std::string& assetsDir,
                          f32 toolbarHeight)
{
    // multi-viewport有効時、ImGui座標はスクリーン座標になるためメインビューポート原点基準で置く
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    f32 displayW = mainVp->Size.x;

    // メニュー / ショートカット / パレットが共有する実行環境（コピー・貼り付けに使う）
    const cmd::Env cmdEnv{scene, assetsDir};

    // 「シーンを開く」(Ctrl+O / メニュー / パレット) の要求を消化する。ファイルダイアログは
    // 窓のハンドルが要るのでこのパネルで出す。仮想入力中（AI 操作中）は OS のダイアログを出さない。
    if (ctx.pendingOpenSceneDialog)
    {
        ctx.pendingOpenSceneDialog = false;
        char loadPath[MAX_PATH] = "";
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = window->GetHwnd();
        ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files\0*.*\0";
        ofn.lpstrFile = loadPath;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST;
        std::string initDir = assetsDir + "scenes";
        std::filesystem::create_directories(initDir);
        ofn.lpstrInitialDir = initDir.c_str();
        if (!dx12e::guard::Blocked("シーンを開くダイアログ") && GetOpenFileNameA(&ofn))
            ctx.pendingLoadPath = loadPath;
    }

    ImGui::SetNextWindowPos(mainVp->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displayW, toolbarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    // 上段(タイトルバー兼メニュー 32px) + 下段(ツール行 36px = 上下 4px の余白 + 28px のボタン)= kToolbarHeight 68。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(8.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // メニューバー行はカスタムタイトルバーを兼ねるので縦paddingを増やして高くする(UE風の32px帯)。
    // Begin時点のFramePaddingでメニューバー高さが決まるためBeginより前にpushする。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(ui::Px(8.0f), (ui::Px(dx12e::theme::size::kTitleBarH) - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, dx12e::theme::Bg1);   // ツール行はパネルと同じ面。タイトルバー(MenuBarBg)は最深の Bg0
    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);

    // ===== メニューバー = カスタムタイトルバー =====
    // (左: ロゴ+メニュー / 中央: シーン名 / 右: 最小化・最大化・閉じる。
    //  余白部分のドラッグ移動は Window::SetCaptionInfo 経由で WM_NCHITTEST が HTCAPTION を返す)
    bool openShortcutsPopup = false;
    bool openAboutPopup     = false;
    ImRect titleBarRect;   // EndMenuBar後のドラッグ判定・キャプション高さ報告に使う
    if (ImGui::BeginMenuBar())
    {
        titleBarRect = ImGui::GetCurrentWindow()->MenuBarRect();

        // ---- ロゴ(タイトルバー左端。UEのエンジンアイコン位置) ----
        if (ctx.icons && ctx.icons->logo)
        {
            const float kLogoSz = ui::Px(20.0f);
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(ctx.icons->logo),
                ImVec2(cur.x, titleBarRect.GetCenter().y - kLogoSz * 0.5f),
                ImVec2(cur.x + kLogoSz, titleBarRect.GetCenter().y + kLogoSz * 0.5f));
            ImGui::Dummy(ImVec2(kLogoSz + ui::Px(8.0f), 0.0f));
        }

        // ---- ファイル ----
        ui::PushMenuStyle();
        const bool menuOpen1 = ImGui::BeginMenu("ファイル");
        dx12e::vinput_gui::AnchorLastItem("menu", "ファイル");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen1)
        {
            // 項目のラベル・アイコン・キー表記は editor/EditorCommandTable.h（唯一の正）から描く。
            // 実処理は cmd::Execute（保存は Application、ファイルダイアログはこのパネル上部が消費）。
            cmd::MenuItem(ctx, cmdEnv, "file.new");
            cmd::MenuItem(ctx, cmdEnv, "file.open");
            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "file.save");
            cmd::MenuItem(ctx, cmdEnv, "file.saveAs");
            // 保存のたびに直前の版が世代として残る（.dx12/backups/）。そこから戻す。
            if (ui::MenuItem(ICON_HISTORY, "以前の版に戻す…", nullptr, false, !ctx.currentScenePath.empty()))
            {
                ctx.sceneBackupTarget = ctx.currentScenePath;
                ctx.sceneBackupNotice.clear();
                ctx.showSceneBackups  = true;
            }
            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "file.newScript");
            cmd::MenuItem(ctx, cmdEnv, "file.newShader");
            ImGui::Separator();
            // プロジェクトを閉じてランチャー（プロジェクト選択/新規作成）に戻る。
            // ファイル操作は一切不要＝現在のプロジェクトフォルダはそのまま残る。
            cmd::MenuItem(ctx, cmdEnv, "file.closeProject");
            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        // ---- 編集 ----
        ui::PushMenuStyle();
        const bool menuOpen2 = ImGui::BeginMenu("編集");
        dx12e::vinput_gui::AnchorLastItem("menu", "編集");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen2)
        {
            // 何が戻るかを名前で出す。AI（MCP）の操作は「AI: <method>」の名前で積まれるので、
            // 人の操作と見分けられる（AI のトランザクション中はその旨も出す）。
            {
                const char* un = ctx.undoSystem.PeekUndoName();
                const char* rn = ctx.undoSystem.PeekRedoName();
                const std::string undoLabel = std::string("元に戻す") + (un ? std::string("（") + un + "）" : "")
                                            + "###edit_undo";
                const std::string redoLabel = std::string("やり直す") + (rn ? std::string("（") + rn + "）" : "")
                                            + "###edit_redo";
                if (ui::MenuItem(ICON_UNDO, undoLabel.c_str(), cmd::ShortcutText("edit.undo"), false, cmd::IsEnabled(ctx, "edit.undo")))
                    cmd::Execute(ctx, cmdEnv, "edit.undo");
                if (ui::MenuItem(ICON_REDO, redoLabel.c_str(), cmd::ShortcutText("edit.redo"), false, cmd::IsEnabled(ctx, "edit.redo")))
                    cmd::Execute(ctx, cmdEnv, "edit.redo");
                if (ctx.mcpUndo.TxOpen())
                    ImGui::TextDisabled("AI のまとめ操作「%s」が進行中（Ctrl+Z で確定して丸ごと戻す）",
                                        ctx.mcpUndo.TxLabel().c_str());
            }

            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "edit.copy");
            cmd::MenuItem(ctx, cmdEnv, "edit.paste");
            cmd::MenuItem(ctx, cmdEnv, "edit.duplicate");
            cmd::MenuItem(ctx, cmdEnv, "edit.delete");
            cmd::MenuItem(ctx, cmdEnv, "edit.rename");
            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "edit.selectNone");
            cmd::MenuItem(ctx, cmdEnv, "edit.focus");
            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "palette.commands");
            cmd::MenuItem(ctx, cmdEnv, "palette.quickOpen");
            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        // ---- 表示 ----
        // ウィンドウの一覧は editor/ToolWindows.h のレジストリから描く（「ツール」「窓▾」と同じ表）。
        ui::PushMenuStyle();
        const bool menuOpen3 = ImGui::BeginMenu("表示");
        dx12e::vinput_gui::AnchorLastItem("menu", "表示");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen3)
        {
            if (ImGui::BeginMenu("ワークスペース"))
            {
                DrawWorkspaceItems(ctx, cmdEnv);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("レイアウト"))
            {
                DrawLayoutItems(ctx, cmdEnv);
                ImGui::EndMenu();
            }
            cmd::MenuItem(ctx, cmdEnv, "view.resetLayout");
            cmd::MenuItem(ctx, cmdEnv, "view.closeTools");
            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "view.viewportBar");   // ビューポート上端の帯（隠すと従来と同じ 3D の矩形）
            cmd::MenuItem(ctx, cmdEnv, "view.outline");       // 選択 / ホバーの輪郭（エディタ専用）
            ImGui::Separator();
            {
                bool fill = ctx.viewportFill > 0.0f;
                if (ui::MenuItem(cmd::IconFor("view.fill"), "編集用の照らし込み", cmd::ShortcutText("view.fill"), &fill))
                    ctx.viewportFill = fill ? 0.35f : 0.0f;
                if (fill)
                {
                    ImGui::SetNextItemWidth(ui::Px(160.0f));
                    ui::SliderFloat("##viewportFill", &ctx.viewportFill, 0.02f, 1.0f, "%.2f");
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetTooltip("暗いシーンを編集するための光。シーンには保存されず、Play 中は効かない。");
            }
            ImGui::Separator();
            DrawToolWindowItems(ctx, tools::MenuHome::View, /*allHomes=*/false);
            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        // ---- ツール ----
        ui::PushMenuStyle();
        const bool menuOpen4 = ImGui::BeginMenu("ツール");
        dx12e::vinput_gui::AnchorLastItem("menu", "ツール");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen4)
        {
            // 「ビルド」はまずビルド設定パネルを開く（構成・開始シーン・出力先を決めてから実行）
            if (ui::MenuItem(ICON_HAMMER, "ビルド"))
                ctx.showBuildSettings = true;
            ImGui::Separator();
            DrawToolWindowItems(ctx, tools::MenuHome::Tools, /*allHomes=*/false);
            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        // ---- ヘルプ ----
        ui::PushMenuStyle();
        const bool menuOpen5 = ImGui::BeginMenu("ヘルプ");
        dx12e::vinput_gui::AnchorLastItem("menu", "ヘルプ");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen5)
        {
            if (ui::MenuItem(ICON_KEYBOARD, "ショートカット一覧"))
                openShortcutsPopup = true;
            cmd::MenuItem(ctx, cmdEnv, "help.whatsNew");
            if (ui::MenuItem(ICON_INFO, "バージョン情報"))
                openAboutPopup = true;
            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        const float menusEndX = ImGui::GetCursorScreenPos().x;   // メニュー列の右端(中央題字の重なり回避)
        const float barH      = titleBarRect.GetHeight();

        // ---- 中央: シーン名(UEのプロジェクト名表示の位置) ----
        {
            std::string title = kEngineName;   // 表示名の単一ソースは Version.cpp
            if (!ctx.currentScenePath.empty())
            {
                // パス区切り・拡張子を手で剥がす(UTF-8のままでも安全な操作だけにする)
                std::string name = ctx.currentScenePath;
                if (size_t p = name.find_last_of("/\\"); p != std::string::npos) name = name.substr(p + 1);
                if (size_t d = name.find_last_of('.');   d != std::string::npos) name = name.substr(0, d);
                if (!name.empty()) title = name + " - " + kEngineName;
            }
            // 未保存なら先頭に * を出す（エディタの慣習。ここが唯一の常時見える手掛かり）
            if (ctx.IsSceneDirty()) title = "*" + title;
            // 版は Version.cpp が単一ソース。題字の右に薄く添える（上段のロゴ + 題字で 1 か所にまとめた）。
            const std::string ver = std::string("v") + kEngineVersion;
            const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            const ImVec2 vs = ImGui::CalcTextSize(ver.c_str());
            const float totalW = ts.x + ui::Px(10.0f) + vs.x;
            float cx = titleBarRect.Min.x + (titleBarRect.GetWidth() - totalW) * 0.5f;
            cx = (std::max)(cx, menusEndX + ui::Px(24.0f));   // 窓が狭い時はメニューの右に退避
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(cx, titleBarRect.GetCenter().y - ts.y * 0.5f),
                ImGui::GetColorU32(dx12e::theme::TextDim), title.c_str());
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(cx + ts.x + ui::Px(10.0f), titleBarRect.GetCenter().y - vs.y * 0.5f),
                ImGui::GetColorU32(dx12e::theme::TextFaint), ver.c_str());
        }

        // ---- 右端: 最小化 / 最大化(復元) / 閉じる(OS標準の代替。グリフはDrawListで描く) ----
        if (window && window->IsCustomTitleBar())
        {
            const float btnW = ui::Px(44.0f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::SetCursorScreenPos(ImVec2(titleBarRect.Max.x - btnW * 3.0f, titleBarRect.Min.y));

            auto captionButton = [&](const char* id, int glyph /*0=min 1=max 2=close*/) -> bool
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton(id, ImVec2(btnW, barH));
                const bool hovered = ImGui::IsItemHovered();
                const ImVec2 p1(p0.x + btnW, p0.y + barH);
                if (hovered)
                    dl->AddRectFilled(p0, p1, glyph == 2 ? IM_COL32(232, 17, 35, 255)
                                                         : IM_COL32(255, 255, 255, 16));
                const ImU32 fg = (glyph == 2 && hovered) ? IM_COL32(255, 255, 255, 255)
                                                         : ImGui::GetColorU32(dx12e::theme::TextMid);
                const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                const float r = ui::Px(5.0f);   // グリフ半径
                const float o = ui::Px(2.0f);   // 復元グリフの重ね量
                const float lw = ui::Px(1.0f);  // グリフ線幅
                if (glyph == 0)          // ─
                    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, lw);
                else if (glyph == 1)     // □ / ❐(復元)
                {
                    if (window->IsMaximized())
                    {
                        dl->AddRect(ImVec2(c.x - r, c.y - r + o), ImVec2(c.x + r - o, c.y + r), fg, lw, 0, lw);
                        dl->AddLine(ImVec2(c.x - r + o, c.y - r + o), ImVec2(c.x - r + o, c.y - r), fg, lw);
                        dl->AddLine(ImVec2(c.x - r + o, c.y - r), ImVec2(c.x + r, c.y - r), fg, lw);
                        dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x + r, c.y + r - o), fg, lw);
                        dl->AddLine(ImVec2(c.x + r, c.y + r - o), ImVec2(c.x + r - o, c.y + r - o), fg, lw);
                    }
                    else
                        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, lw, 0, lw);
                }
                else                     // ✕
                {
                    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, lw);
                    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), fg, lw);
                }
                ImGui::SameLine(0.0f, 0.0f);
                return clicked;
            };

            if (captionButton("##cap_min", 0))   window->Minimize();
            if (captionButton("##cap_max", 1))   window->ToggleMaximize();
            if (captionButton("##cap_close", 2)) window->RequestClose();
        }

        ImGui::EndMenuBar();
    }
    ImGui::PopStyleVar();   // FramePadding(タイトルバー用の縦増し。以降のツール列は通常値に戻す)

    // タイトルバー帯のドラッグ可否をWindowへ報告する。アイテム(メニュー/ボタン)上や
    // ポップアップ表示中はドラッグさせない。値は次のWM_NCHITTESTから効く(1フレーム遅れ許容)。
    if (window && window->IsCustomTitleBar())
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inBand = mouse.x >= titleBarRect.Min.x && mouse.x < titleBarRect.Max.x
                         && mouse.y >= titleBarRect.Min.y && mouse.y < titleBarRect.Max.y;
        const bool blocked = ImGui::IsAnyItemHovered()
                          || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        // キャプション高さはクライアント座標で渡す(multi-viewport有効時のImGui座標はスクリーン座標のため変換)
        window->SetCaptionInfo(static_cast<u32>(titleBarRect.Max.y - mainVp->Pos.y), inBand && !blocked);
    }

    // ===== ツール行（フラットなアイコンボタン + 縦区切り。UE5 風）=====
    // ボタンは 28x28 の正方形・ラベル無し（ホバーでツールチップ）。それより低い部品（コンボ・文字）は
    // 縦中央へ置く。配置はカーソルを明示指定する（SameLine は高さの違う部品の縦位置が揃わないため）。
    namespace th = dx12e::theme;
    const float kBtn   = th::Px(th::size::kToolbarBtn);   // DPI: 論理 28 -> 物理 px
    const float rowTop = ImGui::GetCursorScreenPos().y;
    const float rowMid = rowTop + kBtn * 0.5f;
    float x = mainVp->Pos.x + ui::Px(10.0f);
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    const float frameH = ImGui::GetFrameHeight();
    const float lineH  = ImGui::GetTextLineHeight();
    auto put = [&](float w, float h) { ImGui::SetCursorScreenPos(ImVec2(x, std::floor(rowMid - h * 0.5f + 0.5f))); x += w; };
    auto iconBtn = [&](const char* id, const char* glyph, const char* tip, bool active, const char* anchor,
                       const ImVec4* tint = nullptr, const ImVec4* face = nullptr) -> bool
    {
        put(kBtn + ui::Px(2.0f), kBtn);
        const bool c = ui::IconButton(id, glyph, tip, active, tint, kBtn, 0.0f, face);
        dx12e::vinput_gui::AnchorLastItem("button", anchor);   // dx12_imgui_find 用（従来のラベル名で引ける）
        return c;
    };
    auto sep = [&]()
    {
        x += ui::Px(6.0f);
        tdl->AddLine(ImVec2(x, rowMid - ui::Px(10.0f)), ImVec2(x, rowMid + ui::Px(10.0f)), ImGui::GetColorU32(th::BorderStrong));
        x += ui::Px(8.0f);
    };

    // ※ ギズモ（移動 / 回転 / 拡縮 / 空間）のボタンはビューポート上端の帯（ViewportToolbar）へ一本化した（かつては上段と二重表示）。
    //   キー W/E/R/T は従来どおり。帯を隠している時だけ、操作の入口が無くならないよう上段に出す（帯が正・上段は代替）。
    if (!ctx.vpPrefs.barVisible)
    {
        if (iconBtn("gizmoMove", ICON_MOVE, "移動ギズモ  (W)", ctx.gizmoMode == GizmoMode::Translate, "移動"))
            ctx.gizmoMode = GizmoMode::Translate;
        if (iconBtn("gizmoRotate", ICON_ROTATE, "回転ギズモ  (E)", ctx.gizmoMode == GizmoMode::Rotate, "回転"))
            ctx.gizmoMode = GizmoMode::Rotate;
        if (iconBtn("gizmoScale", ICON_SCALE, "拡大縮小ギズモ  (R)", ctx.gizmoMode == GizmoMode::Scale, "拡縮"))
            ctx.gizmoMode = GizmoMode::Scale;
        if (iconBtn("gizmoSpace", ctx.gizmoLocalSpace ? ICON_SPACE_LOCAL : ICON_SPACE_WORLD,
                    ctx.gizmoLocalSpace ? "ローカル空間  (T で切替)" : "ワールド空間  (T で切替)", false,
                    ctx.gizmoLocalSpace ? "ローカル" : "ワールド"))
            ctx.gizmoLocalSpace = !ctx.gizmoLocalSpace;
        sep();
    }

    // ===== 2D / 3D ビュー切替（Unity の 2D ボタン相当）+ UI 編集モード =====
    if (iconBtn("view2D", ctx.view2D ? ICON_VIEW_2D : ICON_VIEW_3D,
                ctx.view2D
                    ? "2Dビュー（クリックで3D）: 正射・正面固定。WASD/矢印=パン, ホイール=ズーム, 中ドラッグ=パン"
                    : "3Dビュー（クリックで2D）",
                ctx.view2D, ctx.view2D ? "2D" : "3D"))
        ctx.view2D = !ctx.view2D;
    if (iconBtn("uiEdit", ICON_UI_MODE, "UI編集モード（SceneViewでゲーム内UIをプレビュー・編集）",
                ctx.uiEditMode, "UI"))
        ctx.uiEditMode = !ctx.uiEditMode;

    // ※ ゲームビルドはツールバーに常駐させない。メニュー「ツール > ゲームをビルド…」から呼び出す。
    //    （配置先フォルダを毎回ピッカーで選ぶ方式。ユーザー要望でツールバーのボタンは撤去）

    // ===== ツール窓（「窓 ▾」ドロップダウン1個に集約）=====
    sep();
    put(kBtn + ui::Px(14.0f) + ui::Px(2.0f), kBtn);
    if (ui::IconDropdownButton("windows", ICON_WINDOWS, "ツール窓の表示/非表示", tools::AnyOpen(ctx), kBtn))
        ImGui::OpenPopup("##ToolWindowsMenu");
    dx12e::vinput_gui::AnchorLastItem("button", "窓");
    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##ToolWindowsMenu"))
    {
        // 全ツール窓（レジストリ editor/ToolWindows.h）。「表示」「ツール」メニューと同じ表から描く。
        // 右タブの窓は右カラムのインスペクターのタブに、浮かぶ窓はビューポートの上に開く。
        DrawToolWindowItems(ctx, tools::MenuHome::View, /*allHomes=*/true);
        ImGui::Separator();
        cmd::MenuItem(ctx, cmdEnv, "view.closeTools");
        cmd::MenuItem(ctx, cmdEnv, "view.resetLayout");
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    // ===== ワークスペース ▾（レベル編集 / マテリアル / ライティング / アニメ・シーケンサー / VFX / UI）=====
    // 切替は「今の状態を記憶 → 選んだワークスペースの最後の状態（初回はプリセット）を適用」。ビューポートの矩形は揺らさない。
    x += ui::Px(6.0f);
    {
        const ws::Preset* curWs = ws::FindPreset(ctx.currentWorkspace);
        put(0.0f, kBtn);
        if (ui::LabelDropdownButton("workspace", ICON_WINDOWS, curWs ? curWs->title : "ワークスペース",
                                    "ワークスペースの切替とレイアウトの保存 / 復元", false, kBtn))
            ImGui::OpenPopup("##WorkspaceMenu");
        dx12e::vinput_gui::AnchorLastItem("button", "ワークスペース");
        const ImVec2 wsBtnMin = ImGui::GetItemRectMin(), wsBtnMax = ImGui::GetItemRectMax();
        x = wsBtnMax.x + ui::Px(4.0f);
        ui::PushMenuStyle();
        ImGui::SetNextWindowPos(ImVec2(wsBtnMin.x, wsBtnMax.y + ui::Px(2.0f)), ImGuiCond_Appearing);   // ボタンの真下に開く（カーソル位置ではなく）
        if (ImGui::BeginPopup("##WorkspaceMenu"))
        {
            ImGui::TextDisabled("ワークスペース");
            DrawWorkspaceItems(ctx, cmdEnv);
            ImGui::Separator();
            DrawLayoutItems(ctx, cmdEnv);
            ImGui::Separator();
            cmd::MenuItem(ctx, cmdEnv, "view.resetLayout");
            ImGui::EndPopup();
        }
        ui::PopMenuStyle();
    }

    // ===== Play コントロール（画面中央。UE5 と同じ配置）=====
    // Play ボタンが常に画面中央へ来るよう配置（左のツール群と重なる狭い窓では右へ退避）。
    // フラットなアイコン: 停止中は緑の再生アイコン、Play 中は赤い停止アイコン（押下状態 = 薄い赤の面）。
    x = (std::max)(x + ui::Px(12.0f), mainVp->Pos.x + displayW * 0.5f - kBtn * 0.5f);
    if (!isPlaying)
    {
        if (iconBtn("play", ICON_PLAY, "Play  再生（プレイモードへ）", false, "再生", &th::Good))
        {
            // game.lua は任意（各エンティティのスクリプトコンポーネントが動くため）。
            // 旧来の「scripts/game.lua が無いと再生不可」警告は廃止し、そのまま再生する。
            outPendingPlayMode = true;
            outModeChangeRequested = true;
        }
    }
    else
    {
        if (iconBtn("stop", ICON_STOP, "Stop  停止（エディタへ戻る）", true, "停止", &th::Bad, &th::Bad))
        {
            outPendingPlayMode = false;
            outModeChangeRequested = true;
        }

        // 一時停止（Play を続けたままシーンビューを飛び回って調べる）。
        // ★本命は F1。ゲームがマウスをキャプチャしているとこのボタンは押せないため。
        const bool wasPaused = ctx.paused;
        if (iconBtn("pause", ICON_PAUSE,
                    wasPaused
                        ? "Resume  再開（F1）"
                        : "Pause  一時停止（F1）\n"
                          "Play を続けたまま時間だけ止めて、シーンビューを自由に動かせます。\n"
                          "右ドラッグで視点、WASD/Space/Shift で移動。",
                    wasPaused, wasPaused ? "再開" : "一時停止",
                    wasPaused ? &th::Warn : nullptr, &th::Warn))
        {
            ctx.paused = !ctx.paused;
        }
    }

    // ===== マルチプレイ テストロール(フェーズ⑨) =====
    // Play中はロール変更不可(Stopしてから変える)。EnterPlayModeがctx.netTestRoleを見て
    // net:host()/net:join()相当を自動実行する(Luaを書かずに素早く2窓テストできるように)。
    x += ui::Px(10.0f);
    put(ui::Px(150.0f) + ui::Px(8.0f), frameH);
    ImGui::BeginDisabled(isPlaying);
    {
        const char* roleLabels[] = { "オフライン", "ホストとしてPlay", "クライアント参加" };
        int roleIdx = static_cast<int>(ctx.netTestRole);
        ImGui::SetNextItemWidth(ui::Px(150.0f));
        if (ui::Combo("##net_test_role", &roleIdx, roleLabels, IM_ARRAYSIZE(roleLabels)))
            ctx.netTestRole = static_cast<NetTestRole>(roleIdx);
    }
    ImGui::EndDisabled();

    if (ctx.netTestRole == NetTestRole::Client)
    {
        put(ui::Px(120.0f) + ui::Px(6.0f), frameH);
        ImGui::BeginDisabled(isPlaying);
        char ipBuf[64];
        strncpy_s(ipBuf, ctx.netTestJoinAddress.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(ui::Px(120.0f));
        if (ui::InputText("##net_test_ip", ipBuf, sizeof(ipBuf)))
            ctx.netTestJoinAddress = ipBuf;
        ImGui::EndDisabled();
    }

    if (isPlaying && ctx.netTestRole == NetTestRole::Host)
    {
        const char* lbl = "テストクライアント起動";
        const float bw = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        put(bw + ui::Px(6.0f), frameH);
        if (ImGui::Button(lbl))
            ctx.netTestLaunchClientRequested = true;
        put(ui::Px(20.0f), frameH);
        ImGui::TextDisabled("%s", ICON_HELP);
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("同じプロジェクトを --net-client 127.0.0.1:<port> でもう1つ起動して"
                                   "\n自動でこのホストへ接続します(検証用の別ウィンドウ)。");
            ImGui::EndTooltip();
        }
    }

    // ===== Status =====
    x += ui::Px(8.0f);
    {
        const char* st = isPlaying ? ICON_PLAY " プレイ中" : "エディタ";
        put(ImGui::CalcTextSize(st).x + ui::Px(16.0f), lineH);
        if (isPlaying)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, th::Good);
            ImGui::TextUnformatted(st);
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("%s", st);
        }
    }

    // Lua error
    // ★GetLastError だけでは消えない（直してリロードしても点きっぱなしになる）ので、
    //   コンポーネント Lua の生きたエラー状態も併せて見る。
    if (scriptEngine && (!scriptEngine->GetLastError().empty() || scriptEngine->HasScriptErrors()))
    {
        const char* msg = ICON_WARN " Lua Error";
        put(ImGui::CalcTextSize(msg).x + ui::Px(16.0f), lineH);
        ImGui::PushStyleColor(ImGuiCol_Text, th::Bad);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
    }

    // 結果の通知はトースト（右下）へ。以前は「緑文字 1.5 秒（狭いツール行の右端）」「赤文字」
    // 「中央モーダル（OK を押すまで操作不能）」の 3 通りが混在していた。
    // 判断が要る確認（未保存 / 削除 / オートセーブ復旧）だけがモーダルのまま残る。
    // ※ FPS/描画統計は下部ステータスバー(EditorLayer::RenderStatusBar)に集約した。
    //    ここに出すと同じ数字が2箇所に並ぶだけなので置かない。

    // 「Play できません」「モデル差し替え失敗」など、errorMessage + errorFlash で要求されたエラー
    if (ctx.errorFlash > 0.0f)
    {
        ctx.errorFlash = 0.0f;   // トリガー用（消費したら戻す）
        ctx.Notify(ui::ToastKind::Error, ctx.errorMessage, 8.0f);
    }


    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);   // WindowPadding + WindowBorderSize

    // ★OpenPopup は BeginPopupModal と同じウィンドウ（ID スタック）で呼ぶこと。以前は ##Toolbar の中で
    //   OpenPopup していたので ID が食い違い、ヘルプの「ショートカット一覧」「バージョン情報」は
    //   一度も開いていなかった（メニューの項目を押しても何も起きない）。
    if (openShortcutsPopup) ImGui::OpenPopup("ショートカット一覧##ShortcutsPopup");
    if (openAboutPopup)     ImGui::OpenPopup("バージョン情報##AboutPopup");

    // ===== ヘルプ: ショートカット一覧モーダル =====
    // 内容は editor/EditorCommandTable.h のコマンド表から生成する（手書きの静的表は廃止）。
    // キーを足す / 変えるとこの一覧・メニューの表記・実際のキー処理が同時に変わる＝表記と実装が食い違わない。
    {
        ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ui::Px(560.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("ショートカット一覧##ShortcutsPopup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            const float tableH = (std::min)(ImGui::GetMainViewport()->Size.y * 0.78f, ui::Px(800.0f));
            if (ImGui::BeginChild("##shortcutsScroll", ImVec2(0.0f, tableH), false))
            {
                if (ImGui::BeginTable("##shortcuts", 2,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthFixed, ui::Px(190.0f));
                    ImGui::TableSetupColumn("動作", ImGuiTableColumnFlags_WidthStretch);

                    auto row = [](const std::string& key, const char* desc)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ui::PushMono();
                        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::AccentHover);
                        ImGui::TextUnformatted(key.c_str());
                        ImGui::PopStyleColor();
                        ui::PopMono();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(desc);
                    };
                    auto section = [](const char* title)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Dummy(ImVec2(0.0f, ui::Px(4.0f)));
                        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::TextDim);
                        ui::PushBold();
                        ImGui::TextUnformatted(title);
                        ui::PopBold();
                        ImGui::PopStyleColor();
                    };

                    const char* cat = nullptr;
                    for (const cmd::Def& d : cmd::kCommands)
                    {
                        const std::string chord = cmd::ChordLabel(d);
                        if (chord.empty()) continue;   // キーの無いコマンドは一覧に載せない（メニュー / パレットから使う）
                        if (!cat || std::strcmp(cat, d.category) != 0) { section(d.category); cat = d.category; }
                        row(chord, (d.help && d.help[0]) ? d.help : d.label);
                    }
                    section("マウス");
                    for (const cmd::MouseHelp& m : cmd::kMouseHelp)
                        row(m.key, m.desc);
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::Button("閉じる", ui::Px(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ===== ヘルプ: バージョン情報モーダル =====
    {
        ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("バージョン情報##AboutPopup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(kEngineName);
            ImGui::TextDisabled("DirectX 12 ゲームエンジン + エディタ");
            ImGui::Separator();
            ImGui::TextUnformatted("https://github.com/ryuto-alt/dx12");
            ImGui::Spacing();
            if (ImGui::Button("閉じる", ui::Px(120.0f, 0.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // ===== 新規シーン名入力ダイアログ =====
    if (ctx.showNewSceneDialog)
    {
        ImGui::OpenPopup("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3##NewScenePopup");
        ctx.showNewSceneDialog = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(350.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3##NewScenePopup",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe5\x90\x8d:");  // シーン名:
        ImGui::SetNextItemWidth(-1);
        bool enterPressed = ui::InputText("##SceneName", ctx.newSceneNameBuf,
            sizeof(ctx.newSceneNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        // 初回フォーカス
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        ImGui::Separator();

        bool nameValid = std::strlen(ctx.newSceneNameBuf) > 0;

        if (!nameValid) ImGui::BeginDisabled();
        const char* btnLabel = ctx.newSceneDialogIsCreate
            ? "\xe4\xbd\x9c\xe6\x88\x90"    // 作成
            : "\xe4\xbf\x9d\xe5\xad\x98";   // 保存
        if (ImGui::Button(btnLabel, ui::Px(120.0f, 0.0f)) || (enterPressed && nameValid))
        {
            std::string scenesDir = assetsDir + "scenes/";
            std::filesystem::create_directories(scenesDir);
            ctx.currentScenePath = scenesDir + ctx.newSceneNameBuf + ".json";
            ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
            if (ctx.newSceneDialogIsCreate)
            {
                ctx.pendingNewScene = true;  // 新規シーン作成
            }
            else
            {
                // 今のシーンをそのまま名前を付けて保存
                const std::string savedName = std::filesystem::path(ctx.currentScenePath).filename().string();
                if (SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir))
                {
                    ctx.MarkSceneSaved(SceneSettingsFingerprint(*scene));
                    ctx.Notify(ui::ToastKind::Success, "保存しました: " + savedName);
                }
                else
                {
                    Logger::Error("シーンを保存できませんでした: {}", ctx.currentScenePath);
                    ctx.Notify(ui::ToastKind::Error, "保存に失敗しました: " + savedName + "（詳細は dx12_engine.log）");
                }
            }
            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ui::Px(120.0f, 0.0f)))  // キャンセル
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // ===== 新規スクリプト名入力ダイアログ =====
    if (ctx.showNewScriptDialog)
    {
        ImGui::OpenPopup("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88##NewScriptPopup");
        ctx.showNewScriptDialog = false;
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(350.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88##NewScriptPopup",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88\xe5\x90\x8d:");  // スクリプト名:
        ImGui::SetNextItemWidth(-1);
        bool enterPressed = ui::InputText("##ScriptName", ctx.newScriptNameBuf,
            sizeof(ctx.newScriptNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        ImGui::Separator();

        bool nameValid = std::strlen(ctx.newScriptNameBuf) > 0;

        if (!nameValid) ImGui::BeginDisabled();
        if (ImGui::Button("\xe4\xbd\x9c\xe6\x88\x90", ui::Px(120.0f, 0.0f)) || (enterPressed && nameValid))  // 作成
        {
            // assets/scripts/ にテンプレート生成（プロジェクト内＝アセットブラウザに表示され、
            // scriptPath="scripts/<name>.lua" で添付できる。旧 assets/../scripts はブラウザ外だった）
            std::string scriptsDir = assetsDir + "scripts/";
            std::filesystem::create_directories(scriptsDir);
            std::string scriptPath = scriptsDir + ctx.newScriptNameBuf + ".lua";

            if (!std::filesystem::exists(scriptPath))
            {
                std::string body;
                body += std::string("-- ") + ctx.newScriptNameBuf + ".lua\n\n";
                body += "function OnStart()\n";
                body += "    -- Called once when play mode starts\n";
                body += "end\n\n";
                body += "function OnUpdate(dt)\n";
                body += "    -- Called every frame\n";
                body += "end\n";
                const auto wr = dx12e::atomicfile::WriteFile(std::filesystem::path(scriptPath), body);
                if (wr)
                {
                    Logger::Info("Created script: {}", scriptPath);
                    ctx.Notify(ui::ToastKind::Success, "スクリプトを作成しました: " + std::string(ctx.newScriptNameBuf) + ".lua");
                }
                else
                {
                    Logger::Error("スクリプトの作成に失敗しました: {} ({})", scriptPath, wr.error);
                    ctx.Notify(ui::ToastKind::Error, "スクリプトを作成できませんでした: " + wr.error);
                }
            }
            else
            {
                Logger::Warn("スクリプトは既に存在します: {}", scriptPath);
                ctx.Notify(ui::ToastKind::Warn, "スクリプトは既に存在します: " + std::string(ctx.newScriptNameBuf) + ".lua");
            }

            // VS Code で開く
            OpenInVSCode(scriptPath);

            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ui::Px(120.0f, 0.0f)))  // キャンセル
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // ===== 新規カスタムシェーダー名入力ダイアログ =====
    if (ctx.showNewShaderDialog)
    {
        ImGui::OpenPopup("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc##NewShaderPopup");
        ctx.showNewShaderDialog = false;
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(350.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc##NewShaderPopup",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc\xe5\x90\x8d:");  // シェーダー名:
        ImGui::SetNextItemWidth(-1);
        bool enterPressed = ui::InputText("##ShaderName", ctx.newShaderNameBuf,
            sizeof(ctx.newShaderNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        // 雛形の種類。メッシュ用と画面全体用ではルートシグネチャの契約が全く違うので、
        // 「作った .hlsl をどこに割り当てるつもりか」をここで選ばせる。
        // 選び間違えたまま割り当てると PSO 生成に失敗して素通しになるだけで、
        // 何が悪いのか分からない（ログを見ないと気付けない）。
        static int shaderKind = 0;
        ImGui::Spacing();
        ImGui::TextUnformatted("\xe7\xa8\xae\xe9\xa1\x9e:");  // 種類:
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##ShaderKind", &shaderKind,
            "\xe3\x83\xa1\xe3\x83\x83\xe3\x82\xb7\xe3\x83\xa5\xe7\x94\xa8"
            "\xef\xbc\x88MeshRenderer \xe3\x81\xab\xe5\x89\xb2\xe3\x82\x8a\xe5\xbd\x93\xe3\x81\xa6\xe3\x82\x8b\xef\xbc\x89\0"
            "\xe7\x94\xbb\xe9\x9d\xa2\xe5\x85\xa8\xe4\xbd\x93\xe7\x94\xa8"
            "\xef\xbc\x88\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9\xe3\x81\xab\xe5\x89\xb2\xe3\x82\x8a\xe5\xbd\x93\xe3\x81\xa6\xe3\x82\x8b\xef\xbc\x89\0");
        ImGui::TextDisabled(shaderKind == 0
            ? "\xe3\x83\xa2\xe3\x83\x87\xe3\x83\xab 1 \xe4\xbd\x93\xe3\x81\xae\xe6\x8f\x8f\xe3\x81\x8d\xe6\x96\xb9\xe3\x82\x92\xe5\xb7\xae\xe3\x81\x97\xe6\x9b\xbf\xe3\x81\x88\xe3\x82\x8b"
            : "\xe5\xae\x8c\xe6\x88\x90\xe3\x81\x97\xe3\x81\x9f\xe7\x94\xbb\xe9\x9d\xa2\xe3\x81\x9d\xe3\x81\xae\xe3\x82\x82\xe3\x81\xae\xe3\x82\x92\xe6\x9b\xb8\xe3\x81\x8d\xe6\x8f\x9b\xe3\x81\x88\xe3\x82\x8b");

        ImGui::Separator();

        bool nameValid = std::strlen(ctx.newShaderNameBuf) > 0;

        if (!nameValid) ImGui::BeginDisabled();
        if (ImGui::Button("\xe4\xbd\x9c\xe6\x88\x90", ui::Px(120.0f, 0.0f)) || (enterPressed && nameValid))  // 作成
        {
            // assets/shaders/ にテンプレート生成（プロジェクト独自シェーダーの置き場。
            // ShaderManager がここを走査し、Registryに無いものはカスタムとしてホットリロード対象になる）。
            std::string shadersDir = assetsDir + "shaders/";
            std::filesystem::create_directories(shadersDir);
            std::string shaderPath = shadersDir + ctx.newShaderNameBuf + ".hlsl";

            if (!std::filesystem::exists(shaderPath))
            {
                const auto wr = dx12e::atomicfile::WriteFile(std::filesystem::path(shaderPath),
                                                             std::string_view(shaderKind == 1 ? kNewScreenShaderTemplate : kNewShaderTemplate));
                if (wr)
                {
                    Logger::Info("Created shader: {}", shaderPath);
                    ctx.Notify(ui::ToastKind::Success, "シェーダーを作成しました: " + std::string(ctx.newShaderNameBuf) + ".hlsl");
                }
                else
                {
                    Logger::Error("シェーダーの作成に失敗しました: {} ({})", shaderPath, wr.error);
                    ctx.Notify(ui::ToastKind::Error, "シェーダーを作成できませんでした: " + wr.error);
                }
            }
            else
            {
                Logger::Warn("シェーダーは既に存在します: {}", shaderPath);
                ctx.Notify(ui::ToastKind::Warn, "シェーダーは既に存在します: " + std::string(ctx.newShaderNameBuf) + ".hlsl");
            }

            // VS Code で開く
            OpenInVSCode(shaderPath);

            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ui::Px(120.0f, 0.0f)))  // キャンセル
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // ===== 未保存の確認（シーンを開く / 新規 / プロジェクトを閉じる / ウィンドウを閉じる）=====
    // Application::ConfirmDiscardScene が showUnsavedConfirm を立て、ここで選択を受け取る。
    // ★MCP 経由（open_scene / new_scene / open_project）はここを通さない。
    //   AI はモーダルを押せないので、出すと応答不能になる。あちらは ping の sceneDirty で判断させる。
    if (ctx.showUnsavedConfirm && !m_unsavedPopupOpen)
    {
        ImGui::OpenPopup("##UnsavedConfirm");
        m_unsavedPopupOpen = true;
    }
    if (!ctx.showUnsavedConfirm) m_unsavedPopupOpen = false;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##UnsavedConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
            "\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84\xe5\xa4\x89\xe6\x9b\xb4\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99");  // 保存していない変更があります
        ImGui::Spacing();

        const bool canSave = !ctx.currentScenePath.empty();
        if (canSave)
        {
            std::string name = ctx.currentScenePath;
            if (size_t sp = name.find_last_of("/\\"); sp != std::string::npos) name = name.substr(sp + 1);
            ImGui::TextWrapped("%s", name.c_str());
            // 続けると、保存していない変更は元に戻せません。
            ImGui::TextDisabled("\xe7\xb6\x9a\xe3\x81\x91\xe3\x82\x8b\xe3\x81\xa8\xe3\x80\x81\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84\xe5\xa4\x89\xe6\x9b\xb4\xe3\x81\xaf\xe5\x85\x83\xe3\x81\xab\xe6\x88\xbb\xe3\x81\x9b\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82");
        }
        else
        {
            // このシーンはまだ保存先が決まっていません（先に「名前を付けて保存」が要ります）。
            ImGui::TextWrapped("\xe3\x81\x93\xe3\x81\xae\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x81\xaf\xe3\x81\xbe\xe3\x81\xa0\xe4\xbf\x9d\xe5\xad\x98\xe5\x85\x88\xe3\x81\x8c\xe6\xb1\xba\xe3\x81\xbe\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button("\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xa6\xe7\xb6\x9a\xe8\xa1\x8c", ui::Px(132.0f, 0.0f)))  // 保存して続行
        {
            ctx.unsavedChoice = EditorContext::UnsavedChoice::Save;
            ImGui::CloseCurrentPopup();
        }
        if (!canSave) ImGui::EndDisabled();

        ImGui::SameLine();
        // 破棄は誤爆すると作業が消えるので赤くしておく（既定フォーカスも持たせない）
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button("\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x9b\xe3\x81\x9a\xe7\xb6\x9a\xe8\xa1\x8c", ui::Px(132.0f, 0.0f)))  // 保存せず続行
        {
            ctx.unsavedChoice = EditorContext::UnsavedChoice::Discard;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ui::Px(120.0f, 0.0f))  // キャンセル
            || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ctx.unsavedChoice = EditorContext::UnsavedChoice::Cancel;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ===== 以前の版に戻す（シーンの世代つきバックアップ。.dx12/backups/）=====
    // ファイル メニューから開く。壊れた・空のシーンを開けなかったときは自動で開き、復元を案内する。
    // 実際の復元は Application が sceneBackupRestoreId を消化して行う（今の版も先に 1 世代残す）。
    if (ctx.showSceneBackups && !ImGui::IsPopupOpen("##SceneBackups"))
        ImGui::OpenPopup("##SceneBackups");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(640.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##SceneBackups", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const std::filesystem::path target(ctx.sceneBackupTarget);
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "以前の版に戻す");
        ImGui::TextDisabled("%s", target.filename().string().c_str());
        if (!ctx.sceneBackupNotice.empty())
        {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ui::Px(620.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.45f, 1.0f), "%s", ctx.sceneBackupNotice.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const std::filesystem::path projectRoot(PathResolver::BaseDir());
        const auto gens = scenebackup::List(projectRoot, target);
        if (gens.empty())
        {
            ImGui::TextWrapped("このシーンの以前の版はまだありません。保存するたびに、直前の版がここへ残ります。");
        }
        else
        {
            ImGui::BeginChild("##SceneBackupList", ui::Px(0.0f, (std::min)(260.0f, 34.0f * static_cast<float>(gens.size()) + 8.0f)), false);
            for (size_t i = 0; i < gens.size(); ++i)
            {
                const auto& g = gens[i];
                ImGui::PushID(static_cast<int>(i));
                const std::time_t t = static_cast<std::time_t>(g.unixTime);
                std::tm tmv{};
                char when[48] = {};
                if (localtime_s(&tmv, &t) == 0) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", when);
                ImGui::SameLine(ui::Px(190.0f));
                ImGui::TextDisabled("%.1f MB%s%s%s", static_cast<double>(g.bytes) / (1024.0 * 1024.0),
                                    g.hasParts ? "  分割" : "", g.hasInst ? "  インスタンス群" : "", g.hasNav ? "  ナビ" : "");
                ImGui::SameLine(ui::Px(500.0f));
                if (ImGui::Button("この版に戻す", ui::Px(110.0f, 0.0f)))
                {
                    ctx.sceneBackupRestoreId = g.id;
                    ImGui::CloseCurrentPopup();
                    ctx.showSceneBackups = false;
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::TextDisabled("戻す前の今の版も 1 世代として残します（戻した操作も取り消せます）。");
        }
        ImGui::Spacing();
        if (ImGui::Button("バックアップのフォルダを開く", ui::Px(220.0f, 0.0f)))
        {
            std::error_code ec;
            const auto dir = scenebackup::BackupDir(projectRoot);
            std::filesystem::create_directories(dir, ec);
            dx12e::guard::ShellExecuteGuarded(nullptr, "open", dir.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("閉じる", ui::Px(110.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ctx.showSceneBackups = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    else if (!ImGui::IsPopupOpen("##SceneBackups"))
    {
        // 別のモーダルに割り込まれて開けなかったときは次のフレームでもう一度開く（ctx.showSceneBackups は立てたまま）
    }

    // ===== 自動保存からの復旧 =====
    // シーンを開いた直後、オートセーブの方が本体より新しいときだけ出る
    // （＝前回クラッシュした / 保存せずに落ちた）。
    // ★開いたかどうかは自前のフラグではなく ImGui に聞く。自前ラッチだと、別のモーダルに
    //   割り込まれて実際には開かなかったフレームでもラッチだけ立ち、以後二度と開かない
    //   （かつ showAutosaveRecovery が立ちっぱなしになり UpdateAutosave も止まる）。
    if (ctx.showAutosaveRecovery && !ImGui::IsPopupOpen("##AutosaveRecovery"))
        ImGui::OpenPopup("##AutosaveRecovery");

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ui::Px(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##AutosaveRecovery", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        // 保存されていない自動保存が見つかりました
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
            "\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84\xe8\x87\xaa\xe5\x8b\x95\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x8c\xe8\xa6\x8b\xe3\x81\xa4\xe3\x81\x8b\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f");
        ImGui::Spacing();
        ImGui::Text("%s", ctx.autosaveInfo.c_str());
        // シーンファイルより新しい内容です。前回、保存せずに終了した可能性があります。
        ImGui::TextDisabled("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\x88\xe3\x82\x8a\xe6\x96\xb0\xe3\x81\x97\xe3\x81\x84\xe5\x86\x85\xe5\xae\xb9\xe3\x81\xa7\xe3\x81\x99\xe3\x80\x82");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("\xe5\xbe\xa9\xe5\x85\x83\xe3\x81\x99\xe3\x82\x8b", ui::Px(140.0f, 0.0f)))  // 復元する
        {
            ctx.autosaveChoice = EditorContext::AutosaveChoice::Restore;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("\xe7\xa0\xb4\xe6\xa3\x84\xe3\x81\x99\xe3\x82\x8b", ui::Px(140.0f, 0.0f)))  // 破棄する
        {
            ctx.autosaveChoice = EditorContext::AutosaveChoice::Discard;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace dx12e
