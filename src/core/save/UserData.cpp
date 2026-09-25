#include "core/save/UserData.h"

#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"

#include <windows.h>
#include <shlobj.h>

namespace dx12e::save
{

std::string SanitizeFolderName(const std::string& title)
{
    std::string out;
    out.reserve(title.size());
    for (char ch : title)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20) continue;
        switch (c)
        {
        case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
            continue;
        default:
            out.push_back(ch);
        }
    }
    // 末尾の空白とドットは Windows が黙って落とすので、最初から付けない（別名扱いの事故を防ぐ）
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out.empty() ? std::string("DX12Game") : out;
}

std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    return std::filesystem::path(PathResolver::Utf8ToWide(utf8));
}

std::string PathToUtf8(const std::filesystem::path& p)
{
    const std::u8string u8 = p.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path UserDataDir()
{
    if (vfs::InGameMode())
    {
        vfs::BootConfig boot;
        std::string title;
        if (vfs::ReadBootConfig(boot)) title = boot.title;
        std::filesystem::path base;
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known)
        {
            base = std::filesystem::path(known);
            CoTaskMemFree(known);
        }
        else
        {
            // 取れない環境（サービス実行など）は exe の隣へ倒す。書けなければ書く側が警告する。
            base = PathFromUtf8(PathResolver::BaseDir());
        }
        return base / PathFromUtf8(SanitizeFolderName(title));
    }
    // エディタ / ヘッドレス: プロジェクトの .dx12/（BaseDir = プロジェクトルート、末尾 '/'）
    return PathFromUtf8(PathResolver::BaseDir()) / L".dx12";
}

std::filesystem::path SavesDir()
{
    return UserDataDir() / L"saves";
}

} // namespace dx12e::save
