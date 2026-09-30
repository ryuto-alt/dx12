// 仮想ジオメトリ P2: GPU カリング（shaders/vg/*.hlsl）と CPU 参照（renderer/vg/VgCullReference.h）の一致テスト。
//
//   窓なしの D3D12 デバイス（ハードウェアアダプタ。SM 6.6 + リソースバインディング Tier 3 が要る）で compute だけを実行する。
//   GPU が無い / 要件を満たさない環境（CI）では「SKIP」を出して 0 で終わる（落とさない）。
//
//   ・GPU の可視クラスタ集合 = CPU 参照（複数のカメラ x τ x インスタンス配置 x アセット）。浮動小数の境界の 0.1% 以内の差は許容して記録する。
//   ・二相 HZB: 前フレーム HZB（別の遮蔽物 + ずらした prevViewProj）と今フレーム HZB を別々に渡して一致 / 遮蔽物を置くと可視が単調に減る。
//   ・境界: 空 / 全カリング / 全可視 / カメラ内部 / 極小・極大の τ / 極小・極大スケール / ミラー / 非一様スケール。
//   ・階層走査の結果 = 全クラスタの総当たり評価（枝刈りが保守的であることの検証。CPU のみ）。
//   ・キュー溢れ（小さい容量）で落ちない・overflow を立てる・τ が自動で上がる。
//   ・読込エラー（存在しない / 壊れている / VRAM 予算超過）は構造化エラー。非同期読込（RequestAsset）。
//
//   環境変数: DX12E_TEST_D3D_DEBUG=1 で D3D12 デバッグレイヤを有効にする（バリアの誤りを拾う）。
#include "CookBench.h"
#include "VgeoCook.h"

#include "renderer/vg/VgCullReference.h"
#include "renderer/vg/VirtualGeometrySystem.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::vg;
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
    char* buf = nullptr;
    size_t len = 0;
    const bool set = (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr);
    std::free(buf);
    return set;
}

// ── D3D12 の最小ハーネス ──────────────────────────────────────────────────────
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
    ComPtr<ID3D12InfoQueue> info;      // デバッグレイヤが有効なときだけ

    bool Init()
    {
        if (EnvSet("DX12E_TEST_D3D_DEBUG"))
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            {
                dbg->EnableDebugLayer();
                // GPU ベースの検証（バインドレスのリソース状態 / ディスクリプタの誤りまで拾う。遅い）
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
    ID3D12GraphicsCommandList* Begin()
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
};

// HZB テクスチャ（全ミップを CPU の値で埋める。読み取り状態で返す）
ComPtr<ID3D12Resource> MakeHzbTexture(Gpu& g, const HzbCpu& h)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = h.w; d.Height = h.h; d.DepthOrArraySize = 1; d.MipLevels = static_cast<UINT16>(h.mips);
    d.Format = DXGI_FORMAT_R32_FLOAT; d.SampleDesc = {1, 0}; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) return nullptr;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(h.mips);
    std::vector<UINT> rows(h.mips);
    std::vector<UINT64> rowBytes(h.mips);
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&d, 0, h.mips, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    ComPtr<ID3D12Resource> up;
    D3D12_HEAP_PROPERTIES uhp{};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ud{};
    ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; ud.Width = total; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
    ud.SampleDesc = {1, 0}; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) return nullptr;
    uint8_t* p = nullptr;
    D3D12_RANGE nr{0, 0};
    up->Map(0, &nr, reinterpret_cast<void**>(&p));
    for (u32 m = 0; m < h.mips; ++m)
    {
        const auto [mw, mh] = h.size[m];
        for (u32 y = 0; y < mh; ++y)
            std::memcpy(p + fp[m].Offset + static_cast<size_t>(y) * fp[m].Footprint.RowPitch, &h.data[m][static_cast<size_t>(y) * mw], mw * 4);
    }
    up->Unmap(0, nullptr);
    auto* cl = g.Begin();
    for (u32 m = 0; m < h.mips; ++m)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = tex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = m;
        src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[m];
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    g.Submit();
    return tex;
}

// ── テストアセット ────────────────────────────────────────────────────────────
struct TestAsset
{
    std::string name;
    std::vector<u8> bytes;
    CpuAsset cpu;
    u32 id = 0xFFFFFFFFu;
};

bool MakeAsset(const std::string& kind, u64 tris, u32 seed, TestAsset& out)
{
    cook::BenchOptions bo;
    bo.kind = kind; bo.tris = tris; bo.seed = seed; bo.threads = 2;
    VgsrcData d;
    if (!cook::GenerateBench(bo, d).ok()) return false;
    cook::CookOptions co;
    co.threads = 2; co.collectStats = false; co.proxy = false;
    VgeoContent c;
    if (!cook::Cook(std::move(d), co, c).ok()) return false;
    if (!WriteVgeoToMemory(c, out.bytes).ok()) return false;
    MemorySource ms(out.bytes);
    if (!out.cpu.Load(ms)) return false;
    out.name = kind + "_" + std::to_string(tris) + "_" + std::to_string(seed);
    return true;
}

// ── カメラ / インスタンス ─────────────────────────────────────────────────────
struct Cam
{
    DirectX::XMFLOAT3 pos{0, 0, -20}, target{0, 0, 0};
    float fovY = 1.0471976f, zn = 0.1f, zf = 2000.0f;
    float W = 1920, H = 1080;
    float projScale() const { return 0.5f * H / std::tan(fovY * 0.5f); }
    void VP(float out[16]) const
    {
        using namespace DirectX;
        const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&pos), XMLoadFloat3(&target), XMVectorSet(0, 1, 0, 0));
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, W / H, zn, zf);
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, view * proj);
        std::memcpy(out, &m, 64);
    }
};

VgInstanceInput MakeInst(u32 assetId, float px, float py, float pz, float yaw, float pitch, float sx, float sy, float sz, u32 entity)
{
    using namespace DirectX;
    VgInstanceInput in;
    const XMMATRIX m = XMMatrixScaling(sx, sy, sz) * XMMatrixRotationRollPitchYaw(pitch, yaw, 0) * XMMatrixTranslation(px, py, pz);
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);
    std::memcpy(in.world, &f, 64);
    std::memcpy(in.prevWorld, &f, 64);
    in.hasPrev = false;
    in.assetId = assetId;
    in.entityId = entity;
    return in;
}

// GPU / CPU の両方へ同じ入力を渡す 1 ケース
struct Case
{
    Cam cam;
    Cam prevCam;              // HZB 一相目の投影に使う前フレームのカメラ
    float tau = 1.0f;
    float instanceMinPx = 0.5f;
    bool cone = true;
    bool hzb = false;
    const HzbCpu* hzbPrev = nullptr; ComPtr<ID3D12Resource> hzbPrevTex; bool prevValid = false;
    const HzbCpu* hzbCur = nullptr;  ComPtr<ID3D12Resource> hzbCurTex;  bool curValid = false;
    std::vector<VgInstanceInput> insts;
};

float EffTau(float t) { return std::min(std::max(t, 0.05f), 64.0f); }

RefView MakeRefView(const Case& c, float tauScale = 1.0f)
{
    RefView v;
    c.cam.VP(v.viewProj);
    c.prevCam.VP(v.prevViewProj);
    v.camPos[0] = c.cam.pos.x; v.camPos[1] = c.cam.pos.y; v.camPos[2] = c.cam.pos.z;
    v.zNear = c.cam.zn;
    v.projScale = c.cam.projScale();
    v.vpX = 0; v.vpY = 0; v.vpW = c.cam.W; v.vpH = c.cam.H;
    v.tau = EffTau(c.tau) * tauScale;
    v.instanceMinPx = c.instanceMinPx;
    v.coneCulling = c.cone;
    v.hzbPrev = c.hzb ? c.hzbPrev : nullptr; v.prevValid = c.hzb && c.prevValid;
    v.hzbCur = c.hzb ? c.hzbCur : nullptr;   v.curValid = c.hzb && c.curValid;
    return v;
}

struct GpuOut
{
    FrameStats st;
    std::set<std::pair<u32, u32>> visible;
    size_t dumpSize = 0;
};

GpuOut RunGpu(Gpu& g, VirtualGeometrySystem& sys, const Case& c, u32 slot = 0)
{
    ExecuteDesc d;
    d.frameSlot = slot;
    c.cam.VP(d.viewProj);
    c.prevCam.VP(d.prevViewProj);
    d.camPos[0] = c.cam.pos.x; d.camPos[1] = c.cam.pos.y; d.camPos[2] = c.cam.pos.z;
    d.zNear = c.cam.zn;
    d.projScale = c.cam.projScale();
    d.vpX = 0; d.vpY = 0; d.vpW = c.cam.W; d.vpH = c.cam.H;
    d.instances = c.insts.data();
    d.instanceCount = static_cast<u32>(c.insts.size());
    d.settings.tauPx = c.tau;
    d.settings.instanceMinPx = c.instanceMinPx;
    d.settings.coneCulling = c.cone;
    d.settings.hzbCulling = c.hzb;
    if (c.hzb)
    {
        d.hzb.prev = c.hzbPrevTex.Get(); d.hzb.prevValid = c.prevValid;
        d.hzb.cur = c.hzbCurTex.Get();   d.hzb.curValid = c.curValid;
        d.hzb.width = c.hzbCur ? c.hzbCur->w : (c.hzbPrev ? c.hzbPrev->w : 0);
        d.hzb.height = c.hzbCur ? c.hzbCur->h : (c.hzbPrev ? c.hzbPrev->h : 0);
        d.hzb.mips = c.hzbCur ? c.hzbCur->mips : (c.hzbPrev ? c.hzbPrev->mips : 0);
    }
    d.dumpVisible = true;
    d.cmd = g.Begin();
    const bool ok = sys.Execute(d);
    g.Submit();
    CHECK(ok);
    sys.CollectStats(slot);
    GpuOut o;
    o.st = sys.GetStats();
    for (const auto& e : sys.GetDump()) o.visible.insert(e);
    o.dumpSize = sys.GetDump().size();
    return o;
}

struct CmpResult { size_t cpu = 0, gpu = 0, missing = 0, extra = 0; };
CmpResult Compare(const std::set<std::pair<u32, u32>>& gpu, const std::set<std::pair<u32, u32>>& cpu)
{
    CmpResult r;
    r.cpu = cpu.size(); r.gpu = gpu.size();
    for (const auto& e : cpu) if (!gpu.count(e)) ++r.missing;
    for (const auto& e : gpu) if (!cpu.count(e)) ++r.extra;
    return r;
}

struct Agg { size_t cases = 0, cpuTotal = 0, diffTotal = 0, exact = 0; double worst = 0.0; };
Agg g_agg;

// 一致判定: 集合の差が CPU の要素数の 0.1%（最低 2 個）以内
void CheckMatch(const char* label, const GpuOut& g, const RefResult& r, bool verbose = false)
{
    const CmpResult c = Compare(g.visible, r.visible);
    const size_t diff = c.missing + c.extra;
    const size_t allow = std::max<size_t>(2, c.cpu / 1000);
    ++g_agg.cases; g_agg.cpuTotal += c.cpu; g_agg.diffTotal += diff;
    if (diff == 0) ++g_agg.exact;
    if (c.cpu) g_agg.worst = std::max(g_agg.worst, static_cast<double>(diff) / static_cast<double>(c.cpu));
    if (diff > allow || verbose)
        std::printf("  [%s] cpu=%zu gpu=%zu missing=%zu extra=%zu (許容 %zu)\n", label, c.cpu, c.gpu, c.missing, c.extra, allow);
    CHECK(diff <= allow);
    CHECK(!g.st.overflow);
    // 統計の一致（境界の差を許容）
    auto nearTol = [&](const char* name, u64 gpuV, u64 cpuV)
    {
        const u64 d = gpuV > cpuV ? gpuV - cpuV : cpuV - gpuV;
        const u64 tol = std::max<u64>(4, cpuV / 500);
        if (d > tol) std::printf("  [%s] stat %s: gpu=%llu cpu=%llu\n", label, name, static_cast<unsigned long long>(gpuV), static_cast<unsigned long long>(cpuV));
        CHECK(d <= tol);
    };
    nearTol("instances", g.st.instances, r.stats.instances);
    nearTol("instancesFrustum", g.st.instancesFrustum, r.stats.instFrustum);
    nearTol("nodesVisited", g.st.nodesVisited, r.stats.nodesVisited);
    nearTol("groups", g.st.groups, r.stats.groups);
    nearTol("clustersTested", g.st.clustersTested, r.stats.clustersTested);
    nearTol("clustersSelected", g.st.clustersSelected, r.stats.lodSelected);
    nearTol("visible", g.st.visibleClusters, r.stats.visibleP1 + r.stats.visibleP2);
    nearTol("visibleP2", g.st.visibleP2, r.stats.visibleP2);
    nearTol("tris", g.st.trianglesDrawn, r.stats.tris);
    nearTol("srcTris", g.st.sourceTrianglesInFrustum, r.stats.srcTris);
    nearTol("clCullCone", g.st.clustersCulledCone, r.stats.clCullCone);
    nearTol("clDeferred", g.st.clustersDeferred, r.stats.clDeferred);
}

// 全クラスタ総当たり（LOD + 錐台 + コーン。HZB なし）。階層走査が保守的であることの検証用。
std::set<std::pair<u32, u32>> BruteForce(const std::vector<const CpuAsset*>& assets, const std::vector<VgInstanceInput>& inputs, const RefView& v)
{
    std::set<std::pair<u32, u32>> out;
    FrustumPlanes planes;
    ExtractFrustumPlanes(v.viewProj, planes);
    for (u32 i = 0; i < inputs.size(); ++i)
    {
        const VgInstance in = MakeInstance(inputs[i], inputs[i].assetId);
        const CpuAsset& a = *assets[in.assetIndex];
        const VgeoHeader& h = a.meta.header;
        float c[3];
        TransformPoint34(in.world, h.boundingSphere, c);
        const float r = h.boundingSphere[3] * in.maxScale;
        if (SphereOutsideFrustum(planes, c, r)) continue;
        if (v.instanceMinPx > 0.0f)
        {
            const float dx = c[0] - v.camPos[0], dy = c[1] - v.camPos[1], dz = c[2] - v.camPos[2];
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist < v.zNear) dist = v.zNear;
            if (r * v.projScale / dist < v.instanceMinPx) continue;
        }
        float camObj[3] = {0, 0, 0};
        const bool camOk = CameraToObjectSpace(inputs[i].world, v.camPos, camObj);
        for (u32 p = 0; p < h.pageCount; ++p)
        {
            const PageHeader ph = ReadPageHeader(a.pages.data() + static_cast<size_t>(p) * kPageSize);
            for (u32 k = 0; k < ph.clusterCount; ++k)
            {
                const ClusterHeader ch = a.Cluster(p, k);
                const float s = in.maxScale;
                float lc[3], pc[3], cc[3];
                TransformPoint34(in.world, ch.lodSphere, lc);
                TransformPoint34(in.world, ch.parentLodSphere, pc);
                const float lsW[4] = {lc[0], lc[1], lc[2], ch.lodSphere[3] * s};
                const float psW[4] = {pc[0], pc[1], pc[2], ch.parentLodSphere[3] * s};
                if (!(ProjectedErrorPx(ch.parentLodError, psW, v.camPos, s, v.projScale, v.zNear) > v.tau)) continue;
                if (ProjectedErrorPx(ch.lodError, lsW, v.camPos, s, v.projScale, v.zNear) > v.tau) continue;
                TransformPoint34(in.world, ch.cullSphere, cc);
                if (SphereOutsideFrustum(planes, cc, ch.cullSphere[3] * s)) continue;
                if (v.coneCulling && camOk)
                {
                    const float cutoff = static_cast<float>(ConeByte(ch.coneS8, 3)) / 127.0f;
                    const float axis[3] = {ConeAxisComponent(ch.coneS8, 0), ConeAxisComponent(ch.coneS8, 1), ConeAxisComponent(ch.coneS8, 2)};
                    if (cutoff < 1.0f && ConeCulls(ch.cullSphere, ch.cullSphere[3], axis, cutoff, camObj)) continue;
                }
                out.insert({i, MakeClusterRef(p, k)});
            }
        }
    }
    return out;
}

// 深度バッファ（NDC 深度）: 矩形 [x0,x1) x [y0,y1) に depth、それ以外は 1（何も描かれていない）
std::vector<float> MakeDepth(u32 w, u32 h, u32 x0, u32 y0, u32 x1, u32 y1, float depth)
{
    std::vector<float> d(static_cast<size_t>(w) * h, 1.0f);
    for (u32 y = y0; y < y1 && y < h; ++y)
        for (u32 x = x0; x < x1 && x < w; ++x) d[static_cast<size_t>(y) * w + x] = depth;
    return d;
}

std::vector<VgInstanceInput> RandomInstances(std::mt19937& rng, u32 count, const std::vector<u32>& assetIds, float extent, bool allowMirror)
{
    std::uniform_real_distribution<float> pos(-extent, extent), ang(0.0f, 6.2831853f), sc(0.4f, 2.5f);
    std::vector<VgInstanceInput> v;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 a = assetIds[rng() % assetIds.size()];
        float sx = sc(rng), sy = sx, sz = sx;
        if (i % 5 == 3) { sx = sc(rng); sy = sc(rng); sz = sc(rng); }              // 非一様スケール
        if (allowMirror && i % 7 == 6) sx = -sx;                                   // ミラー
        v.push_back(MakeInst(a, pos(rng), pos(rng) * 0.5f, pos(rng), ang(rng), ang(rng) * 0.3f, sx, sy, sz, i));
    }
    return v;
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // クラッシュしても進捗が残るように
    Gpu g;
    if (!g.Init())
    {
        std::printf("SKIP: 利用できる D3D12 ハードウェアデバイスが無い（CI 想定）\n");
        return 0;
    }
    std::wprintf(L"GPU: %s\n", g.adapterName.c_str());
    if (EnvSet("DX12E_TEST_D3D_DEBUG")) std::printf("D3D12 デバッグレイヤ: %s\n", g.info ? "有効" : "利用できない（Graphics Tools 未導入）");

    std::filesystem::path shaderDir = std::filesystem::path(DX12E_SHADER_DIR);
    SystemDesc sd;
    sd.device = g.dev.Get();
    sd.directQueue = g.q.Get();
    sd.shaderDir = shaderDir.wstring();
    if (!sd.shaderDir.empty() && sd.shaderDir.back() != L'/' && sd.shaderDir.back() != L'\\') sd.shaderDir += L'/';
    sd.verifyPageCrc = true;
    VirtualGeometrySystem sys;
    std::string initErr;
    if (!sys.Initialize(sd, &initErr))
    {
        // SM 6.6 / Tier 3 が無い GPU はスキップ。それ以外（シェーダ未ビルド等）は失敗。
        if (initErr.find("6.6") != std::string::npos || initErr.find("Tier") != std::string::npos)
        {
            std::printf("SKIP: この GPU は VG の要件を満たさない（%s）\n", initErr.c_str());
            return 0;
        }
        std::printf("FAIL: VirtualGeometrySystem の初期化に失敗: %s\n", initErr.c_str());
        return 1;
    }

    // ── アセット ──────────────────────────────────────────────────────────────
    TestAsset blob, torus, knot, rock;
    CHECK(MakeAsset("blob", 60000, 1, blob));
    CHECK(MakeAsset("torus", 30000, 2, torus));
    CHECK(MakeAsset("knot", 40000, 3, knot));
    CHECK(MakeAsset("rock", 600000, 4, rock));   // 大きめ（BVH が深い・ページが多い）
    {
        LoadError le;
        for (TestAsset* a : {&blob, &torus, &knot, &rock})
        {
            MemorySource ms(a->bytes);
            const bool ok = sys.LoadAssetFromSource(ms, a->name, &a->id, &le);
            if (!ok) std::printf("読込失敗 %s: %s\n", a->name.c_str(), le.ToString().c_str());
            CHECK(ok);
        }
        // 同じ名前は同じ ID
        uint32_t again = 0;
        MemorySource ms(blob.bytes);
        CHECK(sys.LoadAssetFromSource(ms, blob.name, &again, &le) && again == blob.id);
        AssetInfo ai;
        CHECK(sys.GetAssetInfo(blob.id, ai) && ai.ready && ai.vramBytes > 0 && ai.pageCount == blob.cpu.meta.header.pageCount);
        std::printf("アセット: blob %u ページ / BVH 深さ %u / VRAM %.1f MB, torus %u ページ, knot %u ページ, rock %u ページ / 最大 BVH 深さ %u\n",
                    ai.pageCount, ai.bvhDepth, ai.vramBytes / 1048576.0, torus.cpu.meta.header.pageCount, knot.cpu.meta.header.pageCount, rock.cpu.meta.header.pageCount, sys.MaxBvhDepth());
    }
    const std::vector<const CpuAsset*> assets = [&] {
        std::vector<const CpuAsset*> v(4, nullptr);
        v[blob.id] = &blob.cpu; v[torus.id] = &torus.cpu; v[knot.id] = &knot.cpu; v[rock.id] = &rock.cpu;
        return v;
    }();
    const std::vector<u32> ids = {blob.id, torus.id, knot.id, rock.id};

    // ── 1. 選択集合の一致（HZB なし）: カメラ x τ x 配置 ───────────────────────
    {
        std::mt19937 rng(12345);
        const float dists[] = {6.0f, 14.0f, 40.0f, 120.0f, 600.0f};
        const float taus[] = {0.25f, 1.0f, 4.0f};
        int n = 0;
        for (float dist : dists)
            for (float tau : taus)
            {
                Case c;
                const float yaw = 0.7f * static_cast<float>(n);
                c.cam.pos = {dist * std::sin(yaw), 0.3f * dist, -dist * std::cos(yaw)};
                c.cam.target = {0, 0, 0};
                c.prevCam = c.cam;
                c.tau = tau;
                c.insts = RandomInstances(rng, (n % 3 == 0) ? 1 : (n % 3 == 1 ? 6 : 40), ids, 10.0f, true);
                const GpuOut go = RunGpu(g, sys, c, static_cast<u32>(n % 3));
                const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
                char label[64];
                std::snprintf(label, sizeof(label), "no-hzb d=%.0f tau=%.2f n=%zu", dist, tau, c.insts.size());
                CheckMatch(label, go, rr);
                ++n;
            }
        std::printf("選択集合の一致（HZB なし）: %zu ケース / CPU 合計 %zu 個 / 差 %zu 個 / 完全一致 %zu ケース / 最悪 %.4f%%\n",
                    g_agg.cases, g_agg.cpuTotal, g_agg.diffTotal, g_agg.exact, g_agg.worst * 100.0);
    }

    // ── 2. 階層走査 = 総当たり（枝刈りが保守的）───────────────────────────────
    {
        std::mt19937 rng(777);
        size_t total = 0, diffTotal = 0;
        for (int k = 0; k < 12; ++k)
        {
            Case c;
            const float dist = (k % 4 == 0) ? 8.0f : (k % 4 == 1) ? 25.0f : (k % 4 == 2) ? 90.0f : 400.0f;
            c.cam.pos = {dist * std::sin(1.3f * k), 0.5f * dist, -dist * std::cos(1.3f * k)};
            c.prevCam = c.cam;
            c.tau = (k % 3 == 0) ? 0.5f : (k % 3 == 1) ? 1.0f : 3.0f;
            c.insts = RandomInstances(rng, 8, ids, 8.0f, true);
            const RefView v = MakeRefView(c);
            const RefResult rr = CullReference(assets, c.insts, v);
            const auto bf = BruteForce(assets, c.insts, v);
            const CmpResult cr = Compare(rr.visible, bf);
            total += bf.size(); diffTotal += cr.missing + cr.extra;
            if (cr.missing + cr.extra) std::printf("  [tree vs brute k=%d] tree=%zu brute=%zu missing=%zu extra=%zu\n", k, cr.cpu, cr.gpu, cr.missing, cr.extra);
            CHECK(cr.missing + cr.extra <= std::max<size_t>(2, bf.size() / 1000));
        }
        std::printf("階層走査 vs 総当たり: 総当たり %zu 個 / 差 %zu 個\n", total, diffTotal);
    }

    // ── 3. 二相 HZB ───────────────────────────────────────────────────────────
    {
        const u32 W = 512, H = 288;
        // 前フレーム: 左寄りの大きな遮蔽物 / 今フレーム: 中央の大きな遮蔽物
        const HzbCpu hzbPrev = HzbCpu::FromDepth(MakeDepth(W, H, 40, 30, 300, 260, 0.6f), W, H);
        const HzbCpu hzbCur = HzbCpu::FromDepth(MakeDepth(W, H, 120, 40, 420, 250, 0.55f), W, H);
        const ComPtr<ID3D12Resource> texPrev = MakeHzbTexture(g, hzbPrev);
        const ComPtr<ID3D12Resource> texCur = MakeHzbTexture(g, hzbCur);
        CHECK(texPrev && texCur);
        std::mt19937 rng(4242);
        size_t hzbCases = 0, p2Total = 0, deferredTotal = 0;
        for (int k = 0; k < 10; ++k)
        {
            Case c;
            c.cam.W = static_cast<float>(W); c.cam.H = static_cast<float>(H);
            const float dist = (k % 2) ? 12.0f : 30.0f;
            c.cam.pos = {dist * std::sin(0.6f * k), 0.2f * dist, -dist * std::cos(0.6f * k)};
            c.prevCam = c.cam;
            // 前フレームのカメラを少しずらす（一相目の再投影が prevViewProj を使うことの確認）
            c.prevCam.pos.x += 0.4f; c.prevCam.target.y += 0.2f;
            c.tau = 1.0f;
            c.insts = RandomInstances(rng, 10, ids, 6.0f, true);
            c.hzb = true;
            c.hzbPrev = &hzbPrev; c.hzbPrevTex = texPrev; c.prevValid = (k % 5 != 4);   // 4 ケース目ごとに一相目の HZB を無効にする
            c.hzbCur = &hzbCur;   c.hzbCurTex = texCur;   c.curValid = true;
            const GpuOut go = RunGpu(g, sys, c, static_cast<u32>(k % 3));
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            char label[64];
            std::snprintf(label, sizeof(label), "hzb k=%d prevValid=%d", k, c.prevValid ? 1 : 0);
            CheckMatch(label, go, rr);
            p2Total += go.st.visibleP2; deferredTotal += go.st.clustersDeferred + go.st.nodesDeferred + go.st.instancesHzbRejected;
            ++hzbCases;

            // 単調性: HZB を切った同じケースの可視集合が、HZB ありの可視集合を含む（遮蔽は可視を減らすだけ）
            Case off = c; off.hzb = false;
            const GpuOut goOff = RunGpu(g, sys, off, static_cast<u32>((k + 1) % 3));
            size_t notSubset = 0;
            for (const auto& e : go.visible) if (!goOff.visible.count(e)) ++notSubset;
            CHECK(notSubset <= std::max<size_t>(2, go.visible.size() / 1000));
            CHECK(go.st.visibleClusters <= goOff.st.visibleClusters + 2);
        }
        std::printf("二相 HZB: %zu ケース / 二相目で救われたクラスタ 合計 %zu / HZB で二相目へ回った要素 合計 %zu\n", hzbCases, p2Total, deferredTotal);
        CHECK(deferredTotal > 0);   // 遮蔽物が効いていること（テストが空振りしていない）
    }

    // ── 4. 境界ケース ─────────────────────────────────────────────────────────
    {
        std::mt19937 rng(9);
        // 空
        {
            Case c; c.prevCam = c.cam;
            const GpuOut go = RunGpu(g, sys, c);
            CHECK(go.visible.empty() && go.st.instances == 0 && go.st.visibleClusters == 0);
        }
        // 全カリング（カメラの後ろ向き）
        {
            Case c;
            c.cam.pos = {0, 0, -30}; c.cam.target = {0, 0, -100};
            c.prevCam = c.cam;
            c.insts = RandomInstances(rng, 20, ids, 5.0f, true);
            const GpuOut go = RunGpu(g, sys, c);
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            CHECK(rr.visible.empty() && go.visible.empty());
            CHECK(go.st.instancesFrustum == 0);
        }
        // 全可視・τ 最小（LOD0 まで下がる）と τ 最大（ルートだけ）
        for (float tau : {0.0f, 1000.0f})
        {
            Case c;
            c.cam.pos = {0, 0, -9}; c.prevCam = c.cam; c.tau = tau;
            c.insts = {MakeInst(blob.id, 0, 0, 0, 0.3f, 0.1f, 1, 1, 1, 0)};
            const GpuOut go = RunGpu(g, sys, c);
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            char label[48];
            std::snprintf(label, sizeof(label), "tau=%.0f", tau);
            CheckMatch(label, go, rr, true);
            if (tau >= 1000.0f) CHECK(go.st.visibleClusters <= 16 && go.st.trianglesDrawn < 3000);   // τ は 64 px に頭打ち（粗いレベルだけ）
            else CHECK(go.st.trianglesDrawn > 20000);
        }
        // カメラがアセットの内部（境界球の中心）
        {
            Case c;
            c.cam.pos = {0.2f, 0.1f, 0.0f}; c.cam.target = {0, 0.1f, 5};
            c.prevCam = c.cam; c.tau = 1.0f;
            c.insts = {MakeInst(blob.id, 0, 0, 0, 0, 0, 1, 1, 1, 0), MakeInst(knot.id, 1, 0, 3, 0, 0, 1, 1, 1, 1)};
            const GpuOut go = RunGpu(g, sys, c);
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            CheckMatch("camera-inside", go, rr, true);
        }
        // 極小 / 極大の投影スケール（画面高 8 px / 100 万 px）
        for (float H : {8.0f, 1.0e6f})
        {
            Case c;
            c.cam.pos = {0, 0, -14}; c.cam.H = H; c.cam.W = H * 16.0f / 9.0f; c.prevCam = c.cam;
            c.insts = RandomInstances(rng, 4, ids, 3.0f, true);
            const GpuOut go = RunGpu(g, sys, c);
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            char label[48];
            std::snprintf(label, sizeof(label), "screenH=%.0f", H);
            CheckMatch(label, go, rr, true);
        }
        // 極小 / 極大のインスタンススケール
        for (float sc : {1.0e-3f, 1.0e3f})
        {
            Case c;
            c.cam.pos = {0, 0, -14.0f * sc}; c.cam.zn = 0.1f * sc; c.cam.zf = 2000.0f * sc; c.prevCam = c.cam;
            c.instanceMinPx = 0.0f;
            c.insts = {MakeInst(torus.id, 0, 0, 0, 0.4f, 0.2f, sc, sc, sc, 0)};
            const GpuOut go = RunGpu(g, sys, c);
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            char label[48];
            std::snprintf(label, sizeof(label), "scale=%g", sc);
            CheckMatch(label, go, rr, true);
        }
        // 極小インスタンスの棄却（画面上の半径 < 0.5 px）と instanceMinPx=0
        {
            Case c;
            c.cam.pos = {0, 0, -5000}; c.prevCam = c.cam;
            c.insts = {MakeInst(blob.id, 0, 0, 0, 0, 0, 1, 1, 1, 0)};
            const GpuOut go = RunGpu(g, sys, c);
            CHECK(go.st.instancesFrustum == 0 && go.visible.empty());
            c.instanceMinPx = 0.0f;
            const GpuOut go2 = RunGpu(g, sys, c);
            const RefResult rr = CullReference(assets, c.insts, MakeRefView(c));
            CheckMatch("far-no-minpx", go2, rr, true);
        }
        // コーン OFF / ON で ON の方が少ない（閉じた形状）
        {
            Case c;
            c.cam.pos = {0, 0, -8}; c.prevCam = c.cam; c.tau = 0.5f;
            c.insts = {MakeInst(blob.id, 0, 0, 0, 0, 0, 1, 1, 1, 0)};
            const GpuOut on = RunGpu(g, sys, c);
            c.cone = false;
            const GpuOut off = RunGpu(g, sys, c);
            CHECK(on.st.visibleClusters < off.st.visibleClusters);
            std::printf("コーン棄却: OFF %u → ON %u クラスタ（%.0f%% 減）\n", off.st.visibleClusters, on.st.visibleClusters,
                        100.0 * (1.0 - static_cast<double>(on.st.visibleClusters) / std::max(1u, off.st.visibleClusters)));
        }
        // 数フレーム連続（スロットを回す）で結果が安定
        {
            Case c;
            c.cam.pos = {3, 2, -12}; c.prevCam = c.cam;
            c.insts = RandomInstances(rng, 12, ids, 6.0f, true);
            u32 first = 0;
            for (u32 f = 0; f < 6; ++f)
            {
                const GpuOut go = RunGpu(g, sys, c, f % 3);
                if (f == 0) first = go.st.visibleClusters;
                CHECK(std::abs(static_cast<int>(go.st.visibleClusters) - static_cast<int>(first)) <= 2);
            }
        }
    }

    // ── 5. キュー溢れ ─────────────────────────────────────────────────────────
    {
        VirtualGeometrySystem smallSys;
        SystemDesc s2 = sd;
        s2.visibleCap = 64; s2.groupQueueCap = 256; s2.nodeQueueCap = 64; s2.deferredCap = 64;
        std::string e;
        CHECK(smallSys.Initialize(s2, &e));
        uint32_t id = 0;
        MemorySource ms(blob.bytes);
        LoadError le;
        CHECK(smallSys.LoadAssetFromSource(ms, blob.name, &id, &le));
        Case c;
        c.cam.pos = {0, 0, -6}; c.prevCam = c.cam; c.tau = 0.25f;
        c.insts = {MakeInst(id, 0, 0, 0, 0, 0, 1, 1, 1, 0)};
        const GpuOut a = RunGpu(g, smallSys, c, 0);
        CHECK(a.st.overflow);
        CHECK(a.dumpSize <= 64);
        const GpuOut b = RunGpu(g, smallSys, c, 1);
        CHECK(b.st.tauUsed > EffTau(c.tau));   // 次フレームの τ が自動で上がる
        std::printf("キュー溢れ: overflow=%d visibleOverflow=%u / 次フレームの τ %.3f → %.3f\n", a.st.overflow ? 1 : 0, a.st.overflowVisible, a.st.tauUsed, b.st.tauUsed);
        smallSys.Shutdown();
    }

    // ── 6. 読込エラー / 非同期読込 / VRAM 予算 ────────────────────────────────
    {
        LoadError le;
        uint32_t id = 0;
        CHECK(!sys.LoadAssetSync("Z:/no/such/file.vgeo", &id, &le) && le.code == LoadCode::OpenFailed);
        // 壊れたファイル（ヘッダの 1 バイト）
        std::vector<u8> bad = blob.bytes;
        bad[100] ^= 0xFF;
        MemorySource ms(bad);
        CHECK(!sys.LoadAssetFromSource(ms, "bad_header", &id, &le) && le.code == LoadCode::Invalid);
        // ページの破損（verifyPageCrc = true なので検出される）
        std::vector<u8> bad2 = blob.bytes;
        bad2[bad2.size() - 5000] ^= 0x5A;
        MemorySource ms2(bad2);
        const bool okBad2 = sys.LoadAssetFromSource(ms2, "bad_page", &id, &le);
        CHECK(!okBad2 && (le.code == LoadCode::Invalid));
        // VRAM 予算
        {
            VirtualGeometrySystem tiny;
            SystemDesc s3 = sd;
            s3.vramBudgetMB = 1;
            std::string e;
            CHECK(tiny.Initialize(s3, &e));
            MemorySource m3(blob.bytes);
            CHECK(!tiny.LoadAssetFromSource(m3, "over", &id, &le) && le.code == LoadCode::OverBudget);
            std::printf("VRAM 予算超過: %s\n", le.ToString().c_str());
            tiny.Shutdown();
        }
        // 非同期読込（ワーカースレッド）
        {
            const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "vg_cull_gpu_test_torus.vgeo";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(torus.bytes.data()), static_cast<std::streamsize>(torus.bytes.size()));
            }
            const uint32_t rid = sys.RequestAsset(tmp.string());
            CHECK(rid != 0xFFFFFFFFu);
            for (int i = 0; i < 4000 && !sys.IsAssetReady(rid); ++i) { sys.Pump(); Sleep(2); }
            CHECK(sys.IsAssetReady(rid));
            AssetInfo ai;
            CHECK(sys.GetAssetInfo(rid, ai) && ai.ready && ai.pageCount == torus.cpu.meta.header.pageCount);
            // 非同期で読んだアセットでも同じ結果
            std::vector<const CpuAsset*> as2 = assets;
            as2.resize(std::max<size_t>(as2.size(), rid + 1), nullptr);
            as2[rid] = &torus.cpu;
            Case c;
            c.cam.pos = {0, 2, -9}; c.prevCam = c.cam;
            c.insts = {MakeInst(rid, 0, 0, 0, 0.5f, 0, 1, 1, 1, 0)};
            const GpuOut go = RunGpu(g, sys, c);
            const RefResult rr = CullReference(as2, c.insts, MakeRefView(c));
            CheckMatch("async-loaded", go, rr, true);
            std::filesystem::remove(tmp);
        }
    }

    const FrameStats& fs = sys.GetStats();
    std::printf("最後のフレーム: カリング GPU %.3f ms / VRAM %.1f MB\n", fs.cullGpuMs, fs.vramBytes / 1048576.0);
    sys.Shutdown();

    // デバッグレイヤのメッセージ（エラー / 破損 / 警告）を数える
    if (g.info)
    {
        UINT64 bad = 0;
        const UINT64 n = g.info->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i)
        {
            SIZE_T len = 0;
            g.info->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(g.info->GetMessage(i, m, &len)) && (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING))
            {
                ++bad;
                if (bad <= 10) std::printf("D3D12 メッセージ(sev=%d id=%d): %s\n", static_cast<int>(m->Severity), static_cast<int>(m->ID), m->pDescription);
            }
        }
        std::printf("D3D12 デバッグレイヤ: 保存メッセージ %llu 件 / エラー・警告 %llu 件\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(bad));
        CHECK(bad == 0);
    }
    std::printf("VgCullGpuTests: %d チェック / 失敗 %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
