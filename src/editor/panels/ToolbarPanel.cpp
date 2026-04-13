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

    // Play/Stop
    if (!isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
        if (ImGui::Button("\xe2\x96\xb6 \xe5\x86\x8d\xe7\x94\x9f"))  // Play
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
        if (ImGui::Button("\xe2\x96\xa0 \xe5\x81\x9c\xe6\xad\xa2"))  // Stop
        {
            outPendingPlayMode = false;
            outModeChangeRequested = true;
        }
        ImGui::PopStyleColor(2);
    }

    ImGui::SameLine(0, 12);
    if (isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
        ImGui::Text("\xe2\x97\x8f \xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe4\xb8\xad");  // Playing
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf");  // Editor
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
        ImGui::Text("\xe2\x9c\x93 Reloaded");
        ImGui::PopStyleColor();
        ctx.hotReloadFlash -= clock->GetDeltaTime();
    }

    // Gizmo mode buttons
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

    // Save/Load
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);

    if (ImGui::Button("Save"))
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
            Logger::Info("Scene saved: {}", ctx.currentScenePath);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Load"))
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
        {
            ctx.pendingLoadPath = loadPath;
        }
    }

    // FPS
    ImGui::SameLine(displayW - 100);
    ImGui::Text("%.0f FPS", clock->GetFPS());

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace dx12e
