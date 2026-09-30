// 仮想ジオメトリ P3: メッシュシェーダのラスタ（shaders/vg/VgRaster.hlsl）の検証。
//
//   窓なしの D3D12 デバイスで、カリング + 二相ラスタを実行して可視性バッファ（R32_UINT）と深度（D32）を読み戻し、
//   CPU の三角形ラスタライザ（この TU 内。固定小数点 1/256 px の頂点スナップ + トップレフト規則 = HW と同じ規則）と画素単位で比べる。
//
//   ・(a) 可視クラスタ表（GPU が出した物）を CPU が同じ行列で描いた画像 = GPU の画像（クラスタ / 三角形の ID・深度）。
//         カメラ x τ x インスタンス配置 x { MS のみ / AS 経由 } x { 小三角形カリング ON / OFF }。
//   ・(b) 全 LOD0 を素朴に描いた参照（CPU が .vgeo の LOD0 葉クラスタを全部描く。VG 経路を通らない）との比較:
//         forceLod0 の GPU 画像 = 参照（被覆・深度。ID はクラスタ選択が違うので比べない）/ τ = 1 px の GPU 画像 = 参照 —
//         「穴」（参照が覆っているのに VG が空の画素）が、参照のシルエット境界から 1.5 px より内側に 0 個、
//         VG が参照のシルエットの外へはみ出す画素も同様に 0 個（LOD 誤差 1 px の範囲で説明できること）。
//   ・既存の深度（非 VG のプリパスに相当。左半分に手前の面）に対して LESS で負けること。
//   ・計測: 断片数 >= 被覆画素数 / 被覆画素数 = CPU の数え直しと完全一致。
//   ・二相のラスタ（HZB あり: 前フレームと今フレームで別の遮蔽物）でも欠落が無いこと（HZB の効果 = 可視が減るだけ）。
//
//   GPU が無い / メッシュシェーダ Tier 1 が無い環境では「SKIP」を出して 0 で終わる。
//   環境変数: DX12E_TEST_D3D_DEBUG=1 でデバッグレイヤ（バリアの誤りを拾う）。DX12E_TEST_D3D_GBV=1 で GPU ベース検証。
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

D3D12_RESOURCE_BARRIER Trans(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return x;
}

// ── アセット ─────────────────────────────────────────────────────────────────
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

struct Cam
{
    DirectX::XMFLOAT3 pos{0, 0, -20}, target{0, 0, 0};
    float fovY = 1.0471976f, zn = 0.1f, zf = 2000.0f;
    float W = 512, H = 288;
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

// ── CPU の三角形ラスタライザ（HW と同じ規則: 頂点を 1/256 px に丸め、画素中心 + トップレフト規則、整数の辺関数）────────
struct Fb
{
    u32 W = 0, H = 0;
    std::vector<u32> id;       // (スロット << 7) | 三角形（ID を持たない参照では 0 / 空 = 0xFFFFFFFF）
    std::vector<float> z;      // NDC 深度（LESS）
    void Init(u32 w, u32 h, float depthInit = 1.0f)
    {
        W = w; H = h;
        id.assign(static_cast<size_t>(w) * h, 0xFFFFFFFFu);
        z.assign(static_cast<size_t>(w) * h, depthInit);
    }
};

struct ScreenVert { double x, y, z; bool ok; };

// クリップ座標 → 画素（ビューポート = フレームバッファ全体）。前方（w > 0.05）かつ深度 [0,1] のときだけ ok（近クリップは扱わない）
ScreenVert ToScreen(const float clip[4], u32 W, u32 H)
{
    ScreenVert s{0, 0, 0, false};
    if (!(clip[3] > 0.05f)) return s;
    const double iw = 1.0 / clip[3];
    const double nx = clip[0] * iw, ny = clip[1] * iw, nz = clip[2] * iw;
    if (!(nz >= 0.0 && nz <= 1.0)) return s;
    s.x = (nx * 0.5 + 0.5) * W;
    s.y = (1.0 - (ny * 0.5 + 0.5)) * H;
    s.z = nz;
    s.ok = true;
    return s;
}

// 1 三角形。戻り値 = 描いた画素数（深度テスト後）。tieCount = 深度がほぼ同値で ID が入れ替わった回数（診断用）
u64 RasterTri(Fb& fb, const ScreenVert& a, const ScreenVert& b, const ScreenVert& c, u32 idv, bool writeId)
{
    if (!a.ok || !b.ok || !c.ok) return 0;
    auto snap = [](double v) { return static_cast<i64>(std::llround(v * 256.0)); };
    i64 ax = snap(a.x), ay = snap(a.y), bx = snap(b.x), by = snap(b.y), cx = snap(c.x), cy = snap(c.y);
    // 面積（符号付き）
    i64 area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0) return 0;
    double za = a.z, zb = b.z, zc = c.z;
    if (area < 0) { std::swap(bx, cx); std::swap(by, cy); std::swap(zb, zc); area = -area; }
    const i64 minx = std::max<i64>(0, (std::min({ax, bx, cx}) - 128) >> 8);
    const i64 maxx = std::min<i64>(static_cast<i64>(fb.W) - 1, (std::max({ax, bx, cx}) + 128) >> 8);
    const i64 miny = std::max<i64>(0, (std::min({ay, by, cy}) - 128) >> 8);
    const i64 maxy = std::min<i64>(static_cast<i64>(fb.H) - 1, (std::max({ay, by, cy}) + 128) >> 8);
    if (minx > maxx || miny > maxy) return 0;
    // 辺関数 E(p) = (p - v0) x (v1 - v0) の符号。area > 0 の向き（画面座標 y 下向き）で内側 = 全て >= 0 になるように並べる
    struct Edge { i64 A, B, C; bool tl; };
    auto mk = [](i64 x0, i64 y0, i64 x1, i64 y1) {
        Edge e;
        e.A = y0 - y1;                 // E(x, y) = A*x + B*y + C
        e.B = x1 - x0;
        e.C = x0 * y1 - x1 * y0;
        // トップレフト規則（D3D）: area > 0 は画面（y 下向き）で時計回り。上辺 = 水平で右向き（dy == 0, dx > 0）、左辺 = 上向き（dy < 0）。
        const i64 dx = x1 - x0, dy = y1 - y0;
        e.tl = (dy < 0) || (dy == 0 && dx > 0);
        return e;
    };
    // area > 0 = cross(b-a, c-a) > 0。E(p) = cross(v1-v0, p-v0) は辺 a→b, b→c, c→a のどれも内側で正。
    const Edge e0 = mk(bx, by, cx, cy), e1 = mk(cx, cy, ax, ay), e2 = mk(ax, ay, bx, by);
    u64 drawn = 0;
    for (i64 py = miny; py <= maxy; ++py)
        for (i64 px = minx; px <= maxx; ++px)
        {
            const i64 sx = px * 256 + 128, sy = py * 256 + 128;   // 画素中心
            const i64 w0 = e0.A * sx + e0.B * sy + e0.C;
            const i64 w1 = e1.A * sx + e1.B * sy + e1.C;
            const i64 w2 = e2.A * sx + e2.B * sy + e2.C;
            auto inside = [](i64 w, const Edge& e) { return w > 0 || (w == 0 && e.tl); };
            if (!(inside(w0, e0) && inside(w1, e1) && inside(w2, e2))) continue;
            const double d = (static_cast<double>(w0) * za + static_cast<double>(w1) * zb + static_cast<double>(w2) * zc) / static_cast<double>(area);
            const size_t idx = static_cast<size_t>(py) * fb.W + static_cast<size_t>(px);
            if (static_cast<float>(d) < fb.z[idx])
            {
                fb.z[idx] = static_cast<float>(d);
                if (writeId) fb.id[idx] = idv;
                else fb.id[idx] = idv;      // 参照（ID を持たない）はインスタンス番号 + 1 を入れる（シルエット境界の判定用）
                ++drawn;
            }
        }
    return drawn;
}

void ClipOf(const float world[16], const float vp[16], const float p[3], float clip[4])
{
    // GPU と同じ: world = 3x4（TransformPoint34）→ clip = [world, 1] * VP（行ベクトル）
    float rows[12];
    PackWorld34(world, rows);
    float w[3];
    TransformPoint34(rows, p, w);
    for (int j = 0; j < 4; ++j) clip[j] = w[0] * vp[0 * 4 + j] + w[1] * vp[1 * 4 + j] + w[2] * vp[2 * 4 + j] + 1.0f * vp[3 * 4 + j];
}

// 1 クラスタを描く（CPU）。slot は可視性バッファに入る値（-1 なら ID を書かない）
u64 RasterCluster(Fb& fb, const CpuAsset& a, u32 page, u32 idx, const float world[16], const float vp[16], i64 slot, u32 refTag = 0)
{
    DecodedCluster dc;
    const VgeoHeader& h = a.meta.header;
    if (!DecodeCluster(a.pages.data() + static_cast<size_t>(page) * kPageSize, idx, h.posOrigin, h.posStep, dc)) return 0;
    std::vector<ScreenVert> sv(dc.vertexCount);
    for (u32 i = 0; i < dc.vertexCount; ++i)
    {
        float clip[4];
        ClipOf(world, vp, &dc.pos[i * 3], clip);
        sv[i] = ToScreen(clip, fb.W, fb.H);
    }
    u64 n = 0;
    for (u32 t = 0; t < dc.triangleCount; ++t)
    {
        const u32 idv = slot >= 0 ? (static_cast<u32>(slot) << 7) | t : refTag;
        n += RasterTri(fb, sv[dc.tri[t * 3]], sv[dc.tri[t * 3 + 1]], sv[dc.tri[t * 3 + 2]], idv, slot >= 0);
    }
    return n;
}

// ── GPU 実行 + 読み戻し ───────────────────────────────────────────────────────
struct GpuTargets
{
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    u32 W = 0, H = 0;
    bool Init(Gpu& g, u32 w, u32 h)
    {
        W = w; H = h;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_TYPELESS; d.SampleDesc = {1, 0}; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        // 最適化クリア値は付けない（左半分だけ別の値で消すので、付けると「値が違う」警告が出る）
        if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&depth)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
        if (FAILED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)))) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        g.dev->CreateDepthStencilView(depth.Get(), &dv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return dsvHeap->GetCPUDescriptorHandleForHeapStart(); }
};

std::vector<u32> ReadbackTexture(Gpu& g, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, u32 w, u32 h)
{
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    g.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc = {1, 0}; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    auto* cl = g.Begin();
    const auto b0 = Trans(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto b1 = Trans(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    cl->ResourceBarrier(1, &b1);
    g.Submit();
    std::vector<u32> out(static_cast<size_t>(w) * h);
    u8* p = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
    rb->Map(0, &rr, reinterpret_cast<void**>(&p));
    for (u32 y = 0; y < h; ++y) std::memcpy(&out[static_cast<size_t>(y) * w], p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<size_t>(w) * 4);
    D3D12_RANGE nr{0, 0};
    rb->Unmap(0, &nr);
    return out;
}

struct Scene
{
    Cam cam;
    float tau = 1.0f;
    bool forceLod0 = false;
    bool useAs = false, smallPrim = true, measure = false;
    bool occluderLeftHalf = false;                     // 左半分に手前の面（NDC 深度 0.9992）を置く
    std::vector<VgInstanceInput> insts;
};

struct GpuImage
{
    std::vector<u32> vis;      // 可視性バッファ
    std::vector<float> depth;
    std::vector<std::pair<u32, u32>> visible;   // 可視クラスタ表 {instance, clusterRef}（スロット順）
    FrameStats st;
};

constexpr float kOccluderZ = 0.9992f;

GpuImage RunGpu(Gpu& g, VirtualGeometrySystem& sys, GpuTargets& tg, const Scene& sc, u32 slot = 0)
{
    // 深度を初期化（1.0。occluder のときは左半分だけ手前の値）して NON_PIXEL_SHADER_RESOURCE へ
    {
        auto* cl = g.Begin();
        const auto b0 = Trans(tg.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl->ResourceBarrier(1, &b0);
        cl->ClearDepthStencilView(tg.Dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        if (sc.occluderLeftHalf)
        {
            const D3D12_RECT r{0, 0, static_cast<LONG>(tg.W / 2), static_cast<LONG>(tg.H)};
            cl->ClearDepthStencilView(tg.Dsv(), D3D12_CLEAR_FLAG_DEPTH, kOccluderZ, 0, 1, &r);
        }
        const auto b1 = Trans(tg.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b1);
        g.Submit();
    }
    ExecuteDesc d;
    d.frameSlot = slot;
    sc.cam.VP(d.viewProj);
    std::memcpy(d.prevViewProj, d.viewProj, 64);
    d.camPos[0] = sc.cam.pos.x; d.camPos[1] = sc.cam.pos.y; d.camPos[2] = sc.cam.pos.z;
    d.zNear = sc.cam.zn;
    d.projScale = sc.cam.projScale();
    d.vpX = 0; d.vpY = 0; d.vpW = sc.cam.W; d.vpH = sc.cam.H;
    d.instances = sc.insts.data();
    d.instanceCount = static_cast<u32>(sc.insts.size());
    d.settings.tauPx = sc.tau;
    d.settings.hzbCulling = false;
    d.settings.forceLod0 = sc.forceLod0;
    d.dumpVisible = true;
    d.raster.enabled = true;
    d.raster.width = tg.W; d.raster.height = tg.H;
    d.raster.depth = tg.depth.Get();
    d.raster.dsv = tg.Dsv();
    ID3D12Resource* depthRes = tg.depth.Get();
    d.raster.depthToRaster = [depthRes](ID3D12GraphicsCommandList* c)
    { const auto b = Trans(depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE); c->ResourceBarrier(1, &b); };
    d.raster.depthToSample = [depthRes](ID3D12GraphicsCommandList* c)
    { const auto b = Trans(depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); c->ResourceBarrier(1, &b); };
    d.raster.useAs = sc.useAs;
    d.raster.smallPrimCull = sc.smallPrim;
    d.raster.measure = sc.measure;
    d.raster.edgeHist = sc.measure;
    d.cmd = g.Begin();
    const bool ok = sys.Execute(d);
    g.Submit();
    CHECK(ok);
    sys.CollectStats(slot);
    GpuImage o;
    o.st = sys.GetStats();
    o.visible = sys.GetDump();
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    o.vis = ReadbackTexture(g, sys.VisibilityBuffer(), rd, tg.W, tg.H);
    const std::vector<u32> zb = ReadbackTexture(g, tg.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, tg.W, tg.H);
    o.depth.resize(zb.size());
    std::memcpy(o.depth.data(), zb.data(), zb.size() * 4);
    return o;
}

// CPU: GPU が出した可視クラスタ表を、同じ行列で描く
Fb CpuFromVisible(const GpuImage& gi, const Scene& sc, const std::vector<const CpuAsset*>& assets, u32 W, u32 H)
{
    Fb fb;
    fb.Init(W, H);
    if (sc.occluderLeftHalf)
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W / 2; ++x) fb.z[static_cast<size_t>(y) * W + x] = kOccluderZ;
    float vp[16];
    sc.cam.VP(vp);
    for (size_t slot = 0; slot < gi.visible.size(); ++slot)
    {
        const auto& [inst, ref] = gi.visible[slot];
        const VgInstanceInput& in = sc.insts[inst];
        RasterCluster(fb, *assets[in.assetId], ClusterRefPage(ref), ClusterRefIndex(ref), in.world, vp, static_cast<i64>(slot));
    }
    return fb;
}

// CPU: 全 LOD0 を素朴に描く参照（VG 経路を通らない。深度だけ / 被覆だけ）
Fb CpuLod0Reference(const Scene& sc, const std::vector<const CpuAsset*>& assets, u32 W, u32 H)
{
    Fb fb;
    fb.Init(W, H);
    if (sc.occluderLeftHalf)
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W / 2; ++x) fb.z[static_cast<size_t>(y) * W + x] = kOccluderZ;
    float vp[16];
    sc.cam.VP(vp);
    u32 tag = 0;
    for (const VgInstanceInput& in : sc.insts)
    {
        ++tag;
        const CpuAsset& a = *assets[in.assetId];
        for (u32 p = 0; p < a.meta.header.pageCount; ++p)
        {
            const PageHeader ph = ReadPageHeader(a.pages.data() + static_cast<size_t>(p) * kPageSize);
            for (u32 k = 0; k < ph.clusterCount; ++k)
            {
                const ClusterHeader ch = a.Cluster(p, k);
                if (ch.flags & kClusterFlagLod0) RasterCluster(fb, a, p, k, in.world, vp, -1, tag);
            }
        }
    }
    return fb;
}

struct Diff
{
    size_t covered = 0, bothEmpty = 0, idEqual = 0, idDiffTie = 0, idDiffHard = 0, coverOnly = 0;
    double maxDepthDiff = 0.0;
};

Diff CompareId(const GpuImage& gi, const Fb& cpu)
{
    Diff d;
    const size_t n = cpu.id.size();
    for (size_t i = 0; i < n; ++i)
    {
        const u32 gv = gi.vis[i], cv = cpu.id[i];
        const bool ge = gv == 0xFFFFFFFFu, ce = cv == 0xFFFFFFFFu;
        if (ge && ce) { ++d.bothEmpty; continue; }
        if (ge != ce) { ++d.coverOnly; continue; }
        ++d.covered;
        const double dz = std::fabs(static_cast<double>(gi.depth[i]) - static_cast<double>(cpu.z[i]));
        d.maxDepthDiff = std::max(d.maxDepthDiff, dz);
        if (gv == cv) ++d.idEqual;
        else if (dz < 2.0e-6) ++d.idDiffTie;          // 同じ深度（共有辺・重なった面）で先着が入れ替わった
        else ++d.idDiffHard;
    }
    return d;
}

// 参照のシルエット境界からの距離（チェビシェフ）で「境界帯」を作る: 参照の被覆マスクを r 画素だけ膨張 / 収縮した物と比べる
std::vector<u8> Dilate(const std::vector<u8>& m, u32 W, u32 H, int r, bool erode)
{
    std::vector<u8> o(m.size());
    for (int y = 0; y < static_cast<int>(H); ++y)
        for (int x = 0; x < static_cast<int>(W); ++x)
        {
            u8 v = erode ? 1 : 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx)
                {
                    const int xx = x + dx, yy = y + dy;
                    const u8 s = (xx < 0 || yy < 0 || xx >= static_cast<int>(W) || yy >= static_cast<int>(H)) ? static_cast<u8>(erode ? 1 : 0) : m[static_cast<size_t>(yy) * W + xx];
                    if (erode) v &= s; else v |= s;
                }
            o[static_cast<size_t>(y) * W + x] = v;
        }
    return o;
}

struct HoleReport
{
    size_t refCovered = 0, vgCovered = 0;
    size_t holes = 0, extras = 0;             // 境界帯の外側の穴 / はみ出し（0 であること）
    std::vector<size_t> holePixels;           // 穴の画素番号（診断用）
    size_t holesBand = 0, extrasBand = 0;     // 境界帯（1.5 px）の中の差（LOD の誤差で説明できる分）
    double p50 = 0, p99 = 0, pmax = 0;        // 両方が覆う画素の深度差（NDC）
};

HoleReport CompareCoverage(const std::vector<float>& vgDepth, const std::vector<u32>& vgVis, const Fb& ref, u32 W, u32 H, bool useVisForCover, double bandPx)
{
    HoleReport r;
    const size_t n = static_cast<size_t>(W) * H;
    std::vector<u8> refM(n), vgM(n);
    // 被覆 = 「何か VG の面が描かれた」: ref は id != empty、GPU は vis != empty
    for (size_t i = 0; i < n; ++i)
    {
        refM[i] = ref.id[i] != 0xFFFFFFFFu;
        vgM[i] = useVisForCover ? (vgVis[i] != 0xFFFFFFFFu) : 0;
        r.refCovered += refM[i]; r.vgCovered += vgM[i];
    }
    const int band = static_cast<int>(std::ceil(bandPx));
    const std::vector<u8> refDil = Dilate(refM, W, H, band, false);
    // 収縮は「窓の中が全部同じインスタンスの面」で判定する（別のインスタンスと画面上で接している所は、それぞれのシルエットの境界 =
    // LOD の誤差で 1 px 隙間が空きうる。和集合のマスクだけで見ると内側に見えて、穴と誤判定する）
    std::vector<u8> refEro(n);
    for (int y = 0; y < static_cast<int>(H); ++y)
        for (int x = 0; x < static_cast<int>(W); ++x)
        {
            const u32 c = ref.id[static_cast<size_t>(y) * W + x];
            u8 v = (c != 0xFFFFFFFFu) ? 1 : 0;
            for (int dy = -band; dy <= band && v; ++dy)
                for (int dx = -band; dx <= band; ++dx)
                {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= static_cast<int>(W) || yy >= static_cast<int>(H)) continue;   // 画面の外は境界にしない
                    if (ref.id[static_cast<size_t>(yy) * W + xx] != c) { v = 0; break; }
                }
            refEro[static_cast<size_t>(y) * W + x] = v;
        }
    std::vector<double> dz;
    for (size_t i = 0; i < n; ++i)
    {
        if (refM[i] && !vgM[i]) { if (refEro[i]) { ++r.holes; r.holePixels.push_back(i); } else ++r.holesBand; }
        else if (!refM[i] && vgM[i]) { if (!refDil[i]) ++r.extras; else ++r.extrasBand; }
        else if (refM[i] && vgM[i]) dz.push_back(std::fabs(static_cast<double>(vgDepth[i]) - static_cast<double>(ref.z[i])));
    }
    if (!dz.empty())
    {
        std::sort(dz.begin(), dz.end());
        r.p50 = dz[dz.size() / 2]; r.p99 = dz[dz.size() * 99 / 100]; r.pmax = dz.back();
    }
    return r;
}

std::vector<VgInstanceInput> RandomInstances(std::mt19937& rng, u32 count, const std::vector<u32>& assetIds, float extent)
{
    std::uniform_real_distribution<float> pos(-extent, extent), ang(0.0f, 6.2831853f), sc(0.6f, 1.6f);
    std::vector<VgInstanceInput> v;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 a = assetIds[rng() % assetIds.size()];
        float sx = sc(rng), sy = sx, sz = sx;
        if (i % 5 == 3) { sx = sc(rng); sy = sc(rng); sz = sc(rng); }              // 非一様スケール
        if (i % 7 == 6) sx = -sx;                                                  // ミラー
        v.push_back(MakeInst(a, pos(rng), pos(rng) * 0.4f, pos(rng), ang(rng), ang(rng) * 0.3f, sx, sy, sz, i));
    }
    return v;
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Gpu g;
    if (!g.Init())
    {
        std::printf("SKIP: 利用できる D3D12 ハードウェアデバイスが無い（CI 想定）\n");
        return 0;
    }
    std::wprintf(L"GPU: %s\n", g.adapterName.c_str());

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
        if (initErr.find("6.6") != std::string::npos || initErr.find("Tier") != std::string::npos)
        {
            std::printf("SKIP: この GPU は VG の要件を満たさない（%s）\n", initErr.c_str());
            return 0;
        }
        std::printf("FAIL: VirtualGeometrySystem の初期化に失敗: %s\n", initErr.c_str());
        return 1;
    }
    if (!sys.RasterSupported())
    {
        std::printf("SKIP: この GPU / ドライバはメッシュシェーダのラスタを使えない（MeshShaderTier なし、または DX12_DISABLE_MESHSHADER=1）\n");
        return 0;
    }

    TestAsset blob, torus, knot;
    CHECK(MakeAsset("blob", 60000, 1, blob));
    CHECK(MakeAsset("torus", 40000, 2, torus));
    CHECK(MakeAsset("knot", 50000, 3, knot));
    {
        LoadError le;
        for (TestAsset* a : {&blob, &torus, &knot})
        {
            MemorySource ms(a->bytes);
            const bool ok = sys.LoadAssetFromSource(ms, a->name, &a->id, &le);
            if (!ok) std::printf("読込失敗 %s: %s\n", a->name.c_str(), le.ToString().c_str());
            CHECK(ok);
        }
    }
    std::vector<const CpuAsset*> assets(3, nullptr);
    assets[blob.id] = &blob.cpu; assets[torus.id] = &torus.cpu; assets[knot.id] = &knot.cpu;
    const std::vector<u32> ids = {blob.id, torus.id, knot.id};

    const u32 W = 512, H = 288;
    GpuTargets tg;
    CHECK(tg.Init(g, W, H));

    // ── (a) GPU の可視クラスタ表を CPU が描いた画像 = GPU の画像 ─────────────────────
    {
        std::mt19937 rng(20260930);
        const float dists[] = {9.0f, 18.0f, 45.0f, 140.0f};
        const float taus[] = {0.5f, 1.0f, 3.0f};
        int n = 0;
        size_t pixCovered = 0, pixEqual = 0, pixTie = 0, pixHard = 0, pixCoverOnly = 0;
        double worstDz = 0.0;
        for (float dist : dists)
            for (float tau : taus)
                for (int variant = 0; variant < 4; ++variant)
                {
                    Scene sc;
                    const float yaw = 0.9f * static_cast<float>(n);
                    sc.cam.pos = {dist * std::sin(yaw), 0.25f * dist, -dist * std::cos(yaw)};
                    sc.tau = tau;
                    sc.useAs = (variant & 1) != 0;
                    sc.smallPrim = (variant & 2) == 0;
                    sc.occluderLeftHalf = (n % 3 == 2);
                    sc.insts = RandomInstances(rng, (n % 2) ? 5 : 2, ids, 4.0f);
                    const GpuImage gi = RunGpu(g, sys, tg, sc, static_cast<u32>(n % 3));
                    CHECK(gi.st.rasterActive);
                    const Fb cpu = CpuFromVisible(gi, sc, assets, W, H);
                    const Diff d = CompareId(gi, cpu);
                    const size_t total = d.covered + d.coverOnly;
                    // 縁の画素中心（固定小数点の丸め差）だけ被覆が食い違うことを許す。ID の食い違い（深度が違う）はほぼ 0
                    const bool okCover = d.coverOnly <= std::max<size_t>(4, total / 500);
                    const bool okId = d.idDiffHard <= std::max<size_t>(4, total / 2000);
                    if (!okCover || !okId)
                        std::printf("  [a n=%d d=%.0f tau=%.1f as=%d sp=%d occ=%d] 被覆 %zu / 一致 %zu / 同深度入替 %zu / ID 不一致 %zu / 被覆のみ片側 %zu / 最大深度差 %.2e / 可視 %zu クラスタ\n",
                                    n, dist, tau, sc.useAs ? 1 : 0, sc.smallPrim ? 1 : 0, sc.occluderLeftHalf ? 1 : 0, d.covered, d.idEqual, d.idDiffTie, d.idDiffHard, d.coverOnly, d.maxDepthDiff, gi.visible.size());
                    CHECK(okCover);
                    CHECK(okId);
                    CHECK(d.maxDepthDiff < 1.0e-5);
                    CHECK(total > 0 || gi.visible.empty());
                    pixCovered += d.covered; pixEqual += d.idEqual; pixTie += d.idDiffTie; pixHard += d.idDiffHard; pixCoverOnly += d.coverOnly;
                    worstDz = std::max(worstDz, d.maxDepthDiff);
                    ++n;
                }
        std::printf("(a) GPU 画像 = CPU ラスタ: %d ケース / 被覆 %zu 画素 / ID 一致 %zu / 同深度の入替 %zu / ID 不一致 %zu / 被覆が片側だけ %zu（縁）/ 最大深度差 %.2e\n",
                    n, pixCovered, pixEqual, pixTie, pixHard, pixCoverOnly, worstDz);
    }

    // ── (a2) P2 の CPU 参照（VgCullReference.h。GPU のカリングを通らない）+ CPU ラスタ = GPU の画像（被覆 / 深度）──────────
    //   (a) は GPU が出した可視クラスタ表を CPU が描いた。ここは可視クラスタ表そのものも CPU（HZB なし）で作る = カリングからラスタまで全部が独立した参照。
    {
        std::mt19937 rng(424242);
        size_t cases = 0, cov = 0, coverOnly = 0, hard = 0;
        double worst = 0.0;
        for (float dist : {9.0f, 20.0f, 60.0f})
            for (float tau : {0.5f, 1.0f, 3.0f})
                for (int rep = 0; rep < 2; ++rep)
                {
                    Scene sc;
                    const float yaw = 0.8f * static_cast<float>(cases);
                    sc.cam.pos = {dist * std::sin(yaw), 0.3f * dist, -dist * std::cos(yaw)};
                    sc.tau = tau;
                    sc.useAs = (rep == 1);
                    sc.smallPrim = false;
                    sc.occluderLeftHalf = (cases % 4 == 3);
                    sc.insts = RandomInstances(rng, 4, ids, 4.0f);
                    const GpuImage gi = RunGpu(g, sys, tg, sc, static_cast<u32>(cases % 3));
                    // CPU 参照のカリング（LOD 選択 + 錐台 + 法線コーン。HZB なし）
                    RefView rv;
                    sc.cam.VP(rv.viewProj);
                    std::memcpy(rv.prevViewProj, rv.viewProj, 64);
                    rv.camPos[0] = sc.cam.pos.x; rv.camPos[1] = sc.cam.pos.y; rv.camPos[2] = sc.cam.pos.z;
                    rv.zNear = sc.cam.zn; rv.projScale = sc.cam.projScale();
                    rv.vpX = 0; rv.vpY = 0; rv.vpW = sc.cam.W; rv.vpH = sc.cam.H;
                    rv.tau = std::min(std::max(sc.tau, 0.05f), 64.0f);
                    rv.instanceMinPx = 0.5f;
                    rv.coneCulling = true;
                    const RefResult rr = CullReference(assets, sc.insts, rv);
                    GpuImage cpuSet;
                    cpuSet.visible.assign(rr.visible.begin(), rr.visible.end());
                    const Fb cpu = CpuFromVisible(cpuSet, sc, assets, W, H);
                    const Diff d = CompareId(gi, cpu);     // スロット番号は違うので、被覆と深度だけを見る（ID の不一致は数えない）
                    size_t hardDepth = 0;
                    for (size_t i = 0; i < cpu.id.size(); ++i)
                        if (gi.vis[i] != 0xFFFFFFFFu && cpu.id[i] != 0xFFFFFFFFu && std::fabs(static_cast<double>(gi.depth[i]) - static_cast<double>(cpu.z[i])) > 1.0e-5) ++hardDepth;
                    const size_t total = d.covered + d.coverOnly;
                    if (d.coverOnly > std::max<size_t>(8, total / 500) || hardDepth > std::max<size_t>(4, total / 2000))
                        std::printf("  [a2 d=%.0f tau=%.1f as=%d occ=%d] 可視 GPU %zu / CPU %zu クラスタ / 被覆 %zu / 片側だけ %zu / 深度差 > 1e-5 %zu\n", dist, tau, sc.useAs ? 1 : 0, sc.occluderLeftHalf ? 1 : 0, gi.visible.size(), rr.visible.size(), d.covered, d.coverOnly, hardDepth);
                    CHECK(d.coverOnly <= std::max<size_t>(8, total / 500));
                    CHECK(hardDepth <= std::max<size_t>(4, total / 2000));
                    cov += d.covered; coverOnly += d.coverOnly; hard += hardDepth;
                    worst = std::max(worst, d.maxDepthDiff);
                    ++cases;
                }
        std::printf("(a2) CPU 参照のカリング + CPU ラスタ = GPU の画像: %zu ケース / 被覆 %zu 画素 / 被覆が片側だけ %zu / 深度差 > 1e-5 が %zu / 最大深度差 %.2e\n", cases, cov, coverOnly, hard, worst);
    }

    // ── (b) 全 LOD0 を素朴に描いた参照との比較 ───────────────────────────────────
    {
        std::mt19937 rng(777);
        size_t cases = 0, holesTotal = 0, extrasTotal = 0, bandTotal = 0;
        double worstP99 = 0.0;
        const float dists[] = {10.0f, 28.0f, 90.0f};
        for (float dist : dists)
            for (int k = 0; k < 3; ++k)
            {
                Scene sc;
                const float yaw = 1.1f * static_cast<float>(cases);
                sc.cam.pos = {dist * std::sin(yaw), 0.3f * dist, -dist * std::cos(yaw)};
                sc.insts = RandomInstances(rng, 3, ids, 3.0f);
                sc.occluderLeftHalf = (k == 2);
                const Fb ref = CpuLod0Reference(sc, assets, W, H);

                // (b1) forceLod0 の GPU = 参照（VG の選択 / カリング / デコード / ラスタ全部を通した物が素朴な LOD0 と一致する）
                Scene s0 = sc; s0.forceLod0 = true;
                const GpuImage g0 = RunGpu(g, sys, tg, s0, static_cast<u32>(cases % 3));
                CHECK(!g0.st.overflow);
                const HoleReport r0 = CompareCoverage(g0.depth, g0.vis, ref, W, H, true, 1.5);
                const size_t edge0 = r0.holesBand + r0.extrasBand;
                std::printf("  (b1 d=%.0f k=%d) LOD0 tri %llu / 可視 %zu クラスタ / 参照被覆 %zu / VG 被覆 %zu / 穴 %zu / はみ出し %zu / 縁の差 %zu / 深度差 p50 %.1e p99 %.1e max %.1e\n",
                            dist, k, static_cast<unsigned long long>(g0.st.trianglesDrawn), g0.visible.size(), r0.refCovered, r0.vgCovered, r0.holes, r0.extras, edge0, r0.p50, r0.p99, r0.pmax);
                CHECK(r0.holes == 0 && r0.extras == 0);
                CHECK(edge0 <= std::max<size_t>(8, r0.refCovered / 500));
                CHECK(r0.p99 < 2.0e-5);

                // (b2) τ = 1 px の GPU = 参照（LOD の誤差 1 px の範囲でしか違わない）
                for (float tau : {1.0f, 4.0f})
                {
                    Scene s1 = sc; s1.tau = tau; s1.useAs = (k == 1); s1.smallPrim = true;
                    const GpuImage g1 = RunGpu(g, sys, tg, s1, static_cast<u32>((cases + 1) % 3));
                    const double band = 1.5 + tau;                  // 誤差 τ px + 縁の 1.5 px
                    const HoleReport r1 = CompareCoverage(g1.depth, g1.vis, ref, W, H, true, band);
                    std::printf("  (b2 d=%.0f k=%d tau=%.0f) tri %llu (LOD0 の %.1f%%) / 可視 %zu クラスタ / 穴 %zu / はみ出し %zu / 縁の差 %zu / 深度差 p50 %.1e p99 %.1e\n",
                                dist, k, tau, static_cast<unsigned long long>(g1.st.trianglesDrawn),
                                g0.st.trianglesDrawn ? 100.0 * static_cast<double>(g1.st.trianglesDrawn) / static_cast<double>(g0.st.trianglesDrawn) : 0.0,
                                g1.visible.size(), r1.holes, r1.extras, r1.holesBand + r1.extrasBand, r1.p50, r1.p99);
                    for (size_t hp : r1.holePixels)
                    {
                        const int hx = static_cast<int>(hp % W), hy = static_cast<int>(hp / W);
                        std::printf("    穴 (%d,%d): 周囲の可視性 ", hx, hy);
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const u32 v = g1.vis[static_cast<size_t>(hy + dy) * W + (hx + dx)];
                                if (v == 0xFFFFFFFFu) { std::printf("[空] "); continue; }
                                const auto& [ins, ref2] = g1.visible[v >> 7];
                                const ClusterHeader ch = assets[s1.insts[ins].assetId]->Cluster(ClusterRefPage(ref2), ClusterRefIndex(ref2));
                                std::printf("[i%u p%u c%u L%u t%u] ", ins, ClusterRefPage(ref2), ClusterRefIndex(ref2), ClusterLevel(ch), v & 127u);
                            }
                        std::printf("\n");
                    }
                    CHECK(r1.holes == 0 && r1.extras == 0);
                    holesTotal += r1.holes; extrasTotal += r1.extras; bandTotal += r1.holesBand + r1.extrasBand;
                    worstP99 = std::max(worstP99, r1.p99);
                }
                ++cases;
            }
        std::printf("(b) 全 LOD0 の素朴な参照との比較: %zu 姿勢 / 穴 %zu / はみ出し %zu / 縁の帯の差 %zu 画素 / τ 掃引の最悪 p99 深度差 %.2e\n", cases, holesTotal, extrasTotal, bandTotal, worstP99);
    }

    // ── 計測モード: 断片数 >= 被覆画素数、被覆画素数 = 画像の数え直し ───────────────────
    {
        Scene sc;
        sc.cam.pos = {0, 2, -12};
        sc.insts = {MakeInst(blob.id, 0, 0, 0, 0.3f, 0.1f, 1, 1, 1, 0), MakeInst(knot.id, 3, 0, 2, 0.5f, 0, 1, 1, 1, 1)};
        sc.measure = true;
        const GpuImage gi = RunGpu(g, sys, tg, sc, 1);
        size_t cov = 0;
        for (u32 v : gi.vis) cov += (v != 0xFFFFFFFFu);
        std::printf("計測: 被覆 %llu（GPU 集計）/ %zu（読み戻しの数え直し）/ 断片 %llu / オーバードロー %.2f / 三角形出力 %llu / 小三角形カリング %llu\n",
                    static_cast<unsigned long long>(gi.st.coveredPixels), cov, static_cast<unsigned long long>(gi.st.psInvocations),
                    gi.st.coveredPixels ? static_cast<double>(gi.st.psInvocations) / static_cast<double>(gi.st.coveredPixels) : 0.0,
                    static_cast<unsigned long long>(gi.st.msTrisOut), static_cast<unsigned long long>(gi.st.msPrimCulled));
        CHECK(gi.st.coveredPixels == cov);
        CHECK(gi.st.psInvocations >= gi.st.coveredPixels);
        CHECK(gi.st.msTrisOut == gi.st.trianglesDrawn);
        u64 edgeSum = 0;
        for (u32 i = 0; i < VG_EDGE_BINS; ++i) edgeSum += gi.st.edgeClusters[i];
        CHECK(edgeSum == gi.st.visibleClusters);
    }

    // ── 空のシーン / 全カリング: 可視性バッファは全部空 ───────────────────────────
    {
        Scene sc;
        sc.cam.pos = {0, 0, -20};
        const GpuImage gi = RunGpu(g, sys, tg, sc, 0);
        size_t cov = 0;
        for (u32 v : gi.vis) cov += (v != 0xFFFFFFFFu);
        CHECK(cov == 0 && gi.visible.empty());
        for (float z : gi.depth) if (z != 1.0f) { CHECK(false); break; }
    }

    // ── 二相ラスタ + HZB: 遮蔽物で可視が減るだけ / 最終 HZB の再構築コールバックが呼ばれる ───────
    {
        // HZB は CPU の値で作る必要があるので、ここでは「buildBetweenPhases / buildFinal が正しい順序で 1 回ずつ呼ばれる」ことと
        // ラスタの二相（一相目 + 二相目）が全可視クラスタを 1 回ずつ描くこと（rasterClusters == visibleClusters）を確かめる。
        Scene sc;
        sc.cam.pos = {0, 2, -14};
        sc.insts = {MakeInst(blob.id, 0, 0, 0, 0.3f, 0.1f, 1, 1, 1, 0), MakeInst(torus.id, 1, 0, 3, 0.5f, 0, 1, 1, 1, 1)};
        const GpuImage gi = RunGpu(g, sys, tg, sc, 2);
        CHECK(gi.st.rasterClusters == gi.st.visibleClusters);
        CHECK(gi.st.visibleClusters == gi.visible.size());
        CHECK(gi.st.rasterActive);
    }

    const FrameStats& fs = sys.GetStats();
    std::printf("最後のフレーム: カリング %.3f ms / ラスタ %.3f ms / 全体 %.3f ms\n", fs.cullGpuMs, fs.rasterGpuMs, fs.executeGpuMs);
    sys.Shutdown();

    if (g.info)
    {
        UINT64 bad = 0;
        const UINT64 nmsg = g.info->GetNumStoredMessages();
        for (UINT64 i = 0; i < nmsg; ++i)
        {
            SIZE_T len = 0;
            g.info->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            // ID 820 / 821 = 「クリア値が最適化クリア値と違う」（性能の警告。左半分だけ別の深度で消すテストの都合）は数えない
            if (SUCCEEDED(g.info->GetMessage(i, m, &len)) && (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) && m->ID != 820 && m->ID != 821)
            {
                ++bad;
                if (bad <= 10) std::printf("D3D12 メッセージ(sev=%d id=%d): %s\n", static_cast<int>(m->Severity), static_cast<int>(m->ID), m->pDescription);
            }
        }
        std::printf("D3D12 デバッグレイヤ: 保存メッセージ %llu 件 / エラー・警告 %llu 件\n", static_cast<unsigned long long>(nmsg), static_cast<unsigned long long>(bad));
        CHECK(bad == 0);
    }
    std::printf("VgRasterGpuTests: %d チェック / 失敗 %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
