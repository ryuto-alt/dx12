#include "editor/panels/ToolbarPanel.h"
#include "core/VirtualGuard.h"
#include "gui/VirtualInputImGui.h"   // dx12_imgui_find 用アンカー   // 仮想入力モード中は ShellExecute / ダイアログを実行しない
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "scripting/ScriptEngine.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneSettingsHash.h"   // 未保存判定に使う設定の指紋
#include "core/Window.h"
#include "core/Logger.h"
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

void ToolbarPanel::Render(bool isPlaying,
                          EditorContext& ctx,
                          bool& outModeChangeRequested,
                          bool& outPendingPlayMode,
                          ScriptEngine* scriptEngine,
                          GameClock* clock,
                          Scene* scene,
                          Window* window,
                          AudioSystem* /*audioSystem*/,
                          const std::string& assetsDir,
                          f32 toolbarHeight)
{
    // multi-viewport有効時、ImGui座標はスクリーン座標になるためメインビューポート原点基準で置く
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    f32 displayW = mainVp->Size.x;

    ImGui::SetNextWindowPos(mainVp->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displayW, toolbarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    // 上段(タイトルバー兼メニュー 32px) + 下段(ツール行 36px = 上下 4px の余白 + 28px のボタン)= kToolbarHeight 68。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    // メニューバー行はカスタムタイトルバーを兼ねるので縦paddingを増やして高くする(UE風の32px帯)。
    // Begin時点のFramePaddingでメニューバー高さが決まるためBeginより前にpushする。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(8, (dx12e::theme::size::kTitleBarH - ImGui::GetFontSize()) * 0.5f));
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
            const float kLogoSz = 20.0f;
            ImVec2 cur = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(ctx.icons->logo),
                ImVec2(cur.x, titleBarRect.GetCenter().y - kLogoSz * 0.5f),
                ImVec2(cur.x + kLogoSz, titleBarRect.GetCenter().y + kLogoSz * 0.5f));
            ImGui::Dummy(ImVec2(kLogoSz + 8.0f, 0.0f));
        }

        // ---- ファイル ----
        ui::PushMenuStyle();
        const bool menuOpen1 = ImGui::BeginMenu("ファイル");
        dx12e::vinput_gui::AnchorLastItem("menu", "ファイル");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen1)
        {
            if (ui::MenuItem(ICON_FILE_PLUS, "新規シーン", "Ctrl+N"))
            {
                ctx.showNewSceneDialog = true;
                ctx.newSceneDialogIsCreate = true;
                std::memset(ctx.newSceneNameBuf, 0, sizeof(ctx.newSceneNameBuf));
                strncpy_s(ctx.newSceneNameBuf, "NewScene", _TRUNCATE);
            }

            if (ui::MenuItem(ICON_FOLDER_OPEN, "シーンを開く", "Ctrl+O"))
            {
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

            ImGui::Separator();

            if (ui::MenuItem(ICON_SAVE, "保存", "Ctrl+S"))
            {
                if (ctx.currentScenePath.empty())
                {
                    char savePath[MAX_PATH] = "";
                    OPENFILENAMEA ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = window->GetHwnd();
                    ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files\0*.*\0";
                    ofn.lpstrFile = savePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrDefExt = "json";
                    ofn.Flags = OFN_OVERWRITEPROMPT;
                    std::string initDir = assetsDir + "scenes";
                    std::filesystem::create_directories(initDir);
                    ofn.lpstrInitialDir = initDir.c_str();
                    if (!dx12e::guard::Blocked("シーン保存ダイアログ") && GetSaveFileNameA(&ofn))
                        ctx.currentScenePath = savePath;
                }
                if (!ctx.currentScenePath.empty())
                {
                    // ★Save の戻り値を見ずに緑の「✓ Saved」を出し、SaveLastOpenedScene まで
                    //   走らせていた。書けていないのに保存できたように見え、しかも
                    //   プロジェクトは「そのシーンを開いていた」と記録してしまう。
                    if (SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir))
                    {
                        ctx.MarkSceneSaved(SceneSettingsFingerprint(*scene));
                        ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
                        ctx.hotReloadFlash = 1.5f;
                    }
                    else
                    {
                        Logger::Error("シーンを保存できませんでした: {}", ctx.currentScenePath);
                        ctx.saveErrorFlash = 6.0f;
                    }
                }
            }

            if (ui::MenuItem(ICON_SAVE, "名前を付けて保存"))
            {
                char savePath[MAX_PATH] = "";
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = window->GetHwnd();
                ofn.lpstrFilter = "Scene Files (*.json)\0*.json\0All Files\0*.*\0";
                ofn.lpstrFile = savePath;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrDefExt = "json";
                ofn.Flags = OFN_OVERWRITEPROMPT;
                std::string initDir = assetsDir + "scenes";
                std::filesystem::create_directories(initDir);
                ofn.lpstrInitialDir = initDir.c_str();
                if (!dx12e::guard::Blocked("シーン保存ダイアログ") && GetSaveFileNameA(&ofn))
                {
                    ctx.currentScenePath = savePath;
                    if (SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir))
                    {
                        ctx.MarkSceneSaved(SceneSettingsFingerprint(*scene));
                        ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
                        ctx.hotReloadFlash = 1.5f;
                    }
                    else
                    {
                        Logger::Error("シーンを保存できませんでした: {}", ctx.currentScenePath);
                        ctx.saveErrorFlash = 6.0f;
                    }
                }
            }

            ImGui::Separator();

            if (ui::MenuItem(ICON_FILE_CODE, "新規スクリプト", "Ctrl+L"))
            {
                ctx.showNewScriptDialog = true;
                std::memset(ctx.newScriptNameBuf, 0, sizeof(ctx.newScriptNameBuf));
                strncpy_s(ctx.newScriptNameBuf, "NewScript", _TRUNCATE);
            }

            if (ui::MenuItem(ICON_T_SHADER, "新規シェーダー"))
            {
                ctx.showNewShaderDialog = true;
                std::memset(ctx.newShaderNameBuf, 0, sizeof(ctx.newShaderNameBuf));
                strncpy_s(ctx.newShaderNameBuf, "NewShader", _TRUNCATE);
            }

            ImGui::Separator();

            // プロジェクトを閉じてランチャー（プロジェクト選択/新規作成）に戻る。
            // ファイル操作は一切不要＝現在のプロジェクトフォルダはそのまま残る。
            if (ui::MenuItem(ICON_POWER, "プロジェクトを閉じる（ランチャーに戻る）"))
                ctx.pendingCloseProject = true;

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
                if (ui::MenuItem(ICON_UNDO, undoLabel.c_str(), "Ctrl+Z", false, ctx.undoSystem.CanUndo()))
                    ctx.pendingUndo = true;
                if (ui::MenuItem(ICON_REDO, redoLabel.c_str(), "Ctrl+Y", false, ctx.undoSystem.CanRedo()))
                    ctx.pendingRedo = true;
                if (ctx.mcpUndo.TxOpen())
                    ImGui::TextDisabled("AI のまとめ操作「%s」が進行中（Ctrl+Z で確定して丸ごと戻す）",
                                        ctx.mcpUndo.TxLabel().c_str());
            }

            ImGui::Separator();

            const bool hasSel = ctx.HasSelection();
            if (ui::MenuItem(ICON_COPY, "コピー", "Ctrl+C", false, hasSel))
            {
                ctx.clipboard.clear();
                for (auto e : SceneSerializer::TopmostRoots(*scene, ctx.selectedEntities))
                {
                    std::string snap = SceneSerializer::SerializeSubtree(*scene, e, assetsDir);
                    if (!snap.empty())
                        ctx.clipboard.push_back(std::move(snap));
                }
            }
            if (ui::MenuItem(ICON_PASTE, "貼り付け", "Ctrl+V", false, !ctx.clipboard.empty()))
                ctx.pendingPastes = ctx.clipboard;
            if (ui::MenuItem(ICON_T_LAYERS, "複製", "Ctrl+D", false, hasSel))
            {
                for (auto e : ctx.selectedEntities)
                    ctx.pendingDuplications.push_back(e);
            }
            if (ui::MenuItem(ICON_TRASH, "削除", "Del", false, hasSel))
            {
                for (auto e : ctx.selectedEntities)
                    ctx.pendingDeletions.push_back(e);
            }

            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        // ---- 表示 ----
        ui::PushMenuStyle();
        const bool menuOpen3 = ImGui::BeginMenu("表示");
        dx12e::vinput_gui::AnchorLastItem("menu", "表示");   // dx12_imgui_find 用（メニューバーの項目）
        if (menuOpen3)
        {
            if (ui::MenuItem(ICON_REFRESH, "レイアウトをリセット"))
                ctx.resetLayout = true;
            ImGui::Separator();
            {
                bool fill = ctx.viewportFill > 0.0f;
                if (ui::MenuItem(ICON_T_SUN, "編集用の照らし込み", "F2", &fill))
                    ctx.viewportFill = fill ? 0.35f : 0.0f;
                if (fill)
                {
                    ImGui::SetNextItemWidth(160.0f);
                    ui::SliderFloat("##viewportFill", &ctx.viewportFill, 0.02f, 1.0f, "%.2f");
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetTooltip("暗いシーンを編集するための光。シーンには保存されず、Play 中は効かない。");
            }
            ImGui::Separator();
            ImGui::TextDisabled("ツール窓（右下に開く）");
            ui::MenuItem(ICON_T_LAYERS, "Post Process",            nullptr, &ctx.showPostProcess);
            ui::MenuItem(ICON_T_SLIDERS, "Post Process パラメータ",  nullptr, &ctx.showPostParams);
            ui::MenuItem(ICON_T_SUN, "Skybox / IBL",            nullptr, &ctx.showSkybox);
            ui::MenuItem(ICON_T_GRID, "SSAO",                    nullptr, &ctx.showSSAO);
            ui::MenuItem(ICON_T_MONITOR, "SSR / SSGI",              nullptr, &ctx.showScreenSpaceGi);
            ui::MenuItem(ICON_T_FOG, "Volumetric Fog",          nullptr, &ctx.showVolumetricFog);
            ui::MenuItem(ICON_SETTINGS, "エンジン設定",            nullptr, &ctx.showEngineSettings);
            ui::MenuItem(ICON_HAMMER, "ビルド設定",              nullptr, &ctx.showBuildSettings);
            ui::MenuItem(ICON_T_SPLINE, "Scene Flow",              nullptr, &ctx.showSceneFlow);
            ui::MenuItem(ICON_FILM, "トランジション",          nullptr, &ctx.showTransitionPreview);
            ui::MenuItem(ICON_FOLDER, "Project",                 nullptr, &ctx.showProject);
            ui::MenuItem(ICON_GIT_BRANCH, "Git 変更",                nullptr, &ctx.showVersionControl);
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
            // シーンの光を1画面で詰めるパネル（太陽/影/スカイ/プリセット）
            ui::MenuItem(ICON_T_LIGHT, "ライティング",             nullptr, &ctx.showLighting);
            // 追いかける AI 用の経路探索メッシュを焼く窓
            ui::MenuItem(ICON_T_NAV, "ナビメッシュ",             nullptr, &ctx.showNavMesh);
            // バスのメーター/フェーダー・スナップショット・鳴っている音
            ui::MenuItem(ICON_T_AUDIO, "オーディオミキサー",       nullptr, &ctx.showAudioMixer);
            ui::MenuItem(ICON_T_PARTICLE, "パーティクルエディタ",     nullptr, &ctx.showVfxEditor);
            ui::MenuItem(ICON_T_ANIM, "UIアニメーション",         nullptr, &ctx.showAnimEditor);
            ui::MenuItem(ICON_T_SPRITE, "スプライトシート",         nullptr, &ctx.showSpriteSheetEditor);
            ui::MenuItem(ICON_T_MATERIAL, "マテリアルエディタ",       nullptr, &ctx.showMaterialEditor);
            ui::MenuItem(ICON_PACKAGE, "マテリアルライブラリ (Poly Haven)", nullptr, &ctx.showMaterialLibrary);
            ui::MenuItem(ICON_CLOUD, "MCP / AI Bridge",         nullptr, &ctx.showMcpBridge);
            ui::MenuItem(ICON_T_NET, "Network",                 nullptr, &ctx.showNetworkStatus);
            ui::MenuItem(ICON_SETTINGS, "Network 設定",             nullptr, &ctx.showNetworkSettings);
            ImGui::Separator();
            ui::MenuItem(ICON_BUG, "エンジン診断 (UI 自動テスト)", nullptr, &ctx.showEngineDiagnostics);
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
            if (ui::MenuItem(ICON_INFO, "バージョン情報"))
                openAboutPopup = true;
            ImGui::EndMenu();
        }
        ui::PopMenuStyle();

        const float menusEndX = ImGui::GetCursorScreenPos().x;   // メニュー列の右端(中央題字の重なり回避)
        const float barH      = titleBarRect.GetHeight();

        // ---- 中央: シーン名(UEのプロジェクト名表示の位置) ----
        {
            std::string title = "DX12 Engine";
            if (!ctx.currentScenePath.empty())
            {
                // パス区切り・拡張子を手で剥がす(UTF-8のままでも安全な操作だけにする)
                std::string name = ctx.currentScenePath;
                if (size_t p = name.find_last_of("/\\"); p != std::string::npos) name = name.substr(p + 1);
                if (size_t d = name.find_last_of('.');   d != std::string::npos) name = name.substr(0, d);
                if (!name.empty()) title = name + " - DX12 Engine";
            }
            // 未保存なら先頭に * を出す（エディタの慣習。ここが唯一の常時見える手掛かり）
            if (ctx.IsSceneDirty()) title = "*" + title;
            // 版は Version.cpp が単一ソース。題字の右に薄く添える（上段のロゴ + 題字で 1 か所にまとめた）。
            const std::string ver = std::string("v") + kEngineVersion;
            const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            const ImVec2 vs = ImGui::CalcTextSize(ver.c_str());
            const float totalW = ts.x + 10.0f + vs.x;
            float cx = titleBarRect.Min.x + (titleBarRect.GetWidth() - totalW) * 0.5f;
            cx = (std::max)(cx, menusEndX + 24.0f);   // 窓が狭い時はメニューの右に退避
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(cx, titleBarRect.GetCenter().y - ts.y * 0.5f),
                ImGui::GetColorU32(dx12e::theme::TextDim), title.c_str());
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(cx + ts.x + 10.0f, titleBarRect.GetCenter().y - vs.y * 0.5f),
                ImGui::GetColorU32(dx12e::theme::TextFaint), ver.c_str());
        }

        // ---- 右端: 最小化 / 最大化(復元) / 閉じる(OS標準の代替。グリフはDrawListで描く) ----
        if (window && window->IsCustomTitleBar())
        {
            const float btnW = 44.0f;
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
                const float r = 5.0f;   // グリフ半径
                if (glyph == 0)          // ─
                    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, 1.0f);
                else if (glyph == 1)     // □ / ❐(復元)
                {
                    if (window->IsMaximized())
                    {
                        dl->AddRect(ImVec2(c.x - r, c.y - r + 2), ImVec2(c.x + r - 2, c.y + r), fg, 1.0f, 0, 1.0f);
                        dl->AddLine(ImVec2(c.x - r + 2, c.y - r + 2), ImVec2(c.x - r + 2, c.y - r), fg, 1.0f);
                        dl->AddLine(ImVec2(c.x - r + 2, c.y - r), ImVec2(c.x + r, c.y - r), fg, 1.0f);
                        dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x + r, c.y + r - 2), fg, 1.0f);
                        dl->AddLine(ImVec2(c.x + r, c.y + r - 2), ImVec2(c.x + r - 2, c.y + r - 2), fg, 1.0f);
                    }
                    else
                        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 1.0f, 0, 1.0f);
                }
                else                     // ✕
                {
                    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 1.0f);
                    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), fg, 1.0f);
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

    if (openShortcutsPopup) ImGui::OpenPopup("ショートカット一覧##ShortcutsPopup");
    if (openAboutPopup)     ImGui::OpenPopup("バージョン情報##AboutPopup");

    // ===== ツール行（フラットなアイコンボタン + 縦区切り。UE5 風）=====
    // ボタンは 28x28 の正方形・ラベル無し（ホバーでツールチップ）。それより低い部品（コンボ・文字）は
    // 縦中央へ置く。配置はカーソルを明示指定する（SameLine は高さの違う部品の縦位置が揃わないため）。
    namespace th = dx12e::theme;
    const float kBtn   = th::size::kToolbarBtn;
    const float rowTop = ImGui::GetCursorScreenPos().y;
    const float rowMid = rowTop + kBtn * 0.5f;
    float x = mainVp->Pos.x + 10.0f;
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    const float frameH = ImGui::GetFrameHeight();
    const float lineH  = ImGui::GetTextLineHeight();
    auto put = [&](float w, float h) { ImGui::SetCursorScreenPos(ImVec2(x, std::floor(rowMid - h * 0.5f + 0.5f))); x += w; };
    auto iconBtn = [&](const char* id, const char* glyph, const char* tip, bool active, const char* anchor,
                       const ImVec4* tint = nullptr, const ImVec4* face = nullptr) -> bool
    {
        put(kBtn + 2.0f, kBtn);
        const bool c = ui::IconButton(id, glyph, tip, active, tint, kBtn, 0.0f, face);
        dx12e::vinput_gui::AnchorLastItem("button", anchor);   // dx12_imgui_find 用（従来のラベル名で引ける）
        return c;
    };
    auto sep = [&]()
    {
        x += 6.0f;
        tdl->AddLine(ImVec2(x, rowMid - 10.0f), ImVec2(x, rowMid + 10.0f), ImGui::GetColorU32(th::BorderStrong));
        x += 8.0f;
    };

    // ===== ギズモ（移動 / 回転 / 拡縮 / 空間）=====
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

    // ===== 2D / 3D ビュー切替（Unity の 2D ボタン相当）+ UI 編集モード =====
    sep();
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
    put(kBtn + 14.0f + 2.0f, kBtn);
    if (ui::IconDropdownButton("windows", ICON_WINDOWS, "ツール窓の表示/非表示", ctx.AnyToolWindowOpen(), kBtn))
        ImGui::OpenPopup("##ToolWindowsMenu");
    dx12e::vinput_gui::AnchorLastItem("button", "窓");
    ui::PushMenuStyle();
    if (ImGui::BeginPopup("##ToolWindowsMenu"))
    {
        ImGui::TextDisabled("ツール窓（右下にタブで開く）");
        ImGui::Separator();
        ui::MenuItem(ICON_T_LAYERS,  "Post Process",           nullptr, &ctx.showPostProcess);
        ui::MenuItem(ICON_T_SLIDERS, "Post Process パラメータ", nullptr, &ctx.showPostParams);
        ui::MenuItem(ICON_T_SUN,     "Skybox / IBL",           nullptr, &ctx.showSkybox);
        ui::MenuItem(ICON_T_GRID,    "SSAO",                   nullptr, &ctx.showSSAO);
        ui::MenuItem(ICON_T_MONITOR, "SSR / SSGI",             nullptr, &ctx.showScreenSpaceGi);
        ui::MenuItem(ICON_T_FOG,     "Volumetric Fog",         nullptr, &ctx.showVolumetricFog);
        ui::MenuItem(ICON_SETTINGS,  "エンジン設定",           nullptr, &ctx.showEngineSettings);
        ui::MenuItem(ICON_HAMMER,    "ビルド設定",             nullptr, &ctx.showBuildSettings);
        ui::MenuItem(ICON_T_SPLINE,  "Scene Flow",             nullptr, &ctx.showSceneFlow);
        ui::MenuItem(ICON_FOLDER,    "Project",                nullptr, &ctx.showProject);
        ui::MenuItem(ICON_GIT_BRANCH, "Git 変更",              nullptr, &ctx.showVersionControl);
        ui::MenuItem(ICON_CLOUD,     "MCP / AI Bridge",        nullptr, &ctx.showMcpBridge);
        ui::MenuItem(ICON_T_NET,     "Network",                nullptr, &ctx.showNetworkStatus);
        ui::MenuItem(ICON_SETTINGS,  "Network 設定",            nullptr, &ctx.showNetworkSettings);
        ui::MenuItem(ICON_T_LIGHT,   "ライティング",            nullptr, &ctx.showLighting);
        ui::MenuItem(ICON_T_NAV,     "ナビメッシュ",            nullptr, &ctx.showNavMesh);
        ui::MenuItem(ICON_T_AUDIO,   "オーディオミキサー",      nullptr, &ctx.showAudioMixer);
        ui::MenuItem(ICON_T_PARTICLE, "パーティクルエディタ",    nullptr, &ctx.showVfxEditor);
        ui::MenuItem(ICON_T_UI,      "UIエディタ",              nullptr, &ctx.showUiEditor);
        ui::MenuItem(ICON_T_ANIM,    "UIアニメーション",        nullptr, &ctx.showAnimEditor);
        ui::MenuItem(ICON_T_SPRITE,  "スプライトシート",        nullptr, &ctx.showSpriteSheetEditor);
        ui::MenuItem(ICON_FILM,      "トランジション",          nullptr, &ctx.showTransitionPreview);
        ImGui::Separator();
        if (ui::MenuItem(ICON_CLOSE, "すべて閉じる"))
        {
            ctx.showScreenSpaceGi = ctx.showVolumetricFog =
            ctx.showPostProcess = ctx.showPostParams = ctx.showSkybox = ctx.showSSAO =
                ctx.showEngineSettings = ctx.showSceneFlow = ctx.showProject =
                ctx.showVersionControl = ctx.showMcpBridge = ctx.showBuildSettings =
                ctx.showNetworkStatus = ctx.showNetworkSettings =
                ctx.showVfxEditor = ctx.showUiEditor =
                ctx.showAnimEditor = ctx.showSpriteSheetEditor =
                ctx.showTransitionPreview = false;
            ctx.showLighting = false;
            ctx.showAudioMixer = false;
        }
        ImGui::EndPopup();
    }
    ui::PopMenuStyle();

    // ===== Play コントロール（画面中央。UE5 と同じ配置）=====
    // Play ボタンが常に画面中央へ来るよう配置（左のツール群と重なる狭い窓では右へ退避）。
    // フラットなアイコン: 停止中は緑の再生アイコン、Play 中は赤い停止アイコン（押下状態 = 薄い赤の面）。
    x = (std::max)(x + 12.0f, mainVp->Pos.x + displayW * 0.5f - kBtn * 0.5f);
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
    x += 10.0f;
    put(150.0f + 8.0f, frameH);
    ImGui::BeginDisabled(isPlaying);
    {
        const char* roleLabels[] = { "オフライン", "ホストとしてPlay", "クライアント参加" };
        int roleIdx = static_cast<int>(ctx.netTestRole);
        ImGui::SetNextItemWidth(150);
        if (ui::Combo("##net_test_role", &roleIdx, roleLabels, IM_ARRAYSIZE(roleLabels)))
            ctx.netTestRole = static_cast<NetTestRole>(roleIdx);
    }
    ImGui::EndDisabled();

    if (ctx.netTestRole == NetTestRole::Client)
    {
        put(120.0f + 6.0f, frameH);
        ImGui::BeginDisabled(isPlaying);
        char ipBuf[64];
        strncpy_s(ipBuf, ctx.netTestJoinAddress.c_str(), _TRUNCATE);
        ImGui::SetNextItemWidth(120);
        if (ui::InputText("##net_test_ip", ipBuf, sizeof(ipBuf)))
            ctx.netTestJoinAddress = ipBuf;
        ImGui::EndDisabled();
    }

    if (isPlaying && ctx.netTestRole == NetTestRole::Host)
    {
        const char* lbl = "テストクライアント起動";
        const float bw = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        put(bw + 6.0f, frameH);
        if (ImGui::Button(lbl))
            ctx.netTestLaunchClientRequested = true;
        put(20.0f, frameH);
        ImGui::TextDisabled("%s", ICON_HELP);
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("同じプロジェクトを --net-client 127.0.0.1:<port> でもう1つ起動して"
                                   "\n自動でこのホストへ接続します(検証用の別ウィンドウ)。");
            ImGui::EndTooltip();
        }
    }

    // ===== Status =====
    x += 8.0f;
    {
        const char* st = isPlaying ? ICON_PLAY " プレイ中" : "エディタ";
        put(ImGui::CalcTextSize(st).x + 16.0f, lineH);
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
        put(ImGui::CalcTextSize(msg).x + 16.0f, lineH);
        ImGui::PushStyleColor(ImGuiCol_Text, th::Bad);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
    }

    // Hot reload flash
    if (ctx.hotReloadFlash > 0.0f)
    {
        const char* msg = ICON_CHECK " 保存しました";
        put(ImGui::CalcTextSize(msg).x + 16.0f, lineH);
        ImGui::PushStyleColor(ImGuiCol_Text, th::WithAlpha(th::Good, (std::min)(1.0f, ctx.hotReloadFlash)));
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ctx.hotReloadFlash -= clock->GetDeltaTime();
    }
    // 保存失敗。緑より長く出す（見逃すと書けていないことに気づけない）
    if (ctx.saveErrorFlash > 0.0f)
    {
        const char* msg = ICON_CLOSE " 保存に失敗 (dx12_engine.log)";
        put(ImGui::CalcTextSize(msg).x + 16.0f, lineH);
        ImGui::PushStyleColor(ImGuiCol_Text, th::Bad);
        ImGui::TextUnformatted(msg);
        ImGui::PopStyleColor();
        ctx.saveErrorFlash -= clock->GetDeltaTime();
    }

    // ※ FPS/描画統計は下部ステータスバー(EditorLayer::RenderStatusBar)に集約した。
    //    ここに出すと同じ数字が2箇所に並ぶだけなので置かない。

    // Error popup (中央モーダル)
    if (ctx.errorFlash > 0.0f)
    {
        ctx.errorFlash = 0.0f;  // フラグをリセット（トリガー用のみ）
        ImGui::OpenPopup("##ErrorPopup");
    }

    // ポップアップの最小サイズを設定
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(500, 300));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, dx12e::theme::Bg2);
    ImGui::PushStyleColor(ImGuiCol_Border, dx12e::theme::WithAlpha(dx12e::theme::Bad, 0.6f));

    if (ImGui::BeginPopupModal("##ErrorPopup", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar))
    {
        // ウィンドウ中央に配置
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetWindowPos(ImVec2(center.x - ImGui::GetWindowWidth() * 0.5f,
                                    center.y - ImGui::GetWindowHeight() * 0.5f));

        // 警告アイコン（大）
        ImGui::PushFont(nullptr);  // デフォルトフォント
        ImGui::SetWindowFontScale(2.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::Bad);
        ImGui::TextUnformatted(ICON_WARN);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        ImGui::SameLine();

        // タイトル
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, dx12e::theme::Bad);
        ImGui::SetWindowFontScale(1.3f);
        ImGui::Text("Play \xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93");  // Playできません
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // メッセージ本文（選択・コピー可能な InputText）
        ImGui::SetWindowFontScale(1.1f);
        static char errorBuf[512] = {};
        strncpy_s(errorBuf, ctx.errorMessage.c_str(), _TRUNCATE);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, dx12e::theme::InputBg);
        ImGui::InputTextMultiline("##ErrorMsg", errorBuf, sizeof(errorBuf),
            ImVec2(-1, ImGui::GetTextLineHeight() * 3.5f),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);

        ImGui::Spacing();

        // コピー + OK ボタン
        float totalWidth = 120.0f + 8.0f + 120.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        // コピーボタン
        ImGui::PushStyleColor(ImGuiCol_Button, dx12e::theme::Bg3);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dx12e::theme::Bg4);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, dx12e::theme::Bg4);
        if (ImGui::Button(ICON_COPY " コピー", ImVec2(120.0f, 32.0f)))
        {
            ImGui::SetClipboardText(ctx.errorMessage.c_str());
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8.0f);

        // OK ボタン
        ImGui::PushStyleColor(ImGuiCol_Button, dx12e::theme::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dx12e::theme::AccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, dx12e::theme::AccentPressed);
        if (ImGui::Button("OK", ImVec2(120.0f, 32.0f)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::PopStyleVar();

        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);   // WindowPadding + WindowBorderSize

    // ===== ヘルプ: ショートカット一覧モーダル =====
    {
        ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("ショートカット一覧##ShortcutsPopup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            struct KeyRow { const char* key; const char* desc; };
            static const KeyRow rows[] = {
                {"W / E / R",     "ギズモ切替（移動 / 回転 / スケール）"},
                {"T",             "ローカル / ワールド空間の切替"},
                {"左クリック",    "エンティティ選択（Ctrl+クリックで複数選択）"},
                {"右クリック+WASD","フライカメラ移動（Space/Shift で上下）"},
                {"F",             "選択エンティティにフォーカス"},
                {"F2",            "編集用の照らし込み（暗いシーンを見る。ゲームには影響なし）"},
                {"L + マウス移動", "太陽（DirectionalLight）の向きを直接回す"},
                {"F11",           "ボーダレスフルスクリーン切替"},
                {"Ctrl+Z / Y",   "元に戻す / やり直す"},
                {"Ctrl+C / V",   "コピー / 貼り付け"},
                {"Ctrl+D",        "複製"},
                {"Del",           "削除"},
                {"Ctrl+S / N",   "シーン保存 / 新規シーン"},
                {"Ctrl+O / L",   "シーンを開く / 新規スクリプト"},
            };
            if (ImGui::BeginTable("##shortcuts", 2,
                    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("キー", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("動作", ImGuiTableColumnFlags_WidthStretch);
                for (const auto& r : rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                    ImGui::TextUnformatted(r.key);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(r.desc);
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            if (ImGui::Button("閉じる", ImVec2(120, 0)))
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
            ImGui::Text("DX12 Engine");
            ImGui::TextDisabled("DirectX 12 ゲームエンジン + エディタ");
            ImGui::Separator();
            ImGui::TextUnformatted("https://github.com/ryuto-alt/dx12");
            ImGui::Spacing();
            if (ImGui::Button("閉じる", ImVec2(120, 0)))
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
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Appearing);
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
        if (ImGui::Button(btnLabel, ImVec2(120, 0)) || (enterPressed && nameValid))
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
                if (SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir))
                    ctx.MarkSceneSaved(SceneSettingsFingerprint(*scene));
                ctx.hotReloadFlash = 1.5f;
            }
            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
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
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Appearing);
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
        if (ImGui::Button("\xe4\xbd\x9c\xe6\x88\x90", ImVec2(120, 0)) || (enterPressed && nameValid))  // 作成
        {
            // assets/scripts/ にテンプレート生成（プロジェクト内＝アセットブラウザに表示され、
            // scriptPath="scripts/<name>.lua" で添付できる。旧 assets/../scripts はブラウザ外だった）
            std::string scriptsDir = assetsDir + "scripts/";
            std::filesystem::create_directories(scriptsDir);
            std::string scriptPath = scriptsDir + ctx.newScriptNameBuf + ".lua";

            if (!std::filesystem::exists(scriptPath))
            {
                std::ofstream ofs(scriptPath);
                ofs << "-- " << ctx.newScriptNameBuf << ".lua\n\n";
                ofs << "function OnStart()\n";
                ofs << "    -- Called once when play mode starts\n";
                ofs << "end\n\n";
                ofs << "function OnUpdate(dt)\n";
                ofs << "    -- Called every frame\n";
                ofs << "end\n";
                ofs.close();

                Logger::Info("Created script: {}", scriptPath);
                ctx.hotReloadFlash = 1.5f;
            }
            else
            {
                Logger::Warn("スクリプトは既に存在します: {}", scriptPath);
            }

            // VS Code で開く
            OpenInVSCode(scriptPath);

            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
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
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Appearing);
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
        if (ImGui::Button("\xe4\xbd\x9c\xe6\x88\x90", ImVec2(120, 0)) || (enterPressed && nameValid))  // 作成
        {
            // assets/shaders/ にテンプレート生成（プロジェクト独自シェーダーの置き場。
            // ShaderManager がここを走査し、Registryに無いものはカスタムとしてホットリロード対象になる）。
            std::string shadersDir = assetsDir + "shaders/";
            std::filesystem::create_directories(shadersDir);
            std::string shaderPath = shadersDir + ctx.newShaderNameBuf + ".hlsl";

            if (!std::filesystem::exists(shaderPath))
            {
                std::ofstream ofs(shaderPath);
                ofs << (shaderKind == 1 ? kNewScreenShaderTemplate : kNewShaderTemplate);
                ofs.close();

                Logger::Info("Created shader: {}", shaderPath);
                ctx.hotReloadFlash = 1.5f;
            }
            else
            {
                Logger::Warn("シェーダーは既に存在します: {}", shaderPath);
            }

            // VS Code で開く
            OpenInVSCode(shaderPath);

            ImGui::CloseCurrentPopup();
        }
        if (!nameValid) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0)))  // キャンセル
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
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
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
        if (ImGui::Button("\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x97\xe3\x81\xa6\xe7\xb6\x9a\xe8\xa1\x8c", ImVec2(132, 0)))  // 保存して続行
        {
            ctx.unsavedChoice = EditorContext::UnsavedChoice::Save;
            ImGui::CloseCurrentPopup();
        }
        if (!canSave) ImGui::EndDisabled();

        ImGui::SameLine();
        // 破棄は誤爆すると作業が消えるので赤くしておく（既定フォーカスも持たせない）
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button("\xe4\xbf\x9d\xe5\xad\x98\xe3\x81\x9b\xe3\x81\x9a\xe7\xb6\x9a\xe8\xa1\x8c", ImVec2(132, 0)))  // 保存せず続行
        {
            ctx.unsavedChoice = EditorContext::UnsavedChoice::Discard;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(120, 0))  // キャンセル
            || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            ctx.unsavedChoice = EditorContext::UnsavedChoice::Cancel;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
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

        if (ImGui::Button("\xe5\xbe\xa9\xe5\x85\x83\xe3\x81\x99\xe3\x82\x8b", ImVec2(140, 0)))  // 復元する
        {
            ctx.autosaveChoice = EditorContext::AutosaveChoice::Restore;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("\xe7\xa0\xb4\xe6\xa3\x84\xe3\x81\x99\xe3\x82\x8b", ImVec2(140, 0)))  // 破棄する
        {
            ctx.autosaveChoice = EditorContext::AutosaveChoice::Discard;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace dx12e
