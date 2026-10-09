// ===========================================================================
// 同梱サンプルプロジェクトの初回コピー（<exe>/samples/<名前>/ → ドキュメント\UnoProjects\<名前>）
// ---------------------------------------------------------------------------
//   ・エディタの起動時に 1 回だけ（自動化中・ゲームモードでは走らない）。MCP hw_setup {action:"install_samples"} からも呼べる。
//   ・目印 %LOCALAPPDATA%\UnoEngine\samples_installed.json {"installed":["ArduinoLab",...]}。載っていれば何もしない
//     （ユーザーが消したものを復活させない）。force:true（MCP のみ）は目印を無視する。
//   ・コピー先が空でないフォルダなら上書きせず飛ばす（目印には載せる）。.dx12 フォルダは持っていかない。
//   ・何も削除しない。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "hardware/HwToolchain.h"
#include "project/ProjectManager.h"

#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace dx12e
{

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{

fs::path ExeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    if (::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf))) == 0) return {};
    return fs::path(buf).parent_path();
}

fs::path MarkerPath()
{
    const std::string d = hw::UnoEngineDataDir();
    return d.empty() ? fs::path() : fs::path(PathResolver::Utf8ToWide(d)) / L"samples_installed.json";
}

std::vector<std::string> ReadMarker()
{
    std::vector<std::string> out;
    const fs::path p = MarkerPath();
    if (p.empty()) return out;
    std::ifstream f(p, std::ios::binary);
    if (!f) return out;
    try
    {
        const json j = json::parse(f, nullptr, true, true);
        if (j.contains("installed") && j["installed"].is_array())
            for (const json& v : j["installed"]) if (v.is_string()) out.push_back(v.get<std::string>());
    }
    catch (...) {}
    return out;
}

void WriteMarker(const std::vector<std::string>& names)
{
    const fs::path p = MarkerPath();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const json j{{"installed", names}};
    const atomicfile::Result r = atomicfile::WriteFile(p, j.dump(2));
    if (!r.ok) Logger::Warn("[samples] samples_installed.json を書けませんでした: {}", r.error);
}

fs::path DocumentsUnoProjects()
{
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = fs::path(p) / L"UnoProjects";
    if (p) ::CoTaskMemFree(p);
    return out;
}

// samples の下の「*.dx12proj を持つフォルダ」の一覧（名前順）
std::vector<fs::path> BundledSampleDirs(const fs::path& samples)
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(samples, ec)) return out;
    for (const fs::directory_entry& e : fs::directory_iterator(samples, ec))
    {
        if (!e.is_directory(ec)) continue;
        bool has = false;
        for (const fs::directory_entry& c : fs::directory_iterator(e.path(), ec))
            if (c.is_regular_file(ec) && c.path().extension() == L".dx12proj") { has = true; break; }
        if (has) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool DirNotEmpty(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec) && fs::directory_iterator(p, ec) != fs::directory_iterator();
}

// .dx12 を除いて再帰コピー。失敗は例外（呼び側が受ける）
void CopyTreeSkipDx12(const fs::path& src, const fs::path& dst)
{
    fs::create_directories(dst);
    for (fs::recursive_directory_iterator it(src), end; it != end; ++it)
    {
        const fs::path rel = fs::relative(it->path(), src);
        if (it->is_directory())
        {
            if (it->path().filename() == L".dx12") { it.disable_recursion_pending(); continue; }
            fs::create_directories(dst / rel);
        }
        else if (it->is_regular_file())
        {
            fs::create_directories((dst / rel).parent_path());
            fs::copy_file(it->path(), dst / rel, fs::copy_options::skip_existing);
        }
    }
}

} // namespace

json Application::HwSamplesStatus()
{
    const fs::path samples = ExeDir() / L"samples";
    json avail = json::array();
    for (const fs::path& d : BundledSampleDirs(samples)) avail.push_back(PathResolver::WideToUtf8(d.filename().wstring()));
    return {{"bundledDir", PathResolver::WideToUtf8(samples.wstring())},
            {"available", avail},
            {"installed", ReadMarker()},
            {"markerPath", PathResolver::WideToUtf8(MarkerPath().wstring())}};
}

json Application::InstallBundledSamples(const std::string& destOverride, bool force, bool startup)
{
    const fs::path samples = ExeDir() / L"samples";
    json rep{{"bundledDir", PathResolver::WideToUtf8(samples.wstring())}, {"installed", json::array()}, {"skipped", json::array()}};
    const std::vector<fs::path> dirs = BundledSampleDirs(samples);
    if (dirs.empty())
    {
        rep["note"] = "同梱サンプルがありません（samples フォルダが無い = 開発ビルド）";
        return rep;
    }
    const bool overridden = !destOverride.empty();   // 検証用の置き場: 目印・最近のプロジェクトには触らない
    const fs::path root = overridden ? fs::path(PathResolver::Utf8ToWide(destOverride)) : DocumentsUnoProjects();
    if (root.empty())
    {
        rep["error"] = "ドキュメントフォルダを取得できませんでした";
        return rep;
    }
    rep["dest"] = PathResolver::WideToUtf8(root.wstring());
    std::vector<std::string> marker = ReadMarker();
    bool markerChanged = false;

    for (const fs::path& src : dirs)
    {
        const std::string name = PathResolver::WideToUtf8(src.filename().wstring());
        const bool listed = std::find(marker.begin(), marker.end(), name) != marker.end();
        if (listed && !force)
        {
            rep["skipped"].push_back({{"name", name}, {"reason", "目印に載っている（消されていても置き直さない）"}});
            continue;
        }
        const fs::path dst = root / src.filename();
        auto record = [&]() {
            if (!overridden && !listed) { marker.push_back(name); markerChanged = true; }
        };
        if (DirNotEmpty(dst))
        {
            rep["skipped"].push_back({{"name", name}, {"reason", "コピー先に同名のフォルダがある（上書きしない）"}});
            record();
            continue;
        }
        try
        {
            CopyTreeSkipDx12(src, dst);
        }
        catch (const std::exception& e)
        {
            Logger::Warn("[samples] {} のコピーに失敗: {}", name, e.what());
            rep["skipped"].push_back({{"name", name}, {"reason", std::string("コピーに失敗: ") + e.what()}});
            continue;
        }
        if (!overridden)
        {
            ProjectInfo info;
            info.name = name;
            info.rootDir = PathResolver::WideToUtf8(dst.generic_wstring());
            ProjectManager::AddToRecents(info);
        }
        record();
        rep["installed"].push_back(name);
        Logger::Info("[samples] {} を {} に置きました", name, PathResolver::WideToUtf8(dst.wstring()));
    }
    if (markerChanged) WriteMarker(marker);

    if (startup && !rep["installed"].empty())
    {
        std::string names;
        for (const json& n : rep["installed"])
        {
            const std::string s = n.get<std::string>();
            names += (names.empty() ? "" : "、") + s + (s == "ArduinoLab" ? "（Arduino / ESP32 の実験）" : "");
        }
        m_startupToast = names + "をプロジェクト一覧に追加しました";
    }
    return rep;
}

} // namespace dx12e
