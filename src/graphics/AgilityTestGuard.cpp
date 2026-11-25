// テスト exe が Agility のランタイムで走っていることを終了時に検査する（uno_agility_test_exe() が足す）。
// 既存のテストのソースは変えない。静的初期化で atexit を登録し、終了時に
//   D3D12Core.dll が読み込まれていて、かつ export する D3D12SDKVersion が lock の SDK 未満なら
// 終了コード 98 で落とす（in-box のランタイムで黙って走っていた、を検出する）。
// デバイスを作らなかったテスト（SKIP など）は D3D12Core.dll が読み込まれないので何もしない。
#include <Windows.h>

#include <cstdio>
#include <cstdlib>

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

struct Registrar
{
    Registrar() { std::atexit(&CheckAgilityAtExit); }
} g_registrar;
} // namespace
