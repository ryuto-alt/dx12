// マテリアルグラフ G2a: メインのルートシグネチャに立てた CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED（バインドレス）の実機検証。
//
//   実 GPU（ハードウェアアダプタ）が要る。デバイスが作れない / SM 6.6 + Resource Binding Tier 3 が無い環境では
//   「SKIP」を出して 0 で終わる（CI を落とさない。非対応 GPU ではエンジンもフラグを立てず、G2b 以降の機能だけが無効になる）。
//   環境変数 DX12E_TEST_D3D_DEBUG=1 でデバッグレイヤを有効にする（SetDescriptorHeaps の順序制約などを拾う）。
//
//   1. RS: RootSignature::IsBindless() == SupportsDynamicResources()。直列化したブロブのフラグと DWORD 数（62/64 のまま）。
//   2. PSO: 既存のフォワード系（Forward / ForwardSkinned / Terrain / LDR / ALPHA_TEST / 半透明ブレンド）と
//      カスタムシェーダー（水テンプレート）がフラグ付きメイン RS で作れる。G2b 予行 probe（ps_6_6 + ResourceDescriptorHeap[]）も作れる。
//      対照として、フラグ無しの RS で probe の PSO が作れるかも記録する（ドライバによっては作成時でなく実行時に弾くので失敗にはしない）。
//   3. SRV 添字: ResourceManager が返す永続 SRV 添字（GetOrLoadTextureSrvIndex）が
//        ・一意で容量内（AuditTextureSrvIndices）
//        ・ホットリロード（ReloadChangedAssets, 中身を書き換えて force）の前後で不変（SnapshotTextureSrvIndices）
//        ・実 GPU の compute から ResourceDescriptorHeap[添字] で引くと、そのテクスチャの色が読める（書き換え後は新しい色）
//      ことを確かめる。これが G2b（パラメータプールへテクスチャの添字を書く）の前提。
#include "core/Logger.h"
#include "core/Window.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/PipelineState.h"
#include "graphics/RootSignature.h"
#include "graphics/Texture.h"
#include "renderer/Mesh.h"
#include "resource/ModelLoader.h"      // CachedModel の unique_ptr 群の完全型（Mesh / Material / Skeleton / AnimationClip / NodeGraph）
#include "resource/ResourceManager.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dx12e;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks   = 0;
}

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
#define CHECK(cond) CHECK_MSG(cond, "")

namespace
{

bool EnvSet(const char* name)
{
    char* buf = nullptr;
    size_t len = 0;
    const bool set = (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr);
    std::free(buf);
    return set;
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---- DXC（実行時ロード）----------------------------------------------------------------
struct Dxc
{
    ComPtr<IDxcUtils>          utils;
    ComPtr<IDxcCompiler3>      compiler;
    ComPtr<IDxcIncludeHandler> inc;

    bool Init()
    {
        HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
        if (!dll) return false;
        auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
        if (!create) return false;
        if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return false;
        if (FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return false;
        return SUCCEEDED(utils->CreateDefaultIncludeHandler(&inc));
    }

    std::vector<uint8_t> Compile(const fs::path& file, const wchar_t* entry, const wchar_t* profile,
                                 const wchar_t* define, std::string& log) const
    {
        const std::string src = ReadFile(file);
        if (src.empty()) { log = "ソースが読めない: " + file.string(); return {}; }
        std::vector<std::wstring> a = { L"-E", entry, L"-T", profile, L"-I", file.parent_path().wstring(),
                                        L"-I", fs::path(DX12E_SHADER_DIR).wstring() };
        if (define && *define) { a.push_back(L"-D"); a.push_back(define); }
        std::vector<LPCWSTR> args;
        for (const auto& s : a) args.push_back(s.c_str());
        DxcBuffer buf{ src.data(), src.size(), DXC_CP_UTF8 };
        ComPtr<IDxcResult> res;
        if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), inc.Get(), IID_PPV_ARGS(&res)))) return {};
        HRESULT st = E_FAIL;
        res->GetStatus(&st);
        ComPtr<IDxcBlobUtf8> errs;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength() > 0)
            log.assign(errs->GetStringPointer(), errs->GetStringLength());
        if (FAILED(st)) return {};
        ComPtr<IDxcBlob> obj;
        if (FAILED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) || !obj) return {};
        const auto* p = static_cast<const uint8_t*>(obj->GetBufferPointer());
        return std::vector<uint8_t>(p, p + obj->GetBufferSize());
    }
};

// ---- 最小のコマンド実行ハーネス ----------------------------------------------------------
struct Exec
{
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    UINT64 fv = 0;

    bool Init(ID3D12Device* dev)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;   // list は記録中の状態で返る
    }
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = { list.Get() };
        q->ExecuteCommandLists(1, l);
        ++fv;
        q->Signal(fence.Get(), fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
    }
    void Reset()
    {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
    }
};

// 単色の 4x4 24bit BMP（WIC が読める最小のフォーマット）
void WriteBmp(const fs::path& path, uint8_t r, uint8_t g, uint8_t b)
{
    constexpr int W = 4, H = 4;
    std::vector<uint8_t> f(54 + W * H * 3, 0);
    auto le32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) f[o + i] = uint8_t(v >> (8 * i)); };
    f[0] = 'B'; f[1] = 'M';
    le32(2, uint32_t(f.size())); le32(10, 54); le32(14, 40); le32(18, W); le32(22, H);
    f[26] = 1; f[28] = 24; le32(34, W * H * 3);
    for (int i = 0; i < W * H; ++i) { f[54 + i * 3 + 0] = b; f[54 + i * 3 + 1] = g; f[54 + i * 3 + 2] = r; }
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

// ---- PSO 作成 -------------------------------------------------------------------------------
struct PsoCase
{
    const char* name;
    const char* vsFile; const wchar_t* vsProfile;
    const char* psFile; const wchar_t* psProfile;
    const wchar_t* define;
    DXGI_FORMAT rtv;
    bool alphaBlend;
};

bool TryBuildPso(GraphicsDevice& dev, ID3D12RootSignature* rs, const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps,
                 DXGI_FORMAT rtv, bool alphaBlend, std::string& err)
{
    try
    {
        PipelineStateBuilder b;
        b.SetRootSignature(rs)
         .SetVertexShader(vs.data(), vs.size())
         .SetPixelShader(ps.data(), ps.size())
         .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
         .SetRenderTargetFormat(rtv)
         .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
         .SetDepthEnabled(true)
         .SetCullMode(D3D12_CULL_MODE_NONE);
        if (alphaBlend) b.SetAlphaBlendEnabled(true).SetDepthWrite(false);
        auto pso = b.Build(dev);
        return pso != nullptr;
    }
    catch (const std::exception& e) { err = e.what(); return false; }
    catch (...) { err = "unknown exception"; return false; }
}

// ---- compute で ResourceDescriptorHeap[idx] を読む（実 GPU での「添字 → テクスチャ」の検証）------------
struct HeapReader
{
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> out, readback;
    static constexpr UINT kMax = 16;

    bool Init(ID3D12Device* dev, const std::vector<uint8_t>& cs)
    {
        D3D12_ROOT_PARAMETER1 p[2]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.Num32BitValues = 1;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[1].Descriptor.ShaderRegister = 0;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vd{};
        vd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        vd.Desc_1_1.NumParameters = 2;
        vd.Desc_1_1.pParameters = p;
        vd.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeVersionedRootSignature(&vd, &blob, &err))) return false;
        if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = rs.Get();
        cd.CS = { cs.data(), cs.size() };
        if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)))) return false;

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kMax * 16; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc = { 1, 0 }; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)))) return false;
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return false;
        return true;
    }

    // srvHeap の各添字のテクスチャの (0,0) を読み、RGBA を返す。★SetDescriptorHeaps は RS を Set する【前】。
    std::vector<float> Read(Exec& ex, DescriptorHeap& srvHeap, const std::vector<Texture*>& texs, const std::vector<u32>& idx)
    {
        std::vector<float> res;
        ex.Reset();
        ID3D12DescriptorHeap* heaps[] = { srvHeap.GetHeap() };
        ex.list->SetDescriptorHeaps(1, heaps);
        ex.list->SetComputeRootSignature(rs.Get());
        ex.list->SetPipelineState(pso.Get());
        for (Texture* t : texs)   // compute から読むので NON_PIXEL_SHADER_RESOURCE も要る
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = t->GetResource();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t->GetCurrentState();
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (b.Transition.StateBefore != b.Transition.StateAfter) ex.list->ResourceBarrier(1, &b);
            t->SetCurrentState(b.Transition.StateAfter);
        }
        for (size_t i = 0; i < idx.size() && i < kMax; ++i)
        {
            ex.list->SetComputeRoot32BitConstant(0, idx[i], 0);
            ex.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress() + i * 16);
            ex.list->Dispatch(1, 1, 1);
        }
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; tb.Transition.pResource = out.Get();
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        ex.list->ResourceBarrier(1, &tb);
        ex.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, kMax * 16);
        std::swap(tb.Transition.StateBefore, tb.Transition.StateAfter);
        ex.list->ResourceBarrier(1, &tb);
        ex.Submit();
        void* m = nullptr;
        D3D12_RANGE rr{ 0, kMax * 16 };
        if (SUCCEEDED(readback->Map(0, &rr, &m)))
        {
            const float* f = static_cast<const float*>(m);
            res.assign(f, f + idx.size() * 4);
            D3D12_RANGE wr{ 0, 0 };
            readback->Unmap(0, &wr);
        }
        return res;
    }
};

const char* kHeapReadCs =
    "cbuffer C : register(b0) { uint idx; };\n"
    "RWByteAddressBuffer o : register(u0);\n"
    "[numthreads(1, 1, 1)]\n"
    "void CSMain()\n"
    "{\n"
    "    Texture2D<float4> t = ResourceDescriptorHeap[idx];\n"
    "    const float4 c = t.Load(int3(0, 0, 0));\n"
    "    o.Store4(0, asuint(c));\n"
    "}\n";

int CountRootDwords(const D3D12_ROOT_SIGNATURE_DESC1& d)
{
    int n = 0;
    for (UINT i = 0; i < d.NumParameters; ++i)
    {
        switch (d.pParameters[i].ParameterType)
        {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: n += int(d.pParameters[i].Constants.Num32BitValues); break;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: n += 1; break;
        default: n += 2; break;   // ルート CBV / SRV / UAV = 64bit アドレス
        }
    }
    return n;
}

bool Near(float a, float b) { return std::fabs(a - b) <= 1.5f / 255.0f; }

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 異常終了しても途中経過が残るように
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC（テクスチャ読み込み）が要る
    // DX12_D3D_DEBUG=1 のときは D3D12 デバッグレイヤの指摘（GraphicsDevice が Logger へ流す）をコンソールへ出す
    if (EnvSet("DX12_D3D_DEBUG")) Logger::Init();
    // ---- デバイス ----
    Window window;   // GraphicsDevice::Initialize は窓を使わない（引数の互換だけ）
    GraphicsDevice dev;
    try { dev.Initialize(window); }
    catch (...) { std::printf("SKIP: D3D12 デバイスが作れない（GPU なし）\n"); return 0; }
    if (!dev.SupportsDynamicResources())
    {
        std::printf("SKIP: SM 6.6 + Resource Binding Tier 3 が無い GPU（メイン RS はフラグ無しのまま = 仕様どおりの縮退）\n");
        RootSignature rs;
        rs.Initialize(dev);
        CHECK_MSG(!rs.IsBindless(), "非対応 GPU なのに IsBindless() が真");
        std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    std::printf("GPU: SM=%d.%d bindingTier=%d\n", (int(dev.GetHighestShaderModel()) >> 4) & 0xF, int(dev.GetHighestShaderModel()) & 0xF,
                int(dev.GetResourceBindingTier()));
    ID3D12Device* d3d = dev.GetDevice();

    std::printf("stage: RS\n");
    // ---- 1. RS ----
    RootSignature rs;
    rs.Initialize(dev);
    const bool envOff = EnvSet("DX12_DISABLE_MAIN_BINDLESS");
    CHECK_MSG(rs.IsBindless() == !envOff, "IsBindless()=%d（DX12_DISABLE_MAIN_BINDLESS=%d）", rs.IsBindless() ? 1 : 0, envOff ? 1 : 0);
    {
        ComPtr<ID3D12VersionedRootSignatureDeserializer> des;
        const HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(rs.GetSerializedBlob()->GetBufferPointer(),
                                                                         rs.GetSerializedBlob()->GetBufferSize(), IID_PPV_ARGS(&des));
        CHECK(SUCCEEDED(hr));
        if (SUCCEEDED(hr))
        {
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
            des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd);
            CHECK(vd != nullptr);
            if (vd)
            {
                const bool flag = (vd->Desc_1_1.Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) != 0;
                CHECK_MSG(flag == rs.IsBindless(), "直列化ブロブのフラグ(%d)と IsBindless()(%d)が食い違う", flag ? 1 : 0, rs.IsBindless() ? 1 : 0);
                CHECK_MSG((vd->Desc_1_1.Flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0, "IA フラグが消えている");
                const int dw = CountRootDwords(vd->Desc_1_1);
                CHECK_MSG(dw == 62, "ルート DWORD が %d（62 のはず。フラグは DWORD を消費しない）", dw);
                CHECK_MSG(vd->Desc_1_1.NumStaticSamplers == 9, "静的サンプラが %u 個（9 = 既存 6 + グラフ用 s6..s8 のはず）", vd->Desc_1_1.NumStaticSamplers);
            }
        }
    }
    std::printf("メイン RS: bindless=%s\n", rs.IsBindless() ? "on" : "off");

    std::printf("stage: PSO\n");
    // ---- 2. PSO ----
    Dxc dxc;
    if (!dxc.Init())
    {
        std::printf("SKIP: dxcompiler.dll が読めない（PSO / 添字の検証は実施しない）\n");
        std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    const fs::path sh = fs::path(DX12E_SHADER_DIR);
    const fs::path fwd = sh / "forward";
    const PsoCase cases[] = {
        { "Forward",              "forward/Forward.hlsl",        L"vs_6_0", "forward/Forward.hlsl",        L"ps_6_0", nullptr,             DXGI_FORMAT_R16G16B16A16_FLOAT, false },
        { "Forward+blend",        "forward/Forward.hlsl",        L"vs_6_0", "forward/Forward.hlsl",        L"ps_6_0", nullptr,             DXGI_FORMAT_R16G16B16A16_FLOAT, true  },
        { "ForwardLdr",           "forward/Forward.hlsl",        L"vs_6_0", "forward/Forward.hlsl",        L"ps_6_0", L"LDR_OUTPUT=1",     DXGI_FORMAT_R8G8B8A8_UNORM,     false },
        { "ForwardMask",          "forward/Forward.hlsl",        L"vs_6_0", "forward/Forward.hlsl",        L"ps_6_0", L"ALPHA_TEST=1",     DXGI_FORMAT_R16G16B16A16_FLOAT, false },
        { "ForwardSkinned",       "forward/ForwardSkinned.hlsl", L"vs_6_0", "forward/ForwardSkinned.hlsl", L"ps_6_0", nullptr,             DXGI_FORMAT_R16G16B16A16_FLOAT, false },
        { "Terrain",              "forward/Terrain.hlsl",        L"vs_6_0", "forward/Terrain.hlsl",        L"ps_6_0", nullptr,             DXGI_FORMAT_R16G16B16A16_FLOAT, false },
        { "CustomWater(template)", "templates/Water.hlsl",       L"vs_6_0", "templates/Water.hlsl",        L"ps_6_0", nullptr,             DXGI_FORMAT_R16G16B16A16_FLOAT, true  },
    };
    for (const PsoCase& c : cases)
    {
        std::string log, err;
        // 各 .hlsl は VSMain / PSMain。Terrain / Skinned の VS は同じファイルの VSMain。
        auto vs = dxc.Compile(sh / c.vsFile, L"VSMain", c.vsProfile, c.define, log);
        auto ps = dxc.Compile(sh / c.psFile, L"PSMain", c.psProfile, c.define, log);
        CHECK_MSG(!vs.empty() && !ps.empty(), "%s のコンパイルに失敗: %s", c.name, log.substr(0, 800).c_str());
        if (vs.empty() || ps.empty()) continue;
        const bool ok = TryBuildPso(dev, rs.Get(), vs, ps, c.rtv, c.alphaBlend, err);
        CHECK_MSG(ok, "%s の PSO がメイン RS（bindless=%d）で作れない: %s", c.name, rs.IsBindless() ? 1 : 0, err.c_str());
    }

    // G2b 予行 probe: ps_6_6 + ResourceDescriptorHeap[]。フラグ付きなら作れ、フラグ無しでは作れない。
    {
        std::string log, err;
        const fs::path probe = fs::path(DX12E_TEST_DATA_DIR) / "forward_shade_probe.hlsl";
        auto vs = dxc.Compile(fwd / "Forward.hlsl", L"VSMain", L"vs_6_0", nullptr, log);
        auto ps = dxc.Compile(probe, L"PSMain", L"ps_6_6", nullptr, log);
        CHECK_MSG(!vs.empty() && !ps.empty(), "probe のコンパイルに失敗: %s", log.substr(0, 800).c_str());
        if (!vs.empty() && !ps.empty())
        {
            if (rs.IsBindless())
            {
                const bool ok = TryBuildPso(dev, rs.Get(), vs, ps, DXGI_FORMAT_R16G16B16A16_FLOAT, false, err);
                CHECK_MSG(ok, "probe（バインドレス）の PSO がフラグ付きメイン RS で作れない: %s", err.c_str());

                // 対照: フラグ無しのメイン RS を作ると（環境変数で強制）probe の PSO は作れない
                _putenv_s("DX12_DISABLE_MAIN_BINDLESS", "1");
                RootSignature legacy;
                legacy.Initialize(dev);
                _putenv_s("DX12_DISABLE_MAIN_BINDLESS", "");
                CHECK_MSG(!legacy.IsBindless(), "DX12_DISABLE_MAIN_BINDLESS=1 でもフラグが立った");
                std::string err2;
                const bool legacyOk = TryBuildPso(dev, legacy.Get(), vs, ps, DXGI_FORMAT_R16G16B16A16_FLOAT, false, err2);
                // ★ドライバによっては PSO 作成時ではなく実行時に弾くので、「作れてしまった」は失敗にせず記録だけ残す。
                std::printf("対照: フラグ無し RS での probe の PSO 作成は %s\n", legacyOk ? "成功（このドライバは作成時に弾かない）" : "失敗（期待どおり: フラグが要る）");
                // 縮退経路でも既存の PSO（Forward）は作れる
                auto fps = dxc.Compile(fwd / "Forward.hlsl", L"PSMain", L"ps_6_0", nullptr, log);
                std::string err3;
                CHECK_MSG(TryBuildPso(dev, legacy.Get(), vs, fps, DXGI_FORMAT_R16G16B16A16_FLOAT, false, err3),
                          "縮退（フラグ無し）RS で Forward の PSO が作れない: %s", err3.c_str());
            }
        }
    }

    std::printf("stage: SRV\n");
    // ---- 3. SRV 添字の永続性 + 実 GPU での ResourceDescriptorHeap[添字] ----
    {
        Exec ex;
        CHECK(ex.Init(d3d));
        DescriptorHeap srvHeap;
        srvHeap.Initialize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true);
        ResourceManager rm;
        rm.Initialize(&dev, &srvHeap, ex.list.Get());

        const fs::path dir = fs::temp_directory_path() / "dx12e_main_rs_bindless_test";
        std::error_code ec;
        fs::create_directories(dir, ec);
        struct Tex { fs::path path; uint8_t r, g, b; };
        std::vector<Tex> texs = { { dir / "a.bmp", 255, 0, 0 }, { dir / "b.bmp", 0, 255, 0 }, { dir / "c.bmp", 0, 0, 255 } };
        for (auto& t : texs) WriteBmp(t.path, t.r, t.g, t.b);

        std::vector<u32> idx;
        std::vector<Texture*> ptrs;
        for (auto& t : texs)
        {
            const u32 i = rm.GetOrLoadTextureSrvIndex(t.path.wstring(), ex.list.Get(), /*srgb=*/false);
            CHECK_MSG(i != ResourceManager::kInvalidSrvIndex, "%s が読めない", t.path.string().c_str());
            idx.push_back(i);
            ptrs.push_back(rm.GetOrLoadTexture(t.path.wstring(), ex.list.Get(), false));
            CHECK(ptrs.back() && ptrs.back()->GetSrvIndex() == i);
            CHECK(rm.FindTextureSrvIndex(t.path.wstring(), false) == i);
        }
        // 同じパスでも srgb が違えば別のキャッシュ = 別の添字（取り違え事故を実際に踏んで分けてある）
        const u32 iSrgb = rm.GetOrLoadTextureSrvIndex(texs[0].path.wstring(), ex.list.Get(), /*srgb=*/true);
        CHECK_MSG(iSrgb != ResourceManager::kInvalidSrvIndex && iSrgb != idx[0], "srgb 違いが同じ添字を共有している");
        CHECK(rm.FindTextureSrvIndex((dir / "none.bmp").wstring(), false) == ResourceManager::kInvalidSrvIndex);
        CHECK(rm.GetOrLoadTextureSrvIndex((dir / "missing.bmp").wstring(), ex.list.Get(), false) == ResourceManager::kInvalidSrvIndex);
        ex.Submit();
        rm.FinishUploads();

        auto audit = rm.AuditTextureSrvIndices();
        CHECK_MSG(audit.Ok(), "監査: invalid=%u outOfRange=%u duplicates=%u（%u 枚）", audit.invalid, audit.outOfRange, audit.duplicates, audit.textures);
        CHECK(audit.textures >= 7);   // 3 枚 + srgb 版 + 既定 4 枚 - 失敗キャッシュ(nullptr)は数えない
        CHECK(audit.capacity == 65536);

        // 実 GPU: ResourceDescriptorHeap[添字] → そのテクスチャの色
        std::string log;
        std::vector<uint8_t> csBytes;
        {
            const fs::path csPath = dir / "heap_read.hlsl";
            std::ofstream(csPath) << kHeapReadCs;
            csBytes = dxc.Compile(csPath, L"CSMain", L"cs_6_6", nullptr, log);
        }
        CHECK_MSG(!csBytes.empty(), "heap read CS のコンパイルに失敗: %s", log.c_str());
        HeapReader reader;
        bool allLoaded = !ptrs.empty();
        for (Texture* t : ptrs) allLoaded = allLoaded && t != nullptr;
        CHECK_MSG(allLoaded, "テクスチャの読み込みに失敗（GPU での添字検証を飛ばす）");
        if (allLoaded && !csBytes.empty() && reader.Init(d3d, csBytes))
        {
            auto px = reader.Read(ex, srvHeap, ptrs, idx);
            CHECK(px.size() == idx.size() * 4);
            for (size_t i = 0; i < texs.size() && px.size() >= (i + 1) * 4; ++i)
            {
                const bool okc = Near(px[i * 4 + 0], texs[i].r / 255.0f) && Near(px[i * 4 + 1], texs[i].g / 255.0f) &&
                                 Near(px[i * 4 + 2], texs[i].b / 255.0f);
                CHECK_MSG(okc, "添字 %u（%s）を GPU で引いた色 (%.3f %.3f %.3f) が期待 (%d %d %d) と違う", idx[i], texs[i].path.filename().string().c_str(),
                          px[i * 4 + 0], px[i * 4 + 1], px[i * 4 + 2], texs[i].r, texs[i].g, texs[i].b);
            }

            // ホットリロード: 中身を書き換えて force。添字は不変・同じ Texture オブジェクトで、GPU で引くと新しい色。
            const auto before = rm.SnapshotTextureSrvIndices();
            for (auto& t : texs) { t.r = uint8_t(255 - t.r); t.g = uint8_t(255 - t.g); t.b = uint8_t(255 - t.b); WriteBmp(t.path, t.r, t.g, t.b); }
            ex.Reset();
            const AssetReloadResult rr = rm.ReloadChangedAssets(ex.list.Get(), "", /*force=*/true);
            ex.Submit();
            rm.FinishUploads();
            CHECK_MSG(rr.textures.size() >= 3, "ホットリロードで読み直せたのが %zu 枚", rr.textures.size());
            const auto after = rm.SnapshotTextureSrvIndices();
            CHECK_MSG(before == after, "ホットリロードの前後で SRV 添字が動いた（%zu → %zu 件）", before.size(), after.size());
            for (size_t i = 0; i < texs.size(); ++i)
                CHECK_MSG(rm.FindTextureSrvIndex(texs[i].path.wstring(), false) == idx[i], "リロード後に添字が変わった: %s", texs[i].path.string().c_str());
            CHECK(rm.AuditTextureSrvIndices().Ok());
            auto px2 = reader.Read(ex, srvHeap, ptrs, idx);
            CHECK(px2.size() == idx.size() * 4);
            for (size_t i = 0; i < texs.size() && px2.size() >= (i + 1) * 4; ++i)
            {
                const bool okc = Near(px2[i * 4 + 0], texs[i].r / 255.0f) && Near(px2[i * 4 + 1], texs[i].g / 255.0f) &&
                                 Near(px2[i * 4 + 2], texs[i].b / 255.0f);
                CHECK_MSG(okc, "リロード後、添字 %u を GPU で引いた色 (%.3f %.3f %.3f) が期待 (%d %d %d) と違う", idx[i],
                          px2[i * 4 + 0], px2[i * 4 + 1], px2[i * 4 + 2], texs[i].r, texs[i].g, texs[i].b);
            }
        }
        else if (allLoaded) CHECK_MSG(false, "heap read の PSO / バッファが作れない");
        fs::remove_all(dir, ec);
    }

    // ---- 4. UnoNormalFromTangentSpace（グラフ経路）== PerturbNormal（旧経路）: 同じ入力で数値一致 ----
    //   法線マップのサンプル(0..1 の RG)から旧経路が作る接空間法線と、グラフが渡す -1..1 の単位ベクトルに同じ
    //   z >= 0.35 ガードが掛かること（xy が 1 を超える入力も含める）。G2b の標準テンプレートのパリティの前提。
    {
        constexpr UINT kN = 512;
        const fs::path dir = fs::temp_directory_path() / "dx12e_main_rs_bindless_test";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path csPath = dir / "normal_equiv.hlsl";
        std::ofstream(csPath) <<
            "#include \"forward/PBR.hlsli\"\n"
            "#include \"forward/UnoSurface.hlsli\"\n"
            "StructuredBuffer<float4> gIn : register(t0);\n"
            "RWStructuredBuffer<float4> gOut : register(u0);\n"
            "[numthreads(64, 1, 1)]\n"
            "void CSMain(uint3 id : SV_DispatchThreadID)\n"
            "{\n"
            "    const float3 n  = gIn[id.x * 3 + 0].xyz;\n"
            "    const float3 t  = gIn[id.x * 3 + 1].xyz;\n"
            "    const float  tw = gIn[id.x * 3 + 1].w;\n"
            "    const float2 xy01 = gIn[id.x * 3 + 2].xy;\n"
            "    const float3 a = PerturbNormal(n, t, tw, float3(xy01, 0.0));\n"
            "    const float2 xy = xy01 * 2.0 - 1.0;\n"
            "    const float  z  = sqrt(saturate(1.0 - dot(xy, xy)));\n"
            "    const float3 b = UnoNormalFromTangentSpace(n, t, tw, float3(xy, z));\n"
            "    gOut[id.x] = float4(a - b, 0.0);\n"
            "}\n";
        std::string log;
        auto cs = dxc.Compile(csPath, L"CSMain", L"cs_6_0", nullptr, log);
        CHECK_MSG(!cs.empty(), "normal_equiv CS のコンパイルに失敗: %s", log.substr(0, 800).c_str());
        if (!cs.empty())
        {
            // 入力（乱数。xy01 の 1/3 は角に寄せて |xy| > 1 = z ガードが効く領域を踏む）
            std::vector<float> in(kN * 12);
            uint32_t s = 12345;
            auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float((s >> 8) & 0xFFFF) / 65535.0f; };
            for (UINT i = 0; i < kN; ++i)
            {
                float n[3] = { rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1 };
                float t[3] = { rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1 };
                float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-3f;
                for (float& v : n) v /= nl;
                for (int k = 0; k < 3; ++k) { in[i * 12 + k] = n[k]; in[i * 12 + 4 + k] = t[k]; }
                in[i * 12 + 7] = (rnd() < 0.5f) ? -1.0f : 1.0f;
                const bool corner = (i % 3) == 0;
                in[i * 12 + 8]  = corner ? (rnd() < 0.5f ? 0.02f : 0.98f) : rnd();
                in[i * 12 + 9]  = corner ? (rnd() < 0.5f ? 0.02f : 0.98f) : rnd();
            }
            const UINT64 inBytes = UINT64(in.size()) * sizeof(float), outBytes = UINT64(kN) * 16;
            auto makeBuf = [&](D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
                ComPtr<ID3D12Resource> r;
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
                D3D12_RESOURCE_DESC rd{};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc = { 1, 0 }; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
                d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
                return r;
            };
            auto up  = makeBuf(D3D12_HEAP_TYPE_UPLOAD, inBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto out = makeBuf(D3D12_HEAP_TYPE_DEFAULT, outBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto rb  = makeBuf(D3D12_HEAP_TYPE_READBACK, outBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            CHECK(up && out && rb);
            if (up && out && rb)
            {
                void* m = nullptr;
                up->Map(0, nullptr, &m);
                std::memcpy(m, in.data(), size_t(inBytes));
                up->Unmap(0, nullptr);

                D3D12_ROOT_PARAMETER p[2]{};
                p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[0].Descriptor.ShaderRegister = 0;
                p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[1].Descriptor.ShaderRegister = 0;
                D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = p;
                ComPtr<ID3DBlob> blob, err;
                ComPtr<ID3D12RootSignature> crs;
                ComPtr<ID3D12PipelineState> cpso;
                const bool built = SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
                                   SUCCEEDED(d3d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&crs)));
                D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
                cd.pRootSignature = crs.Get();
                cd.CS = { cs.data(), cs.size() };
                CHECK_MSG(built && SUCCEEDED(d3d->CreateComputePipelineState(&cd, IID_PPV_ARGS(&cpso))), "normal_equiv の PSO が作れない");
                if (cpso)
                {
                    Exec ex;
                    CHECK(ex.Init(d3d));
                    ex.list->SetComputeRootSignature(crs.Get());
                    ex.list->SetPipelineState(cpso.Get());
                    ex.list->SetComputeRootShaderResourceView(0, up->GetGPUVirtualAddress());
                    ex.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
                    ex.list->Dispatch(kN / 64, 1, 1);
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = out.Get();
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    ex.list->ResourceBarrier(1, &b);
                    ex.list->CopyBufferRegion(rb.Get(), 0, out.Get(), 0, outBytes);
                    ex.Submit();
                    D3D12_RANGE rr{ 0, size_t(outBytes) };
                    const float* f = nullptr;
                    if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(const_cast<float**>(&f)))))
                    {
                        float maxd = 0.0f; UINT bad = 0;
                        for (UINT i = 0; i < kN; ++i)
                        {
                            const float d = std::fmax(std::fabs(f[i * 4]), std::fmax(std::fabs(f[i * 4 + 1]), std::fabs(f[i * 4 + 2])));
                            maxd = std::fmax(maxd, d);
                            if (!(d < 1e-4f)) ++bad;   // NaN も不一致に数える
                        }
                        CHECK_MSG(bad == 0, "UnoNormalFromTangentSpace と PerturbNormal が %u / %u 点で不一致（最大差 %g）", bad, kN, maxd);
                        std::printf("法線: UnoNormalFromTangentSpace vs PerturbNormal 最大差 %g（%u 点）\n", maxd, kN);
                        D3D12_RANGE wr{ 0, 0 };
                        rb->Unmap(0, &wr);
                    }
                }
            }
        }
        fs::remove_all(dir, ec);
    }

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
