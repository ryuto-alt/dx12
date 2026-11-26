// AgilityLoadTests（gpu）: テスト exe から作ったデバイスが Agility のランタイムで動いている。
//   loadedSdkVersion >= lock の SDK、fromRedistDir == true（<exe>\D3D12\ から読んだ）、sha256Match == true。
#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <cstdio>

#include "graphics/AgilityRuntime.h"

using Microsoft::WRL::ComPtr;

int main()
{
    ComPtr<ID3D12Device> device;
    const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
    {
        std::printf("FAIL: D3D12CreateDevice が失敗しました (0x%08X)\n", static_cast<unsigned>(hr));
        return 1;
    }

    const dx12e::AgilityInfo info = dx12e::QueryAgilityRuntime();
    std::printf("D3D12Core: %ls\n  fileVersion=%ls loadedSdk=%u required=%u fromRedistDir=%d sha256Match=%d\n",
                info.corePath.c_str(), info.fileVersion.c_str(), info.loadedSdkVersion,
                dx12e::RequiredAgilitySdk(), info.fromRedistDir ? 1 : 0, info.sha256Match ? 1 : 0);

    int failures = 0;
    if (info.loadedSdkVersion < dx12e::RequiredAgilitySdk())
    {
        std::printf("FAIL: 読み込まれた SDK 版 %u が必要な %u 未満\n", info.loadedSdkVersion, dx12e::RequiredAgilitySdk());
        ++failures;
    }
    if (!info.fromRedistDir)
    {
        std::printf("FAIL: <exe>\\D3D12\\ から読み込まれていない\n");
        ++failures;
    }
    if (!info.sha256Match)
    {
        std::printf("FAIL: 読み込まれた D3D12Core.dll の SHA256 が lock と一致しない\n");
        ++failures;
    }
    if (failures == 0)
        std::printf("OK\n");
    return failures == 0 ? 0 : 1;
}
