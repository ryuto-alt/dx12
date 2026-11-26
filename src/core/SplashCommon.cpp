#include "core/SplashCommon.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "core/DpiScale.h"
#include "core/SplashTips.h"

namespace dx12e::splash
{

std::wstring Utf8ToWide(std::string_view s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<std::wstring> TipsWide()
{
    std::vector<std::wstring> v;
    v.reserve(kSplashTipCount);
    for (size_t i = 0; i < kSplashTipCount; ++i) v.push_back(Utf8ToWide(kSplashTips[i]));
    return v;
}

std::wstring BuildInfoText()
{
    std::tm tmv{};
    bool ok = false;
    {
        const HMODULE h = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
        if (h && dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(h) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                const std::time_t ts = static_cast<std::time_t>(nt->FileHeader.TimeDateStamp);
                const std::time_t now = std::time(nullptr);
                // /Brepro（再現可能ビルド）だとハッシュ値が入る。2020 年以降かつ未来でなければ本物とみなす。
                if (ts > 1577836800 && ts < now + 86400 && localtime_s(&tmv, &ts) == 0) ok = true;
            }
        }
    }
    wchar_t buf[64];
    if (ok)
    {
        std::swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d ビルド", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        return buf;
    }
    // __DATE__ = "Sep 30 2026" / __TIME__ = "12:34:56"
    static const char* kMon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char mon[4] = {}; int d = 0, y = 0, hh = 0, mm = 0;
    if (sscanf_s(__DATE__, "%3s %d %d", mon, static_cast<unsigned>(sizeof(mon)), &d, &y) == 3 && sscanf_s(__TIME__, "%d:%d", &hh, &mm) == 2)
    {
        int mi = 0;
        for (int i = 0; i < 12; ++i) if (std::strncmp(mon, kMon[i], 3) == 0) mi = i + 1;
        std::swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d ビルド", y, mi, d, hh, mm);
        return buf;
    }
    return {};
}

std::vector<std::wstring> ParseRecentNames(std::string_view json, int max)
{
    std::vector<std::wstring> out;
    try
    {
        const nlohmann::json j = nlohmann::json::parse(json.begin(), json.end());
        if (!j.is_object() || !j.contains("recents") || !j["recents"].is_array()) return out;
        for (const auto& e : j["recents"])
        {
            if (static_cast<int>(out.size()) >= max) break;
            if (!e.is_object()) continue;
            const std::string name = e.value("name", std::string());
            const std::string path = e.value("path", std::string());
            if (name.empty() || path.empty()) continue;
            std::error_code ec;
            const std::u8string u8(path.begin(), path.end());
            if (!std::filesystem::exists(std::filesystem::path(u8), ec)) continue;   // 消えたフォルダは出さない
            out.push_back(Utf8ToWide(name));
        }
    }
    catch (...) { out.clear(); }
    return out;
}

std::vector<std::wstring> RecentProjectNames(int max)
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) return {};
    const std::filesystem::path p = std::filesystem::path(appData) / L"DX12Engine" / L"recent.json";
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return ParseRecentNames(text, max);
}

float EffectiveDpiScale()
{
    using Fn = UINT(WINAPI*)();
    UINT d = 96;
    if (HMODULE u = GetModuleHandleW(L"user32.dll"))
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(u, "GetDpiForSystem"))) { const UINT v = fn(); if (v) d = v; }
    return dpi::EffectiveScale(dpi::DpiToScale(d));
}

float MonitorDpiScale(void* hmonitor)
{
    // Shcore の GetDpiForMonitor（MDT_EFFECTIVE_DPI = 0）。リンクせず動的に引く（Win8.1 以降にある。無ければ従来の倍率）
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (hmonitor)
        if (HMODULE m = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        {
            UINT dx = 0, dy = 0;
            const auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "GetDpiForMonitor"));
            const bool ok = fn && SUCCEEDED(fn(static_cast<HMONITOR>(hmonitor), 0, &dx, &dy)) && dx > 0;
            FreeLibrary(m);
            if (ok) return dpi::EffectiveScale(dpi::DpiToScale(dx));
        }
    return EffectiveDpiScale();
}

bool ParseGaugeStyle(std::wstring_view cmd, std::wstring_view env)
{
    auto lower = [](std::wstring_view v) {
        std::wstring o(v);
        for (auto& c : o) c = static_cast<wchar_t>(::towlower(c));
        return o;
    };
    const std::wstring c = lower(cmd);
    const std::wstring key = L"--splash-style";
    size_t pos = c.find(key);
    while (pos != std::wstring::npos)
    {
        size_t i = pos + key.size();
        if (i < c.size() && (c[i] == L' ' || c[i] == L'=' || c[i] == L'	'))
        {
            while (i < c.size() && (c[i] == L' ' || c[i] == L'=' || c[i] == L'	' || c[i] == L'"')) ++i;
            const std::wstring rest = c.substr(i, 7);
            if (rest.rfind(L"classic", 0) == 0) return false;
            if (rest.rfind(L"gauge", 0) == 0) return true;
        }
        pos = c.find(key, pos + 1);
    }
    return lower(env) != L"classic";
}

bool UseGaugeStyle()
{
    wchar_t env[16] = {};
    GetEnvironmentVariableW(L"UNO_SPLASH_STYLE", env, 16);
    return ParseGaugeStyle(GetCommandLineW(), env);
}

bool ReduceMotionRequested()
{
    wchar_t env[8] = {};
    if (GetEnvironmentVariableW(L"UNO_SPLASH_REDUCE_MOTION", env, 8) > 0 && env[0] != L'0') return true;
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) && !on) return true;
    return false;
}

std::wstring SplashFontPath(const std::wstring& assetsRoot)
{
    if (assetsRoot.empty()) return {};
    const auto p = std::filesystem::path(assetsRoot) / L"editor" / L"splash" / L"fonts" / L"splash_subset.ttf";
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? p.wstring() : std::wstring();
}

} // namespace dx12e::splash
