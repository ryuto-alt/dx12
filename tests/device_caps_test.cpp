// DeviceCapsTests（gpu）: QueryDeviceCaps の全項目が取れ、独立の問い合わせと一致する。
//   SM は 6.9 を期待（Agility のランタイムで動く）。ドライバが古く 6.8 の場合は理由を出して許容する。
#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <set>
#include <string>

#include "graphics/DeviceCaps.h"

using Microsoft::WRL::ComPtr;
using namespace dx12e;

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("FAIL: " __VA_ARGS__);            \
            std::printf("\n");                            \
            ++g_fail;                                     \
        }                                                 \
    } while (0)

int main()
{
    // nullptr でも例外を出さず既定値を返す
    {
        const DeviceCaps none = QueryDeviceCaps(nullptr);
        CHECK(!none.enhancedBarriers && none.raytracingTier == 0 && !none.device10, "nullptr デバイスで既定値");
        CHECK(AsList7(nullptr) == nullptr, "AsList7(nullptr)");
    }

    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
    {
        std::printf("FAIL: D3D12CreateDevice (0x%08X)\n", static_cast<unsigned>(hr));
        return 1;
    }

    const DeviceCaps c = QueryDeviceCaps(device.Get());
    const std::string summary = DeviceCapsSummary(c);
    std::printf("%s\n", summary.c_str());

    // 全項目が重複なく並ぶ
    {
        std::set<std::string> names;
        for (const DeviceCapsField& f : DeviceCapsFields(c))
        {
            CHECK(names.insert(f.name).second, "項目名が重複: %s", f.name);
            std::printf("  %-44s %llu\n", f.name, static_cast<unsigned long long>(f.value));
        }
        CHECK(names.size() >= 21, "項目が 21 未満: %zu", names.size());
    }

    // SM: 独立に 6.9 から下げて確かめる（実装と別のコード）
    {
        UINT expect = 0;
        for (int v = D3D_SHADER_MODEL_6_9; v >= D3D_SHADER_MODEL_6_0 && expect == 0; --v)
        {
            D3D12_FEATURE_DATA_SHADER_MODEL d{ static_cast<D3D_SHADER_MODEL>(v) };
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &d, sizeof(d))))
                expect = static_cast<UINT>(d.HighestShaderModel);
        }
        CHECK(c.highestShaderModel == expect, "SM が独立の問い合わせと不一致 (%X vs %X)", c.highestShaderModel, expect);
        if (c.highestShaderModel < 0x69)
            std::printf("NOTE: SM が 6.9 未満（0x%X）。ドライバが 595 未満なら想定内。\n", c.highestShaderModel);
        CHECK(c.highestShaderModel >= 0x68, "SM が 6.8 未満 (0x%X)", c.highestShaderModel);
    }

    // OPTIONS12 / 16 / 5 / 7 を独立に
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))))
            CHECK(c.enhancedBarriers == (o12.EnhancedBarriersSupported != FALSE), "enhancedBarriers 不一致");
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16))))
            CHECK(c.gpuUploadHeap == (o16.GPUUploadHeapSupported != FALSE), "gpuUploadHeap 不一致");
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
            CHECK(c.raytracingTier == static_cast<UINT>(o5.RaytracingTier), "raytracingTier 不一致");
        CHECK(c.omm == (c.raytracingTier >= static_cast<UINT>(D3D12_RAYTRACING_TIER_1_2)), "omm は DXR 1.2 以上と一致");
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
            CHECK(c.meshShaderTier == static_cast<UINT>(o7.MeshShaderTier), "meshShaderTier 不一致");
    }

    CHECK(c.rootSignatureMaxVersion >= 11, "ルートシグネチャ 1.1 以上のはず (%u)", c.rootSignatureMaxVersion);
    CHECK(c.resourceBindingTier >= 1 && c.resourceBindingTier <= 3, "resourceBindingTier の範囲");

    // device10 と list7 は QueryInterface の可否と一致
    {
        ComPtr<ID3D12Device10> d10;
        CHECK(c.device10 == SUCCEEDED(device.As(&d10)), "device10 不一致");
    }
    if (c.list7)
    {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
            && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
        {
            ID3D12GraphicsCommandList7* l7 = AsList7(list.Get());
            CHECK(l7 != nullptr, "AsList7 が null");
            list->Close();
        }
    }

    // 要約の形
    CHECK(summary.rfind("caps: SM", 0) == 0 && summary.find("EB=") != std::string::npos, "要約の形");

    if (g_fail == 0) std::printf("OK\n");
    return g_fail == 0 ? 0 : 1;
}
