#include "editor/panels/ToolbarPanel.h"
#include "editor/EditorContext.h"
#include "scripting/ScriptEngine.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "core/Window.h"
#include "core/Logger.h"
#include "project/ProjectManager.h"

#include <commdlg.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace
{
void OpenInVSCode(const std::string& filePath)
{
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

    ShellExecuteA(nullptr, "open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    f32 displayW = ImGui::GetIO().DisplaySize.x;

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(displayW, toolbarHeight), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.140f, 0.140f, 0.140f, 1.0f));
    ImGui::Begin("##Toolbar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);

    // アイコンボタン用ヘルパ（アイコン未読込なら従来のテキストボタンにフォールバック）。
    const EditorUiIcons* ic = ctx.icons;
    const float kIconSz = 20.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
    auto iconBtn = [&](u64 tex, const char* id, const char* fallback, const char* tip) -> bool
    {
        bool clicked = tex
            ? ImGui::ImageButton(id, static_cast<ImTextureID>(tex), ImVec2(kIconSz, kIconSz))
            : ImGui::Button(fallback);
        if (tip && *tip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
        return clicked;
    };

    // ===== File メニュー =====
    if (iconBtn(ic ? ic->file : 0, "##file",
                "\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab",  // ファイル
                "File  (New / Save / Load)"))
        ImGui::OpenPopup("FileMenu");

    if (ImGui::BeginPopup("FileMenu"))
    {
        // 新規シーン
        if (ImGui::MenuItem("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3", "Ctrl+N"))  // 新規シーン
        {
            ctx.showNewSceneDialog = true;
            ctx.newSceneDialogIsCreate = true;
            std::memset(ctx.newSceneNameBuf, 0, sizeof(ctx.newSceneNameBuf));
            strncpy_s(ctx.newSceneNameBuf, "NewScene", _TRUNCATE);
        }

        // 保存
        if (ImGui::MenuItem("\xe4\xbf\x9d\xe5\xad\x98", "Ctrl+S"))  // 保存
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
                if (GetSaveFileNameA(&ofn))
                    ctx.currentScenePath = savePath;
            }
            if (!ctx.currentScenePath.empty())
            {
                SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir);
                ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
                ctx.hotReloadFlash = 1.5f;
            }
        }

        // 名前を付けて保存
        if (ImGui::MenuItem("\xe5\x90\x8d\xe5\x89\x8d\xe3\x82\x92\xe4\xbb\x98\xe3\x81\x91\xe3\x81\xa6\xe4\xbf\x9d\xe5\xad\x98"))  // 名前を付けて保存
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
            if (GetSaveFileNameA(&ofn))
            {
                ctx.currentScenePath = savePath;
                SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir);
                ProjectManager::SaveLastOpenedScene(ctx.currentScenePath);
                ctx.hotReloadFlash = 1.5f;
            }
        }

        ImGui::Separator();

        // 新規スクリプト
        if (ImGui::MenuItem("\xe6\x96\xb0\xe8\xa6\x8f\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88", "Ctrl+L"))  // 新規スクリプト
        {
            ctx.showNewScriptDialog = true;
            std::memset(ctx.newScriptNameBuf, 0, sizeof(ctx.newScriptNameBuf));
            strncpy_s(ctx.newScriptNameBuf, "NewScript", _TRUNCATE);
        }

        ImGui::Separator();

        // 読み込み
        if (ImGui::MenuItem("\xe8\xaa\xad\xe3\x81\xbf\xe8\xbe\xbc\xe3\x81\xbf", "Ctrl+O"))  // 読み込み
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
            if (GetOpenFileNameA(&ofn))
                ctx.pendingLoadPath = loadPath;
        }

        ImGui::EndPopup();
    }

    // ===== Play/Stop =====
    ImGui::SameLine(0, 12);
    if (!isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
        if (iconBtn(ic ? ic->play : 0, "##play",
                    "\xe2\x96\xb6 \xe5\x86\x8d\xe7\x94\x9f", "Play  (enter play mode)"))
        {
            // Lua スクリプトの存在チェック
            std::string scriptCheck = assetsDir + "../scripts/game.lua";
            if (!std::filesystem::exists(scriptCheck))
            {
                ImGui::OpenPopup("##PlayWarning");
            }
            else
            {
                outPendingPlayMode = true;
                outModeChangeRequested = true;
            }
        }
        ImGui::PopStyleColor(2);

        // 警告ポップアップ
        if (ImGui::BeginPopupModal("##PlayWarning", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            ImGui::Text("\xe2\x9a\xa0 scripts/game.lua \xe3\x81\x8c\xe8\xa6\x8b\xe3\x81\xa4\xe3\x81\x8b\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93");
            ImGui::PopStyleColor();
            ImGui::TextWrapped(
                "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84\xe3\x81\xa8\xe3\x80\x81"
                "\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9\xe3\x82\x84\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x83\xad\xe3\x82\xb8\xe3\x83\x83\xe3\x82\xaf\xe3\x81\x8c\xe5\x8b\x95\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82");
            // ゲームスクリプトがないと、カメラやゲームロジックが動きません。
            ImGui::Separator();
            if (ImGui::Button("\xe3\x81\x9d\xe3\x82\x8c\xe3\x81\xa7\xe3\x82\x82\xe5\x86\x8d\xe7\x94\x9f", ImVec2(140, 0)))  // それでも再生
            {
                outPendingPlayMode = true;
                outModeChangeRequested = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab", ImVec2(140, 0)))  // キャンセル
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
        if (iconBtn(ic ? ic->stop : 0, "##stop",
                    "\xe2\x96\xa0 \xe5\x81\x9c\xe6\xad\xa2", "Stop  (back to editor)"))
        {
            outPendingPlayMode = false;
            outModeChangeRequested = true;
        }
        ImGui::PopStyleColor(2);
    }

    // ===== Status =====
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    if (isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
        ImGui::Text("\xe2\x97\x8f \xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe4\xb8\xad");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf");
    }

    // Lua error
    if (scriptEngine && !scriptEngine->GetLastError().empty())
    {
        ImGui::SameLine(0, 16);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("\xe2\x9a\xa0 Lua Error");
        ImGui::PopStyleColor();
    }

    // Hot reload flash
    if (ctx.hotReloadFlash > 0.0f)
    {
        ImGui::SameLine(0, 12);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, ctx.hotReloadFlash));
        ImGui::Text("\xe2\x9c\x93 Saved");
        ImGui::PopStyleColor();
        ctx.hotReloadFlash -= clock->GetDeltaTime();
    }

    // Error popup (中央モーダル)
    if (ctx.errorFlash > 0.0f)
    {
        ctx.errorFlash = 0.0f;  // フラグをリセット（トリガー用のみ）
        ImGui::OpenPopup("##ErrorPopup");
    }

    // ポップアップの最小サイズを設定
    ImGui::SetNextWindowSizeConstraints(ImVec2(360, 0), ImVec2(500, 300));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.15f, 0.15f, 0.18f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.35f, 0.35f, 0.6f));

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
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::Text("\xe2\x9a\xa0");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();

        ImGui::SameLine();

        // タイトル
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
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
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.12f, 1.0f));
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
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.35f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.4f, 0.4f, 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.3f, 1.0f));
        if (ImGui::Button("\xf0\x9f\x93\x8b Copy", ImVec2(120.0f, 32.0f)))
        {
            ImGui::SetClipboardText(ctx.errorMessage.c_str());
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8.0f);

        // OK ボタン
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.25f, 0.25f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
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

    // ===== Gizmo mode =====
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);

    bool isTrans = (ctx.gizmoMode == GizmoMode::Translate);
    bool isRot   = (ctx.gizmoMode == GizmoMode::Rotate);
    bool isScl   = (ctx.gizmoMode == GizmoMode::Scale);

    const ImVec4 kActiveCol(0.3f, 0.5f, 0.8f, 1.0f);

    if (isTrans) ImGui::PushStyleColor(ImGuiCol_Button, kActiveCol);
    if (iconBtn(ic ? ic->gizmoMove : 0, "##gMove", "W Move", "Move  (W)"))
        ctx.gizmoMode = GizmoMode::Translate;
    if (isTrans) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isRot) ImGui::PushStyleColor(ImGuiCol_Button, kActiveCol);
    if (iconBtn(ic ? ic->gizmoRotate : 0, "##gRot", "E Rot", "Rotate  (E)"))
        ctx.gizmoMode = GizmoMode::Rotate;
    if (isRot) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isScl) ImGui::PushStyleColor(ImGuiCol_Button, kActiveCol);
    if (iconBtn(ic ? ic->gizmoScale : 0, "##gScl", "R Scl", "Scale  (R)"))
        ctx.gizmoMode = GizmoMode::Scale;
    if (isScl) ImGui::PopStyleColor();

    ImGui::SameLine();
    u64 spaceTex = ctx.gizmoLocalSpace ? (ic ? ic->spaceLocal : 0)
                                       : (ic ? ic->spaceWorld : 0);
    if (iconBtn(spaceTex, "##space",
                ctx.gizmoLocalSpace ? "Local" : "World",
                ctx.gizmoLocalSpace ? "Local space  (click: World)"
                                    : "World space  (click: Local)"))
        ctx.gizmoLocalSpace = !ctx.gizmoLocalSpace;

    // ===== ゲームビルド =====
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.42f, 0.68f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.82f, 1.0f));
    if (iconBtn(ic ? ic->build : 0, "##build",
                "\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89", "Build game  (export)"))  // ビルド
        ctx.pendingBuildGame = true;
    ImGui::PopStyleColor(2);
    if (ctx.buildCompleteFlash > 0.0f)
    {
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
        ImGui::Text("\xe2\x9c\x93 \xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86");  // ✓ ビルド完了
        ImGui::PopStyleColor();
        ctx.buildCompleteFlash -= clock->GetDeltaTime();
    }

    // ===== シーン名表示 =====
    ImGui::SameLine(0, 16);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);
    if (!ctx.currentScenePath.empty())
    {
        std::string sceneName = std::filesystem::path(ctx.currentScenePath).stem().string();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
        ImGui::Text("[%s]", sceneName.c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("[Untitled]");
    }

    // ===== FPS =====
    ImGui::SameLine(displayW - 100);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.0f FPS", clock->GetFPS());

    ImGui::PopStyleVar();  // FramePadding
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

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
        bool enterPressed = ImGui::InputText("##SceneName", ctx.newSceneNameBuf,
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
                SceneSerializer::Save(*scene, ctx.currentScenePath, assetsDir);
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
        bool enterPressed = ImGui::InputText("##ScriptName", ctx.newScriptNameBuf,
            sizeof(ctx.newScriptNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);

        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere(-1);

        ImGui::Separator();

        bool nameValid = std::strlen(ctx.newScriptNameBuf) > 0;

        if (!nameValid) ImGui::BeginDisabled();
        if (ImGui::Button("\xe4\xbd\x9c\xe6\x88\x90", ImVec2(120, 0)) || (enterPressed && nameValid))  // 作成
        {
            // scripts/ ディレクトリにテンプレート生成
            std::string scriptsDir = assetsDir + "../scripts/";
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
                Logger::Warn("Script already exists: {}", scriptPath);
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
}

} // namespace dx12e
