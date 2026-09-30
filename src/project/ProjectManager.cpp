#include "project/ProjectManager.h"
#include "project/GitIntegration.h"
#include "core/Logger.h"
#include "core/VirtualGuard.h"   // 仮想入力モード中はネイティブダイアログを出さない

#include <Windows.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <mutex>
#include <thread>
#include <commdlg.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <nlohmann/json.hpp>

namespace dx12e
{

namespace
{
namespace fs = std::filesystem;

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

// recent.json / editor_state.json の読み書きは同じプロセス内の複数スレッド（ロードスレッド + メイン）から来る。
std::mutex& StoreMutex()
{
    static std::mutex m;
    return m;
}

bool ReadJsonFile(const fs::path& p, nlohmann::json& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out = nlohmann::json::parse(f, nullptr, /*allow_exceptions*/ false);
    return !out.is_discarded();
}

bool WriteJsonFile(const fs::path& p, const nlohmann::json& j)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // 途中で落ちても壊れた JSON を残さないよう、一時ファイルへ書いてから置き換える。
    const fs::path tmp = p.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(2);
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec)
    {
        // 置き換えに失敗（別プロセスが握っている等）したら直接書く。
        fs::remove(tmp, ec);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(2);
    }
    return true;
}
}  // namespace

// ---------------------------------------------------------------- 保存先

std::string ProjectManager::DataDir()
{
    // 検証用の差し替え（実ユーザーの一覧を触らない）。
    char* env = nullptr;
    size_t n = 0;
    if (_dupenv_s(&env, &n, "DX12E_DATA_DIR") == 0 && env && *env)
    {
        std::string r(env);
        free(env);
        std::error_code ec;
        fs::create_directories(launcher::PathFromUtf8(r), ec);
        return r;
    }
    if (env) free(env);

    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
    {
        const fs::path dir = fs::path(appData) / L"DX12Engine";
        CoTaskMemFree(appData);
        std::error_code ec;
        fs::create_directories(dir, ec);
        return WideToUtf8(dir.wstring());
    }
    return ".";
}

std::string ProjectManager::GetRecentsFilePath()
{
    return launcher::JoinPath(DataDir(), "recent.json");
}

std::string ProjectManager::GetEditorStatePath()
{
    return launcher::JoinPath(DataDir(), "editor_state.json");
}

int64_t ProjectManager::NowEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- 最近のプロジェクト

static std::vector<launcher::RecentRecord> ReadRecentsLocked(const std::string& file)
{
    std::vector<launcher::RecentRecord> out;
    nlohmann::json j;
    if (!ReadJsonFile(launcher::PathFromUtf8(file), j) || !j.is_object()) return out;
    const auto it = j.find("recents");
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& e : *it)
    {
        if (!e.is_object()) continue;
        launcher::RecentRecord r;
        r.name       = e.value("name", std::string());
        r.path       = e.value("path", std::string());
        r.lastOpened = e.value("lastOpened", static_cast<int64_t>(0));
        r.pinned     = e.value("pinned", false);
        if (!r.name.empty() && !r.path.empty()) out.push_back(std::move(r));
    }
    return out;
}

static void WriteRecentsLocked(const std::string& file, const std::vector<launcher::RecentRecord>& v)
{
    nlohmann::json j;
    j["recents"] = nlohmann::json::array();
    for (const auto& r : v)
    {
        nlohmann::json e;
        e["name"] = r.name;
        e["path"] = r.path;
        if (r.lastOpened > 0) e["lastOpened"] = r.lastOpened;
        if (r.pinned) e["pinned"] = true;
        j["recents"].push_back(std::move(e));
    }
    WriteJsonFile(launcher::PathFromUtf8(file), j);
}

std::vector<launcher::RecentRecord> ProjectManager::LoadRecents()
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    auto v = ReadRecentsLocked(GetRecentsFilePath());
    launcher::SortRecents(v);
    return v;
}

void ProjectManager::AddToRecents(const ProjectInfo& info)
{
    if (info.rootDir.empty()) return;
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetRecentsFilePath();
    auto v = ReadRecentsLocked(file);
    launcher::UpsertRecent(v, info.name, info.rootDir, NowEpoch());
    WriteRecentsLocked(file, v);
}

bool ProjectManager::RemoveFromRecents(const std::string& projectRoot)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetRecentsFilePath();
    auto v = ReadRecentsLocked(file);
    const bool removed = launcher::RemoveRecent(v, projectRoot);
    if (removed) WriteRecentsLocked(file, v);
    return removed;
}

bool ProjectManager::SetRecentPinned(const std::string& projectRoot, bool pinned)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetRecentsFilePath();
    auto v = ReadRecentsLocked(file);
    const bool ok = launcher::SetPinned(v, projectRoot, pinned);
    if (ok) WriteRecentsLocked(file, v);
    return ok;
}

std::vector<ProjectInfo> ProjectManager::GetRecents()
{
    std::vector<ProjectInfo> recents;
    for (const auto& r : LoadRecents())
    {
        ProjectInfo info;
        info.name    = r.name;
        info.rootDir = r.path;
        recents.push_back(std::move(info));
    }
    return recents;
}

// ---------------------------------------------------------------- 開く

bool ProjectManager::OpenProjectDialog(ProjectInfo& outInfo, HWND hwnd)
{
    std::string picked;
    if (!PickProjectFile(hwnd, picked)) return false;
    if (Project::Load(picked, outInfo))
    {
        AddToRecents(outInfo);
        return true;
    }
    return false;
}

// 専用の STA スレッドでダイアログを出す共通処理。
// ★メインスレッドは XAudio2 が COINIT_MULTITHREADED(MTA) で COM 初期化済み。
//   IFileOpenDialog は STA を要求するため、MTA スレッドで Show() すると固まる。
//   → 専用の STA スレッドで開く。オーナー window は渡さない（メインスレッドが
//     join() でブロック中なので、クロススレッド SendMessage によるデッドロックを避ける）。
template <class Setup>
static bool RunOpenDialog(std::string& outPath, Setup setup)
{
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
            setup(dlg);
            if (SUCCEEDED(dlg->Show(nullptr)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)))
                {
                    PWSTR pathW = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pathW)) && pathW)
                    {
                        picked = WideToUtf8(pathW);
                        ok = !picked.empty();
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

bool ProjectManager::PickFolder(HWND /*hwnd*/, std::string& outPath, const wchar_t* title)
{
    if (dx12e::guard::Blocked("フォルダ選択ダイアログ")) return false;   // 仮想入力モード: モーダルでフォーカスを奪う
    return RunOpenDialog(outPath, [&](IFileOpenDialog* dlg)
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (title) dlg->SetTitle(title);
    });
}

bool ProjectManager::PickProjectFile(HWND /*hwnd*/, std::string& outPath)
{
    if (dx12e::guard::Blocked("プロジェクトを開くダイアログ")) return false;
    return RunOpenDialog(outPath, [&](IFileOpenDialog* dlg)
    {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        static const COMDLG_FILTERSPEC kFilter[] = {
            { L"Uno Engine プロジェクト (*.dx12proj)", L"*.dx12proj" },
            { L"すべてのファイル", L"*.*" },
        };
        dlg->SetFileTypes(2, kFilter);
        dlg->SetTitle(L"プロジェクトを開く");
    });
}

bool ProjectManager::NewProjectDialog(ProjectInfo& outInfo, HWND hwnd)
{
    // 作成先フォルダを選ぶだけ。ディスク作成は呼び出し側が非同期で行う。
    std::string folder;
    if (!PickFolder(hwnd, folder, L"新規プロジェクトの作成先フォルダを選択"))
        return false;

    const fs::path projDir = launcher::PathFromUtf8(folder);
    outInfo.name         = launcher::PathToUtf8(projDir.filename());
    if (outInfo.name.empty()) outInfo.name = "MyGame";
    outInfo.rootDir      = launcher::PathToUtf8(projDir);
    outInfo.assetsDir    = launcher::PathToUtf8(projDir / "assets") + "/";
    outInfo.scriptsDir   = launcher::PathToUtf8(projDir / "scripts") + "/";
    outInfo.defaultScene = "scenes/main.json";
    // templateId は呼び出し側が設定する
    return true;
}

// クローンしたフォルダから ProjectInfo を組み立てる。
// .dx12proj があれば読み込み、無ければフォルダをプロジェクトルートとして合成する。
static bool MakeProjectFromFolder(const std::string& repoDir, ProjectInfo& out)
{
    std::error_code ec;
    const fs::path dirPath = launcher::PathFromUtf8(repoDir);
    if (!fs::exists(dirPath, ec)) return false;

    // .dx12proj を探す
    for (auto& e : fs::directory_iterator(dirPath, ec))
    {
        if (e.is_regular_file() && e.path().extension() == ".dx12proj")
        {
            if (Project::Load(launcher::PathToUtf8(e.path()), out))
                return true;
        }
    }

    // 無ければフォルダをそのままプロジェクトとして扱う
    out = ProjectInfo{};
    out.name         = launcher::PathToUtf8(dirPath.filename());
    if (out.name.empty()) out.name = "ClonedProject";
    out.rootDir      = launcher::PathToUtf8(dirPath);
    out.assetsDir    = launcher::PathToUtf8(dirPath / "assets") + "/";
    out.scriptsDir   = launcher::PathToUtf8(dirPath / "scripts") + "/";
    out.defaultScene = "scenes/default.json";
    return true;
}

bool ProjectManager::ProjectFromFolder(const std::string& dir, ProjectInfo& out)
{
    return MakeProjectFromFolder(dir, out);
}

bool ProjectManager::ResolveProjectPath(const std::string& rawPath, ProjectInfo& out, std::string& err)
{
    std::string p = launcher::Trim(rawPath);
    // 「パスのコピー」で付く引用符を外す
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
    if (p.empty()) { err = "パスを入力するか、「参照」から選んでください"; return false; }

    std::error_code ec;
    const fs::path path = launcher::PathFromUtf8(p);
    if (!fs::exists(path, ec)) { err = "そのパスが見つかりません"; return false; }
    if (fs::is_directory(path, ec))
    {
        bool has = false;
        for (auto it = fs::directory_iterator(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".dx12proj") { has = true; break; }
        if (!has) { err = "このフォルダに .dx12proj が見つかりません（プロジェクトのフォルダを指定してください）"; return false; }
        return MakeProjectFromFolder(p, out);
    }
    if (path.extension() != ".dx12proj") { err = ".dx12proj のファイルか、プロジェクトのフォルダを指定してください"; return false; }
    if (!Project::Load(p, out)) { err = "プロジェクトファイルを読み込めませんでした"; return false; }
    return true;
}

// ---------------------------------------------------------------- エディタ状態

static nlohmann::json ReadEditorStateLocked(const std::string& file)
{
    nlohmann::json j;
    if (!ReadJsonFile(launcher::PathFromUtf8(file), j) || !j.is_object())
        j = nlohmann::json::object();
    return j;
}

void ProjectManager::SaveLastOpenedScene(const std::string& scenePath)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetEditorStatePath();
    nlohmann::json j = ReadEditorStateLocked(file);   // 既存の state を読み込んでマージ
    j["lastOpenedScene"] = scenePath;
    WriteJsonFile(launcher::PathFromUtf8(file), j);
}

std::string ProjectManager::LoadLastOpenedScene()
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    return ReadEditorStateLocked(GetEditorStatePath()).value("lastOpenedScene", std::string());
}

bool ProjectManager::GetEditorBool(const char* key, bool def)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const nlohmann::json j = ReadEditorStateLocked(GetEditorStatePath());
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

void ProjectManager::SetEditorBool(const char* key, bool value)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetEditorStatePath();
    nlohmann::json j = ReadEditorStateLocked(file);
    j[key] = value;
    WriteJsonFile(launcher::PathFromUtf8(file), j);
}

std::string ProjectManager::GetEditorString(const char* key, const std::string& def)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const nlohmann::json j = ReadEditorStateLocked(GetEditorStatePath());
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

void ProjectManager::SetEditorString(const char* key, const std::string& value)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetEditorStatePath();
    nlohmann::json j = ReadEditorStateLocked(file);
    j[key] = value;
    WriteJsonFile(launcher::PathFromUtf8(file), j);
}

std::string ProjectManager::LoadEditorSection(const char* key)
{
    std::lock_guard<std::mutex> lk(StoreMutex());
    const nlohmann::json j = ReadEditorStateLocked(GetEditorStatePath());
    if (j.contains(key) && j[key].is_object()) return j[key].dump();
    return {};
}

void ProjectManager::SaveEditorSection(const char* key, const std::string& jsonText)
{
    nlohmann::json section = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions*/ false);
    if (section.is_discarded() || !section.is_object()) return;
    std::lock_guard<std::mutex> lk(StoreMutex());
    const std::string file = GetEditorStatePath();
    nlohmann::json j = ReadEditorStateLocked(file);   // 既存のキーは保つ（lastOpenedScene など）
    j[key] = std::move(section);
    WriteJsonFile(launcher::PathFromUtf8(file), j);
}

} // namespace dx12e
