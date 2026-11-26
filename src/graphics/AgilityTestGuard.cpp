// テスト exe が Agility のランタイムで走っていることを検査する（uno_agility_test_exe() が足す）。
// 既存のテストのソースは変えない。静的初期化で次の 3 つをやる。
//   (1) 起動時: <exe>\D3D12\D3D12Core.dll が無ければ即座に終了コード 98 で落とす
//       （GPU テストが raw な D3D12CreateDevice で SKIP して緑になる穴を塞ぐ）。
//   (2) 親から継承した UNO_DEBUG_LAYER_FAILED を消す（このプロセスの中で起きた失敗だけを数える）。
//   (3) atexit を登録し、終了時に
//   D3D12Core.dll が読み込まれていて、かつ export する D3D12SDKVersion が lock の SDK 未満なら
// 終了コード 98 で落とす（in-box のランタイムで黙って走っていた、を検出する）。
// デバイスを作らなかったテスト（SKIP など）は D3D12Core.dll が読み込まれないので何もしない。
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "UnoAgilityLock.h"


namespace
{
void CheckAgilityAtExit()
{
    wchar_t flag[4] = {};
    if (::GetEnvironmentVariableW(L"UNO_DEBUG_LAYER_FAILED", flag, 4) > 0 && flag[0] == L'1')
    {
        std::fprintf(stderr, "[AgilityTestGuard] Debug Layer を要求しましたが D3D12GetDebugInterface が失敗しました。\n");
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 97);
    }
    HMODULE core = ::GetModuleHandleW(L"D3D12Core.dll");
    if (!core)
        return;
    const auto* ver = reinterpret_cast<const UINT*>(::GetProcAddress(core, "D3D12SDKVersion"));
    const UINT loaded = ver ? *ver : 0;
    if (loaded < static_cast<UINT>(UNO_AGILITY_SDK))
    {
        std::fprintf(stderr,
                     "[AgilityTestGuard] D3D12Core.dll の SDK 版 %u が lock の %d 未満です。"
                     "Agility のランタイム（<exe>\\D3D12\\）で走っていません。\n",
                     loaded, static_cast<int>(UNO_AGILITY_SDK));
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 98);
    }
}

void CheckRedistPresentAtStartup()
{
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring dir(buf, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        dir.resize(slash);
    const std::wstring core = dir + L"\\D3D12\\D3D12Core.dll";
    if (::GetFileAttributesW(core.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::fprintf(stderr,
                     "[AgilityTestGuard] <exe>\\D3D12\\D3D12Core.dll がありません。"
                     "Agility のランタイム無しでテストを走らせません（cmake のビルドをやり直す）。\n");
        std::fflush(stderr);
        ::TerminateProcess(::GetCurrentProcess(), 98);
    }
}

struct Registrar
{
    Registrar()
    {
        // 親（エンジンの MCP 経由のテスト起動など）から継承した値は、このプロセスの失敗ではない。
        ::SetEnvironmentVariableW(L"UNO_DEBUG_LAYER_FAILED", nullptr);
        CheckRedistPresentAtStartup();
        std::atexit(&CheckAgilityAtExit);
    }
} g_registrar;
} // namespace
