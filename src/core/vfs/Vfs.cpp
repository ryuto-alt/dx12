#include "core/vfs/Vfs.h"
#include "core/vfs/PakArchive.h"
#include "core/vfs/PakFormat.h"
#include "core/PathResolver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <optional>
#include <fstream>
#include <cctype>

namespace dx12e::vfs
{

namespace
{
std::optional<PakArchive> g_pak;

// ディスクからファイル全体を読む（バイト等価。narrow パス = 既存ローダと同じ）。
std::vector<uint8_t> ReadFileBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return {};
    const std::streamoff size = in.tellg();
    if (size < 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

// プレフィックス比較用に小文字化 + バックスラッシュ -> スラッシュ。
std::string ToLowerSlash(const std::string& s)
{
    std::string r = s;
    for (char& c : r)
    {
        if (c == '\\')
            c = '/';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), needed, nullptr, nullptr);
    return s;
}

// 簡易 JSON 抽出（manifest は自前生成で構造既知）。
bool ExtractJsonString(const std::string& json, const std::string& key, std::string& out)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t p = json.find(needle);
    if (p == std::string::npos)
        return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos)
        return false;
    p = json.find('"', p + 1);
    if (p == std::string::npos)
        return false;
    ++p;
    std::string v;
    while (p < json.size())
    {
        const char c = json[p];
        if (c == '\\' && p + 1 < json.size())
        {
            const char n = json[p + 1];
            v.push_back(n == 'n' ? '\n' : (n == 't' ? '\t' : n));
            p += 2;
            continue;
        }
        if (c == '"')
            break;
        v.push_back(c);
        ++p;
    }
    out = v;
    return true;
}

bool ExtractJsonInt(const std::string& json, const std::string& key, int& out)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t p = json.find(needle);
    if (p == std::string::npos)
        return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos)
        return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
        ++p;
    std::size_t start = p;
    if (p < json.size() && (json[p] == '-' || json[p] == '+'))
        ++p;
    bool any = false;
    while (p < json.size() && std::isdigit(static_cast<unsigned char>(json[p])))
    {
        ++p;
        any = true;
    }
    if (!any)
        return false;
    out = std::atoi(json.substr(start, p - start).c_str());
    return true;
}
} // namespace

bool MountPak(const std::string& pakPath)
{
    g_pak.emplace();
    if (!g_pak->Mount(pakPath))
    {
        g_pak.reset();
        return false;
    }
    return true;
}

void Unmount()
{
    g_pak.reset();
}

bool InGameMode()
{
    return g_pak.has_value();
}

std::vector<uint8_t> ReadAsset(const std::string& relPath)
{
    if (g_pak)
    {
        std::vector<uint8_t> out;
        if (g_pak->Read(relPath, out))
            return out;
        return {};
    }
    // ディスクモード: AssetsDir() + relPath
    return ReadFileBytes(PathResolver::AssetsDir() + relPath);
}

std::vector<uint8_t> ReadAssetAbs(const std::string& absPath)
{
    const std::string a    = ToLowerSlash(absPath);
    const std::string base = ToLowerSlash(PathResolver::AssetsDir());

    if (!base.empty() && a.size() >= base.size() && a.compare(0, base.size(), base) == 0)
    {
        // AssetsDir プレフィックスを剥がして相対キーを復元（hash 側で再 Normalize される）。
        return ReadAsset(a.substr(base.size()));
    }

    // assets/ の外の絶対パス。
    if (g_pak)
    {
        // ゲームモード: BaseDir(=exe フォルダ) 相対のキーで pak を引く。
        // scripts/game.lua や shaders/Foo.cso は assets/ の外に在るが、pak には
        // "scripts/"・"shaders/" プレフィックス付きで封入されている（BuildGame 参照）。
        const std::string root = ToLowerSlash(PathResolver::BaseDir());
        if (!root.empty() && a.size() >= root.size() && a.compare(0, root.size(), root) == 0)
            return ReadAsset(a.substr(root.size()));
        return {}; // ゲームモードで未知のパス
    }
    return ReadFileBytes(absPath); // ディスクモードは生読み（エディタ挙動維持）
}

std::vector<uint8_t> ReadAssetAbs(const std::wstring& absPath)
{
    return ReadAssetAbs(WideToUtf8(absPath));
}

bool Exists(const std::string& relPath)
{
    if (g_pak)
        return g_pak->Has(relPath);
    std::ifstream in(PathResolver::AssetsDir() + relPath, std::ios::binary);
    return in.is_open();
}

bool ExistsAbs(const std::string& absPath)
{
    const std::string a    = ToLowerSlash(absPath);
    const std::string base = ToLowerSlash(PathResolver::AssetsDir());

    if (!base.empty() && a.size() >= base.size() && a.compare(0, base.size(), base) == 0)
        return Exists(a.substr(base.size()));

    if (g_pak)
    {
        const std::string root = ToLowerSlash(PathResolver::BaseDir());
        if (!root.empty() && a.size() >= root.size() && a.compare(0, root.size(), root) == 0)
            return Exists(a.substr(root.size()));
        return false; // ゲームモードで未知のパス
    }

    std::ifstream in(absPath, std::ios::binary);
    return in.is_open();
}

bool ExistsAbs(const std::wstring& absPath)
{
    return ExistsAbs(WideToUtf8(absPath));
}

bool ReadBootConfig(BootConfig& out)
{
    if (!g_pak)
        return false;
    std::vector<uint8_t> bytes;
    if (!g_pak->Read("__manifest__", bytes) || bytes.empty())
        return false;
    const std::string json(bytes.begin(), bytes.end());
    ExtractJsonString(json, "title", out.title);
    ExtractJsonString(json, "startScene", out.startScene);
    ExtractJsonInt(json, "windowWidth", out.windowWidth);
    ExtractJsonInt(json, "windowHeight", out.windowHeight);
    ExtractJsonString(json, "uiFont", out.uiFont);
    return true;
}

} // namespace dx12e::vfs
