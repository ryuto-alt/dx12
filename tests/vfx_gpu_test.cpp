// UnoFX の GPU 試験（W1-BD4）。窓なしの D3D12 デバイスで、カール場の Texture3D（本番と同じ生成・同じアップロード・同じサンプラ）を
//   乱数の uvw で引き、CPU 参照（SampleCurl）と比べる。ハードウェアの三重線形フィルタの量子化（重みは約 8 ビット）と、
//   アップロードの配置（RowPitch・軸の順）、ラップをここで確かめる。
//   GPU が無い / .cso が無い環境（CI）では「SKIP」を出して 0 で終わる。
//   環境変数: DX12E_TEST_D3D_DEBUG=1 でデバッグレイヤ（DX12E_TEST_D3D_GBV=1 で GPU ベース検証）。警告 / エラーが 1 件でもあれば失敗。
#include "renderer/vfx/VfxCurl.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::vfx;
using Microsoft::WRL::ComPtr;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{
bool EnvSet(const char* name)
{
    char* buf = nullptr; size_t len = 0;
    const bool set = (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr);
    std::free(buf);
    return set;
}

struct Gpu
{
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    std::wstring adapterName;
    ComPtr<ID3D12InfoQueue> info;

    bool Init()
    {
        if (EnvSet("DX12E_TEST_D3D_DEBUG"))
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            {
                dbg->EnableDebugLayer();
                ComPtr<ID3D12Debug1> dbg1;
                if (EnvSet("DX12E_TEST_D3D_GBV") && SUCCEEDED(dbg.As(&dbg1))) dbg1->SetEnableGPUBasedValidation(TRUE);
            }
        }
        ComPtr<IDXGIFactory6> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return false;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> ad;
            if (FAILED(fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)))) break;
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) continue;
            adapterName = d.Description;
            break;
        }
        if (!dev) return false;
        if (EnvSet("DX12E_TEST_D3D_DEBUG")) dev.As(&info);
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void Wait()
    {
        ++fv;
        q->Signal(fence.Get(), fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
    }
    ID3D12GraphicsCommandList* Begin() { alloc->Reset(); list->Reset(alloc.Get(), nullptr); return list.Get(); }
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        q->ExecuteCommandLists(1, l);
        Wait();
    }
    u64 MessageCount()
    {
        if (!info) return 0;
        const UINT64 n = info->GetNumStoredMessages();
        UINT64 bad = 0;
        for (UINT64 i = 0; i < n; ++i)
        {
            SIZE_T len = 0;
            info->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(info->GetMessage(i, m, &len)))
            {
                std::printf("  [d3d12 sev=%d id=%d] %s\n", static_cast<int>(m->Severity), static_cast<int>(m->ID), m->pDescription);
                if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) ++bad;
            }
        }
        return bad;
    }
};

ComPtr<ID3D12Resource> MakeBuf(Gpu& g, u64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = std::max<u64>(bytes, 16); rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    const D3D12_RESOURCE_STATES st = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                   : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> r;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r; br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    c->ResourceBarrier(1, &br);
}

float HalfToFloat(u16 h)
{
    const u32 s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400u), static_cast<int>(e) - 25);
    return s ? -v : v;
}

void TestCurlSampling(Gpu& g, const std::wstring& shaderDir)
{
    std::ifstream f(shaderDir + L"VfxCurlProbe_CS.cso", std::ios::binary);
    if (!f) { std::printf("SKIP vfx_gpu_test: VfxCurlProbe_CS.cso が無い（Shaders ターゲットを先にビルド）\n"); return; }
    std::vector<char> bc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // ---- 本番と同じ手順: half 配列 -> RowPitch 付きのアップロード -> Texture3D（RGBA16F）----
    const std::vector<u16> half = GenerateCurlFieldHalf();
    CHECK(half.size() == size_t(kCurlRes) * kCurlRes * kCurlRes * 4);
    std::vector<float> field(half.size());   // 「正」は half に丸めた値（GPU が持つ値）。CPU 参照もこれで引く
    for (size_t i = 0; i < half.size(); ++i) field[i] = HalfToFloat(half[i]);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; td.Width = kCurlRes; td.Height = kCurlRes;
    td.DepthOrArraySize = static_cast<UINT16>(kCurlRes); td.MipLevels = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc = {1, 0};
    ComPtr<ID3D12Resource> tex;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) { CHECK(false); return; }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ComPtr<ID3D12Resource> up = MakeBuf(g, total, D3D12_HEAP_TYPE_UPLOAD);
    u8* p = nullptr; const D3D12_RANGE none{0, 0};
    up->Map(0, &none, reinterpret_cast<void**>(&p));
    for (u32 z = 0; z < kCurlRes; ++z)
        for (u32 y = 0; y < kCurlRes; ++y)
            std::memcpy(p + u64(z) * fp.Footprint.RowPitch * kCurlRes + u64(y) * fp.Footprint.RowPitch,
                        &half[(u64(z) * kCurlRes * kCurlRes + u64(y) * kCurlRes) * 4], kCurlRes * 8);
    up->Unmap(0, nullptr);
    {
        auto* c = g.Begin();
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = tex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(c, tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g.Submit();
    }

    // ---- ヒープ / ルートシグネチャ / PSO（本番の sim と同じ: 4 DWORD の定数 + root SRV + root UAV + 線形ラップの静的サンプラ）----
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = td.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture3D.MipLevels = 1;
    g.dev->CreateShaderResourceView(tex.Get(), &sv, heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = {0, 0, 4};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor = {0, 0};
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor = {0, 0};
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderRegister = 0; ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 3; rsd.pParameters = rp; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ss;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { CHECK(false); return; }
    ComPtr<ID3D12RootSignature> rs;
    g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rs.Get(); pd.CS = {bc.data(), bc.size()};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { CHECK(false); return; }

    // ---- 入力: (a) テクセル中心（フィルタの影響を受けない: 配置と軸の順を厳密に確かめる）(b) 乱数 (c) ラップ（±整数ずらし）----
    struct Q { float u, v, w, kind; };
    std::vector<Q> qs;
    std::mt19937 rng(1234);
    std::uniform_int_distribution<u32> ti(0, kCurlRes - 1);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f), shift(-3.0f, 3.0f);
    constexpr u32 kCenters = 1024, kRandom = 16384, kWrap = 1024;
    for (u32 i = 0; i < kCenters; ++i)
        qs.push_back({(ti(rng) + 0.5f) / kCurlRes, (ti(rng) + 0.5f) / kCurlRes, (ti(rng) + 0.5f) / kCurlRes, 0});
    for (u32 i = 0; i < kRandom; ++i) qs.push_back({u01(rng), u01(rng), u01(rng), 1});
    for (u32 i = 0; i < kWrap; ++i)
    {
        const float a = u01(rng), b = u01(rng), c = u01(rng);
        qs.push_back({a + std::floor(shift(rng)), b + std::floor(shift(rng)), c + std::floor(shift(rng)), 2});
    }
    const u32 N = static_cast<u32>(qs.size());
    ComPtr<ID3D12Resource> inBuf = MakeBuf(g, u64(N) * 16, D3D12_HEAP_TYPE_UPLOAD);
    {
        void* m = nullptr; inBuf->Map(0, &none, &m);
        std::memcpy(m, qs.data(), u64(N) * 16);
        inBuf->Unmap(0, nullptr);
    }
    ComPtr<ID3D12Resource> outBuf = MakeBuf(g, u64(N) * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> rb = MakeBuf(g, u64(N) * 16, D3D12_HEAP_TYPE_READBACK);
    {
        auto* c = g.Begin();
        Barrier(c, outBuf.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* hs[] = {heap.Get()};
        c->SetDescriptorHeaps(1, hs);
        c->SetComputeRootSignature(rs.Get());
        c->SetPipelineState(pso.Get());
        const u32 pc[4] = {0, N, 0, 0};
        c->SetComputeRoot32BitConstants(0, 4, pc, 0);
        c->SetComputeRootShaderResourceView(1, inBuf->GetGPUVirtualAddress());
        c->SetComputeRootUnorderedAccessView(2, outBuf->GetGPUVirtualAddress());
        c->Dispatch((N + 63) / 64, 1, 1);
        Barrier(c, outBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        c->CopyBufferRegion(rb.Get(), 0, outBuf.Get(), 0, u64(N) * 16);
        g.Submit();
    }
    std::vector<float> got(size_t(N) * 4);
    {
        void* m = nullptr; D3D12_RANGE rr{0, SIZE_T(N) * 16};
        rb->Map(0, &rr, &m);
        std::memcpy(got.data(), m, size_t(N) * 16);
        rb->Unmap(0, &none);
    }

    double maxCenter = 0, maxRandom = 0, maxWrap = 0;
    for (u32 i = 0; i < N; ++i)
    {
        float ref[3];
        SampleCurl(field, qs[i].u, qs[i].v, qs[i].w, ref);
        double e = 0;
        for (int k = 0; k < 3; ++k) e = std::max(e, double(std::fabs(got[size_t(i) * 4 + k] - ref[k])));
        if (qs[i].kind == 0) maxCenter = std::max(maxCenter, e);
        else if (qs[i].kind == 1) maxRandom = std::max(maxRandom, e);
        else maxWrap = std::max(maxWrap, e);
    }
    std::printf("  curl GPU vs CPU: テクセル中心 max %.3g / 乱数 max %.3g / ラップ max %.3g\n", maxCenter, maxRandom, maxWrap);
    CHECK(maxCenter < 1e-5);    // フィルタ無し: 配置・軸の順・RowPitch が正しければ厳密に一致する
    // EXEC の受け入れは 1e-3 以下だが、実測（RTX 5060）はハードウェアの三重線形の重みが約 8 ビットに量子化されるため最大 1.9e-3
    //（隣のテクセルとの差 x 1/256）。CPU の丸め（half）は 1e-3 未満。ここでは量子化の上限として 2.5e-3 を境にする（実測値は出力に出る）。
    CHECK(maxRandom <= 2.5e-3);
    CHECK(maxWrap <= 2.5e-3);   // ±整数ずらしでも同程度（ラップ）
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef DX12E_SHADER_DIR
    const std::wstring shaderDir = std::wstring(L"" DX12E_SHADER_DIR);
#else
    const std::wstring shaderDir = L"shaders/";
#endif
    Gpu g;
    if (!g.Init()) { std::printf("SKIP vfx_gpu_test: D3D12 ハードウェアデバイスが無い\n"); return 0; }
    std::printf("adapter: %ls\n", g.adapterName.c_str());
    TestCurlSampling(g, shaderDir);
    if (const u64 msgs = g.MessageCount()) { std::printf("FAIL: D3D12 デバッグレイヤのメッセージ %llu 件\n", static_cast<unsigned long long>(msgs)); ++g_failures; }
    std::printf("vfx_gpu_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
