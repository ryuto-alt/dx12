#include "project/ProjectManager.h"
#include "project/GitIntegration.h"
#include "core/Logger.h"

#include <Windows.h>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>
#include <thread>
#include <commdlg.h>
#include <ShlObj.h>
#include <shobjidl.h>
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

bool ProjectManager::PickFolder(HWND /*hwnd*/, std::string& outPath, const wchar_t* title)
{
    // ★メインスレッドは XAudio2 が COINIT_MULTITHREADED(MTA) で COM 初期化済み。
    //   IFileOpenDialog は STA を要求するため、MTA スレッドで Show() すると固まる。
    //   → 専用の STA スレッドで開く。オーナー window は渡さない（メインスレッドが
    //     join() でブロック中なので、クロススレッド SendMessage によるデッドロックを避ける）。
    std::string picked;
    bool ok = false;

    std::thread worker([&]()
    {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hrInit))
            return;

        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dlg))))
        {
            DWORD opts = 0;
            dlg->GetOptions(&opts);
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (title) dlg->SetTitle(title);

            if (SUCCEEDED(dlg->Show(nullptr)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)))
                {
                    PWSTR pathW = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathW)) && pathW)
                    {
                        int len = WideCharToMultiByte(CP_UTF8, 0, pathW, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 0)
                        {
                            std::string s(static_cast<size_t>(len), '\0');
                            WideCharToMultiByte(CP_UTF8, 0, pathW, -1, s.data(), len, nullptr, nullptr);
                            s.pop_back();
                            picked = s;
                            ok = true;
                        }
                        CoTaskMemFree(pathW);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        CoUninitialize();
    });
    worker.join();

    if (ok) outPath = picked;
    return ok;
}

bool ProjectManager::NewProjectDialog(ProjectInfo& outInfo, HWND hwnd)
{
    // 作成先フォルダを選ぶだけ。ディスク作成は呼び出し側が非同期で行う。
    std::string folder;
    if (!PickFolder(hwnd, folder, L"新規プロジェクトの作成先フォルダを選択"))
        return false;

    std::filesystem::path projDir(folder);
    outInfo.name         = projDir.filename().string();
    if (outInfo.name.empty()) outInfo.name = "MyGame";
    outInfo.rootDir      = projDir.string();
    outInfo.assetsDir    = (projDir / "assets").string() + "/";
    outInfo.scriptsDir   = (projDir / "scripts").string() + "/";
    outInfo.defaultScene = "scenes/main.json";
    // templateId は呼び出し側(ランチャーのテンプレ選択)が設定する
    return true;
}

// クローンしたフォルダから ProjectInfo を組み立てる。
// .dx12proj があれば読み込み、無ければフォルダをプロジェクトルートとして合成する。
static bool MakeProjectFromFolder(const std::string& repoDir, ProjectInfo& out)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(repoDir, ec)) return false;

    // .dx12proj を探す
    for (auto& e : fs::directory_iterator(repoDir, ec))
    {
        if (e.is_regular_file() && e.path().extension() == ".dx12proj")
        {
            if (Project::Load(e.path().string(), out))
                return true;
        }
    }

    // 無ければフォルダをそのままプロジェクトとして扱う
    fs::path dir(repoDir);
    out = ProjectInfo{};
    out.name         = dir.filename().string();
    if (out.name.empty()) out.name = "ClonedProject";
    out.rootDir      = dir.string();
    out.assetsDir    = (dir / "assets").string() + "/";
    out.scriptsDir   = (dir / "scripts").string() + "/";
    out.defaultScene = "scenes/default.json";
    return true;
}

// アイコン付きの大きなアクションボタン（アイコン無しなら絵文字フォールバック）
static bool IconActionButton(unsigned long long icon, const char* fallback,
                             const char* label, const char* sub, const ImVec2& size)
{
    ImGui::PushID(label);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button("##btn", size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float pad = 14.0f;
    float iconSz = size.y - pad * 2.0f;
    float textX = cursor.x + pad;
    if (icon != 0)
    {
        dl->AddImage(static_cast<ImTextureID>(icon),
                     ImVec2(cursor.x + pad, cursor.y + pad),
                     ImVec2(cursor.x + pad + iconSz, cursor.y + pad + iconSz));
        textX = cursor.x + pad + iconSz + 12.0f;
    }
    else
    {
        dl->AddText(ImVec2(cursor.x + pad, cursor.y + size.y * 0.5f - 8.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), fallback);
        textX = cursor.x + pad + 24.0f;
    }
    float titleY = sub ? cursor.y + size.y * 0.5f - 14.0f : cursor.y + size.y * 0.5f - 8.0f;
    dl->AddText(ImVec2(textX, titleY), ImGui::GetColorU32(ImGuiCol_Text), label);
    if (sub)
        dl->AddText(ImVec2(textX, cursor.y + size.y * 0.5f + 2.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), sub);
    ImGui::PopID();
    return clicked;
}

LauncherAction ProjectManager::RenderLauncher(ProjectInfo& outInfo, HWND hwnd,
                                              const LauncherIcons& icons)
{
    LauncherAction action = LauncherAction::None;

    // 画面全体を覆う背景（モーダル感）
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.09f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##LauncherBG", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoScrollbar);

    // ── 重い外部チェックはフレーム毎に実行しない ──────────────────
    // ランチャーはカーソルを動かすたび毎フレーム再描画される。git/gh の存在確認は
    // 内部で CreateProcess（git/gh --version 起動）、GetRecents は recent.json の
    // ディスク読込なので、毎フレームやると重くてカクつく（＝点滅して見える）。
    // ウィンドウ出現時にだけ評価してキャッシュする。
    const bool launcherAppearing = ImGui::IsWindowAppearing();
    static int  s_gitAvail = -1, s_ghAvail = -1;
    static std::vector<ProjectInfo> s_recents;
    static bool s_cacheReady = false;
    if (launcherAppearing || !s_cacheReady)
    {
        s_cacheReady = true;
        s_gitAvail = GitIntegration::IsGitAvailable() ? 1 : 0;
        s_ghAvail  = GitIntegration::IsGhAvailable()  ? 1 : 0;
        s_recents  = GetRecents();
    }

    // 中央パネル
    ImVec2 panelSize(560, 520);
    ImVec2 panelPos(vp->Pos.x + (vp->Size.x - panelSize.x) * 0.5f,
                    vp->Pos.y + (vp->Size.y - panelSize.y) * 0.5f);
    ImGui::SetCursorScreenPos(panelPos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.13f, 0.17f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::BeginChild("##LauncherPanel", panelSize, ImGuiChildFlags_None);

    ImGui::Dummy(ImVec2(0, 8));
    // ロゴ + タイトル
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImGui::GetCursorScreenPos();
        float logoSz = 56.0f;
        if (icons.logo != 0)
            dl->AddImage(static_cast<ImTextureID>(icons.logo),
                         ImVec2(c.x + 24, c.y), ImVec2(c.x + 24 + logoSz, c.y + logoSz));
        dl->AddText(ImGui::GetFont(), 28.0f, ImVec2(c.x + 24 + logoSz + 16, c.y + 4),
                    IM_COL32(255, 255, 255, 255), "DX12 Engine");
        dl->AddText(ImVec2(c.x + 24 + logoSz + 16, c.y + 36),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), "Game Engine v0.1");
        ImGui::Dummy(ImVec2(0, logoSz + 8));
    }
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    float btnW = panelSize.x - 48.0f;
    ImGui::SetCursorPosX(24);
    if (IconActionButton(icons.newProject, "[N]", "新規プロジェクト",
                         "テンプレートを選んで作成 (FPS / TPS / 2D / 空)", ImVec2(btnW, 64)))
    {
        ImGui::OpenPopup("テンプレートを選択");
    }

    // --- テンプレート選択ポップアップ ---
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("テンプレートを選択", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("作りたいゲームのテンプレートを選んでください。"
                           "選ぶと作成先フォルダを聞かれ、そのテンプレートで初期化されます。");
        ImGui::Dummy(ImVec2(0, 6));

        // テンプレ選択 → フォルダ選択 → info 構築 → CreateNew
        auto pick = [&](const char* tmpl)
        {
            ProjectInfo tmp;
            if (NewProjectDialog(tmp, hwnd))
            {
                tmp.templateId = tmpl;
                outInfo = tmp;
                action  = LauncherAction::CreateNew;
                ImGui::CloseCurrentPopup();
            }
        };

        float tW = 472.0f;
        if (IconActionButton(icons.tmplFps, "[F]", "FPS  (一人称シューター)",
                             "マウスルック + WASD。囲い壁アリーナ + クロスヘア", ImVec2(tW, 60)))
            pick("fps");
        if (IconActionButton(icons.tmplTps, "[T]", "TPS  (三人称アクション)",
                             "追従カメラ + キャラ移動。マウスで周回", ImVec2(tW, 60)))
            pick("tps");
        if (IconActionButton(icons.tmpl2d, "[2]", "2D  (横スクロール)",
                             "正射投影 + 重力ジャンプ。A/D 移動・Space ジャンプ", ImVec2(tW, 60)))
            pick("2d");
        if (IconActionButton(icons.tmplEmpty, "[E]", "空  (最小構成)",
                             "グリッド + 床 + ライト + カメラだけ", ImVec2(tW, 60)))
            pick("empty");

        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("キャンセル", ImVec2(120, 30)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetCursorPosX(24);
    if (IconActionButton(icons.openProject, "[O]", "プロジェクトを開く",
                         "既存の .dx12proj を開く", ImVec2(btnW, 64)))
    {
        if (OpenProjectDialog(outInfo, hwnd))
            action = LauncherAction::OpenExisting;
    }

    // --- Git からクローンして開く ---
    static std::array<char, 512> s_cloneUrl{};
    static std::string s_cloneStatus;
    ImGui::SetCursorPosX(24);
    if (IconActionButton(icons.openProject, "[G]", "Git からクローン",
                         "URL を貼り付けてリポジトリを取得", ImVec2(btnW, 64)))
    {
        s_cloneStatus.clear();
        ImGui::OpenPopup("Clone from Git");
    }

    // クローン用ポップアップ
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Clone from Git", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("リポジトリの URL を貼り付けて「クローン」を押すと、保存先フォルダを選んで取得します。");
        ImGui::TextDisabled("例: https://github.com/owner/repo.git");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##cloneurl", "https://github.com/owner/repo.git",
                                 s_cloneUrl.data(), s_cloneUrl.size());

        if (s_gitAvail == 0)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "git が見つかりません（PATH を通してください）");

        if (!s_cloneStatus.empty())
            ImGui::TextWrapped("%s", s_cloneStatus.c_str());

        ImGui::Dummy(ImVec2(0, 4));
        bool hasUrl = s_cloneUrl[0] != '\0';
        ImGui::BeginDisabled(!hasUrl || s_gitAvail != 1);
        if (ImGui::Button("クローン", ImVec2(160, 30)))
        {
            std::string parent;
            if (PickFolder(hwnd, parent, L"クローン先の親フォルダを選択"))
            {
                s_cloneStatus = "クローン中...";
                std::string repoDir;
                auto r = GitIntegration::Clone(s_cloneUrl.data(), parent, repoDir);
                if (r.ok() && MakeProjectFromFolder(repoDir, outInfo))
                {
                    AddToRecents(outInfo);
                    action = LauncherAction::OpenExisting;
                    s_cloneUrl.fill('\0');
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    s_cloneStatus = r.output.empty() ? "クローンに失敗しました" : r.output;
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("閉じる", ImVec2(120, 30)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- GitHub ログイン状態 ---
    {
        static bool        s_loginChecked = false;
        static std::string s_loginUser;
        static bool        s_loginRunning = false;
        static std::atomic<bool> s_loginDone{false};
        static std::string s_loginPendingUser;
        // ponytail: ランチャーはアプリ全体の m_gitAbort を持たないので、ログイン待ちスレッドは
        // detach する（gh.exe の終了待ちだけで Application/ProjectManager の状態には触れないので
        // プロセス終了時に残っても無害）。中断UIが要るなら Application 側に寄せる。
        static const std::atomic<bool> s_neverAbort{false};

        if (launcherAppearing) s_loginChecked = false;  // ランチャー再入時だけ再確認
        if (!s_loginChecked && !s_loginRunning)
        {
            s_loginChecked = true;
            s_loginUser = (s_ghAvail == 1) ? GitIntegration::GitHubUser() : std::string();
        }
        if (s_loginRunning && s_loginDone.load())        // 非同期ログイン待ちの完了を取り込む
        {
            s_loginUser    = s_loginPendingUser;
            s_loginRunning = false;
        }

        ImGui::SetCursorPosX(24);
        if (s_ghAvail == 0)
            ImGui::TextDisabled("GitHub CLI (gh) が無いため、ログインは使えません");
        else if (s_loginRunning)
            ImGui::TextDisabled("ログイン待ち中...（別ウィンドウでブラウザ認証してや）");
        else if (s_loginUser.empty())
        {
            ImGui::TextDisabled("GitHub: 未ログイン");
            ImGui::SameLine();
            if (ImGui::SmallButton("ログイン"))
            {
                s_loginRunning = true;
                s_loginDone.store(false);
                std::thread([]{
                    auto r = GitIntegration::LoginAndWait(s_neverAbort);
                    s_loginPendingUser = r.output;
                    s_loginDone.store(true);
                }).detach();
            }
        }
        else
        {
            ImGui::TextDisabled("GitHub: @%s でログイン中", s_loginUser.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("再確認"))
                s_loginChecked = false;
        }
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SetCursorPosX(24);
    ImGui::TextDisabled("最近のプロジェクト");
    ImGui::SetCursorPosX(24);
    ImGui::BeginChild("##recents", ImVec2(btnW, 210), ImGuiChildFlags_Borders);

    auto& recents = s_recents;
    if (recents.empty())
    {
        ImGui::TextDisabled("まだプロジェクトがありません");
    }
    else
    {
        for (const auto& recent : recents)
        {
            ImGui::PushID(recent.rootDir.c_str());
            std::filesystem::path projFile =
                std::filesystem::path(recent.rootDir) / (recent.name + ".dx12proj");
            bool exists = std::filesystem::exists(projFile);

            ImVec2 c = ImGui::GetCursorScreenPos();
            bool sel = ImGui::Selectable("##rec", false, ImGuiSelectableFlags_None, ImVec2(0, 40));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float ic = 28.0f;
            if (icons.recent != 0)
                dl->AddImage(static_cast<ImTextureID>(icons.recent),
                             ImVec2(c.x + 4, c.y + 6), ImVec2(c.x + 4 + ic, c.y + 6 + ic));
            ImU32 nameCol = exists ? IM_COL32(235, 235, 235, 255) : IM_COL32(130, 130, 130, 255);
            dl->AddText(ImVec2(c.x + 4 + ic + 8, c.y + 4), nameCol, recent.name.c_str());
            dl->AddText(ImVec2(c.x + 4 + ic + 8, c.y + 22),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), recent.rootDir.c_str());

            if (sel && exists && Project::Load(projFile.string(), outInfo))
            {
                AddToRecents(outInfo);
                action = LauncherAction::OpenExisting;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetCursorPosX(24);
    if (ImGui::Button("スキップ（組み込みデフォルトで開く）", ImVec2(btnW, 28)))
    {
        outInfo = ProjectInfo{};
        outInfo.name    = "Default";
        outInfo.rootDir = "";  // 組み込みパスを使う合図
        action = LauncherAction::Skip;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    return action;
}

std::string ProjectManager::GetEditorStatePath()
{
    char appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath)))
    {
        std::filesystem::path dir = std::filesystem::path(appDataPath) / "DX12Engine";
        std::filesystem::create_directories(dir);
        return (dir / "editor_state.json").string();
    }
    return "editor_state.json";
}

void ProjectManager::SaveLastOpenedScene(const std::string& scenePath)
{
    std::string filePath = GetEditorStatePath();

    // 既存の state を読み込んでマージ
    nlohmann::json j;
    {
        std::ifstream ifs(filePath);
        if (ifs.is_open())
        {
            try { ifs >> j; }
            catch (...) { j = nlohmann::json::object(); }
        }
    }

    j["lastOpenedScene"] = scenePath;

    std::ofstream ofs(filePath);
    if (ofs.is_open())
        ofs << j.dump(2);
}

bool ProjectManager::ProjectFromFolder(const std::string& dir, ProjectInfo& out)
{
    return MakeProjectFromFolder(dir, out);
}

std::string ProjectManager::LoadLastOpenedScene()
{
    std::string filePath = GetEditorStatePath();
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) return "";

    try
    {
        nlohmann::json j;
        ifs >> j;
        return j.value("lastOpenedScene", "");
    }
    catch (...)
    {
        return "";
    }
}

} // namespace dx12e
