#include "core/PathResolver.h"

#include <Windows.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace dx12e
{
bool         PathResolver::s_initialized = false;
std::string  PathResolver::s_assets;
std::wstring PathResolver::s_shaderW;
std::string  PathResolver::s_scripts;
std::string  PathResolver::s_base;

void PathResolver::Initialize(bool gameMode)
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path exeDir = fs::path(buf).parent_path();

    // exe 隣に assets があれば配布レイアウトとみなす（--game でも配布扱い）
    const bool dist = gameMode || fs::exists(exeDir / "assets");

    if (dist)
    {
        s_base    = exeDir.generic_string() + "/";
        s_assets  = (exeDir / "assets").generic_string()  + "/";
        s_scripts = (exeDir / "scripts").generic_string() + "/";
        s_shaderW = (exeDir / "shaders").generic_wstring() + L"/";
    }
    else
    {
        // 開発時: コンパイル時マクロ（ソース/ビルドツリーの絶対パス）にフォールバック
        s_assets  = ASSETS_DIR;
        s_scripts = SCRIPTS_DIR;
        s_shaderW = SHADER_DIR;
        // ASSETS_DIR = "<root>/assets/" なので 2 つ親を辿るとプロジェクトルート
        s_base    = fs::path(ASSETS_DIR).parent_path().parent_path().generic_string() + "/";
    }

    s_initialized = true;
}

std::wstring PathResolver::Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), result.data(), len);
    return result;
}

void PathResolver::SetProjectRoot(const std::string& rootDir)
{
    if (rootDir.empty())
        return;  // 空 = 組み込みパスを維持（ランチャーの「スキップ」用）

    fs::path root(rootDir);
    s_base    = root.generic_string() + "/";
    s_assets  = (root / "assets").generic_string()  + "/";
    s_scripts = (root / "scripts").generic_string() + "/";
    // shaders はエンジン側の .cso をそのまま使う（s_shaderW は変更しない）
}

const std::string&  PathResolver::AssetsDir()  { return s_assets;  }
const std::wstring& PathResolver::ShaderDirW() { return s_shaderW; }
const std::string&  PathResolver::ScriptsDir() { return s_scripts; }
const std::string&  PathResolver::BaseDir()    { return s_base;    }

std::string PathResolver::GameLuaPath()
{
    std::string s = s_scripts + "game.lua";
    if (fs::exists(s)) return s;
    std::string a = s_assets + "game.lua";
    if (fs::exists(a)) return a;
    return s;  // 既定（存在チェックは呼び出し側）
}
}
