#include "editor/panels/ToolbarPanel.h"
#include "editor/EditorContext.h"
#include "scripting/ScriptEngine.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "core/Window.h"
#include "core/Logger.h"

#include <commdlg.h>
#include <filesystem>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

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

    // ===== File メニュー =====
    if (ImGui::Button("\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab"))  // ファイル
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
                ctx.hotReloadFlash = 1.5f;
            }
        }

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
        if (ImGui::Button("\xe2\x96\xb6 \xe5\x86\x8d\xe7\x94\x9f"))
        {
            outPendingPlayMode = true;
            outModeChangeRequested = true;
        }
        ImGui::PopStyleColor(2);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("\xe2\x96\xa0 \xe5\x81\x9c\xe6\xad\xa2"))
        {
            outPendingPlayMode = false;
            outModeChangeRequested = true;
        }
        ImGui::PopStyleColor(2);
    }

    // ===== Status =====
    ImGui::SameLine(0, 12);
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

    // ===== Gizmo mode =====
    ImGui::SameLine(0, 12);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);

    bool isTrans = (ctx.gizmoMode == GizmoMode::Translate);
    bool isRot   = (ctx.gizmoMode == GizmoMode::Rotate);
    bool isScl   = (ctx.gizmoMode == GizmoMode::Scale);

    if (isTrans) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
    if (ImGui::Button("W Move")) ctx.gizmoMode = GizmoMode::Translate;
    if (isTrans) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isRot) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
    if (ImGui::Button("E Rot")) ctx.gizmoMode = GizmoMode::Rotate;
    if (isRot) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (isScl) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
    if (ImGui::Button("R Scl")) ctx.gizmoMode = GizmoMode::Scale;
    if (isScl) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (ImGui::Button(ctx.gizmoLocalSpace ? "Local" : "World"))
        ctx.gizmoLocalSpace = !ctx.gizmoLocalSpace;

    // ===== シーン名表示 =====
    ImGui::SameLine(0, 16);
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
    ImGui::Text("%.0f FPS", clock->GetFPS());

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
}

} // namespace dx12e
