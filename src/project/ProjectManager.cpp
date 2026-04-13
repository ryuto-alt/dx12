#include "project/ProjectManager.h"
#include "core/Logger.h"

#include <Windows.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <commdlg.h>
#include <ShlObj.h>
#include <nlohmann/json.hpp>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e
{

std::string ProjectManager::GetRecentsFilePath()
{
    char appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath)))
    {
        std::filesystem::path dir = std::filesystem::path(appDataPath) / "DX12Engine";
        std::filesystem::create_directories(dir);
        return (dir / "recent.json").string();
    }
    return "recent.json";
}

std::vector<ProjectInfo> ProjectManager::GetRecents()
{
    std::vector<ProjectInfo> recents;
    std::string path = GetRecentsFilePath();

    std::ifstream ifs(path);
    if (!ifs.is_open()) return recents;

    nlohmann::json j;
    ifs >> j;

    for (const auto& entry : j.value("recents", nlohmann::json::array()))
    {
        ProjectInfo info;
        info.name    = entry.value("name", "");
        info.rootDir = entry.value("path", "");
        if (!info.name.empty() && !info.rootDir.empty())
            recents.push_back(info);
    }

    return recents;
}

void ProjectManager::AddToRecents(const ProjectInfo& info)
{
    auto recents = GetRecents();

    // Remove existing entry with same path
    recents.erase(
        std::remove_if(recents.begin(), recents.end(),
            [&](const ProjectInfo& p) { return p.rootDir == info.rootDir; }),
        recents.end());

    // Insert at front
    recents.insert(recents.begin(), info);

    // Limit to 10 recent projects
    if (recents.size() > 10)
        recents.resize(10);

    // Save
    nlohmann::json j;
    j["recents"] = nlohmann::json::array();
    for (const auto& r : recents)
    {
        nlohmann::json entry;
        entry["name"] = r.name;
        entry["path"] = r.rootDir;
        j["recents"].push_back(entry);
    }

    std::ofstream ofs(GetRecentsFilePath());
    if (ofs.is_open())
        ofs << j.dump(2);
}

bool ProjectManager::OpenProjectDialog(ProjectInfo& outInfo, HWND hwnd)
{
    char filePath[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "DX12 Project (*.dx12proj)\0*.dx12proj\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;

    if (GetOpenFileNameA(&ofn))
    {
        if (Project::Load(filePath, outInfo))
        {
            AddToRecents(outInfo);
            return true;
        }
    }
    return false;
}

bool ProjectManager::NewProjectDialog(ProjectInfo& outInfo, HWND hwnd)
{
    // Use folder browser dialog
    BROWSEINFOA bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select folder for new project";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;

    char folderPath[MAX_PATH];
    SHGetPathFromIDListA(pidl, folderPath);
    CoTaskMemFree(pidl);

    std::filesystem::path projDir(folderPath);
    outInfo.name         = projDir.filename().string();
    outInfo.rootDir      = projDir.string();
    outInfo.assetsDir    = (projDir / "assets").string() + "/";
    outInfo.scriptsDir   = (projDir / "scripts").string() + "/";
    outInfo.defaultScene = "scenes/default.json";

    Project::CreateDefaultStructure(outInfo);
    Project::Save(outInfo, (projDir / (outInfo.name + ".dx12proj")).string());
    AddToRecents(outInfo);

    return true;
}

bool ProjectManager::RenderLauncher(ProjectInfo& outInfo, HWND hwnd)
{
    bool projectSelected = false;

    // Center modal
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_Always);

    ImGui::Begin("DX12 Engine - Project Launcher", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.39f, 0.58f, 0.93f, 1.0f));
    ImGui::Text("DX12 Engine v0.1");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // New / Open buttons
    if (ImGui::Button("\xe2\x9c\x9a \xe6\x96\xb0\xe8\xa6\x8f\xe3\x83\x97\xe3\x83\xad\xe3\x82\xb8\xe3\x82\xa7\xe3\x82\xaf\xe3\x83\x88", ImVec2(230, 35)))  // New Project
    {
        if (NewProjectDialog(outInfo, hwnd))
            projectSelected = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("\xf0\x9f\x93\x82 \xe3\x83\x97\xe3\x83\xad\xe3\x82\xb8\xe3\x82\xa7\xe3\x82\xaf\xe3\x83\x88\xe3\x82\x92\xe9\x96\x8b\xe3\x81\x8f", ImVec2(230, 35)))  // Open Project
    {
        if (OpenProjectDialog(outInfo, hwnd))
            projectSelected = true;
    }

    ImGui::Separator();
    ImGui::Text("\xe6\x9c\x80\xe8\xbf\x91\xe3\x81\xae\xe3\x83\x97\xe3\x83\xad\xe3\x82\xb8\xe3\x82\xa7\xe3\x82\xaf\xe3\x83\x88:");  // Recent Projects:

    auto recents = GetRecents();
    if (recents.empty())
    {
        ImGui::TextDisabled("\xe3\x81\xbe\xe3\x81\xa0\xe3\x83\x97\xe3\x83\xad\xe3\x82\xb8\xe3\x82\xa7\xe3\x82\xaf\xe3\x83\x88\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93");  // No projects yet
    }
    else
    {
        for (const auto& recent : recents)
        {
            ImGui::PushID(recent.rootDir.c_str());

            // Check if project file still exists
            std::filesystem::path projFile = std::filesystem::path(recent.rootDir) / (recent.name + ".dx12proj");
            bool exists = std::filesystem::exists(projFile);

            if (!exists)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

            if (ImGui::Selectable(recent.name.c_str(), false, ImGuiSelectableFlags_None, ImVec2(0, 30)))
            {
                if (exists && Project::Load(projFile.string(), outInfo))
                {
                    AddToRecents(outInfo);
                    projectSelected = true;
                }
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", recent.rootDir.c_str());

            if (!exists)
                ImGui::PopStyleColor();

            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // Skip button (use default project for development)
    if (ImGui::Button("\xe3\x82\xb9\xe3\x82\xad\xe3\x83\x83\xe3\x83\x97 (\xe3\x83\x87\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x88)", ImVec2(-1, 30)))  // Skip (Default)
    {
        // Use current working directory as project
        outInfo.name       = "Default";
        outInfo.rootDir    = "";  // Signal to use built-in paths
        projectSelected = true;
    }

    ImGui::End();

    return projectSelected;
}

} // namespace dx12e
