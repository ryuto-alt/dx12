#pragma once
// 再帰削除（std::filesystem::remove_all）の最後の砦。エンジンが再帰削除するときは必ずここを通す。
//
// 2026-10-02 15:52、C ドライブのルートからの再帰削除が走り、ユーザーのホーム・C:\vcpkg・Program Files の
// 書き込める部分が消えた。何が実行したかは記録ごと消えて特定できていない。呼び出し側のパスの組み立てが
// 正しいことに頼るのをやめ、「どこから来たパスでも、消してはいけない場所は消さない」を 1 か所で保証する。
//
// 断る（何も消さず 0 を返す）もの:
//   - 空のパス・相対パス（カレントディレクトリ次第で何を指すか決まらない）
//   - ドライブや共有のルート、ルート直下（C:\vcpkg など）
//   - ホーム・その直下（Documents・Desktop・プロジェクトのフォルダなど）・その祖先（C:\Users）
//   - Windows・Program Files・ProgramData・AppData・Temp の各フォルダ自体とその祖先
//   - 実行中の exe のフォルダとその祖先（エンジン本体）
//   - base を渡したときは、base の中（base 自体は含まない）でないもの
// 断ったら ec = operation_not_permitted、理由は LastRefusal()、SetRefusalHook の関数にも渡す（エンジンはログへ）。
//
// 標準ライブラリ + Win32 だけ（Logger・nlohmann 非依存）。純ロジックの静的ライブラリやテストからも使える。

#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace saferm
{
namespace detail
{
namespace fs = std::filesystem;

inline std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// 比較用の形: 絶対・正規化・区切りは \ ・末尾の区切りなし・小文字
inline std::wstring Key(const fs::path& p)
{
    std::wstring s = p.lexically_normal().make_preferred().wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return Lower(s);
}

// a が b 自身か b の祖先なら true（区切りの境目で比べる。C:\Use は C:\Users の祖先ではない）
inline bool IsSameOrAncestor(const std::wstring& a, const std::wstring& b)
{
    if (a.empty() || b.size() < a.size() || b.compare(0, a.size(), a) != 0) return false;
    if (b.size() == a.size()) return true;
    return a.back() == L'\\' || b[a.size()] == L'\\';
}

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline void AddKnown(std::vector<std::wstring>& out, REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p && *p) out.push_back(Key(p));
    ::CoTaskMemFree(p);
}

inline void AddEnv(std::vector<std::wstring>& out, const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ::GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf) && fs::path(buf).is_absolute()) out.push_back(Key(buf));
}

// 「それ自体もその祖先も消してはいけない」フォルダ。毎回引き直す（環境変数が後から変わっても追従する）。
inline std::vector<std::wstring> ProtectedDirs()
{
    std::vector<std::wstring> v;
    AddKnown(v, FOLDERID_Profile);
    AddKnown(v, FOLDERID_Windows);
    AddKnown(v, FOLDERID_ProgramFiles);
    AddKnown(v, FOLDERID_ProgramFilesX86);
    AddKnown(v, FOLDERID_ProgramData);
    AddKnown(v, FOLDERID_RoamingAppData);
    AddKnown(v, FOLDERID_LocalAppData);
    AddKnown(v, FOLDERID_Documents);
    AddKnown(v, FOLDERID_Desktop);
    AddKnown(v, FOLDERID_Downloads);
    AddKnown(v, FOLDERID_Pictures);
    AddKnown(v, FOLDERID_Videos);
    AddKnown(v, FOLDERID_Music);
    AddKnown(v, FOLDERID_SkyDrive);
    AddKnown(v, FOLDERID_PublicDocuments);
    AddEnv(v, L"USERPROFILE");
    AddEnv(v, L"TEMP");
    AddEnv(v, L"TMP");
    AddEnv(v, L"SystemRoot");
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (n > 0 && n < std::size(exe)) v.push_back(Key(fs::path(exe).parent_path()));
    return v;
}

inline std::string& LastRefusalRef() { thread_local std::string s; return s; }
inline void (*&HookRef())(const std::string&) { static void (*h)(const std::string&) = nullptr; return h; }
} // namespace detail

// 断ったときに呼ぶ関数（エンジンは起動時に Logger::Error を繋ぐ）。nullptr で外す。
inline void SetRefusalHook(void (*hook)(const std::string& message)) { detail::HookRef() = hook; }

// 直近に断った理由（このスレッド）。断っていなければ空。
inline const std::string& LastRefusal() { return detail::LastRefusalRef(); }

// p を再帰削除してよいか。だめなら理由（標準語）、よければ空文字列。base が空でなければ p は base の中に限る。
inline std::string WhyUnsafe(const std::filesystem::path& p, const std::filesystem::path& base = {})
{
    namespace fs = std::filesystem;
    using namespace detail;
    if (p.empty()) return "パスが空です";
    if (!p.is_absolute()) return "絶対パスではありません（カレントディレクトリ次第で別の場所を指します）";
    const fs::path norm = p.lexically_normal();
    const std::wstring k = Key(norm);
    // ルート名・ルートの直下（C:\ と C:\xxx）は断る。共有（\\server\share\xxx）も同じ数え方
    const fs::path rel = norm.relative_path();
    size_t depth = 0;
    for (const auto& part : rel) if (!part.empty() && part != L".") ++depth;
    if (depth == 0) return "ドライブのルートです";
    if (depth == 1) return "ドライブのルート直下のフォルダです";
    for (const std::wstring& prot : ProtectedDirs())
    {
        if (IsSameOrAncestor(k, prot)) return "システムかユーザーの大事なフォルダ（またはその親）です: " + WideToUtf8(prot);
    }
    // ホームの直下（Documents・Desktop・各プロジェクトのフォルダなど）も丸ごとは消さない
    {
        std::vector<std::wstring> homes;
        AddKnown(homes, FOLDERID_Profile);
        AddEnv(homes, L"USERPROFILE");
        for (const std::wstring& h : homes)
            if (IsSameOrAncestor(h, k) && Key(norm.parent_path()) == h) return "ホームの直下のフォルダです";
    }
    if (!base.empty())
    {
        if (!base.is_absolute()) return "基準のフォルダが絶対パスではありません";
        const std::wstring b = Key(base);
        if (k == b || !IsSameOrAncestor(b, k)) return "決められたフォルダ（" + WideToUtf8(b) + "）の中ではありません";
    }
    return {};
}

// 安全なら remove_all、だめなら何も消さずに 0（ec = operation_not_permitted、理由は LastRefusal()）。
inline std::uintmax_t RemoveAll(const std::filesystem::path& p, std::error_code& ec, const std::filesystem::path& base = {})
{
    std::string why = WhyUnsafe(p, base);
    detail::LastRefusalRef().clear();
    if (!why.empty())
    {
        std::string msg = "再帰削除を断りました: " + why + " / 対象: " + detail::WideToUtf8(p.wstring());
        detail::LastRefusalRef() = msg;
        if (auto h = detail::HookRef()) h(msg);
        ec = std::make_error_code(std::errc::operation_not_permitted);
        return 0;
    }
    return std::filesystem::remove_all(p, ec);
}

// 例外版（std::filesystem::remove_all(p) の置き換え）。断ったら filesystem_error。
inline std::uintmax_t RemoveAll(const std::filesystem::path& p, const std::filesystem::path& base = {})
{
    std::error_code ec;
    const std::uintmax_t n = RemoveAll(p, ec, base);
    if (ec) throw std::filesystem::filesystem_error(LastRefusal().empty() ? "remove_all" : LastRefusal(), p, ec);
    return n;
}
} // namespace saferm
