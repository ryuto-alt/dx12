// 新しいトーンマップ（shaders/post/Tonemap.hlsli: UE Filmic / 線形 / Khronos PBR Neutral）の HLSL を実際に GPU で動かし、
// C++ の参照実装（renderer/PhotometricMath.h。numpy 実装と突き合わせ済み）と数値で一致することを確かめる。
//   ・ハードウェア GPU を優先し、無ければ WARP（CPU 実装）。どちらも作れなければ SKIP（0 で終わる）
//   ・dxcompiler.dll / dxil.dll が要る（実行ファイルの隣。CMake が同梱する）。無ければ SKIP
//   ・HLSL の式と C++ の式のうち片方だけ直したときに落ちる（PhotometricMath.h と Tonemap.hlsli は写しの関係）
#include "renderer/PhotometricMath.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <dxcapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace ph = dx12e::photo;

namespace { int g_failures = 0, g_checks = 0; }
#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);    \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{
std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<uint8_t> Compile(const fs::path& file, std::string& log)
{
    HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
    if (!dll) { log = "dxcompiler.dll not found"; return {}; }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
    ComPtr<IDxcUtils> utils; ComPtr<IDxcCompiler3> compiler; ComPtr<IDxcIncludeHandler> inc;
    if (!create || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))
        || FAILED(utils->CreateDefaultIncludeHandler(&inc))) { log = "DXC init failed"; return {}; }
    const std::string src = ReadFile(file);
    if (src.empty()) { log = "source not readable: " + file.string(); return {}; }
    std::vector<std::wstring> a = { L"-E", L"CSMain", L"-T", L"cs_6_0", L"-I", file.parent_path().wstring(),
                                    L"-I", fs::path(DX12E_SHADER_DIR).wstring() };
    std::vector<LPCWSTR> args;
    for (const auto& s : a) args.push_back(s.c_str());
    DxcBuffer buf{ src.data(), src.size(), DXC_CP_UTF8 };
    ComPtr<IDxcResult> res;
    if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), inc.Get(), IID_PPV_ARGS(&res)))) { log = "compile call failed"; return {}; }
    HRESULT st = E_FAIL;
    res->GetStatus(&st);
    ComPtr<IDxcBlobUtf8> errs;
    if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength() > 0)
        log.assign(errs->GetStringPointer(), errs->GetStringLength());
    if (FAILED(st)) return {};
    ComPtr<IDxcBlob> obj;
    if (FAILED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj) { log += " (no object)"; return {}; }
    const auto* p = static_cast<const uint8_t*>(obj->GetBufferPointer());
    return std::vector<uint8_t>(p, p + obj->GetBufferSize());
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

struct Case { ph::Rgb in; };

std::vector<Case> Inputs()
{
    std::vector<Case> v;
    const float grays[] = {0.0f, 1e-4f, 0.001f, 0.01f, 0.05f, 0.1f, 0.18f, 0.3f, 0.5f, 0.8f, 1.0f, 2.0f, 5.0f, 10.0f, 50.0f, 1000.0f};
    for (float g : grays) v.push_back({ {g, g, g} });
    const ph::Rgb colors[] = {
        {0.5f, 0.2f, 0.1f}, {0.05f, 0.4f, 0.05f}, {2.0f, 0.5f, 0.1f}, {0.02f, 0.03f, 0.9f}, {4.0f, 1.0f, 0.2f}, {0.3f, 0.3f, 0.05f},
        {0.001f, 0.002f, 0.003f}, {50.0f, 20.0f, 5.0f}, {0.9f, 0.02f, 0.02f}, {0.02f, 0.9f, 0.02f}, {0.02f, 0.02f, 0.9f},
        {1.0f, 0.5f, 0.0f}, {0.0f, 0.5f, 1.0f}, {8.0f, 0.1f, 0.1f}, {0.0f, 0.0f, 3.0f}, {0.12f, 0.12f, 0.02f} };
    for (const auto& c : colors) v.push_back({ c });
    // 対数掃引（暗部〜明部）と色相の掃引
    for (int i = 0; i < 64; ++i)
    {
        const float x = std::pow(10.0f, -4.0f + 0.09f * static_cast<float>(i));
        v.push_back({ {x, 0.5f * x, 0.25f * x} });
        v.push_back({ {0.25f * x, x, 0.5f * x} });
    }
    return v;
}
} // namespace

int main()
{
    // ---- デバイス（ハードウェア → WARP）----
    ComPtr<ID3D12Device> dev;
    const char* adapterName = "hardware";
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
    {
        ComPtr<IDXGIFactory4> f;
        ComPtr<IDXGIAdapter> warp;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
            adapterName = "WARP";
    }
    if (!dev) { std::printf("SKIP: no D3D12 device (hardware or WARP)\n"); return 0; }

    std::string log;
    const fs::path src = fs::path(DX12E_TEST_DATA_DIR) / "tonemap_probe.hlsl";
    const std::vector<uint8_t> cs = Compile(src, log);
    if (cs.empty())
    {
        if (log.find("dxcompiler.dll not found") != std::string::npos) { std::printf("SKIP: %s\n", log.c_str()); return 0; }
        std::printf("FAIL: shader compile: %s\n", log.c_str());
        return 1;
    }

    // ---- ルートシグネチャ: b0(定数 8 DWORD) + 入力の root SRV + 出力の root UAV ----
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 8;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[2].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 3; rsd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    CHECK_MSG(SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)), "serialize RS");
    ComPtr<ID3D12RootSignature> rs;
    CHECK_MSG(SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs))), "create RS");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rs.Get(); pd.CS = { cs.data(), cs.size() };
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { std::printf("FAIL: create PSO\n"); return 1; }

    const std::vector<Case> inputs = Inputs();
    const UINT n = static_cast<UINT>(inputs.size());
    const UINT64 bytes = static_cast<UINT64>(n) * 16;
    auto inBuf = MakeBuffer(dev.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto outBuf = MakeBuffer(dev.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto rbBuf = MakeBuffer(dev.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!inBuf || !outBuf || !rbBuf) { std::printf("FAIL: buffer allocation\n"); return 1; }
    {
        void* m = nullptr; D3D12_RANGE r{0, 0};
        inBuf->Map(0, &r, &m);
        auto* f = static_cast<float*>(m);
        for (UINT i = 0; i < n; ++i) { f[i * 4 + 0] = inputs[i].in.r; f[i * 4 + 1] = inputs[i].in.g; f[i * 4 + 2] = inputs[i].in.b; f[i * 4 + 3] = 1.0f; }
        inBuf->Unmap(0, nullptr);
    }

    ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;

    const ph::FilmParams film[] = { ph::FilmParams(), [] { ph::FilmParams p; p.slope = 1.1f; p.toe = 0.7f; p.shoulder = 0.4f; p.blackClip = 0.02f; p.whiteClip = 0.0f; return p; }() };
    int compared = 0;
    for (int tm = ph::kTmUeFilmic; tm < ph::kTmCount; ++tm)
    {
        for (const ph::FilmParams& fp : film)
        {
            if (tm != ph::kTmUeFilmic && &fp != &film[0]) continue;   // 線形 / PBR Neutral は film を読まない
            // b0 の並び（tonemap_probe.hlsl の cbuffer と一致）: mode, count, whiteClip, pad, film0(slope, toe, shoulder, blackClip)
            UINT consts[8] = { static_cast<UINT>(tm), n, 0, 0, 0, 0, 0, 0 };
            const float fvals[5] = { fp.whiteClip, 0.0f, fp.slope, fp.toe, fp.shoulder };
            std::memcpy(&consts[2], &fvals[0], sizeof(float) * 2);
            std::memcpy(&consts[4], &fvals[2], sizeof(float) * 3);
            std::memcpy(&consts[7], &fp.blackClip, sizeof(float));

            alloc->Reset(); list->Reset(alloc.Get(), nullptr);
            list->SetComputeRootSignature(rs.Get());
            list->SetPipelineState(pso.Get());
            list->SetComputeRoot32BitConstants(0, 8, consts, 0);
            list->SetComputeRootShaderResourceView(1, inBuf->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2, outBuf->GetGPUVirtualAddress());
            list->Dispatch((n + 63) / 64, 1, 1);
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = outBuf.Get(); b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &b);
            list->CopyBufferRegion(rbBuf.Get(), 0, outBuf.Get(), 0, bytes);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            list->ResourceBarrier(1, &b);
            list->Close();
            ID3D12CommandList* l[] = { list.Get() };
            q->ExecuteCommandLists(1, l);
            q->Signal(fence.Get(), ++fv);
            if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }

            void* m = nullptr; D3D12_RANGE rr{0, static_cast<SIZE_T>(bytes)};
            rbBuf->Map(0, &rr, &m);
            const auto* gpu = static_cast<const float*>(m);
            double worst = 0.0; UINT worstI = 0;
            for (UINT i = 0; i < n; ++i)
            {
                const ph::Rgb ref = ph::ToneMapNewDisplay(tm, inputs[i].in, fp);
                const float g[3] = { gpu[i * 4 + 0], gpu[i * 4 + 1], gpu[i * 4 + 2] };
                const float r[3] = { ref.r, ref.g, ref.b };
                for (int k = 0; k < 3; ++k)
                {
                    const double d = std::fabs(static_cast<double>(g[k]) - static_cast<double>(r[k]));
                    if (!(d <= worst)) { worst = d; worstI = i; }
                    ++compared;
                }
                CHECK_MSG(std::isfinite(g[0]) && std::isfinite(g[1]) && std::isfinite(g[2]), "non-finite GPU output at %u (mode %d)", i, tm);
            }
            D3D12_RANGE wr{0, 0}; rbBuf->Unmap(0, &wr);
            // 表示(0..1)で 8bit の 1/4 LSB(≈ 1e-3)以内。超越関数の実装差(GPU の近似)だけを許す
            CHECK_MSG(worst < 1.0e-3, "mode %d film(slope=%.2f) worst |GPU - CPU| = %.3g at input (%g,%g,%g)", tm, fp.slope, worst,
                      inputs[worstI].in.r, inputs[worstI].in.g, inputs[worstI].in.b);
            std::printf("  mode %d slope %.2f: %u inputs, worst |GPU-CPU| = %.3g\n", tm, fp.slope, n, worst);
        }
    }
    CloseHandle(ev);
    std::printf("TonemapGpuTests (%s): %d values compared, %d checks, %d failures\n", adapterName, compared, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
