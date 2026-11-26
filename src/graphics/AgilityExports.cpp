// D3D12 Agility SDK の export。exe に**直接**リンクする（静的ライブラリに入れるとリンカに捨てられる）。
// cmake/UnoAgility.cmake の uno_agility_exe() が DX12Engine / GameRuntime / GPU テスト exe の
// target_sources に足す。版の数字はここに書かず、cmake/agility.lock.json から生成される UnoAgilityLock.h を見る。
#include <Windows.h>

#include <directx/d3d12.h>

#include "UnoAgilityLock.h"

static_assert(D3D12_SDK_VERSION == UNO_AGILITY_SDK,
              "directx-headers と agility.lock.json の版が違う（vcpkg.json の overrides を確認）");

extern "C"
{
    // ランタイムが読む版。D3D12Core.dll がこれ以上の版を export していればそれを使う。
    __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
    // exe のあるフォルダからの相対（作業ディレクトリには依存しない。エディタは --project で cwd が変わる）。
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
