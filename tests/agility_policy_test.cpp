// AgilityVersionPolicy（GPU 不要）: Agility の版の方針を検査する。
//   usage: AgilityPolicyCheck <DX12Engine.exe> <GameRuntime.exe>
//   1. lock の SDK が 600 以上 700 未満（プレビュー版を出荷しない。決定 6）
//   2. 2 本の exe が export する D3D12SDKVersion（実際に読み込んで値を読む）と、同梱 DLL の export と、lock の 3 つが一致
//   3. D3D12SDKPath が ".\D3D12\"
//   4. 同梱 D3D12Core.dll の SHA256 が lock と一致
#include <Windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "UnoAgilityLock.h"

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace
{
int g_failures = 0;

void Fail(const std::string& msg)
{
    std::printf("FAIL: %s\n", msg.c_str());
    ++g_failures;
}

std::string Sha256(const fs::path& p)
{
    HANDLE f = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
    {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        while (::ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
            BCryptHashData(h, buf.data(), got, 0);
        unsigned char d[32] = {};
        if (BCryptFinishHash(h, d, sizeof(d), 0) == 0)
        {
            const char* hex = "0123456789abcdef";
            for (unsigned char b : d) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 15]); }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    ::CloseHandle(f);
    return out;
}

// DLL / exe をコードを実行せずに読み込み（DllMain / エントリポイントは呼ばれない）、export の値を読む。
struct Image
{
    HMODULE mod = nullptr;
    explicit Image(const fs::path& p) { mod = ::LoadLibraryExW(p.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES); }
    ~Image() { if (mod) ::FreeLibrary(mod); }
    bool Ok() const { return mod != nullptr; }
    unsigned SdkVersion() const
    {
        const auto* v = reinterpret_cast<const UINT*>(::GetProcAddress(mod, "D3D12SDKVersion"));
        return v ? *v : 0u;
    }
    std::string SdkPath() const
    {
        const auto* pp = reinterpret_cast<const char* const*>(::GetProcAddress(mod, "D3D12SDKPath"));
        return (pp && *pp) ? std::string(*pp) : std::string();
    }
};

void CheckExe(const fs::path& exe)
{
    std::printf("-- %ls\n", exe.c_str());
    Image img(exe);
    if (!img.Ok()) { Fail("exe を読み込めない: " + exe.string()); return; }
    const unsigned sdk = img.SdkVersion();
    std::printf("   D3D12SDKVersion=%u D3D12SDKPath=\"%s\"\n", sdk, img.SdkPath().c_str());
    if (sdk != static_cast<unsigned>(UNO_AGILITY_SDK))
        Fail(exe.filename().string() + ": D3D12SDKVersion=" + std::to_string(sdk) + " が lock の " +
             std::to_string(UNO_AGILITY_SDK) + " と違う");
    if (img.SdkPath() != ".\\D3D12\\")
        Fail(exe.filename().string() + ": D3D12SDKPath が .\\D3D12\\ ではない");

    const fs::path core = exe.parent_path() / "D3D12" / "D3D12Core.dll";
    if (!fs::exists(core)) { Fail("同梱の D3D12Core.dll が無い: " + core.string()); return; }
    Image coreImg(core);
    if (!coreImg.Ok()) { Fail("D3D12Core.dll を読み込めない"); return; }
    const unsigned coreSdk = coreImg.SdkVersion();
    std::printf("   D3D12Core.dll D3D12SDKVersion=%u\n", coreSdk);
    if (coreSdk != sdk)
        Fail("exe の export(" + std::to_string(sdk) + ") と同梱 DLL の export(" + std::to_string(coreSdk) + ") が違う");
    const std::string sha = Sha256(core);
    if (sha != UNO_AGILITY_CORE_SHA256)
        Fail("D3D12Core.dll の SHA256 が lock と違う: " + sha);
}
} // namespace

int main(int argc, char** argv)
{
    static_assert(UNO_AGILITY_SDK >= 600 && UNO_AGILITY_SDK < 700, "プレビュー版（700 以上）は出荷しない");
    std::printf("lock: version=%s sdk=%d\n", UNO_AGILITY_VERSION, static_cast<int>(UNO_AGILITY_SDK));
    if (argc < 2) { std::printf("usage: AgilityPolicyCheck <exe>...\n"); return 2; }
    for (int i = 1; i < argc; ++i)
        CheckExe(fs::path(argv[i]));
    if (g_failures == 0) std::printf("OK\n");
    return g_failures == 0 ? 0 : 1;
}
