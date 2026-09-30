// マテリアルグラフ G1: 生成 HLSL の DXC コンパイル検証 + WARP（ソフトウェア D3D12）での CPU との数値一致テスト。
//
//   ctest ラベル "dxc"。dxcompiler.dll / dxil.dll が無い環境ではスキップ（終了コード 0 + "SKIP" 表示）。
//   1. ゴールデン 10 本の生成 HLSL を ps_6_6 と cs_6_6 でコンパイルする（参照契約 UnoMatContractRef.hlsli を include）。
//   2. 壊した Custom ノードの DXC エラー行が、`#line "node:<id>"` 経由で nodeId + 本文の行番号へ逆引きできる。
//   3. WARP で CS を実行し、CPU 参照実装（EvaluateCpu）と 1e-4 以内で一致することを確かめる:
//        ・ゴールデン（テクスチャ / Custom を除く）
//        ・数学 / ベクトル系ノードを 1 個ずつ（自動配線）
//        ・ランダムグラフ（シード固定）
//      WARP が SM 6.6 の計算シェーダーを走らせられない環境では GPU 比較だけスキップする（DXC コンパイルは実施済み）。

#include "renderer/matgraph/Compiler.h"
#include "renderer/matgraph/CpuEval.h"
#include "renderer/matgraph/GraphIO.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace dx12e::matgraph;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks   = 0;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

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

// ---------------------------------------------------------------------------
// DXC（dxcompiler.dll を実行時にロード。無ければスキップ）
// ---------------------------------------------------------------------------
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
        if (FAILED(utils->CreateDefaultIncludeHandler(&inc))) return false;
        return true;
    }

    struct Out
    {
        bool success = false;
        std::vector<uint8_t> dxil;
        std::string log;
    };

    Out Compile(const std::string& src, const wchar_t* entry, const wchar_t* profile, const std::vector<std::wstring>& extra) const
    {
        Out o;
        std::vector<std::wstring> a = {L"-E", entry, L"-T", profile, L"-HV", L"2021", L"-I", fs::path(DX12E_MATGRAPH_HLSL_DIR).wstring()};
        for (const std::wstring& e : extra) a.push_back(e);
        std::vector<LPCWSTR> args;
        for (const std::wstring& s : a) args.push_back(s.c_str());
        DxcBuffer buf;
        buf.Ptr = src.data(); buf.Size = src.size(); buf.Encoding = DXC_CP_UTF8;
        ComPtr<IDxcResult> res;
        if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()), inc.Get(), IID_PPV_ARGS(&res)))) return o;
        HRESULT st = E_FAIL;
        res->GetStatus(&st);
        ComPtr<IDxcBlobUtf8> errs;
        if (SUCCEEDED(res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), nullptr)) && errs && errs->GetStringLength() > 0)
            o.log.assign(errs->GetStringPointer(), errs->GetStringLength());
        if (SUCCEEDED(st))
        {
            ComPtr<IDxcBlob> obj;
            if (SUCCEEDED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) && obj && obj->GetBufferSize() > 0)
            {
                o.success = true;
                const auto* p = static_cast<const uint8_t*>(obj->GetBufferPointer());
                o.dxil.assign(p, p + obj->GetBufferSize());
            }
        }
        return o;
    }
};

const char* kPsWrapper =
    "\n"
    "float4 PSMain(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    UnoMatInput mi = (UnoMatInput)0;\n"
    "    mi.uv = uv; mi.worldPos = float3(pos.xy, 0.0); mi.vertexNormalWS = float3(0, 0, 1);\n"
    "    mi.cameraPos = float3(0, 0, 10); mi.vertexColor = float4(1, 1, 1, 1); mi.time = 1.0;\n"
    "    UnoMatPool pool; pool.recordBase = 0;\n"
    "    UnoSurface s;\n"
    "    UnoMatEval(mi, pool, s);\n"
    "    float k = s.ao * s.metallic * s.roughness * s.opacityMask;\n"
    "    return float4(s.baseColor + s.emissive + s.normalTS * (s.hasNormal ? 1.0 : 0.0), s.opacity) * k;\n"
    "}\n";

const char* kCsWrapper =
    "\n"
    "StructuredBuffer<float4>   g_in  : register(t1);\n"
    "RWStructuredBuffer<float4> g_out : register(u0);\n"
    "[numthreads(64, 1, 1)]\n"
    "void CSMain(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    const uint i = id.x;\n"
    "    UnoMatInput mi = (UnoMatInput)0;\n"
    "    const float4 a = g_in[i * 5 + 0];\n"
    "    mi.uv = a.xy; mi.time = a.z;\n"
    "    mi.worldPos = g_in[i * 5 + 1].xyz;\n"
    "    mi.vertexNormalWS = g_in[i * 5 + 2].xyz;\n"
    "    mi.cameraPos = g_in[i * 5 + 3].xyz;\n"
    "    mi.vertexColor = g_in[i * 5 + 4];\n"
    "    UnoMatPool pool; pool.recordBase = 0;\n"
    "    UnoSurface s;\n"
    "    UnoMatEval(mi, pool, s);\n"
    "    g_out[i * 4 + 0] = float4(s.baseColor, s.metallic);\n"
    "    g_out[i * 4 + 1] = float4(s.roughness, s.ao, s.opacity, s.opacityMask);\n"
    "    g_out[i * 4 + 2] = float4(s.emissive, s.hasNormal ? 1.0 : 0.0);\n"
    "    g_out[i * 4 + 3] = float4(s.normalTS, 0.0);\n"
    "}\n";

CompileOptions TestOpt()
{
    CompileOptions o;
    o.includeLine = "#include \"UnoMatContractRef.hlsli\"";
    return o;
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------
// WARP（ソフトウェア D3D12）で CS を実行する
// ---------------------------------------------------------------------------
class Warp
{
public:
    bool Init(std::string& why)
    {
        ComPtr<IDXGIFactory4> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { why = "CreateDXGIFactory1 失敗"; return false; }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) { why = "WARP アダプターが無い"; return false; }
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev)))) { why = "WARP デバイス作成失敗"; return false; }
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
        if (FAILED(m_dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        {
            why = "WARP が Shader Model 6.6 に対応していない";
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (FAILED(m_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))) { why = "キュー作成失敗"; return false; }
        if (FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_alloc)))) { why = "アロケータ作成失敗"; return false; }
        if (FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_alloc.Get(), nullptr, IID_PPV_ARGS(&m_list)))) { why = "コマンドリスト作成失敗"; return false; }
        m_list->Close();
        if (FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) { why = "フェンス作成失敗"; return false; }
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[0].Descriptor = {0, 0}; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[1].Descriptor = {1, 0}; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[2].Descriptor = {0, 0}; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = 3; rd.pParameters = params;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { why = "ルートシグネチャのシリアライズ失敗"; return false; }
        if (FAILED(m_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rs)))) { why = "ルートシグネチャ作成失敗"; return false; }
        return true;
    }

    // dxil を実行。pool = float4 の並び、in = 点ごとに 5 float4、count 点、out = 点ごとに 4 float4
    bool Run(const std::vector<uint8_t>& dxil, const std::vector<float>& pool, const std::vector<float>& in, uint32_t count,
             std::vector<float>& out, std::string& why)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rs.Get();
        pd.CS = {dxil.data(), dxil.size()};
        ComPtr<ID3D12PipelineState> pso;
        HRESULT hr = m_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) { char b[64]; std::snprintf(b, sizeof(b), "CreateComputePipelineState 失敗 0x%08X", static_cast<unsigned>(hr)); why = b; return false; }

        const uint32_t padded = (count + 63) / 64 * 64;
        std::vector<float> inPadded = in;
        inPadded.resize(static_cast<size_t>(padded) * 5 * 4, 0.0f);
        std::vector<float> poolData = pool;
        if (poolData.empty()) poolData.assign(4, 0.0f);

        ComPtr<ID3D12Resource> poolBuf = MakeUpload(poolData);
        ComPtr<ID3D12Resource> inBuf   = MakeUpload(inPadded);
        const UINT64 outBytes = static_cast<UINT64>(padded) * 4 * 16;
        ComPtr<ID3D12Resource> outBuf  = MakeBuffer(D3D12_HEAP_TYPE_DEFAULT, outBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> rbBuf   = MakeBuffer(D3D12_HEAP_TYPE_READBACK, outBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!poolBuf || !inBuf || !outBuf || !rbBuf) { why = "バッファ作成失敗"; return false; }

        m_alloc->Reset();
        m_list->Reset(m_alloc.Get(), pso.Get());
        m_list->SetComputeRootSignature(m_rs.Get());
        m_list->SetComputeRootShaderResourceView(0, poolBuf->GetGPUVirtualAddress());
        m_list->SetComputeRootShaderResourceView(1, inBuf->GetGPUVirtualAddress());
        m_list->SetComputeRootUnorderedAccessView(2, outBuf->GetGPUVirtualAddress());
        m_list->Dispatch(padded / 64, 1, 1);
        D3D12_RESOURCE_BARRIER bar = {};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = outBuf.Get();
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_list->ResourceBarrier(1, &bar);
        m_list->CopyResource(rbBuf.Get(), outBuf.Get());
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        m_queue->Signal(m_fence.Get(), ++m_fenceValue);
        m_fence->SetEventOnCompletion(m_fenceValue, m_event);
        if (WaitForSingleObject(m_event, 60000) != WAIT_OBJECT_0) { why = "GPU の完了待ちがタイムアウト"; return false; }
        if (FAILED(m_dev->GetDeviceRemovedReason())) { why = "デバイスが失われた"; return false; }

        void* p = nullptr;
        D3D12_RANGE rr = {0, static_cast<SIZE_T>(outBytes)};
        if (FAILED(rbBuf->Map(0, &rr, &p))) { why = "Map 失敗"; return false; }
        out.assign(static_cast<const float*>(p), static_cast<const float*>(p) + static_cast<size_t>(count) * 4 * 4);
        D3D12_RANGE wr = {0, 0};
        rbBuf->Unmap(0, &wr);
        return true;
    }

private:
    ComPtr<ID3D12Resource> MakeBuffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = type;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
        return r;
    }
    ComPtr<ID3D12Resource> MakeUpload(const std::vector<float>& data)
    {
        ComPtr<ID3D12Resource> r = MakeBuffer(D3D12_HEAP_TYPE_UPLOAD, data.size() * sizeof(float), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!r) return nullptr;
        void* p = nullptr;
        D3D12_RANGE none = {0, 0};
        if (FAILED(r->Map(0, &none, &p))) return nullptr;
        std::memcpy(p, data.data(), data.size() * sizeof(float));
        r->Unmap(0, nullptr);
        return r;
    }

    ComPtr<ID3D12Device>              m_dev;
    ComPtr<ID3D12CommandQueue>        m_queue;
    ComPtr<ID3D12CommandAllocator>    m_alloc;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence>               m_fence;
    ComPtr<ID3D12RootSignature>       m_rs;
    HANDLE                            m_event = nullptr;
    UINT64                            m_fenceValue = 0;
};

// ---------------------------------------------------------------------------
// CPU / GPU の突き合わせ
// ---------------------------------------------------------------------------
struct Compared
{
    int points = 0;
    int mismatches = 0;
    int skippedNonFinite = 0;
    float worst = 0.0f;
    std::string firstMismatch;
};

// 点ごとの入力を作る（範囲は数値が暴れない領域に限る）
struct Point
{
    EvalEnv env;
    float gpuIn[20];
};

std::vector<Point> MakePoints(uint32_t seed, int n)
{
    std::mt19937 rng(seed);
    auto R = [&](float a, float b) { return a + (b - a) * (static_cast<float>(rng() & 0xFFFFFF) / 16777216.0f); };
    std::vector<Point> v(static_cast<size_t>(n));
    for (Point& p : v)
    {
        EvalEnv& e = p.env;
        e.uv[0] = R(0, 1); e.uv[1] = R(0, 1);
        for (float& x : e.worldPos) x = R(-5, 5);
        float nx = R(-1, 1), ny = R(-1, 1), nz = R(-1, 1);
        const float l = std::sqrt(nx * nx + ny * ny + nz * nz) + 1e-3f;
        e.vertexNormalWS[0] = nx / l; e.vertexNormalWS[1] = ny / l; e.vertexNormalWS[2] = nz / l;
        for (float& x : e.cameraPos) x = R(-8, 8);
        e.cameraPos[2] += 12.0f;   // ピクセルとカメラが同一点にならないように
        for (float& x : e.vertexColor) x = R(0.25f, 1.0f);
        e.time = R(0.5f, 10.0f);
        float* g = p.gpuIn;
        g[0] = e.uv[0]; g[1] = e.uv[1]; g[2] = e.time; g[3] = 0;
        g[4] = e.worldPos[0]; g[5] = e.worldPos[1]; g[6] = e.worldPos[2]; g[7] = 0;
        g[8] = e.vertexNormalWS[0]; g[9] = e.vertexNormalWS[1]; g[10] = e.vertexNormalWS[2]; g[11] = 0;
        g[12] = e.cameraPos[0]; g[13] = e.cameraPos[1]; g[14] = e.cameraPos[2]; g[15] = 0;
        for (int k = 0; k < 4; ++k) g[16 + k] = e.vertexColor[k];
    }
    return v;
}

void Flatten(const SurfaceValues& s, float out[16])
{
    out[0] = s.baseColor[0]; out[1] = s.baseColor[1]; out[2] = s.baseColor[2]; out[3] = s.metallic;
    out[4] = s.roughness; out[5] = s.ao; out[6] = s.opacity; out[7] = s.opacityMask;
    out[8] = s.emissive[0]; out[9] = s.emissive[1]; out[10] = s.emissive[2]; out[11] = s.hasNormal ? 1.0f : 0.0f;
    out[12] = s.normalTS[0]; out[13] = s.normalTS[1]; out[14] = s.normalTS[2]; out[15] = 0.0f;
}

// 1 グラフを CPU と GPU で評価して比較する。GPU を走らせられなければ false（why に理由）
bool CompareGraph(Dxc& dxc, Warp& warp, const CompileResult& r, const std::vector<Point>& pts, Compared& cmp, std::string& why)
{
    const std::string src = "#define UNO_CONTRACT_COMPUTE 1\n" + r.hlsl + kCsWrapper;
    const Dxc::Out co = dxc.Compile(src, L"CSMain", L"cs_6_6", {});
    if (!co.success) { why = "CS のコンパイル失敗:\n" + co.log; return false; }
    std::vector<float> in;
    for (const Point& p : pts) in.insert(in.end(), p.gpuIn, p.gpuIn + 20);
    const std::vector<float> pool = PackParamRecord(r, {}, nullptr);
    std::vector<float> out;
    if (!warp.Run(co.dxil, pool, in, static_cast<uint32_t>(pts.size()), out, why)) return false;

    static const char* kNames[16] = {"baseColor.x", "baseColor.y", "baseColor.z", "metallic", "roughness", "ao", "opacity", "opacityMask",
                                     "emissive.x", "emissive.y", "emissive.z", "hasNormal", "normal.x", "normal.y", "normal.z", "_"};
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const CpuEvalResult cpu = EvaluateCpu(r, pts[i].env);
        float c[16];
        Flatten(cpu.surface, c);
        ++cmp.points;
        for (int k = 0; k < 16; ++k)
        {
            const float g = out[i * 16 + static_cast<size_t>(k)];
            if (!std::isfinite(c[k]) || !std::isfinite(g)) { ++cmp.skippedNonFinite; continue; }
            const float err = std::fabs(c[k] - g) / (1.0f + std::fabs(c[k]) + std::fabs(g));
            cmp.worst = std::max(cmp.worst, err);
            if (err > 1e-4f)
            {
                ++cmp.mismatches;
                if (cmp.firstMismatch.empty())
                {
                    char b[256];
                    std::snprintf(b, sizeof(b), "点 %zu の %s: CPU=%.7g GPU=%.7g", i, kNames[k], static_cast<double>(c[k]), static_cast<double>(g));
                    cmp.firstMismatch = b;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// グラフの自動生成（ノード 1 個ずつ / ランダム）
// ---------------------------------------------------------------------------
struct Src { std::string node, pin; };

// 共通の入力ノード群
struct Sources
{
    std::vector<Src> all;
};

Sources AddSources(MaterialGraph& g)
{
    Sources s;
    g.AddNode("WorldPosition", 0, 0, "wp");        s.all.push_back({"wp", "Out"});
    g.AddNode("VertexColor", 0, 0, "vc");          s.all.push_back({"vc", "RGB"}); s.all.push_back({"vc", "Out"}); s.all.push_back({"vc", "R"});
    g.AddNode("Time", 0, 0, "tm");                 s.all.push_back({"tm", "Out"});
    g.AddNode("TexCoord", 0, 0, "uvn");
    g.SetNodeProp("uvn", "tiling", Json::array({2.0, 3.0}));
    s.all.push_back({"uvn", "UV"});
    g.AddNode("Float", 0, 0, "k1");
    g.SetNodeProp("k1", "value", Json(0.75));
    s.all.push_back({"k1", "Out"});
    g.AddNode("Float3", 0, 0, "k3");
    g.SetNodeProp("k3", "value", Json::array({0.3, -0.6, 1.4}));
    s.all.push_back({"k3", "Out"});
    g.AddNode("VertexNormalWS", 0, 0, "vn");       s.all.push_back({"vn", "Out"});
    return s;
}

// 出力ノードが無いグラフの検証: E_NO_OUTPUT 以外のエラー（到達可能性に関係なく）があるか
bool HasRealErrors(const MaterialGraph& g)
{
    for (const Diagnostic& d : g.Analysis().diags)
        if (d.severity == Severity::Error && d.code != code::kNoOutput) return true;
    return false;
}

// 出力ノードへ、繋がる最初のピンに v を接続する
void WireOutput(MaterialGraph& g, const Src& v, const std::vector<const char*>& pins)
{
    for (const char* p : pins)
        if (g.Connect(v.node, v.pin, "out", p)) return;
}

// def の全入力に、繋がる入力ノードを順に割り当てる。1 個でも繋げなければ false
bool AutoWire(MaterialGraph& g, const std::string& node, const NodeDef& def, const Sources& src, std::mt19937& rng, bool randomize)
{
    for (const PinDecl& pin : def.inputs)
    {
        std::vector<size_t> order(src.all.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        if (randomize) std::shuffle(order.begin(), order.end(), rng);
        else std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>((&pin - def.inputs.data()) * 4 % static_cast<std::ptrdiff_t>(order.size())), order.end());
        bool done = false;
        for (size_t k : order)
        {
            const Src& s = src.all[k];
            if (g.CanConnect(s.node, s.pin, node, pin.name).verdict != ConnectCheck::Verdict::Reject && g.Connect(s.node, s.pin, node, pin.name))
            {
                done = true;
                break;
            }
        }
        if (!done && pin.required) return false;
    }
    return true;
}

} // namespace

int main()
{
    Dxc dxc;
    if (!dxc.Init())
    {
        std::printf("SKIP: dxcompiler.dll を読み込めない（DXC が無い環境）\n");
        return 0;
    }
    const fs::path dataDir = DX12E_MATGRAPH_DATA_DIR;

    // -------- 1. ゴールデンを ps_6_6 / cs_6_6 でコンパイル --------
    std::printf("[dxc-golden]\n");
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dataDir))
        if (e.path().extension() == ".dxmg") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK_MSG(files.size() >= 10, "ゴールデンが足りない (%zu)", files.size());
    struct Golden { std::string name; MaterialGraph g; CompileResult r; };
    std::vector<Golden> goldens;
    for (const fs::path& f : files)
    {
        Golden gd;
        gd.name = f.stem().string();
        std::string err;
        CHECK_MSG(LoadDxmg(ReadFile(f), gd.g, &err), "%s: %s", gd.name.c_str(), err.c_str());
        gd.r = CompileGraph(gd.g, TestOpt());
        CHECK_MSG(gd.r.ok, "%s: コンパイル失敗", gd.name.c_str());
        if (!gd.r.ok) continue;
        const Dxc::Out ps = dxc.Compile(gd.r.hlsl + kPsWrapper, L"PSMain", L"ps_6_6", {});
        CHECK_MSG(ps.success, "%s: ps_6_6 のコンパイル失敗\n%s", gd.name.c_str(), ps.log.c_str());
        const Dxc::Out cs = dxc.Compile("#define UNO_CONTRACT_COMPUTE 1\n" + gd.r.hlsl + kCsWrapper, L"CSMain", L"cs_6_6", {});
        CHECK_MSG(cs.success, "%s: cs_6_6 のコンパイル失敗\n%s", gd.name.c_str(), cs.log.c_str());
        // 警告ゼロ（生成コードは警告を出さない）
        if (ps.success && !ps.log.empty()) std::printf("  %s: DXC 警告あり:\n%s\n", gd.name.c_str(), ps.log.c_str());
        goldens.push_back(std::move(gd));
    }
    std::printf("  compiled %zu golden graphs (ps_6_6 + cs_6_6)\n", goldens.size());

    // -------- 2. DXC エラー → ノードへ逆引き --------
    std::printf("[dxc-error-map]\n");
    {
        MaterialGraph g;
        g.SetIdSeed(7);
        g.AddNode("WorldPosition", 0, 0, "wp");
        g.AddNode("Custom", 0, 0, "bad");
        g.SetNodeProp("bad", "code", Json("float a = In0.x;\nfloat b = a + undefined_symbol;\nreturn b;"));
        g.Connect("wp", "Out", "bad", "In0");
        g.AddNode("MaterialOutput", 0, 0, "out");
        g.Connect("bad", "Out", "out", "Roughness");
        const CompileResult r = CompileGraph(g, TestOpt());
        CHECK(r.ok);   // グラフとしては正しい。DXC で初めて失敗する
        const Dxc::Out ps = dxc.Compile(r.hlsl + kPsWrapper, L"PSMain", L"ps_6_6", {});
        CHECK(!ps.success);
        const std::vector<Diagnostic> ds = MapDxcLog(ps.log);
        bool mapped = false;
        for (const Diagnostic& d : ds)
            if (d.severity == Severity::Error && d.nodeId == "bad" && d.line == 2 && d.message.find("undefined_symbol") != std::string::npos) mapped = true;
        CHECK_MSG(mapped, "DXC のエラーが Custom ノード bad の本文 2 行目へ逆引きできない。ログ:\n%s", ps.log.c_str());
    }

    // -------- 3. WARP で数値比較 --------
    std::printf("[warp-numeric]\n");
    Warp warp;
    std::string why;
    if (!warp.Init(why))
    {
        std::printf("  SKIP GPU 数値比較: %s（DXC コンパイルの検証は完了）\n", why.c_str());
        std::printf("matgraph_dxc: %d checks, %d failures (GPU skipped)\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    const std::vector<Point> pts = MakePoints(1234, 96);
    int graphsRun = 0, totalPoints = 0, totalMismatch = 0, totalSkipped = 0;
    float worst = 0.0f;
    auto runOne = [&](const std::string& label, const CompileResult& r) {
        Compared c;
        std::string w;
        if (!CompareGraph(dxc, warp, r, pts, c, w))
        {
            ++g_checks; ++g_failures;
            std::printf("FAIL %s: GPU 実行できない: %s\n", label.c_str(), w.c_str());
            return;
        }
        ++graphsRun;
        totalPoints += c.points; totalMismatch += c.mismatches; totalSkipped += c.skippedNonFinite; worst = std::max(worst, c.worst);
        CHECK_MSG(c.mismatches == 0, "%s: %d 件不一致（最大誤差 %g）。%s", label.c_str(), c.mismatches, static_cast<double>(c.worst), c.firstMismatch.c_str());
    };

    // 3a. ゴールデン（テクスチャ / Custom は CPU 評価できないので除く）
    for (const Golden& gd : goldens)
    {
        if (!gd.r.ok) continue;
        if (gd.name.find("texture") != std::string::npos || gd.name.find("custom") != std::string::npos) continue;
        runOne("golden " + gd.name, gd.r);
    }

    // 3b. 数学 / ベクトル / 色系ノードを 1 個ずつ（自動配線）
    {
        const NodeLibrary& lib = NodeLibrary::Builtin();
        const std::set<std::string> skip = {"MaterialOutput", "Custom", "Reroute", "TextureSample", "NormalMap", "TextureParameter",
                                            "Select", "Compare", "Int", "Bool", "Split"};
        int n = 0;
        for (const NodeDef* d : lib.List())
        {
            if (skip.count(d->type)) continue;
            bool onlyNumeric = true;
            for (const PinDecl& p : d->inputs) if (p.mode == PinMode::Fixed && (p.type == ValueType::Bool || p.type == ValueType::Int || p.type == ValueType::Tex2D)) onlyNumeric = false;
            if (!onlyNumeric) continue;
            MaterialGraph g;
            g.SetIdSeed(1);
            const Sources srcs = AddSources(g);
            g.AddNode(d->type, 0, 0, "n");
            std::mt19937 rng(99);
            if (!AutoWire(g, "n", *d, srcs, rng, false)) { std::printf("  (配線できずスキップ: %s)\n", d->type.c_str()); continue; }
            g.AddNode("MaterialOutput", 0, 0, "out");
            const Src o{"n", d->outputs[0].name};
            WireOutput(g, o, {"Emissive", "Metallic"});
            const CompileResult r = CompileGraph(g, TestOpt());
            if (!r.ok)
            {
                ++g_checks; ++g_failures;
                std::printf("FAIL node %s: 自動配線したグラフがコンパイルできない\n", d->type.c_str());
                for (const Diagnostic& dg : r.diagnostics) if (dg.severity == Severity::Error) std::printf("    %s [%s.%s] %s\n", dg.code.c_str(), dg.nodeId.c_str(), dg.pin.c_str(), dg.message.c_str());
                continue;
            }
            runOne(std::string("node ") + d->type, r);
            ++n;
        }
        std::printf("  per-node graphs: %d\n", n);
    }

    // 3c. ランダムグラフ（シード固定）
    {
        const std::vector<std::string> pool = {"Add", "Subtract", "Multiply", "Divide", "Lerp", "Clamp", "Saturate", "OneMinus", "Abs", "Power",
                                               "Min", "Max", "Sin", "Cos", "Sqrt", "Dot", "Cross", "Normalize", "Length", "Append",
                                               "ComponentMask", "Smoothstep", "Desaturation", "Fresnel"};
        const NodeLibrary& lib = NodeLibrary::Builtin();
        int built = 0, randomOps = 0;
        for (uint32_t seed = 1; seed <= 40; ++seed)
        {
            std::mt19937 rng(seed * 7919u);
            MaterialGraph g;
            g.SetIdSeed(seed);
            Sources srcs = AddSources(g);
            const size_t baseSources = srcs.all.size();
            int count = 0;
            for (int attempt = 0; attempt < 40 && count < 24; ++attempt)
            {
                const std::string type = pool[rng() % pool.size()];
                const NodeDef* d = lib.Find(type);
                const std::string id = "r" + std::to_string(attempt);
                MaterialGraph trial = g.Clone();
                trial.AddNode(type, 0, 0, id);
                if (type == "Divide") trial.SetNodeProp(id, "safe", Json(true));
                if (type == "ComponentMask")
                {
                    trial.SetNodeProp(id, "r", Json((rng() & 1) != 0)); trial.SetNodeProp(id, "g", Json((rng() & 1) != 0));
                    trial.SetNodeProp(id, "b", Json(false)); trial.SetNodeProp(id, "a", Json(false));
                    if (!trial.GetNodeProp(id, "r").get<bool>() && !trial.GetNodeProp(id, "g").get<bool>()) trial.SetNodeProp(id, "r", Json(true));
                }
                if (!AutoWire(trial, id, *d, srcs, rng, true)) continue;
                // 値の範囲を抑える（Power の指数 / Smoothstep の端は定数にしてある。ここは Clamp で保険）
                const std::string cid = "c" + std::to_string(attempt);
                trial.AddNode("Clamp", 0, 0, cid);
                if (!trial.Connect(id, d->outputs[0].name, cid, "X")) continue;
                trial.SetLiteral(cid, "Min", Value::Float(-16.0f));
                trial.SetLiteral(cid, "Max", Value::Float(16.0f));
                if (HasRealErrors(trial)) continue;
                if (type == "Power") trial.SetLiteral(id, "Exp", Value::Float(1.0f + static_cast<float>(rng() % 20) / 10.0f));
                if (type == "Smoothstep") { trial.SetLiteral(id, "Min", Value::Float(-1.0f)); trial.SetLiteral(id, "Max", Value::Float(2.0f)); }
                if (HasRealErrors(trial)) continue;
                g = std::move(trial);
                srcs.all.push_back({cid, "Out"});
                ++count;
            }
            g.AddNode("MaterialOutput", 0, 0, "out");
            const std::vector<const char*> pins = {"BaseColor", "Emissive", "Metallic", "Roughness", "Opacity", "AmbientOcclusion", "Normal"};
            size_t pinIdx = 0;
            for (const char* p : pins)
            {
                // 後から作ったノード（= 深い枝）を優先して出力へ繋ぐ。無ければ入力ノードから
                const size_t made = srcs.all.size() - baseSources;
                const Src& s = pinIdx < made ? srcs.all[srcs.all.size() - 1 - pinIdx] : srcs.all[rng() % srcs.all.size()];
                ++pinIdx;
                g.Connect(s.node, s.pin, "out", p);
            }
            const CompileResult r = CompileGraph(g, TestOpt());
            if (!r.ok)
            {
                ++g_checks; ++g_failures;
                std::printf("FAIL random seed %u: コンパイルできない\n", seed);
                for (const Diagnostic& dg : r.diagnostics) if (dg.severity == Severity::Error) std::printf("    %s [%s.%s] %s\n", dg.code.c_str(), dg.nodeId.c_str(), dg.pin.c_str(), dg.message.c_str());
                continue;
            }
            ++built;
            randomOps += static_cast<int>(r.ir.ops.size());
            runOne("random seed " + std::to_string(seed), r);
        }
        std::printf("  random graphs: %d (avg IR ops %.1f)\n", built, built ? static_cast<double>(randomOps) / built : 0.0);
        CHECK(built >= 30);
    }

    // 検出力の確認: CPU 側の入力（time）をわざとずらしたら、ちゃんと不一致として検出される
    {
        for (const Golden& gd : goldens)
        {
            if (gd.name.find("time_anim") == std::string::npos) continue;
            std::vector<Point> bad = pts;
            for (Point& p : bad) p.env.time += 1.0f;
            Compared c;
            std::string w;
            const bool ran = CompareGraph(dxc, warp, gd.r, bad, c, w);
            CHECK(ran && c.mismatches > 0);
            std::printf("  detector check: perturbed CPU input -> %d mismatches (expected > 0)\n", c.mismatches);
        }
    }

    const float skipRatio = totalPoints ? static_cast<float>(totalSkipped) / static_cast<float>(totalPoints * 16) : 0.0f;
    std::printf("  graphs run: %d, points: %d, mismatches: %d, non-finite skipped: %d (%.2f%%), worst rel err: %g\n",
                graphsRun, totalPoints, totalMismatch, totalSkipped, static_cast<double>(skipRatio) * 100.0, static_cast<double>(worst));
    CHECK(graphsRun >= 40);
    CHECK(skipRatio < 0.02f);

    std::printf("matgraph_dxc: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
