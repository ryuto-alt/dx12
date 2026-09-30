// パストレーサー(地上真値レンダラ)の検証。GPU 版(shaders/pt/PathTrace.hlsl)を
//   (1) 解析解 …… 白い炉 / GGX の方向アルベド / 点光源 + ランバート平面 / 太陽 + ランバート平面 / 閉じた発光箱
//   (2) 独立した CPU リファレンス(tests/pt/PtRef.h。倍精度・BVH・別のサンプリング戦略)との一致
//   (3) 収束(spp を増やすと誤差が 1/√N で減る)/ 決定論(同じ設定 → 同じ結果)/ NaN が出ない
// で確かめる。窓なしの D3D12 デバイス(DXR 1.1 + SM 6.6 + Tier 3)で動く。GPU が無い / 要件を満たさない環境では
// 「SKIP」を出して 0 で終わる(落とさない)。CPU リファレンスの計算は論理コア/4 スレッドまで。
//
//   環境変数: DX12E_TEST_D3D_DEBUG=1 でデバッグレイヤ / DX12E_PT_TEST_QUICK=1 で重い比較(Cornell / 収束)を縮小
//             DX12E_PT_TEST_DUMP=<dir> で GPU / CPU の画像を PFM + PNG で書き出す(目視・FLIP 用)
#include "pt/PtRef.h"

#include "renderer/PathTracer.h"
#include "renderer/pt/PtImageIO.h"
#include "renderer/pt/PtSceneBuilder.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::pt;
namespace R = dx12e::ptref;
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

#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #cond);      \
            std::printf(__VA_ARGS__);                                        \
            std::printf("\n");                                               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

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
std::string EnvGet(const char* name)
{
    char* buf = nullptr;
    size_t len = 0;
    std::string v;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) v = buf;
    std::free(buf);
    return v;
}

// ── 最小の D3D12 ハーネス ────────────────────────────────────────────────────
struct Gpu
{
    ComPtr<ID3D12Device5> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> heap;
    HANDLE ev = nullptr;
    UINT64 fv = 0;
    UINT heapCursor = 0, descSize = 0;
    std::wstring adapterName;
    ComPtr<ID3D12InfoQueue> info;

    // 失敗理由を返す(空 = 成功)
    std::string Init()
    {
        if (EnvSet("DX12E_TEST_D3D_DEBUG"))
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory6> fac;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return "DXGI ファクトリを作れない";
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> ad;
            if (FAILED(fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)))) break;
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            ComPtr<ID3D12Device5> dv;
            if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dv)))) continue;
            dev = dv;
            adapterName = d.Description;
            break;
        }
        if (!dev) return "ハードウェアの D3D12 アダプタが無い";
        if (EnvSet("DX12E_TEST_D3D_DEBUG")) dev.As(&info);
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return "キューを作れない";
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return "アロケータを作れない";
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return "リストを作れない";
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return "フェンスを作れない";
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 4096;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return "ディスクリプタヒープを作れない";
        descSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return {};
    }
    void Wait()
    {
        ++fv;
        q->Signal(fence.Get(), fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
    }
    ID3D12GraphicsCommandList4* Begin()
    {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        return list.Get();
    }
    void Submit()
    {
        list->Close();
        ID3D12CommandList* l[] = {list.Get()};
        q->ExecuteCommandLists(1, l);
        Wait();
    }
    UINT AllocDesc() { return heapCursor++; }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT i) const { auto h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(i) * descSize; return h; }
};

// ── メッシュのアップロード(96B 頂点 = Mesh の Vertex と同じ)────────────────────
struct Vtx96
{
    float pos[3], nor[3], col[4], uv[2], tan[4];
    uint32_t bi[4]; float bw[4];
};
static_assert(sizeof(Vtx96) == 96, "Mesh の Vertex(96B)と一致させること");

struct GpuMesh
{
    ComPtr<ID3D12Resource> vb, ib;
    uint32_t vbSrv = kNoIndex, ibSrv = kNoIndex;
    uint32_t vertexCount = 0, indexCount = 0;
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t> indices;
};

ComPtr<ID3D12Resource> UploadBuffer(Gpu& g, ID3D12GraphicsCommandList* cmd, const void* data, size_t bytes,
                                    std::vector<ComPtr<ID3D12Resource>>& keep)
{
    auto make = [&](D3D12_HEAP_TYPE t, D3D12_RESOURCE_STATES st) {
        ComPtr<ID3D12Resource> r;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = std::max<size_t>(bytes, 16);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1; rd.SampleDesc = {1, 0};
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
        return r;
    };
    auto dst = make(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto up = make(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{0, 0};
    up->Map(0, &none, &p);
    std::memset(p, 0, std::max<size_t>(bytes, 16));
    std::memcpy(p, data, bytes);
    up->Unmap(0, nullptr);
    cmd->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, std::max<size_t>(bytes, 16));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dst.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
    cmd->ResourceBarrier(1, &b);
    keep.push_back(up);
    return dst;
}

void MakeRawSrv(Gpu& g, ID3D12Resource* res, size_t bytes, uint32_t& outIndex)
{
    outIndex = g.AllocDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = static_cast<UINT>(std::max<size_t>(bytes, 16) / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g.dev->CreateShaderResourceView(res, &srv, g.Cpu(outIndex));
}

// ── ptref::Scene → GPU シーン ───────────────────────────────────────────────
struct GpuScene
{
    SceneBuilder builder;
    std::vector<GpuMesh> meshes;
    std::vector<ComPtr<ID3D12Resource>> keep;
    std::string error;
};

bool BuildGpuScene(Gpu& g, const R::Scene& sc, GpuScene& out)
{
    out.builder.Init(g.dev.Get());
    auto* cmd = g.Begin();

    // 材質ごとに 1 メッシュ。頂点色 = (albedo, alpha)、テクスチャ無し、tint = 1。
    out.meshes.resize(sc.mats.size());
    for (size_t m = 0; m < sc.mats.size(); ++m)
    {
        std::vector<Vtx96> vs;
        GpuMesh& gm = out.meshes[m];
        for (const R::Tri& t : sc.tris)
        {
            if (t.material != static_cast<int>(m)) continue;
            for (int k = 0; k < 3; ++k)
            {
                Vtx96 v{};
                v.pos[0] = static_cast<float>(t.p[k].x); v.pos[1] = static_cast<float>(t.p[k].y); v.pos[2] = static_cast<float>(t.p[k].z);
                v.nor[0] = static_cast<float>(t.n[k].x); v.nor[1] = static_cast<float>(t.n[k].y); v.nor[2] = static_cast<float>(t.n[k].z);
                const R::Material& mt = sc.mats[m];
                v.col[0] = static_cast<float>(mt.albedo.x); v.col[1] = static_cast<float>(mt.albedo.y); v.col[2] = static_cast<float>(mt.albedo.z);
                v.col[3] = static_cast<float>(t.alpha[k]);
                v.tan[0] = 1; v.tan[3] = 1;
                vs.push_back(v);
                gm.positions.push_back({v.pos[0], v.pos[1], v.pos[2]});
                gm.indices.push_back(static_cast<uint32_t>(gm.indices.size()));
            }
        }
        if (vs.empty()) continue;
        gm.vertexCount = static_cast<uint32_t>(vs.size());
        gm.indexCount = static_cast<uint32_t>(gm.indices.size());
        gm.vb = UploadBuffer(g, cmd, vs.data(), vs.size() * sizeof(Vtx96), out.keep);
        gm.ib = UploadBuffer(g, cmd, gm.indices.data(), gm.indices.size() * 4, out.keep);
        MakeRawSrv(g, gm.vb.Get(), vs.size() * sizeof(Vtx96), gm.vbSrv);
        MakeRawSrv(g, gm.ib.Get(), gm.indices.size() * 4, gm.ibSrv);
    }
    // 材質
    for (const R::Material& m : sc.mats)
    {
        MaterialGpu mg{};
        mg.tint[0] = mg.tint[1] = mg.tint[2] = 1.0f;
        mg.opacity = static_cast<float>(m.opacity);
        mg.metallic = static_cast<float>(m.metallic);
        mg.roughness = static_cast<float>(m.roughness);
        mg.alphaCutoff = static_cast<float>(m.cutoff);
        mg.flags = (m.lambert ? kMatLambert : 0u) | (m.emitBoth ? kMatEmitBoth : 0u);
        mg.emissive[0] = static_cast<float>(m.emission.x); mg.emissive[1] = static_cast<float>(m.emission.y); mg.emissive[2] = static_cast<float>(m.emission.z);
        mg.albedoSrv = mg.normalSrv = mg.mrSrv = mg.emissiveSrv = kNoIndex;
        mg.alphaMode = static_cast<uint32_t>(m.alphaMode);
        mg.uvScaleOffset[0] = mg.uvScaleOffset[1] = 1.0f;
        out.builder.AddMaterial(mg);
    }
    // ジオメトリ + インスタンス
    for (size_t m = 0; m < sc.mats.size(); ++m)
    {
        GpuMesh& gm = out.meshes[m];
        if (gm.vertexCount == 0) continue;
        GeometryDesc gd;
        gd.key = 1000 + m;
        gd.vbVA = gm.vb->GetGPUVirtualAddress();
        gd.vbStride = 96;
        gd.vertexCount = gm.vertexCount;
        gd.ibVA = gm.ib->GetGPUVirtualAddress();
        gd.indexCount = gm.indexCount;
        gd.vbSrv = gm.vbSrv; gd.ibSrv = gm.ibSrv;
        gd.cpuPositions = gm.positions.data();
        gd.cpuIndices = gm.indices.data();
        const uint32_t geo = out.builder.AddGeometry(gd);
        InstanceDesc id;
        id.geometry = geo;
        DirectX::XMStoreFloat4x4(&id.world, DirectX::XMMatrixIdentity());
        id.material = static_cast<uint32_t>(m);
        id.emissiveLuma = static_cast<float>(R::Luma(sc.mats[m].emission));
        out.builder.AddInstance(id);
    }
    for (const R::PointLight& l : sc.lights)
    {
        LightGpu lg{};
        lg.position[0] = static_cast<float>(l.pos.x); lg.position[1] = static_cast<float>(l.pos.y); lg.position[2] = static_cast<float>(l.pos.z);
        lg.range = static_cast<float>(l.range);
        lg.color[0] = static_cast<float>(l.color.x); lg.color[1] = static_cast<float>(l.color.y); lg.color[2] = static_cast<float>(l.color.z);
        lg.type = l.spot ? 1.0f : 0.0f;
        lg.direction[0] = static_cast<float>(l.dir.x); lg.direction[1] = static_cast<float>(l.dir.y); lg.direction[2] = static_cast<float>(l.dir.z);
        lg.cosInner = static_cast<float>(l.cosInner); lg.cosOuter = static_cast<float>(l.cosOuter);
        out.builder.AddLight(lg);
    }
    std::string err;
    if (!out.builder.BeginBuild(&err)) { out.error = err; g.Submit(); return false; }
    while (!out.builder.BuildStep(cmd, 1u << 30)) {}
    if (!out.builder.FinishBuild(cmd, &err)) { out.error = err; g.Submit(); return false; }
    g.Submit();
    out.builder.ReleaseTransient();
    return true;
}

struct GpuResult
{
    std::vector<float> rgb;
    uint32_t samples = 0;
    PtStatsOut stats;
    PtProgress progress;
    double wallMs = 0;
};

bool RunGpu(Gpu& g, PathTracer& pt, GpuScene& gs, const R::Scene& sc, uint32_t w, uint32_t h, uint32_t spp, uint32_t bounces,
            uint32_t seed, GpuResult& out, uint32_t flags = kFlagRR | kFlagEnvNee, uint32_t spd = 0, uint32_t tile = 256)
{
    JobDesc jd;
    jd.width = w; jd.height = h; jd.spp = spp; jd.bounces = bounces; jd.seed = seed;
    jd.flags = flags | (sc.physFalloff ? kFlagPhysFalloff : 0u) | (sc.forceLambert ? kFlagForceLambert : 0u);
    jd.tileSize = tile; jd.samplesPerDispatch = (spd > 0) ? spd : std::min<uint32_t>(spp, 16);   // 0 = 自動(呼び出し回数を減らす)
    for (int i = 0; i < 3; ++i)
    {
        jd.cam.pos[i] = static_cast<float>(sc.cam.pos[i]);
        jd.cam.right[i] = static_cast<float>(sc.cam.right[i]);
        jd.cam.up[i] = static_cast<float>(sc.cam.up[i]);
        jd.cam.fwd[i] = static_cast<float>(sc.cam.fwd[i]);
        jd.sun.toLight[i] = static_cast<float>(sc.sun.toLight[i]);
        jd.sun.E[i] = static_cast<float>(sc.sun.E[i]);
        jd.env.uniform[i] = static_cast<float>(sc.env.uniform[i]);
    }
    jd.cam.tanHalfFovY = static_cast<float>(sc.cam.tanHalfFovY);
    jd.cam.aspect = static_cast<float>(sc.cam.aspect);
    jd.sun.enabled = sc.sun.enabled;
    jd.sun.tanRadius = static_cast<float>(sc.sun.tanRadius);
    jd.env.lightScale = static_cast<float>(sc.env.lightScale);
    jd.env.bgScale = static_cast<float>(sc.env.bgScale);
    jd.env.nee = R::Luma(sc.env.uniform) * sc.env.lightScale > 0;
    std::string err;
    if (!pt.BeginJob(jd, gs.builder.GetScene(), &err)) { std::printf("BeginJob: %s\n", err.c_str()); return false; }
    const auto t0 = std::chrono::steady_clock::now();
    while (pt.Running())
    {
        auto* cmd = g.Begin();
        pt.Record(cmd, g.heap.Get(), 0.0f, 0);
        g.Submit();
    }
    out.progress = pt.GetProgress();
    if (!pt.ReadbackAverage(out.rgb, &out.samples, &err)) { std::printf("Readback: %s\n", err.c_str()); return false; }
    pt.ReadbackStats(out.stats);
    out.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// ── 統計 ──────────────────────────────────────────────────────────────────
double Mean(const std::vector<float>& v, size_t stride = 1, size_t off = 0)
{
    double s = 0; size_t n = 0;
    for (size_t i = off; i < v.size(); i += stride) { s += v[i]; ++n; }
    return n ? s / n : 0;
}
double Rmse(const std::vector<float>& a, const std::vector<float>& b)
{
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i) { const double d = double(a[i]) - double(b[i]); s += d * d; }
    return std::sqrt(s / std::max<size_t>(a.size(), 1));
}
// 8x8 ブロック平均に落としてから RMSE(ノイズの影響を抑えた「面の明るさ」の一致)
std::vector<float> BlockMean(const std::vector<float>& rgb, uint32_t w, uint32_t h, uint32_t b)
{
    const uint32_t bw = w / b, bh = h / b;
    std::vector<float> o(static_cast<size_t>(bw) * bh * 3, 0.0f);
    for (uint32_t y = 0; y < bh * b; ++y)
        for (uint32_t x = 0; x < bw * b; ++x)
            for (int c = 0; c < 3; ++c)
                o[((y / b) * bw + x / b) * 3 + c] += rgb[(static_cast<size_t>(y) * w + x) * 3 + c] / float(b * b);
    return o;
}
double PixelAt(const std::vector<float>& rgb, uint32_t w, uint32_t x, uint32_t y, int c = 1) { return rgb[(static_cast<size_t>(y) * w + x) * 3 + c]; }

std::string g_dumpDir;
void Dump(const char* name, const std::vector<float>& rgb, uint32_t w, uint32_t h)
{
    if (g_dumpDir.empty()) return;
    std::filesystem::create_directories(g_dumpDir);
    io::WritePfm(std::filesystem::path(g_dumpDir) / (std::string(name) + ".pfm"), w, h, rgb.data());
    io::WritePreviewPng(std::filesystem::path(g_dumpDir) / (std::string(name) + ".png"), w, h, rgb.data());
}

int CpuThreads()
{
    const unsigned hc = std::thread::hardware_concurrency();
    return static_cast<int>(std::max(1u, hc / 4));
}

// ── シーン ────────────────────────────────────────────────────────────────
R::Camera MakeCam(const R::V3& pos, const R::V3& target, double fovDeg, double aspect)
{
    R::Camera c;
    c.pos = pos;
    c.fwd = R::Norm(target - pos);
    c.right = R::Norm(R::Cross(R::V3(0, 1, 0), c.fwd));
    c.up = R::Cross(c.fwd, c.right);
    c.tanHalfFovY = std::tan(fovDeg * R::kPi / 360.0);
    c.aspect = aspect;
    return c;
}

// 6 面の箱(内向き法線)。中心 0、半サイズ hs。面ごとの材質を渡す(+X -X +Y -Y +Z -Z)。
void AddBox(R::Scene& s, double hs, const int mats[6])
{
    using R::V3;
    // +X 面(法線 -X)
    R::AddQuad(s, V3(hs, -hs, -hs), V3(hs, hs, -hs), V3(hs, hs, hs), V3(hs, -hs, hs), V3(-1, 0, 0), mats[0]);
    // -X 面(法線 +X)
    R::AddQuad(s, V3(-hs, -hs, hs), V3(-hs, hs, hs), V3(-hs, hs, -hs), V3(-hs, -hs, -hs), V3(1, 0, 0), mats[1]);
    // +Y 面(天井、法線 -Y)
    R::AddQuad(s, V3(-hs, hs, -hs), V3(-hs, hs, hs), V3(hs, hs, hs), V3(hs, hs, -hs), V3(0, -1, 0), mats[2]);
    // -Y 面(床、法線 +Y)
    R::AddQuad(s, V3(-hs, -hs, hs), V3(-hs, -hs, -hs), V3(hs, -hs, -hs), V3(hs, -hs, hs), V3(0, 1, 0), mats[3]);
    // +Z 面(奥、法線 -Z)
    R::AddQuad(s, V3(hs, -hs, hs), V3(hs, hs, hs), V3(-hs, hs, hs), V3(-hs, -hs, hs), V3(0, 0, -1), mats[4]);
    // -Z 面(手前、法線 +Z)
    R::AddQuad(s, V3(-hs, -hs, -hs), V3(-hs, hs, -hs), V3(hs, hs, -hs), V3(hs, -hs, -hs), V3(0, 0, 1), mats[5]);
}

// ---------------------------------------------------------------------------
//  (1) 解析解
// ---------------------------------------------------------------------------
void TestWhiteFurnaceLambert(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T1] 白い炉(ランバート albedo=1 の球を一様な空 L=1 で照らす → 放射輝度 = 1)\n");
    R::Scene sc;
    R::Material m; m.albedo = {1, 1, 1}; m.lambert = true;
    sc.mats.push_back(m);
    R::AddSphere(sc, {0, 0, 0}, 1.0, 0, 192, 96);
    sc.env.uniform = {1, 1, 1}; sc.env.lightScale = 1.0; sc.env.bgScale = 1.0;
    sc.cam = MakeCam({0, 0, -4}, {0, 0, 0}, 40, 1.0);
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = 64, H = 64;
    for (uint32_t bounces : {1u, 8u})
    {
        GpuResult r;
        CHECK(RunGpu(g, pt, gs, sc, W, H, 64, bounces, 1, r));
        // 球の内側(中心から半径 0.7 以内)と背景(四隅)を分けて見る
        double sSum = 0, sMax = 0, bMin = 1e9, bMax = -1e9; size_t sN = 0;
        const double rr = (1.0 / 4.0) / sc.cam.tanHalfFovY * 0.5;   // 球の投影半径(NDC の半分)の 0.7 倍程度
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x)
        {
            const double u = (x + 0.5) / W * 2 - 1, v = (y + 0.5) / H * 2 - 1;
            const double d = std::sqrt(u * u + v * v);
            for (int c = 0; c < 3; ++c)
            {
                const double val = PixelAt(r.rgb, W, x, y, c);
                if (d < rr * 0.7) { sSum += val; sMax = std::max(sMax, std::fabs(val - 1.0)); ++sN; }
                if (d > 0.9) { bMin = std::min(bMin, val); bMax = std::max(bMax, val); }
            }
        }
        const double sMean = sSum / std::max<size_t>(sN, 1);
        std::printf("  bounces=%u  球の平均 %.5f  最大誤差 %.5f  背景 [%.6f, %.6f]  NaN=%u  経路=%llu\n",
                    bounces, sMean, sMax, bMin, bMax, r.stats.nanSamples, static_cast<unsigned long long>(r.stats.paths));
        CHECK_MSG(std::fabs(sMean - 1.0) < 0.005, "球の平均 %.5f", sMean);
        CHECK_MSG(sMax < 0.02, "最大誤差 %.5f", sMax);
        CHECK(std::fabs(bMin - 1.0) < 1e-4 && std::fabs(bMax - 1.0) < 1e-4);
        CHECK(r.stats.nanSamples == 0);
        if (bounces == 8) Dump("t1_furnace_lambert", r.rgb, W, H);
    }
}

void TestGgxFurnace(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T2] GGX の白い炉(metallic=1 / albedo=1 → 方向アルベド E(μ)。エネルギー損失 = 1-E = 既知のマルチスキャッタ欠損)\n");
    std::printf("  roughness   GPU(中心画素)  数値積分 E(1)   損失(1-E)   GPU/数値\n");
    for (double rough : {0.15, 0.3, 0.5, 0.7, 1.0})
    {
        R::Scene sc;
        R::Material m; m.albedo = {1, 1, 1}; m.metallic = 1.0; m.roughness = rough;
        sc.mats.push_back(m);
        R::AddSphere(sc, {0, 0, 0}, 1.0, 0, 192, 96);
        sc.env.uniform = {1, 1, 1}; sc.env.lightScale = 1.0; sc.env.bgScale = 1.0;
        sc.cam = MakeCam({0, 0, -6}, {0, 0, 0}, 20, 1.0);
        GpuScene gs;
        CHECK(BuildGpuScene(g, sc, gs));
        GpuResult r;
        const uint32_t W = 33, H = 33;   // 奇数 = 中心画素が球の正面(NdotV=1)
        CHECK(RunGpu(g, pt, gs, sc, W, H, 2048, 1, 3, r));
        double gpuC = 0;   // 中心 3x3 の平均(NdotV ≈ 1。1 画素だとノイズで 1% 動く)
        for (uint32_t yy = 15; yy <= 17; ++yy) for (uint32_t xx = 15; xx <= 17; ++xx) gpuC += PixelAt(r.rgb, W, xx, yy, 1) / 9.0;
        const double ref = R::DirectionalAlbedo(m, 1.0, 512).y;
        std::printf("  %5.2f       %.5f        %.5f        %.4f      %.4f\n", rough, gpuC, ref, 1.0 - ref, gpuC / ref);
        CHECK_MSG(std::fabs(gpuC / ref - 1.0) < 0.02, "rough=%.2f gpu=%.5f ref=%.5f", rough, gpuC, ref);
        CHECK(ref <= 1.0 + 1e-3);
    }
}

void TestPointLightPlane(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T3] 点光源 + ランバート平面(直接光の閉形式解: L = albedo/π · color · att(d) · cosθ)\n");
    for (int phys = 0; phys < 2; ++phys)
    {
        R::Scene sc;
        R::Material m; m.albedo = {0.6, 0.6, 0.6}; m.lambert = true;
        sc.mats.push_back(m);
        R::AddQuad(sc, {-20, 0, -20}, {-20, 0, 20}, {20, 0, 20}, {20, 0, -20}, {0, 1, 0}, 0);
        R::PointLight L; L.pos = {0, 2, 0}; L.range = 12; L.color = {8, 6, 4};
        sc.lights.push_back(L);
        sc.physFalloff = (phys == 1);
        sc.cam = MakeCam({0, 6, -0.001}, {0, 0, 0}, 50, 1.0);
        GpuScene gs;
        CHECK(BuildGpuScene(g, sc, gs));
        GpuResult r;
        const uint32_t W = 65, H = 65;
        CHECK(RunGpu(g, pt, gs, sc, W, H, 32, 1, 5, r));
        double worst = 0;
        for (uint32_t x = 0; x < W; x += 8)
        {
            // 画素 (x, 中心行) の交点(y=0 平面)。カメラは真下を向く。
            const double u = ((x + 0.5) / W * 2 - 1) * sc.cam.tanHalfFovY * sc.cam.aspect;
            const R::V3 d = R::Norm(sc.cam.fwd + sc.cam.right * u + sc.cam.up * (((32 + 0.5) / H * 2 - 1) * -sc.cam.tanHalfFovY));
            const double t = -sc.cam.pos.y / d.y;
            const R::V3 p = sc.cam.pos + d * t;
            const R::V3 toL = L.pos - p;
            const double dist = R::Len(toL), cosT = toL.y / dist;
            double att = phys ? 1.0 / (dist * dist) : ((dist >= L.range) ? 0.0 : std::pow(1 - dist / L.range, 2.0));
            for (int c = 0; c < 3; ++c)
            {
                const double expect = 0.6 / R::kPi * ((c == 0) ? 8 : (c == 1 ? 6 : 4)) * att * cosT;
                const double got = PixelAt(r.rgb, W, x, 32, c);
                if (expect > 1e-3) worst = std::max(worst, std::fabs(got - expect) / expect);
            }
        }
        std::printf("  falloff=%s  最大相対誤差 %.5f\n", phys ? "逆二乗" : "エンジン式", worst);
        CHECK_MSG(worst < 0.01, "worst=%.5f", worst);
        if (phys == 0) Dump("t3_point_plane", r.rgb, W, H);
    }
}

void TestSunPlane(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T4] 太陽 + ランバート平面(L = albedo/π · E · cosθ)+ 一様な空(L = albedo · sky)\n");
    R::Scene sc;
    R::Material m; m.albedo = {0.5, 0.5, 0.5}; m.lambert = true;
    sc.mats.push_back(m);
    R::AddQuad(sc, {-50, 0, -50}, {-50, 0, 50}, {50, 0, 50}, {50, 0, -50}, {0, 1, 0}, 0);
    sc.sun.enabled = true;
    sc.sun.toLight = R::Norm(R::V3(0.3, 1.0, 0.2));
    sc.sun.E = {3.0, 2.8, 2.5};
    sc.env.uniform = {0.2, 0.25, 0.3}; sc.env.lightScale = 1.0; sc.env.bgScale = 0.0;
    sc.cam = MakeCam({0, 8, -8}, {0, 0, 0}, 40, 1.0);
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    GpuResult r;
    const uint32_t W = 48, H = 48;
    CHECK(RunGpu(g, pt, gs, sc, W, H, 256, 2, 7, r));
    const double cosT = sc.sun.toLight.y;
    const double eR = 0.5 / R::kPi * 3.0 * cosT + 0.5 * 0.2;
    const double eG = 0.5 / R::kPi * 2.8 * cosT + 0.5 * 0.25;
    const double eB = 0.5 / R::kPi * 2.5 * cosT + 0.5 * 0.3;
    // 下半分(すべて平面)の平均
    double sR = 0, sG = 0, sB = 0; size_t n = 0;
    for (uint32_t y = H / 2 + 4; y < H; ++y) for (uint32_t x = 0; x < W; ++x) { sR += PixelAt(r.rgb, W, x, y, 0); sG += PixelAt(r.rgb, W, x, y, 1); sB += PixelAt(r.rgb, W, x, y, 2); ++n; }
    sR /= n; sG /= n; sB /= n;
    std::printf("  平均 (%.5f, %.5f, %.5f)  期待 (%.5f, %.5f, %.5f)\n", sR, sG, sB, eR, eG, eB);
    CHECK_MSG(std::fabs(sR / eR - 1) < 0.01 && std::fabs(sG / eG - 1) < 0.01 && std::fabs(sB / eB - 1) < 0.01, "平均が期待とずれた");
}

void TestClosedBox(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T5] 閉じた発光箱(全面 albedo ρ・発光 Le → 放射輝度 L(B) = Le(1-ρ^(B+1))/(1-ρ)。B→∞ で Le/(1-ρ))\n");
    const double rho = 0.5, Le = 1.0;
    R::Scene sc;
    R::Material m; m.albedo = {rho, rho, rho}; m.lambert = true; m.emission = {Le, Le, Le};
    sc.mats.push_back(m);
    const int mats[6] = {0, 0, 0, 0, 0, 0};
    AddBox(sc, 1.0, mats);
    sc.cam = MakeCam({0, 0, 0}, {0, 0, 1}, 70, 1.0);
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    CHECK(gs.builder.GetStats().emissiveTris == 12);
    std::printf("  bounces  GPU平均    期待       誤差\n");
    for (uint32_t B : {1u, 2u, 4u, 8u, 24u})
    {
        GpuResult r;
        const uint32_t W = 32, H = 32;
        CHECK(RunGpu(g, pt, gs, sc, W, H, 256, B, 11, r, (B >= 24) ? (kFlagRR | kFlagEnvNee) : kFlagEnvNee));
        const double expect = Le * (1.0 - std::pow(rho, B + 1.0)) / (1.0 - rho);
        const double got = Mean(r.rgb);
        std::printf("  %2u       %.5f   %.5f   %+.4f%%\n", B, got, expect, (got / expect - 1) * 100);
        CHECK_MSG(std::fabs(got / expect - 1.0) < 0.01, "B=%u got=%.5f expect=%.5f", B, got, expect);
        CHECK(r.stats.nanSamples == 0);
    }
}

// ---------------------------------------------------------------------------
//  (2) CPU リファレンスとの一致
// ---------------------------------------------------------------------------
R::Scene MakeCornell()
{
    using R::V3;
    R::Scene sc;
    R::Material white; white.albedo = {0.73, 0.73, 0.73}; white.roughness = 0.9; white.metallic = 0.0;
    R::Material red;   red.albedo   = {0.65, 0.05, 0.05}; red.roughness = 0.9;
    R::Material green; green.albedo = {0.12, 0.45, 0.15}; green.roughness = 0.9;
    R::Material light; light.albedo = {0, 0, 0}; light.emission = {12, 10, 7}; light.lambert = true;
    R::Material glossy; glossy.albedo = {0.9, 0.7, 0.3}; glossy.metallic = 1.0; glossy.roughness = 0.3;
    sc.mats = {white, red, green, light, glossy};
    const int mats[6] = {2, 1, 0, 0, 0, 0};   // +X 緑 / -X 赤 / 天井 / 床 / 奥 / 手前(手前は白。カメラは箱の外)
    // 手前の面は描かない(カメラを箱の開口の外に置く)。
    AddBox(sc, 1.0, mats);
    sc.tris.resize(sc.tris.size() - 2);   // AddBox の最後 = -Z 面(手前)を外す
    // 天井の光(下向き)
    R::AddQuad(sc, V3(-0.3, 0.995, -0.3), V3(-0.3, 0.995, 0.3), V3(0.3, 0.995, 0.3), V3(0.3, 0.995, -0.3), V3(0, -1, 0), 3);
    // 箱と球
    R::AddSphere(sc, V3(-0.4, -0.55, 0.2), 0.45, 4, 64, 32);
    // 背の低い白い箱(6 面)
    {
        const double a = 0.32, y0 = -1.0, y1 = -0.35;
        const V3 c(0.42, 0, -0.1);
        auto q = [&](V3 p0, V3 p1, V3 p2, V3 p3, V3 n) { R::AddQuad(sc, p0, p1, p2, p3, n, 0); };
        q(c + V3(-a, y1, -a), c + V3(-a, y1, a), c + V3(a, y1, a), c + V3(a, y1, -a), V3(0, 1, 0));
        q(c + V3(a, y0, -a), c + V3(a, y1, -a), c + V3(a, y1, a), c + V3(a, y0, a), V3(1, 0, 0));
        q(c + V3(-a, y0, a), c + V3(-a, y1, a), c + V3(-a, y1, -a), c + V3(-a, y0, -a), V3(-1, 0, 0));
        q(c + V3(-a, y0, a), c + V3(a, y0, a), c + V3(a, y1, a), c + V3(-a, y1, a), V3(0, 0, 1));
        q(c + V3(a, y0, -a), c + V3(-a, y0, -a), c + V3(-a, y1, -a), c + V3(a, y1, -a), V3(0, 0, -1));
    }
    sc.cam = MakeCam({0, 0, -3.4}, {0, 0, 0}, 42, 1.0);
    return sc;
}

void TestCornellVsCpu(Gpu& g, PathTracer& pt, bool quick)
{
    std::printf("\n[T6] コーナーボックス(面光源 + 間接光 + 光沢球): GPU と独立 CPU リファレンスの一致\n");
    const R::Scene sc = MakeCornell();
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = quick ? 32 : 48, H = W;
    const uint32_t spp = quick ? 256 : 1024;
    const uint32_t bounces = 6;

    GpuResult gpu;
    CHECK(RunGpu(g, pt, gs, sc, W, H, spp, bounces, 21, gpu));

    R::RenderParams rp; rp.w = W; rp.h = H; rp.spp = static_cast<int>(spp); rp.bounces = bounces; rp.seed = 99; rp.threads = CpuThreads();
    const auto c0 = std::chrono::steady_clock::now();
    R::Tracer tr(sc);
    const std::vector<float> cpu = tr.Render(rp);
    const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();

    const double mg = Mean(gpu.rgb), mc = Mean(cpu);
    const auto bg = BlockMean(gpu.rgb, W, H, 8), bc = BlockMean(cpu, W, H, 8);
    const double rmse = Rmse(gpu.rgb, cpu), rmseBlock = Rmse(bg, bc);
    // 色チャンネルごとの全体平均
    double gm[3], cm[3];
    for (int c = 0; c < 3; ++c) { gm[c] = Mean(gpu.rgb, 3, c); cm[c] = Mean(cpu, 3, c); }
    std::printf("  %ux%u %u spp / %u bounces  CPU %.0f ms(%d スレッド)  GPU %.0f ms\n", W, H, spp, bounces, cpuMs, rp.threads, gpu.wallMs);
    std::printf("  全体平均 GPU %.5f  CPU %.5f  (比 %.4f)   RGB 平均比 (%.4f, %.4f, %.4f)\n", mg, mc, mg / mc, gm[0] / cm[0], gm[1] / cm[1], gm[2] / cm[2]);
    std::printf("  RMSE(画素) %.5f  RMSE(8x8 ブロック平均) %.5f  相対(ブロック/平均) %.4f\n", rmse, rmseBlock, rmseBlock / mc);
    // 許容 = 統計ノイズの範囲(quick は spp が 1/4 なので広げる)。fullでは全体平均 ±1% / ブロック RMSE 3%。
    const double tolMean = quick ? 0.03 : 0.01, tolBlock = quick ? 0.08 : 0.03;
    CHECK_MSG(std::fabs(mg / mc - 1.0) < tolMean, "全体平均の比 %.4f", mg / mc);
    for (int c = 0; c < 3; ++c) CHECK_MSG(std::fabs(gm[c] / cm[c] - 1.0) < tolMean * 1.5, "ch%d 平均比 %.4f", c, gm[c] / cm[c]);
    CHECK_MSG(rmseBlock / mc < tolBlock, "ブロック RMSE 相対 %.4f", rmseBlock / mc);
    CHECK(gpu.stats.nanSamples == 0);
    Dump("t6_cornell_gpu", gpu.rgb, W, H);
    Dump("t6_cornell_cpu", cpu, W, H);
}

void TestMixedLightsVsCpu(Gpu& g, PathTracer& pt, bool quick)
{
    std::printf("\n[T7] 太陽 + 点光源 2 灯 + スポット + 一様な空 + 光沢/粗い球(エンジン BRDF)の GPU / CPU 一致\n");
    using R::V3;
    R::Scene sc;
    R::Material floorM; floorM.albedo = {0.6, 0.55, 0.5}; floorM.roughness = 0.7;
    R::Material ballA; ballA.albedo = {0.9, 0.2, 0.2}; ballA.roughness = 0.35; ballA.metallic = 0.0;
    R::Material ballB; ballB.albedo = {0.95, 0.85, 0.6}; ballB.roughness = 0.25; ballB.metallic = 1.0;
    sc.mats = {floorM, ballA, ballB};
    R::AddQuad(sc, V3(-8, 0, -8), V3(-8, 0, 8), V3(8, 0, 8), V3(8, 0, -8), V3(0, 1, 0), 0);
    R::AddSphere(sc, V3(-0.9, 0.7, 0.0), 0.7, 1, 64, 32);
    R::AddSphere(sc, V3(0.9, 0.7, 0.4), 0.7, 2, 64, 32);
    sc.sun.enabled = true; sc.sun.toLight = R::Norm(V3(-0.4, 0.8, -0.3)); sc.sun.E = {2.2, 2.0, 1.7};
    R::PointLight p1; p1.pos = {-2, 1.5, -1}; p1.range = 9; p1.color = {6, 3, 1.5};
    R::PointLight p2; p2.pos = {2, 2.5, -1.5}; p2.range = 9; p2.color = {1.5, 3, 6};
    R::PointLight sp; sp.spot = true; sp.pos = {0, 4, -1}; sp.dir = R::Norm(V3(0, -1, 0.3)); sp.range = 12; sp.color = {12, 12, 10};
    sp.cosInner = std::cos(12 * R::kPi / 180); sp.cosOuter = std::cos(26 * R::kPi / 180);
    sc.lights = {p1, p2, sp};
    sc.env.uniform = {0.25, 0.32, 0.45}; sc.env.lightScale = 1.0; sc.env.bgScale = 0.0;
    sc.cam = MakeCam({0, 2.2, -5.5}, {0, 0.6, 0}, 45, 1.0);
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = quick ? 32 : 48, H = W;
    const uint32_t spp = quick ? 256 : 1024;
    GpuResult gpu;
    CHECK(RunGpu(g, pt, gs, sc, W, H, spp, 4, 31, gpu));
    R::RenderParams rp; rp.w = W; rp.h = H; rp.spp = static_cast<int>(spp); rp.bounces = 4; rp.seed = 5; rp.threads = CpuThreads();
    R::Tracer tr(sc);
    const std::vector<float> cpu = tr.Render(rp);
    const double mg = Mean(gpu.rgb), mc = Mean(cpu);
    const auto bg = BlockMean(gpu.rgb, W, H, 8), bc = BlockMean(cpu, W, H, 8);
    const double rmseBlock = Rmse(bg, bc);
    std::printf("  全体平均 GPU %.5f  CPU %.5f  (比 %.4f)  ブロック RMSE 相対 %.4f\n", mg, mc, mg / mc, rmseBlock / mc);
    CHECK_MSG(std::fabs(mg / mc - 1.0) < (quick ? 0.03 : 0.01), "全体平均の比 %.4f", mg / mc);
    CHECK_MSG(rmseBlock / mc < (quick ? 0.06 : 0.03), "ブロック RMSE 相対 %.4f", rmseBlock / mc);
    Dump("t7_mixed_gpu", gpu.rgb, W, H);
    Dump("t7_mixed_cpu", cpu, W, H);
}

// ---------------------------------------------------------------------------
//  インスタンス変換(回転 + 非一様スケール + 平行移動)の検査: 頂点をワールドへ焼き込んだシーン(A)と、
//  ローカル空間のメッシュ + インスタンス行列のシーン(B)は同じ絵になる(法線の逆転置・エミッシブの面積・w2o の検査)。
// ---------------------------------------------------------------------------
void TestInstanceTransform(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T13] インスタンス変換: 焼き込み(A)= ローカル + 行列(B)(回転 + 非一様スケールの面光源と球)\n");
    using R::V3;
    const DirectX::XMMATRIX Wq = DirectX::XMMatrixScaling(2.0f, 1.0f, 0.6f) * DirectX::XMMatrixRotationY(0.5f)
                               * DirectX::XMMatrixTranslation(0.3f, 2.2f, 0.4f);
    const DirectX::XMMATRIX Ws = DirectX::XMMatrixScaling(1.0f, 0.5f, 1.5f) * DirectX::XMMatrixRotationZ(0.4f)
                               * DirectX::XMMatrixTranslation(-1.0f, 0.6f, 0.0f);
    auto xf = [](const DirectX::XMMATRIX& M, const V3& p) {
        DirectX::XMFLOAT3 o; DirectX::XMStoreFloat3(&o, DirectX::XMVector3Transform(DirectX::XMVectorSet(float(p.x), float(p.y), float(p.z), 1), M));
        return V3(o.x, o.y, o.z);
    };
    auto xn = [](const DirectX::XMMATRIX& M, const V3& n) {   // 法線は逆転置
        DirectX::XMMATRIX it = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, M));
        DirectX::XMFLOAT3 o; DirectX::XMStoreFloat3(&o, DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(float(n.x), float(n.y), float(n.z), 0), it)));
        return V3(o.x, o.y, o.z);
    };
    R::Material floorM; floorM.albedo = {0.6, 0.6, 0.6}; floorM.lambert = true;
    R::Material lightM; lightM.albedo = {0, 0, 0}; lightM.lambert = true; lightM.emission = {6, 5, 4};
    R::Material ballM; ballM.albedo = {0.8, 0.3, 0.2}; ballM.roughness = 0.5;

    // ローカルのメッシュ(光源 = XZ の単位正方形で下向き / 球)
    R::Scene local;
    local.mats = {floorM, lightM, ballM};
    R::AddQuad(local, V3(-8, 0, -8), V3(-8, 0, 8), V3(8, 0, 8), V3(8, 0, -8), V3(0, 1, 0), 0);
    R::AddQuad(local, V3(-0.5, 0, -0.5), V3(-0.5, 0, 0.5), V3(0.5, 0, 0.5), V3(0.5, 0, -0.5), V3(0, -1, 0), 1);
    R::AddSphere(local, V3(0, 0, 0), 0.7, 2, 48, 24);
    local.cam = MakeCam({0, 2.0, -5.5}, {0, 0.8, 0}, 45, 1.0);
    local.env.uniform = {0, 0, 0};

    // A: 焼き込み
    R::Scene baked = local;
    baked.tris.clear();
    for (const R::Tri& t : local.tris)
    {
        R::Tri u = t;
        const DirectX::XMMATRIX* M = (t.material == 1) ? &Wq : (t.material == 2 ? &Ws : nullptr);
        if (M) for (int k = 0; k < 3; ++k) { u.p[k] = xf(*M, t.p[k]); u.n[k] = xn(*M, t.n[k]); }
        baked.tris.push_back(u);
    }
    // 反転する行列ではない(det>0)ので巻き方向はそのまま
    GpuScene ga; CHECK(BuildGpuScene(g, baked, ga));
    const uint32_t W = 48, H = 48;
    GpuResult ra; CHECK(RunGpu(g, pt, ga, baked, W, H, 8192, 3, 3, ra));

    // B: ローカル + インスタンス行列(BuildGpuScene を変形版にして組む)
    GpuScene gb;
    {
        gb.builder.Init(g.dev.Get());
        auto* cmd = g.Begin();
        gb.meshes.resize(3);
        for (size_t m = 0; m < 3; ++m)
        {
            std::vector<Vtx96> vs; GpuMesh& gm = gb.meshes[m];
            for (const R::Tri& t : local.tris)
            {
                if (t.material != static_cast<int>(m)) continue;
                for (int k = 0; k < 3; ++k)
                {
                    Vtx96 v{};
                    v.pos[0] = float(t.p[k].x); v.pos[1] = float(t.p[k].y); v.pos[2] = float(t.p[k].z);
                    v.nor[0] = float(t.n[k].x); v.nor[1] = float(t.n[k].y); v.nor[2] = float(t.n[k].z);
                    const R::Material& mt = local.mats[m];
                    v.col[0] = float(mt.albedo.x); v.col[1] = float(mt.albedo.y); v.col[2] = float(mt.albedo.z); v.col[3] = 1;
                    v.tan[0] = 1; v.tan[3] = 1;
                    vs.push_back(v);
                    gm.positions.push_back({v.pos[0], v.pos[1], v.pos[2]});
                    gm.indices.push_back(uint32_t(gm.indices.size()));
                }
            }
            gm.vertexCount = uint32_t(vs.size()); gm.indexCount = uint32_t(gm.indices.size());
            gm.vb = UploadBuffer(g, cmd, vs.data(), vs.size() * sizeof(Vtx96), gb.keep);
            gm.ib = UploadBuffer(g, cmd, gm.indices.data(), gm.indices.size() * 4, gb.keep);
            MakeRawSrv(g, gm.vb.Get(), vs.size() * sizeof(Vtx96), gm.vbSrv);
            MakeRawSrv(g, gm.ib.Get(), gm.indices.size() * 4, gm.ibSrv);
            MaterialGpu mg{};
            mg.tint[0] = mg.tint[1] = mg.tint[2] = 1; mg.opacity = 1;
            mg.metallic = float(local.mats[m].metallic); mg.roughness = float(local.mats[m].roughness);
            mg.flags = local.mats[m].lambert ? kMatLambert : 0u;
            mg.emissive[0] = float(local.mats[m].emission.x); mg.emissive[1] = float(local.mats[m].emission.y); mg.emissive[2] = float(local.mats[m].emission.z);
            mg.albedoSrv = mg.normalSrv = mg.mrSrv = mg.emissiveSrv = kNoIndex;
            mg.uvScaleOffset[0] = mg.uvScaleOffset[1] = 1;
            gb.builder.AddMaterial(mg);
            GeometryDesc gd; gd.key = 5000 + m; gd.vbVA = gm.vb->GetGPUVirtualAddress(); gd.vbStride = 96; gd.vertexCount = gm.vertexCount;
            gd.ibVA = gm.ib->GetGPUVirtualAddress(); gd.indexCount = gm.indexCount; gd.vbSrv = gm.vbSrv; gd.ibSrv = gm.ibSrv;
            gd.cpuPositions = gm.positions.data(); gd.cpuIndices = gm.indices.data();
            InstanceDesc id; id.geometry = gb.builder.AddGeometry(gd); id.material = uint32_t(m);
            DirectX::XMStoreFloat4x4(&id.world, m == 1 ? Wq : (m == 2 ? Ws : DirectX::XMMatrixIdentity()));
            id.emissiveLuma = float(R::Luma(local.mats[m].emission));
            gb.builder.AddInstance(id);
        }
        std::string err;
        CHECK(gb.builder.BeginBuild(&err));
        while (!gb.builder.BuildStep(cmd, 1u << 30)) {}
        CHECK(gb.builder.FinishBuild(cmd, &err));
        g.Submit();
        gb.builder.ReleaseTransient();
    }
    GpuResult rb; CHECK(RunGpu(g, pt, gb, baked, W, H, 8192, 3, 4, rb));
    const double ma = Mean(ra.rgb), mb = Mean(rb.rgb);
    const double rmseBlock = Rmse(BlockMean(ra.rgb, W, H, 8), BlockMean(rb.rgb, W, H, 8));
    std::printf("  平均 A(焼き込み)%.5f  B(行列)%.5f  比 %.4f  ブロック RMSE 相対 %.4f\n", ma, mb, mb / ma, rmseBlock / ma);
    CHECK_MSG(std::fabs(mb / ma - 1.0) < 0.01, "平均比 %.4f", mb / ma);
    CHECK_MSG(rmseBlock / ma < 0.03, "ブロック RMSE 相対 %.4f", rmseBlock / ma);
    Dump("t13_baked", ra.rgb, W, H);
    Dump("t13_instanced", rb.rgb, W, H);
}

void TestAlpha(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T8] 半透明(Blend = 確率的通過)/ アルファテスト(Mask): 発光板の手前に置いた板\n");
    using R::V3;
    struct Case { int mode; double alpha, cutoff, expectFrac; const char* name; };
    const Case cases[] = {
        {R::kBlend, 0.5, 0.5, 0.5, "Blend α=0.5"},
        {R::kBlend, 0.25, 0.5, 0.25, "Blend α=0.25"},
        {R::kMask, 0.3, 0.5, 1.0, "Mask α=0.3 < cutoff(素通し)"},
        {R::kMask, 0.7, 0.5, 0.0, "Mask α=0.7 ≥ cutoff(不透明)"},
    };
    for (const Case& cs : cases)
    {
        R::Scene sc;
        R::Material emit; emit.albedo = {0, 0, 0}; emit.emission = {2, 2, 2}; emit.lambert = true; emit.emitBoth = true;
        R::Material front; front.albedo = {0, 0, 0}; front.lambert = true; front.alphaMode = cs.mode; front.cutoff = cs.cutoff;
        sc.mats = {emit, front};
        R::AddQuad(sc, V3(-5, -5, 5), V3(-5, 5, 5), V3(5, 5, 5), V3(5, -5, 5), V3(0, 0, -1), 0);
        R::AddQuad(sc, V3(-5, -5, 2), V3(-5, 5, 2), V3(5, 5, 2), V3(5, -5, 2), V3(0, 0, -1), 1, cs.alpha);
        sc.cam = MakeCam({0, 0, -1}, {0, 0, 5}, 30, 1.0);
        GpuScene gs;
        CHECK(BuildGpuScene(g, sc, gs));
        GpuResult r;
        CHECK(RunGpu(g, pt, gs, sc, 16, 16, 512, 1, 2, r, kFlagRR));
        // 期待: 通過確率 × 発光(2)。手前の板(黒・ランバート・無ライト)からの放射は 0。
        const double expect = 2.0 * (cs.mode == R::kBlend ? (1.0 - cs.alpha) : cs.expectFrac);
        const double got = Mean(r.rgb);
        std::printf("  %-32s got %.4f  expect %.4f\n", cs.name, got, expect);
        CHECK_MSG(std::fabs(got - expect) < 0.03 * std::max(expect, 0.2), "%s got=%.4f expect=%.4f", cs.name, got, expect);
    }
}

// ---------------------------------------------------------------------------
//  (3) 収束 / 決定論
// ---------------------------------------------------------------------------
void TestConvergence(Gpu& g, PathTracer& pt, bool quick)
{
    std::printf("\n[T9] 収束: 同じ spp で独立に 2 枚描いた差の RMSE/√2 = 標準偏差。spp を 4 倍にすると約 1/2 になる(1/√N)\n");
    const R::Scene sc = MakeCornell();
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = 32, H = 32;
    std::vector<double> xs, ys;
    std::printf("  spp     標準偏差   標準偏差·√spp\n");
    const std::vector<uint32_t> list = quick ? std::vector<uint32_t>{16u, 64u, 256u, 1024u} : std::vector<uint32_t>{16u, 64u, 256u, 1024u, 4096u};
    for (uint32_t spp : list)
    {
        GpuResult a, b;
        CHECK(RunGpu(g, pt, gs, sc, W, H, spp, 6, 1000 + spp, a));
        CHECK(RunGpu(g, pt, gs, sc, W, H, spp, 6, 5000 + spp, b));
        const double sd = Rmse(a.rgb, b.rgb) / std::sqrt(2.0);
        std::printf("  %-6u  %.5f    %.4f\n", spp, sd, sd * std::sqrt(double(spp)));
        xs.push_back(std::log(double(spp)));
        ys.push_back(std::log(sd));
    }
    // 最小二乗の傾き(log-log)。理想は -0.5
    double mx = 0, my = 0;
    for (size_t i = 0; i < xs.size(); ++i) { mx += xs[i]; my += ys[i]; }
    mx /= xs.size(); my /= ys.size();
    double num = 0, den = 0;
    for (size_t i = 0; i < xs.size(); ++i) { num += (xs[i] - mx) * (ys[i] - my); den += (xs[i] - mx) * (xs[i] - mx); }
    const double slope = num / den;
    std::printf("  log-log の傾き = %.3f (理想 -0.5)\n", slope);
    CHECK_MSG(slope < -0.40 && slope > -0.60, "傾き %.3f", slope);   // 理想 -0.5。稀な明るい経路(ファイアフライ)で標準偏差が揺れるので ±0.1
}

void TestDeterminism(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T10] 決定論: 同じ設定(シード / spp / 分割)→ 同じ結果。シードを変えると変わる。分割(タイル / サンプル分割)に依らない\n");
    const R::Scene sc = MakeCornell();
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = 40, H = 24;
    GpuResult a, b, c, d, e;
    CHECK(RunGpu(g, pt, gs, sc, W, H, 64, 5, 42, a));
    CHECK(RunGpu(g, pt, gs, sc, W, H, 64, 5, 42, b));
    CHECK(RunGpu(g, pt, gs, sc, W, H, 64, 5, 43, c));
    CHECK(RunGpu(g, pt, gs, sc, W, H, 64, 5, 42, d, kFlagRR | kFlagEnvNee, 8, 16));   // 1 ディスパッチ 8 サンプル + 小タイル
    double maxAB = 0, maxAD = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i) { maxAB = std::max(maxAB, std::fabs(double(a.rgb[i]) - b.rgb[i])); maxAD = std::max(maxAD, std::fabs(double(a.rgb[i]) - d.rgb[i])); }
    const double diffSeed = Rmse(a.rgb, c.rgb);
    std::printf("  同一設定の最大差 %.3e / 分割違いの最大差 %.3e / シード違いの RMSE %.4f\n", maxAB, maxAD, diffSeed);
    CHECK_MSG(maxAB == 0.0, "同じ設定で結果が変わった(GPU の非決定性)。最大差 %.3e", maxAB);
    CHECK_MSG(maxAD < 1e-5, "分割(タイル / サンプル分割)で結果が変わった。最大差 %.3e", maxAD);
    CHECK(diffSeed > 1e-4);
    (void)e;
}

void TestBudgetedRecord(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T11] 時間予算つき Record: 予算内で少しずつ積んでも(数フレームに分割しても)結果が同じ。GPU 時間の測定が返る\n");
    const R::Scene sc = MakeCornell();
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = 96, H = 96;
    GpuResult ref;
    CHECK(RunGpu(g, pt, gs, sc, W, H, 32, 5, 9, ref, kFlagRR | kFlagEnvNee, 1, 32));
    // 同じジョブを 1 ms 予算で刻む
    JobDesc jd;
    jd.width = W; jd.height = H; jd.spp = 32; jd.bounces = 5; jd.seed = 9; jd.tileSize = 32; jd.samplesPerDispatch = 1;
    for (int i = 0; i < 3; ++i) { jd.cam.pos[i] = float(sc.cam.pos[i]); jd.cam.right[i] = float(sc.cam.right[i]); jd.cam.up[i] = float(sc.cam.up[i]); jd.cam.fwd[i] = float(sc.cam.fwd[i]); }
    jd.cam.tanHalfFovY = float(sc.cam.tanHalfFovY); jd.cam.aspect = float(sc.cam.aspect);
    jd.env.nee = false;   // RunGpu と同じ設定にする(環境が黒なら NEE を省く。乱数の消費順が変わるので揃える)
    std::string err;
    CHECK(pt.BeginJob(jd, gs.builder.GetScene(), &err));
    int calls = 0;
    while (pt.Running() && calls < 100000)
    {
        auto* cmd = g.Begin();
        pt.Record(cmd, g.heap.Get(), 0.5f, 0);
        g.Submit();
        ++calls;
    }
    std::vector<float> rgb; uint32_t samples = 0;
    CHECK(pt.ReadbackAverage(rgb, &samples, &err));
    const PtProgress p = pt.GetProgress();
    double maxd = 0;
    for (size_t i = 0; i < rgb.size(); ++i) maxd = std::max(maxd, std::fabs(double(rgb[i]) - ref.rgb[i]));
    std::printf("  Record 呼び出し %d 回 / ディスパッチ %llu / 1 ユニット %.3f ms / GPU 累計 %.1f ms / 参照との最大差 %.3e\n",
                calls, static_cast<unsigned long long>(p.dispatches), p.msPerUnit, p.gpuMsTotal, maxd);
    CHECK(calls > 1);
    CHECK(maxd < 1e-5);
    CHECK(p.msPerUnit > 0.0);
}

void TestPerformance(Gpu& g, PathTracer& pt)
{
    std::printf("\n[T12] 性能(1080p のコーネルボックス。ms/spp は GPU 実測)\n");
    const R::Scene sc = MakeCornell();
    GpuScene gs;
    CHECK(BuildGpuScene(g, sc, gs));
    const uint32_t W = 1920, H = 1080;
    for (uint32_t bounces : {1u, 6u})
    {
        JobDesc jd;
        jd.width = W; jd.height = H; jd.spp = 8; jd.bounces = bounces; jd.seed = 1; jd.tileSize = 256; jd.samplesPerDispatch = 1;
        jd.flags = kFlagRR | kFlagEnvNee;
        for (int i = 0; i < 3; ++i) { jd.cam.pos[i] = float(sc.cam.pos[i]); jd.cam.right[i] = float(sc.cam.right[i]); jd.cam.up[i] = float(sc.cam.up[i]); jd.cam.fwd[i] = float(sc.cam.fwd[i]); }
        jd.cam.tanHalfFovY = float(sc.cam.tanHalfFovY); jd.cam.aspect = float(W) / float(H);
        jd.env.nee = false;
        std::string err;
        CHECK(pt.BeginJob(jd, gs.builder.GetScene(), &err));
        // 最初の 1 パスは暖機(PSO / キャッシュ)。以後の GPU 時間を測る。
        int calls = 0;
        while (pt.Running() && calls < 4000)
        {
            auto* cmd = g.Begin();
            pt.Record(cmd, g.heap.Get(), 0.0f, 8);
            g.Submit();
            ++calls;
        }
        const PtProgress p = pt.GetProgress();
        std::vector<float> rgb; uint32_t samples = 0;
        CHECK(pt.ReadbackAverage(rgb, &samples, &err));
        // GPU 累計は 1 呼び出し遅れで回収される。1 ユニット = 1 タイル(256x256)× 1 spp
        const double unitsPerSpp = double((W + 255) / 256) * double((H + 255) / 256);
        const double msPerSpp = p.msPerUnit * unitsPerSpp;
        std::printf("  bounces=%u  1 ユニット %.3f ms  → 1080p 1 spp あたり %.2f ms(= %.1f Mpath/s)\n",
                    bounces, p.msPerUnit, msPerSpp, (double(W) * H / 1e6) / (msPerSpp / 1000.0));
        CHECK(p.msPerUnit > 0.0);
        CHECK(samples == 8);
    }
}

} // namespace

int main()
{
    Gpu g;
    const std::string initErr = g.Init();
    if (!initErr.empty()) { std::printf("SKIP: %s(GPU が無い環境では PathTracer のテストを行わない)\n", initErr.c_str()); return 0; }

    // 要件確認(SM 6.6 / Tier 3 / DXR 1.1)。満たさなければ SKIP。
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6
     || FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) || o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3
     || FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) || o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    { std::printf("SKIP: SM 6.6 / Resource Binding Tier 3 / DXR 1.1 のいずれかが無い\n"); return 0; }

    std::filesystem::path shaderDir = std::filesystem::path(DX12E_SHADER_DIR);
    std::wstring sd = shaderDir.wstring();
    if (!sd.empty() && sd.back() != L'/' && sd.back() != L'\\') sd += L'/';

    PathTracer pt;
    std::string err;
    if (!pt.Initialize(g.dev.Get(), g.q.Get(), sd, &err)) { std::printf("FAIL: PathTracer 初期化: %s\n", err.c_str()); return 1; }

    const bool quick = EnvSet("DX12E_PT_TEST_QUICK");
    g_dumpDir = EnvGet("DX12E_PT_TEST_DUMP");
    std::wprintf(L"GPU: %s\n", g.adapterName.c_str());

    TestWhiteFurnaceLambert(g, pt);
    TestGgxFurnace(g, pt);
    TestPointLightPlane(g, pt);
    TestSunPlane(g, pt);
    TestClosedBox(g, pt);
    TestAlpha(g, pt);
    TestInstanceTransform(g, pt);
    TestCornellVsCpu(g, pt, quick);
    TestMixedLightsVsCpu(g, pt, quick);
    TestConvergence(g, pt, quick);
    TestDeterminism(g, pt);
    TestBudgetedRecord(g, pt);
    TestPerformance(g, pt);

    if (g.info)
    {
        const UINT64 nErr = g.info->GetNumStoredMessagesAllowedByRetrievalFilter();
        if (nErr > 0) std::printf("D3D12 デバッグレイヤのメッセージ %llu 件\n", static_cast<unsigned long long>(nErr));
    }
    pt.Shutdown();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
