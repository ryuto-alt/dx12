#include "AgilityRuntime.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <vector>

#include "UnoAgilityLock.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace dx12e
{

namespace
{
std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
}

// 「.」や 8.3 形式を GetFullPathNameW / GetLongPathNameW で正規化してから比べる（D3D12SDKPath の相対指定対策）。
std::wstring Normalize(const std::wstring& path)
{
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD n = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n == 0 || n >= std::size(buf))
        return path;
    std::wstring full(buf, n);
    wchar_t lbuf[MAX_PATH * 4] = {};
    n = ::GetLongPathNameW(full.c_str(), lbuf, static_cast<DWORD>(std::size(lbuf)));
    if (n != 0 && n < std::size(lbuf))
        full.assign(lbuf, n);
    return full;
}

std::atomic<bool> g_debugLayerFailed{false};

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

std::wstring FileVersionString(const std::wstring& path)
{
    DWORD dummy = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0)
        return {};
    std::vector<char> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    // 文字列のファイルバージョン（1.620.2.0.20261106.5 のような形）を優先して読む。
    struct LangCp { WORD lang, cp; };
    LangCp* tr = nullptr;
    UINT trLen = 0;
    if (::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&tr), &trLen) && trLen >= sizeof(LangCp))
    {
        wchar_t sub[64] = {};
        swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileVersion", tr[0].lang, tr[0].cp);
        wchar_t* val = nullptr;
        UINT valLen = 0;
        if (::VerQueryValueW(data.data(), sub, reinterpret_cast<LPVOID*>(&val), &valLen) && val)
            return std::wstring(val);
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (::VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) && ffi)
    {
        wchar_t buf[64] = {};
        swprintf_s(buf, L"%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                   HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        return buf;
    }
    return {};
}
} // namespace

std::uint32_t RequiredAgilitySdk() { return static_cast<std::uint32_t>(UNO_AGILITY_SDK); }
const char*   RequiredAgilityVersion() { return UNO_AGILITY_VERSION; }

std::wstring AgilityRedistDir()
{
    const std::wstring dir = ExeDir();
    return dir.empty() ? std::wstring() : dir + L"\\D3D12";
}

bool AgilityRedistPresent()
{
    const std::wstring dir = AgilityRedistDir();
    if (dir.empty())
        return false;
    const DWORD attr = ::GetFileAttributesW((dir + L"\\D3D12Core.dll").c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Sha256OfFile(const std::wstring& path)
{
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
        && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        std::vector<unsigned char> buf(1 << 20);
        bool ok = true;
        for (;;)
        {
            DWORD got = 0;
            if (!::ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { ok = false; break; }
            if (got == 0) break;
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
        {
            static const char* hex = "0123456789abcdef";
            for (unsigned char b : digest)
            {
                result.push_back(hex[b >> 4]);
                result.push_back(hex[b & 15]);
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    ::CloseHandle(f);
    return result;
}

std::wstring LoadedModulePath(const wchar_t* moduleName)
{
    HMODULE m = ::GetModuleHandleW(moduleName);
    if (!m)
        return {};
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

// テスト exe（リンクに Graphics を含まない）の AgilityTestGuard が読めるよう、プロセス環境変数で伝える。
void NoteDebugLayerFailure()
{
    g_debugLayerFailed.store(true);
    ::SetEnvironmentVariableW(L"UNO_DEBUG_LAYER_FAILED", L"1");
}
bool DebugLayerFailed() { return g_debugLayerFailed.load(); }

AgilityInfo QueryAgilityRuntime()
{
    AgilityInfo info;
    HMODULE core = ::GetModuleHandleW(L"D3D12Core.dll");
    if (!core)
        return info;
    if (const auto* ver = reinterpret_cast<const UINT*>(::GetProcAddress(core, "D3D12SDKVersion")))
        info.loadedSdkVersion = *ver;
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameW(core, buf, static_cast<DWORD>(std::size(buf)));
    info.corePath.assign(buf, n);
    const std::wstring redist = Lower(Normalize(AgilityRedistDir()));
    info.fromRedistDir = !redist.empty() && Lower(Normalize(info.corePath)).rfind(redist + L"\\", 0) == 0;
    info.fileVersion = FileVersionString(info.corePath);
    info.sha256Match = (Sha256OfFile(info.corePath) == UNO_AGILITY_CORE_SHA256);
    return info;
}

} // namespace dx12e
