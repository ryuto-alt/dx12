// DeviceCapsTests（gpu）: QueryDeviceCaps の全項目が取れ、独立の問い合わせと一致する。
//   SM は 6.9 を期待（Agility のランタイムで動く）。ドライバが古く 6.8 の場合は理由を出して許容する。
#include <Windows.h>

#include <directx/d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "core/Window.h"
#include "graphics/DeviceCaps.h"
#include "graphics/GraphicsDevice.h"

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

static bool EnvSet(const char* n)
{
    char b[8]{};
    size_t len = 0;
    return ::getenv_s(&len, b, sizeof(b), n) == 0 && len > 1 && b[0] == '1';
}

// アダプタのベンダと UMD の版。NVIDIA は UMD の版（a.b.c.d）の c の下 1 桁と d の上 2 桁が 5xx.xx の 5xx になる。
struct AdapterInfo { UINT vendor = 0; int nvDriver = 0; unsigned long long umd = 0; };
static AdapterInfo QueryAdapterInfo(ID3D12Device* device)
{
    AdapterInfo r;
    ComPtr<IDXGIFactory4> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return r;
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(f->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&a)))) return r;
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    r.vendor = d.VendorId;
    LARGE_INTEGER v{};
    if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
    {
        r.umd = static_cast<unsigned long long>(v.QuadPart);
        const int c = static_cast<int>((v.QuadPart >> 16) & 0xFFFF);
        const int dd = static_cast<int>(v.QuadPart & 0xFFFF);
        if (d.VendorId == 0x10DE) r.nvDriver = (c % 10) * 100 + dd / 100;
    }
    return r;
}

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
        const AdapterInfo ai = QueryAdapterInfo(device.Get());
        std::printf("アダプタ: vendor=0x%04X UMD=%llu.%llu.%llu.%llu (NVIDIA ドライバ 約 %d)\n", ai.vendor,
                    (ai.umd >> 48) & 0xFFFF, (ai.umd >> 32) & 0xFFFF, (ai.umd >> 16) & 0xFFFF, ai.umd & 0xFFFF, ai.nvDriver);
        if (ai.vendor == 0x10DE && ai.nvDriver >= 595)
            CHECK(c.highestShaderModel >= 0x69, "NVIDIA ドライバ %d（595 以上）なのに SM が 6.9 未満 (0x%X)。ランタイム/ドライバの取り違えの疑い",
                  ai.nvDriver, c.highestShaderModel);
        else
        {
            if (c.highestShaderModel < 0x69)
                std::printf("NOTE: SM 0x%X（6.9 未満）を許容: NVIDIA ドライバが 595 未満または NVIDIA 以外のため。\n", c.highestShaderModel);
            CHECK(c.highestShaderModel >= 0x68, "SM が 6.8 未満 (0x%X)", c.highestShaderModel);
        }
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

    // GraphicsDevice の getter が GetCaps() と一致する（GraphicsDevice::Initialize は窓を使わない）
    {
        Window window;
        GraphicsDevice dev;
        try { dev.Initialize(window); }
        catch (...) { std::printf("FAIL: GraphicsDevice::Initialize\n"); return 1; }
        const DeviceCaps* gc = dev.GetCaps();
        CHECK(gc != nullptr, "GetCaps() が null");
        if (gc)
        {
            CHECK(static_cast<u32>(dev.GetHighestShaderModel()) == gc->highestShaderModel, "GetHighestShaderModel 不一致");
            CHECK(static_cast<u32>(dev.GetRaytracingTier()) == gc->raytracingTier, "GetRaytracingTier 不一致");
            CHECK(static_cast<u32>(dev.GetResourceBindingTier()) == gc->resourceBindingTier, "GetResourceBindingTier 不一致");
            CHECK(static_cast<u32>(dev.GetMeshShaderTier()) == gc->meshShaderTier, "GetMeshShaderTier 不一致");
            CHECK(dev.SupportsDxr12() == (gc->raytracingTier >= static_cast<u32>(D3D12_RAYTRACING_TIER_1_2)), "SupportsDxr12 不一致");
            CHECK((dev.GetDevice10() != nullptr) == gc->device10, "GetDevice10 と caps.device10 不一致");
            // 上書きが無ければ、素の問い合わせとも一致する
            if (!EnvSet("DX12_DISABLE_DXR") && !EnvSet("DX12_DISABLE_MESHSHADER"))
            {
                CHECK(gc->highestShaderModel == c.highestShaderModel, "GraphicsDevice の caps と素の問い合わせで SM 不一致");
                CHECK(gc->raytracingTier == c.raytracingTier, "GraphicsDevice の caps と素の問い合わせで DXR tier 不一致");
            }
        }
    }
    // 環境変数の上書きが caps に反映される
    {
        _putenv_s("DX12_DISABLE_DXR", "1");
        _putenv_s("DX12_DISABLE_MESHSHADER", "1");
        Window window;
        GraphicsDevice dev;
        try { dev.Initialize(window); }
        catch (...) { std::printf("FAIL: GraphicsDevice::Initialize（上書き）\n"); return 1; }
        const DeviceCaps* gc = dev.GetCaps();
        CHECK(gc && gc->raytracingTier == 0 && !gc->omm && !gc->serActuallyReorders, "DX12_DISABLE_DXR が caps に反映されない");
        CHECK(gc && gc->meshShaderTier == 0 && !gc->meshShader, "DX12_DISABLE_MESHSHADER が caps に反映されない");
        CHECK(!dev.SupportsDxr12() && dev.GetMeshShaderTier() == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED, "getter に上書きが反映されない");
        _putenv_s("DX12_DISABLE_DXR", "");
        _putenv_s("DX12_DISABLE_MESHSHADER", "");
    }

    // 要約の形
    CHECK(summary.rfind("caps: SM", 0) == 0 && summary.find("EB=") != std::string::npos, "要約の形");

    if (g_fail == 0) std::printf("OK\n");
    return g_fail == 0 ? 0 : 1;
}
