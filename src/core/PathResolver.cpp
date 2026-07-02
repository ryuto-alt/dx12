#include "core/PathResolver.h"

#include <Windows.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace dx12e
{
namespace
{
// fs::path -> UTF-8 の std::string('/' 区切り)。VFS は UTF-8 前提(WideToUtf8/Utf8ToWide)なので
// パスのプレフィックス文字列も UTF-8 に揃える。generic_string() は ACP を返し、非 ASCII
// フォルダ("新しいフォルダー"等)で pak キーのプレフィックス剥がしが一致しなくなる。
std::string Utf8Generic(const fs::path& p)
{
    const std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}
} // namespace

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
        s_base    = Utf8Generic(exeDir) + "/";
        s_assets  = Utf8Generic(exeDir / "assets")  + "/";
        s_scripts = Utf8Generic(exeDir / "scripts") + "/";
        s_shaderW = (exeDir / "shaders").generic_wstring() + L"/";
    }
    else
    {
        // 開発時: コンパイル時マクロ（ソース/ビルドツリーの絶対パス）にフォールバック
        s_assets  = ASSETS_DIR;
        s_scripts = SCRIPTS_DIR;
        s_shaderW = SHADER_DIR;
        // ASSETS_DIR = "<root>/assets/" なので 2 つ親を辿るとプロジェクトルート
        s_base    = Utf8Generic(fs::path(ASSETS_DIR).parent_path().parent_path()) + "/";
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
    s_base    = Utf8Generic(root) + "/";
    s_assets  = Utf8Generic(root / "assets")  + "/";
    s_scripts = Utf8Generic(root / "scripts") + "/";
    // shaders はエンジン側の .cso をそのまま使う（s_shaderW は変更しない）
}

const std::string&  PathResolver::AssetsDir()  { return s_assets;  }
const std::wstring& PathResolver::ShaderDirW() { return s_shaderW; }
const std::string&  PathResolver::ScriptsDir() { return s_scripts; }
const std::string&  PathResolver::BaseDir()    { return s_base;    }

std::string PathResolver::GameLuaPath()
{
    // s_* は UTF-8。fs::exists(std::string) は ACP 解釈なので、非 ASCII / 壊れた
    // フォルダ名だと例外を投げ得る -> error_code 版で握りつぶす(ゲームモードは pak 経由で読む)。
    std::error_code ec;
    std::string s = s_scripts + "game.lua";
    if (fs::exists(s, ec)) return s;
    std::string a = s_assets + "game.lua";
    if (fs::exists(a, ec)) return a;
    return s;  // 既定（存在チェックは呼び出し側）
}
}
