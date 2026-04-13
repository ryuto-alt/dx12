#pragma once

#include "project/Project.h"
#include <Windows.h>
#include <vector>
#include <string>

namespace dx12e
{

class ProjectManager
{
public:
    // Recent projects
    static std::vector<ProjectInfo> GetRecents();
    static void AddToRecents(const ProjectInfo& info);

    // Open project via file dialog
    static bool OpenProjectDialog(ProjectInfo& outInfo, HWND hwnd);

    // Create new project via folder dialog
    static bool NewProjectDialog(ProjectInfo& outInfo, HWND hwnd);

    // Render the launcher UI (ImGui modal). Returns true when a project is selected
    static bool RenderLauncher(ProjectInfo& outInfo, HWND hwnd);

    // Editor state persistence (last opened scene etc.)
    static void SaveLastOpenedScene(const std::string& scenePath);
    static std::string LoadLastOpenedScene();

private:
    static std::string GetRecentsFilePath();
    static std::string GetEditorStatePath();
};

} // namespace dx12e
