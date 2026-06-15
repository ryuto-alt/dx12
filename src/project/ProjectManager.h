#pragma once

#include "project/Project.h"
#include <Windows.h>
#include <vector>
#include <string>

namespace dx12e
{

// ランチャー/各ボタンで使う UI アイコンの ImTextureID(=ImU64) 群。
// 0 のものはアイコン無しでテキストのみ表示する。
struct LauncherIcons
{
    unsigned long long logo        = 0;
    unsigned long long newProject  = 0;
    unsigned long long openProject = 0;
    unsigned long long recent      = 0;
};

// ランチャーの結果。どのアクションが選ばれたか + 選ばれたプロジェクト情報。
enum class LauncherAction { None, OpenExisting, CreateNew, Skip };

class ProjectManager
{
public:
    // Recent projects
    static std::vector<ProjectInfo> GetRecents();
    static void AddToRecents(const ProjectInfo& info);

    // Open project via file dialog（既存プロジェクトを読み込む。即ロード）
    static bool OpenProjectDialog(ProjectInfo& outInfo, HWND hwnd);

    // 新規プロジェクトの作成先フォルダをモダンダイアログで選ぶ。
    // ★ディスクへの書き込みはしない（非同期で作成するため info を埋めるだけ）。
    static bool NewProjectDialog(ProjectInfo& outInfo, HWND hwnd);

    // モダンなフォルダ選択ダイアログ（IFileOpenDialog / FOS_PICKFOLDERS）。
    static bool PickFolder(HWND hwnd, std::string& outPath, const wchar_t* title);

    // Render the launcher UI (ImGui modal).
    // 戻り値で選ばれたアクションを返す。OpenExisting/CreateNew のとき outInfo を埋める。
    static LauncherAction RenderLauncher(ProjectInfo& outInfo, HWND hwnd,
                                         const LauncherIcons& icons = {});

    // Editor state persistence (last opened scene etc.)
    static void SaveLastOpenedScene(const std::string& scenePath);
    static std::string LoadLastOpenedScene();

private:
    static std::string GetRecentsFilePath();
    static std::string GetEditorStatePath();
};

} // namespace dx12e
