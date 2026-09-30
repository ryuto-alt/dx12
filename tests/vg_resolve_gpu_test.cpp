// 仮想ジオメトリ P4: 可視性バッファからの属性復元（resolve / G-Buffer の土台）と決定論の検証。
//
//   窓なしの D3D12 デバイスで VG のカリング + ラスタを回し、次を CPU の独立な参照（double）と画素単位で突き合わせる。
//   ・(a) 重心座標と 1 画素差分（VgVisShade.hlsli の VgBarycentrics。2D 同次座標の逆行列）:
//         参照 = 画素中心のレイと三角形の平面の交点（Möller–Trumbore を平面の交点として使う）から求めた重心座標。
//         x + 1 / y + 1 の画素でも同じ交点を取り、GPU の「λ + 差分」と比べる（= HW の ddx_fine 相当が正しいこと）。
//   ・(b) UV と UV の 1 画素差分（SampleGrad に渡す勾配）: 参照の重心座標で .vgeo の頂点 UV を補間した値と差分。
//   ・(c) G-Buffer + 速度（VgGBuffer.hlsl）: 前フレームの world をずらしたインスタンスの速度 / 頂点法線の補間（裏面は反転）/
//         roughness・metallic（材質表・インスタンスの上書き・8bit 量子化）。
//   ・(d) 決定論（stableOrder）: 同じ入力を 2 回描いて可視性バッファがビット一致・可視リストが {instance, clusterRef} の昇順。
//   ・(e) 両面材質（VG_MAT_DOUBLE_SIDED）の材質を登録すると、K2 の法線コーン棄却が 0 件になる。
//   ・D3D12 デバッグレイヤ（DX12E_TEST_D3D_DEBUG=1 / GBV = DX12E_TEST_D3D_GBV=1）で警告・エラー 0。
//
//   GPU が無い / SM 6.6 + Tier 3 が無い / メッシュシェーダが無い環境は SKIP で 0 終了。
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
    float W = 384, H = 216;
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

VgInstanceInput MakeInst(u32 assetId, float px, float py, float pz, float yaw, float pitch, float s, u32 entity)
{
    using namespace DirectX;
    VgInstanceInput in;
    const XMMATRIX m = XMMatrixScaling(s, s, s) * XMMatrixRotationRollPitchYaw(pitch, yaw, 0) * XMMatrixTranslation(px, py, pz);
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);
    std::memcpy(in.world, &f, 64);
    std::memcpy(in.prevWorld, &f, 64);
    in.hasPrev = false;
    in.assetId = assetId;
    in.entityId = entity;
    return in;
}

// ── レンダーターゲット（テスト用。RTV は非シェーダ可視ヒープ）──────────────────────
struct Rt
{
    ComPtr<ID3D12Resource> tex;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    u32 W = 0, H = 0, bpp = 0;
    bool Init(Gpu& g, u32 w, u32 h, DXGI_FORMAT f, u32 bytesPerPixel, float clearV)
    {
        W = w; H = h; fmt = f; bpp = bytesPerPixel;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = f; d.SampleDesc = {1, 0}; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // 最適化クリア値 = テストが消す値（-2 / -7）。無いとクリアのたびに id=820 の警告になる
        D3D12_CLEAR_VALUE cv{};
        cv.Format = f;
        cv.Color[0] = cv.Color[1] = cv.Color[2] = cv.Color[3] = clearV;
        if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&tex)))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        if (FAILED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)))) return false;
        g.dev->CreateRenderTargetView(tex.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv() const { return rtvHeap->GetCPUDescriptorHandleForHeapStart(); }
    void Clear(ID3D12GraphicsCommandList* cl, float v) const
    {
        const float c[4] = {v, v, v, v};
        cl->ClearRenderTargetView(Rtv(), c, 0, nullptr);
    }
};

struct DepthTarget
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
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
        if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&depth)))) return false;
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

// テクスチャを読み戻す（bpp バイト / 画素）
std::vector<u8> Readback(Gpu& g, ID3D12Resource* tex, D3D12_RESOURCE_STATES state, u32 w, u32 h, u32 bpp)
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
    std::vector<u8> out(static_cast<size_t>(w) * h * bpp);
    u8* p = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(total)};
    rb->Map(0, &rr, reinterpret_cast<void**>(&p));
    for (u32 y = 0; y < h; ++y) std::memcpy(&out[static_cast<size_t>(y) * w * bpp], p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<size_t>(w) * bpp);
    D3D12_RANGE nr{0, 0};
    rb->Unmap(0, &nr);
    return out;
}

float HalfToFloat(u16 h)
{
    const u32 s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? std::nanf("") : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
    return s ? -v : v;
}

// ── 1 フレーム実行（カリング + ラスタ）──────────────────────────────────────
struct Frame
{
    std::vector<u32> vis;
    std::vector<std::pair<u32, u32>> visible;
    FrameStats st;
};

struct Scene
{
    Cam cam;
    float tau = 1.0f;
    bool stable = false;
    bool cone = true;
    std::vector<VgInstanceInput> insts;
    float prevVP[16]{};      // 速度用の前フレーム VP（ジッタ無し）。0 のままなら今と同じ
    bool hasPrevVP = false;
};

bool ExecuteFrame(VirtualGeometrySystem& sys, DepthTarget& dt, const Scene& sc, u32 slot, ID3D12GraphicsCommandList* cl)
{
    {
        const auto b0 = Trans(dt.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl->ResourceBarrier(1, &b0);
        cl->ClearDepthStencilView(dt.Dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const auto b1 = Trans(dt.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &b1);
    }
    ExecuteDesc d;
    d.frameSlot = slot;
    sc.cam.VP(d.viewProj);
    std::memcpy(d.prevViewProj, d.viewProj, 64);
    std::memcpy(d.viewProjNJ, d.viewProj, 64);
    std::memcpy(d.prevViewProjNJ, sc.hasPrevVP ? sc.prevVP : d.viewProj, 64);
    d.hasVelocityVp = true;
    d.camPos[0] = sc.cam.pos.x; d.camPos[1] = sc.cam.pos.y; d.camPos[2] = sc.cam.pos.z;
    d.zNear = sc.cam.zn;
    d.projScale = sc.cam.projScale();
    d.vpX = 0; d.vpY = 0; d.vpW = sc.cam.W; d.vpH = sc.cam.H;
    d.instances = sc.insts.data();
    d.instanceCount = static_cast<u32>(sc.insts.size());
    d.settings.tauPx = sc.tau;
    d.settings.hzbCulling = false;
    d.settings.coneCulling = sc.cone;
    d.settings.stableOrder = sc.stable;
    d.dumpVisible = true;
    d.raster.enabled = true;
    d.raster.width = dt.W; d.raster.height = dt.H;
    d.raster.depth = dt.depth.Get();
    d.raster.dsv = dt.Dsv();
    ID3D12Resource* depthRes = dt.depth.Get();
    d.raster.depthToRaster = [depthRes](ID3D12GraphicsCommandList* c)
    { const auto b = Trans(depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE); c->ResourceBarrier(1, &b); };
    d.raster.depthToSample = [depthRes](ID3D12GraphicsCommandList* c)
    { const auto b = Trans(depthRes, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); c->ResourceBarrier(1, &b); };
    d.cmd = cl;
    return sys.Execute(d);
}

Frame RunFrame(Gpu& g, VirtualGeometrySystem& sys, DepthTarget& dt, const Scene& sc, u32 slot = 0)
{
    auto* cl = g.Begin();
    const bool ok = ExecuteFrame(sys, dt, sc, slot, cl);
    g.Submit();
    CHECK(ok);
    sys.CollectStats(slot);
    Frame f;
    f.st = sys.GetStats();
    f.visible = sys.GetDump();
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const std::vector<u8> v = Readback(g, sys.VisibilityBuffer(), rd, dt.W, dt.H, 4);
    f.vis.resize(static_cast<size_t>(dt.W) * dt.H);
    std::memcpy(f.vis.data(), v.data(), v.size());
    return f;
}

// ── CPU 参照 ────────────────────────────────────────────────────────────────
struct Tri
{
    double w[3][3];     // ワールド位置
    double lp[3][3];    // アセット空間
    double n[3][3];     // ワールド法線（正規化済み）
    double uv[3][2];
};

bool CpuTri(const CpuAsset& a, const VgInstanceInput& in, u32 ref, u32 t, Tri& o)
{
    DecodedCluster dc;
    const VgeoHeader& h = a.meta.header;
    if (!DecodeCluster(a.pages.data() + static_cast<size_t>(ClusterRefPage(ref)) * kPageSize, ClusterRefIndex(ref), h.posOrigin, h.posStep, dc)) return false;
    if (t >= dc.triangleCount) return false;
    float rows[12];
    PackWorld34(in.world, rows);
    for (int k = 0; k < 3; ++k)
    {
        const u32 vi = dc.tri[t * 3 + k];
        float wp[3];
        TransformPoint34(rows, &dc.pos[vi * 3], wp);
        for (int c = 0; c < 3; ++c) { o.w[k][c] = wp[c]; o.lp[k][c] = dc.pos[vi * 3 + c]; }
        // 法線: mul(n, (float3x3)model) = (dot(n, row0.xyz), dot(n, row1.xyz), dot(n, row2.xyz))（rows = 4x4 の列）
        double nn[3];
        for (int c = 0; c < 3; ++c)
            nn[c] = dc.normal[vi * 3 + 0] * rows[c * 4 + 0] + dc.normal[vi * 3 + 1] * rows[c * 4 + 1] + dc.normal[vi * 3 + 2] * rows[c * 4 + 2];
        const double l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        for (int c = 0; c < 3; ++c) o.n[k][c] = nn[c] / (l > 0 ? l : 1);
        o.uv[k][0] = dc.uv[vi * 2]; o.uv[k][1] = dc.uv[vi * 2 + 1];
    }
    return true;
}

// 画素中心 (sx, sy) のレイ（逆 VP で近 / 遠平面から作る）と三角形の平面の交点の重心座標（Möller–Trumbore。内外は問わない）
bool CpuBary(const Tri& t, const double invVP[16], double sx, double sy, u32 W, u32 H, double out[3])
{
    const double nx = sx / W * 2.0 - 1.0, ny = 1.0 - sy / H * 2.0;
    auto unproj = [&](double z, double p[3])
    {
        double v[4];
        for (int j = 0; j < 4; ++j) v[j] = nx * invVP[0 * 4 + j] + ny * invVP[1 * 4 + j] + z * invVP[2 * 4 + j] + invVP[3 * 4 + j];
        for (int j = 0; j < 3; ++j) p[j] = v[j] / v[3];
    };
    double o[3], f[3];
    unproj(0.0, o);
    unproj(0.5, f);
    double dir[3] = {f[0] - o[0], f[1] - o[1], f[2] - o[2]};
    double e1[3], e2[3], s[3], pv[3], qv[3];
    for (int c = 0; c < 3; ++c) { e1[c] = t.w[1][c] - t.w[0][c]; e2[c] = t.w[2][c] - t.w[0][c]; s[c] = o[c] - t.w[0][c]; }
    auto cross = [](const double a[3], const double b[3], double r[3]) { r[0] = a[1] * b[2] - a[2] * b[1]; r[1] = a[2] * b[0] - a[0] * b[2]; r[2] = a[0] * b[1] - a[1] * b[0]; };
    auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    cross(dir, e2, pv);
    const double det = dot(e1, pv);
    if (std::fabs(det) < 1e-300) return false;
    const double u = dot(s, pv) / det;
    cross(s, e1, qv);
    const double v = dot(dir, qv) / det;
    out[0] = 1.0 - u - v; out[1] = u; out[2] = v;
    return true;
}

// VP（行優先 4x4、行ベクトル規約）の逆行列を double で（ガウス・ジョルダン。参照の精度のため float の XMMatrixInverse は使わない）
void InvertVP(const float vp[16], double inv[16])
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = (c < 4) ? static_cast<double>(vp[r * 4 + c]) : (c - 4 == r ? 1.0 : 0.0);
    for (int col = 0; col < 4; ++col)
    {
        int piv = col;
        for (int r = col + 1; r < 4; ++r) if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[piv][c]);
        const double d = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= d;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][c + 4];
}

size_t g_nonFinite = 0;   // Pct に来た NaN / Inf の数（★std::sort に NaN を渡すと比較が壊れて範囲外を読む＝即死するので除外して数える）
double Pct(std::vector<double> v, double q)
{
    const size_t n0 = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
    g_nonFinite += n0 - v.size();
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1)))];
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

    SystemDesc sd;
    sd.device = g.dev.Get();
    sd.directQueue = g.q.Get();
    sd.shaderDir = std::filesystem::path(DX12E_SHADER_DIR).wstring();
    // DX12E_VG_SHADER_DIR = .cso の置き場の差し替え（ビルド出力のスナップショットで回すとき）
    {
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, "DX12E_VG_SHADER_DIR") == 0 && v && v[0]) sd.shaderDir = std::filesystem::path(v).wstring();
        std::free(v);
    }
    if (!sd.shaderDir.empty() && sd.shaderDir.back() != L'/' && sd.shaderDir.back() != L'\\') sd.shaderDir += L'/';
    sd.frameCount = 3;
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
        std::printf("SKIP: この GPU / ドライバはメッシュシェーダのラスタを使えない\n");
        return 0;
    }

    std::printf("VG 初期化 OK\n");
    TestAsset blob, torus, sphere;
    CHECK(MakeAsset("blob", 30000, 11, blob));
    CHECK(MakeAsset("torus", 20000, 12, torus));
    CHECK(MakeAsset("sphere", 12000, 13, sphere));
    for (TestAsset* a : {&blob, &torus, &sphere})
    {
        MemorySource ms(a->bytes);
        LoadError le;
        const bool ok = sys.LoadAssetFromSource(ms, a->name, &a->id, &le);
        if (!ok) std::printf("読込失敗 %s: %s\n", a->name.c_str(), le.ToString().c_str());
        CHECK(ok);
    }
    std::vector<const CpuAsset*> assets(3, nullptr);
    assets[blob.id] = &blob.cpu; assets[torus.id] = &torus.cpu; assets[sphere.id] = &sphere.cpu;

    const u32 W = 384, H = 216;
    DepthTarget dt;
    CHECK(dt.Init(g, W, H));
    Rt raw, vel, gbuf;
    CHECK(raw.Init(g, W, H, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, -2.0f));
    CHECK(vel.Init(g, W, H, DXGI_FORMAT_R16G16_FLOAT, 4, -7.0f));
    CHECK(gbuf.Init(g, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, -7.0f));

    Scene sc;
    sc.cam.W = static_cast<float>(W); sc.cam.H = static_cast<float>(H);
    sc.cam.pos = {1.5f, 2.0f, -9.0f};
    sc.cam.target = {0.0f, 0.2f, 0.0f};
    sc.tau = 1.0f;
    sc.insts.push_back(MakeInst(blob.id, -1.6f, 0.0f, 0.0f, 0.4f, 0.1f, 1.6f, 1));
    sc.insts.push_back(MakeInst(torus.id, 1.8f, 0.3f, 0.5f, 1.1f, 0.7f, 1.8f, 2));
    sc.cone = false;   // 裏向きのクラスタも描く（裏面の画素の復元も踏む）
    // 速度: インスタンス 1 の前フレームを少しずらす / インスタンス 2 は材質の上書き
    {
        VgInstanceInput& a = sc.insts[0];
        a.hasPrev = true;
        std::memcpy(a.prevWorld, a.world, 64);
        a.prevWorld[12] -= 0.21f; a.prevWorld[13] += 0.07f; a.prevWorld[14] -= 0.13f;
        VgInstanceInput& b = sc.insts[1];
        b.overrideMetallic = 0.75f; b.overrideRoughness = 0.25f;
    }
    // 前フレームのカメラも少し動かす（速度 = カメラ + 物体）
    {
        Cam pc = sc.cam;
        pc.pos.x -= 0.05f; pc.pos.y += 0.03f;
        pc.VP(sc.prevVP);
        sc.hasPrevVP = true;
    }

    // カメラが球の内側にいるシーン: 前方は球の内面（裏面）、カメラのすぐ後ろ〜横の三角形は近平面をまたぐ（頂点の w <= 0）。
    Scene scIn = sc;
    scIn.insts.clear();
    {
        // 細分割球: 中心 = AABB の中心、半径 = AABB の半幅（★ヘッダの境界球は AABB の対角由来で √3 倍大きい）
        const VgeoHeader& hh = sphere.cpu.meta.header;
        const float cx = 0.5f * (hh.aabbMin[0] + hh.aabbMax[0]), cy = 0.5f * (hh.aabbMin[1] + hh.aabbMax[1]), cz = 0.5f * (hh.aabbMin[2] + hh.aabbMax[2]);
        const float R = 0.5f * (hh.aabbMax[0] - hh.aabbMin[0]);
        const float s = 40.0f;   // 大きく = 三角形が粗い = 最寄りの壁の三角形がカメラ面（w=0）をまたぐ
        // 最寄りの壁（距離 0.05 < 近平面 0.1）を視線から 60° 斜めの方向に置く
        // = 壁を斜めに見るので、近平面が壁を横切る（近平面クリップの三角形を必ず踏む）+ 反対側は球の奥の内壁（裏面）
        using namespace DirectX;
        const XMVECTOR fwd = XMVector3Normalize(XMVectorSet(-1.5f, -1.8f, 9.0f, 0.0f));
        const XMVECTOR side = XMVector3Normalize(XMVector3Cross(fwd, XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)));
        const XMVECTOR u = XMVector3Normalize(XMVectorAdd(XMVectorScale(fwd, 0.5f), XMVectorScale(side, 0.8660254f)));
        const float dist = R * s - 0.05f;
        XMFLOAT3 uf;
        XMStoreFloat3(&uf, u);
        scIn.insts.push_back(MakeInst(sphere.id, 1.5f - uf.x * dist - cx * s, 2.0f - uf.y * dist - cy * s, -9.0f - uf.z * dist - cz * s,
                                      0.0f, 0.0f, s, 4));
    }

    // ── (a)(b) 重心座標・1 画素差分・UV・UV 勾配 ─────────────────────────────
    auto checkRecon = [&](const Scene& scn, const char* label, bool insideSphere)
    {
        Frame fr = RunFrame(g, sys, dt, scn);
        std::printf("[%s] 可視クラスタ %zu\n", label, fr.visible.size());
        auto rawDraw = [&](u32 mode) -> std::vector<float>
        {
            auto* cl = g.Begin();
            raw.Clear(cl, -2.0f);
            DebugDrawDesc dd;
            dd.cmd = cl; dd.frameSlot = 0; dd.mode = mode; dd.rtv = raw.Rtv(); dd.rtFormat = raw.fmt; dd.width = W; dd.height = H;
            CHECK(sys.DrawDebug(dd));
            g.Submit();
            const std::vector<u8> b = Readback(g, raw.tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, W, H, 16);
            std::vector<float> f(b.size() / 4);
            std::memcpy(f.data(), b.data(), b.size());
            return f;
        };
        // DrawDebug は直近の Execute（同じスロット）の可視性バッファを読む → 3 回描く間に Execute し直さない
        const std::vector<float> bary = rawDraw(VG_DBG_RAW_BARY);
        const std::vector<float> uvg  = rawDraw(VG_DBG_RAW_UVGRAD);
        const std::vector<float> uvv  = rawDraw(VG_DBG_RAW_UV);

        float vpf[16];
        scn.cam.VP(vpf);
        double inv[16];
        InvertVP(vpf, inv);
        std::vector<double> eB, eBx, eBy, eUv, eG;
        size_t covered = 0, bad = 0, emptyOk = 0, emptyBad = 0, nearClip = 0;
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W; ++x)
            {
                const size_t i = static_cast<size_t>(y) * W + x;
                const u32 v = fr.vis[i];
                const float* gb = &bary[i * 4];
                if (v == VG_VIS_EMPTY) { if (gb[0] == -1.0f && gb[3] == -1.0f) ++emptyOk; else ++emptyBad; continue; }
                ++covered;
                const u32 slot = v >> VG_VIS_TRI_BITS, t = v & VG_VIS_TRI_MASK;
                if (slot >= fr.visible.size()) { ++bad; continue; }
                CHECK(static_cast<u32>(gb[3]) == slot);
                const auto [inst, ref] = fr.visible[slot];
                Tri tr;
                if (!CpuTri(*assets[scn.insts[inst].assetId], scn.insts[inst], ref, t, tr)) { ++bad; continue; }
                double b0[3], bx[3], by[3];
                if (!CpuBary(tr, inv, x + 0.5, y + 0.5, W, H, b0) || !CpuBary(tr, inv, x + 1.5, y + 0.5, W, H, bx) || !CpuBary(tr, inv, x + 0.5, y + 1.5, W, H, by)) { ++bad; continue; }
                // 頂点の w（近平面をまたぐ三角形かどうか）
                {
                    bool behind = false;
                    for (int k = 0; k < 3; ++k)
                    {
                        const double wv = tr.w[k][0] * vpf[3] + tr.w[k][1] * vpf[7] + tr.w[k][2] * vpf[11] + vpf[15];
                        if (wv <= 0.0) behind = true;
                    }
                    if (behind) ++nearClip;
                }
                double e = 0, ex = 0, ey = 0;
                for (int k = 0; k < 3; ++k)
                {
                    e  = std::max(e,  std::fabs(gb[k] - b0[k]));
                    // GPU の差分（x+1 の λ − λ）は RAW_BARY からは読めないので UV で見る（下）。ここは参照同士の整合だけ
                }
                eB.push_back(e);
                // UV（xy）と UV の 1 画素差分
                double uv0[2] = {0, 0}, uvx[2] = {0, 0}, uvy[2] = {0, 0};
                for (int k = 0; k < 3; ++k)
                    for (int c = 0; c < 2; ++c) { uv0[c] += b0[k] * tr.uv[k][c]; uvx[c] += bx[k] * tr.uv[k][c]; uvy[c] += by[k] * tr.uv[k][c]; }
                const double duvx[2] = {uvx[0] - uv0[0], uvx[1] - uv0[1]}, duvy[2] = {uvy[0] - uv0[0], uvy[1] - uv0[1]};
                const float* gu = &uvv[i * 4];
                const float* gg = &uvg[i * 4];
                eUv.push_back(std::max(std::fabs(gu[0] - uv0[0]), std::fabs(gu[1] - uv0[1])));
                const double scale = std::max({std::fabs(duvx[0]), std::fabs(duvx[1]), std::fabs(duvy[0]), std::fabs(duvy[1]), 1e-6});
                ex = std::max(std::fabs(gg[0] - duvx[0]), std::fabs(gg[1] - duvx[1])) / scale;
                ey = std::max(std::fabs(gg[2] - duvy[0]), std::fabs(gg[3] - duvy[1])) / scale;
                eG.push_back(std::max(ex, ey));
                eBx.push_back(ex); eBy.push_back(ey);
            }
        std::printf("(a) 重心座標: 被覆 %zu 画素（近平面をまたぐ三角形 %zu）/ 参照との最大差 p50 %.2e p99 %.2e max %.2e / 復元失敗 %zu / 空の画素 %zu（誤出力 %zu）\n",
                    covered, nearClip, Pct(eB, 0.5), Pct(eB, 0.99), Pct(eB, 1.0), bad, emptyOk, emptyBad);
        std::printf("(b) UV: 絶対差 p99 %.2e max %.2e / UV 勾配（1 画素差分）の相対差 p50 %.2e p99 %.2e p999 %.2e max %.2e\n",
                    Pct(eUv, 0.99), Pct(eUv, 1.0), Pct(eG, 0.5), Pct(eG, 0.99), Pct(eG, 0.999), Pct(eG, 1.0));
        CHECK(covered > 5000);
        if (insideSphere) CHECK(nearClip > 0);
        CHECK(bad == 0);
        CHECK(emptyBad == 0);
        CHECK(Pct(eB, 0.99) < 1e-3);
        CHECK(Pct(eB, 1.0) < 2e-2);
        CHECK(Pct(eUv, 0.99) < 1e-3);
        CHECK(Pct(eG, 0.99) < 1e-2);
        CHECK(Pct(eG, 0.999) < 5e-2);
    };
    checkRecon(sc, "通常", false);
    checkRecon(scIn, "球の内側", true);

    // ── (c) G-Buffer + 速度 ─────────────────────────────────────────────────────
    //   材質: blob = roughness 0.8 / metallic 0.1、torus = 未登録（既定 0.5 / 0）。インスタンス 2 は上書き 0.25 / 0.75。
    {
        VgMaterialGpu m{};
        m.albedoSrv = m.normalSrv = m.metalRoughSrv = m.emissiveSrv = VG_NONE;
        m.metallic = 0.1f; m.roughness = 0.8f;
        m.uvScaleOffset[0] = m.uvScaleOffset[1] = 1.0f;
        CHECK(sys.SetAssetMaterials(blob.id, &m, 1));
        CHECK(sys.AssetMaterialsBound(blob.id));
    }
    auto checkGBuffer = [&](const Scene& scn, const char* label, bool insideSphere)
    {
        Frame fr = RunFrame(g, sys, dt, scn);
        {
            auto* cl = g.Begin();
            vel.Clear(cl, -7.0f);
            gbuf.Clear(cl, -7.0f);
            GBufferDesc gd;
            gd.cmd = cl; gd.frameSlot = 0; gd.velocityRtv = vel.Rtv(); gd.gbufferRtv = gbuf.Rtv();
            CHECK(sys.DrawGBuffer(gd));
            g.Submit();
        }
        const std::vector<u8> vb = Readback(g, vel.tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, W, H, 4);
        const std::vector<u8> gbb = Readback(g, gbuf.tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, W, H, 8);
        float vpf[16];
        scn.cam.VP(vpf);
        double inv[16];
        InvertVP(vpf, inv);
        auto proj = [](const float m[16], const double p[3], double ndc[2])
        {
            double c[4];
            for (int j = 0; j < 4; ++j) c[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
            const double w = std::max(std::fabs(c[3]), 1e-6);
            ndc[0] = c[0] / w; ndc[1] = c[1] / w;
        };
        auto octEnc = [](double n[3], double o[2])
        {
            const double l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
            double x = n[0] / l1, y = n[1] / l1;
            if (n[2] < 0) { const double ox = (1 - std::fabs(y)) * (x >= 0 ? 1 : -1), oy = (1 - std::fabs(x)) * (y >= 0 ? 1 : -1); x = ox; y = oy; }
            o[0] = x; o[1] = y;
        };
        std::vector<double> eV, eN;
        size_t covered = 0, untouched = 0, touchedEmpty = 0, matBad = 0, backFaces = 0, grazingFlip = 0;
        for (u32 y = 0; y < H; ++y)
            for (u32 x = 0; x < W; ++x)
            {
                const size_t i = static_cast<size_t>(y) * W + x;
                const u16* pv = reinterpret_cast<const u16*>(&vb[i * 4]);
                const u16* pg = reinterpret_cast<const u16*>(&gbb[i * 8]);
                const float gv[2] = {HalfToFloat(pv[0]), HalfToFloat(pv[1])};
                const float gg[4] = {HalfToFloat(pg[0]), HalfToFloat(pg[1]), HalfToFloat(pg[2]), HalfToFloat(pg[3])};
                const u32 v = fr.vis[i];
                if (v == VG_VIS_EMPTY)
                {
                    // VG でない画素は discard（クリア値 -7 のまま）
                    if (gv[0] == -7.0f && gg[0] == -7.0f) ++untouched; else ++touchedEmpty;
                    continue;
                }
                ++covered;
                const auto [inst, ref] = fr.visible[v >> VG_VIS_TRI_BITS];
                const VgInstanceInput& in = scn.insts[inst];
                Tri tr;
                double b[3];
                if (!CpuTri(*assets[in.assetId], in, ref, v & VG_VIS_TRI_MASK, tr) || !CpuBary(tr, inv, x + 0.5, y + 0.5, W, H, b)) continue;
                // 速度
                double lp[3] = {0, 0, 0}, wp[3] = {0, 0, 0};
                for (int k = 0; k < 3; ++k) for (int c = 0; c < 3; ++c) { lp[c] += b[k] * tr.lp[k][c]; wp[c] += b[k] * tr.w[k][c]; }
                float rowsPrev[12];
                PackWorld34(in.hasPrev ? in.prevWorld : in.world, rowsPrev);
                double pw[3];
                for (int c = 0; c < 3; ++c) pw[c] = lp[0] * rowsPrev[c * 4 + 0] + lp[1] * rowsPrev[c * 4 + 1] + lp[2] * rowsPrev[c * 4 + 2] + rowsPrev[c * 4 + 3];
                double nc[2], np[2];
                proj(vpf, wp, nc);
                proj(scn.prevVP, pw, np);
                const double ev[2] = {(nc[0] - np[0]) * 0.5, (nc[1] - np[1]) * -0.5};
                const double mag = std::max(std::fabs(ev[0]), std::fabs(ev[1]));
                eV.push_back(std::max(std::fabs(gv[0] - ev[0]), std::fabs(gv[1] - ev[1])) / std::max(mag, 1e-3));
                // 法線（補間 → 正規化 → 裏面なら反転）
                double n[3] = {0, 0, 0};
                for (int k = 0; k < 3; ++k) for (int c = 0; c < 3; ++c) n[c] += b[k] * tr.n[k][c];
                const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (double& c : n) c /= l;
                double e1[3], e2[3], fn[3], toCam[3];
                for (int c = 0; c < 3; ++c) { e1[c] = tr.w[1][c] - tr.w[0][c]; e2[c] = tr.w[2][c] - tr.w[0][c]; }
                fn[0] = e1[1] * e2[2] - e1[2] * e2[1]; fn[1] = e1[2] * e2[0] - e1[0] * e2[2]; fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
                toCam[0] = scn.cam.pos.x - tr.w[0][0]; toCam[1] = scn.cam.pos.y - tr.w[0][1]; toCam[2] = scn.cam.pos.z - tr.w[0][2];
                const double facing = fn[0] * toCam[0] + fn[1] * toCam[1] + fn[2] * toCam[2];
                const double fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]) * std::sqrt(toCam[0] * toCam[0] + toCam[1] * toCam[1] + toCam[2] * toCam[2]);
                if (facing <= 0) { for (double& c : n) c = -c; ++backFaces; }
                double oe[2];
                octEnc(n, oe);
                const double en = std::max(std::fabs(gg[0] - oe[0]), std::fabs(gg[1] - oe[1]));
                if (fl > 0 && std::fabs(facing) / fl < 1e-3 && en > 2e-2) ++grazingFlip;   // ほぼ真横の面: 表裏の判定が float / double で割れうる
                else eN.push_back(en);
                // 粗さ / 金属（8bit 量子化 → fp16）
                float er, em;
                if (in.overrideRoughness >= 0) { er = in.overrideRoughness; em = in.overrideMetallic; }
                else if (in.assetId == blob.id) { er = 0.8f; em = 0.1f; }
                else { er = 0.5f; em = 0.0f; }
                er = std::floor(std::clamp(std::max(er, 0.04f), 0.0f, 1.0f) * 255.0f + 0.5f) / 255.0f;
                em = std::floor(std::clamp(em, 0.0f, 1.0f) * 255.0f + 0.5f) / 255.0f;
                if (std::fabs(gg[2] - er) > 1e-3f || std::fabs(gg[3] - em) > 1e-3f) ++matBad;
            }
        std::printf("[%s] ", label);
        std::printf("(c) G-Buffer: 被覆 %zu 画素（裏面 %zu / ほぼ真横で表裏が割れた %zu）/ 速度の相対差 p99 %.2e max %.2e / 法線 oct の差 p99 %.2e max %.2e / 材質の不一致 %zu / VG 以外の画素 %zu（書いてしまった %zu）\n",
                    covered, backFaces, grazingFlip, Pct(eV, 0.99), Pct(eV, 1.0), Pct(eN, 0.99), Pct(eN, 1.0), matBad, untouched, touchedEmpty);
        CHECK(grazingFlip * 1000 <= covered);
        CHECK(covered > 5000);
        CHECK(touchedEmpty == 0);
        CHECK(matBad == 0);
        CHECK(Pct(eV, 0.99) < 1e-2);
        CHECK(Pct(eN, 0.99) < 2e-3);
        CHECK(Pct(eN, 1.0) < 2e-2);
        if (insideSphere) CHECK(backFaces * 2 > covered);   // 内面（裏面）が大半
    };
    checkGBuffer(sc, "通常", false);
    checkGBuffer(scIn, "球の内側", true);

    // ── (d) 決定論（stableOrder）──────────────────────────────────────────────
    {
        Scene ds = sc;
        ds.insts.clear();
        std::mt19937 rng(4242);
        std::uniform_real_distribution<float> U(-1.0f, 1.0f);
        for (u32 k = 0; k < 48; ++k)
            ds.insts.push_back(MakeInst((k & 1) ? blob.id : torus.id, U(rng) * 2.5f, U(rng) * 1.2f, U(rng) * 2.5f, U(rng) * 3.0f, U(rng), 0.9f + 0.3f * U(rng), 100 + k));
        // 同じ場所に同じインスタンスを 2 体（完全に同じ深さの面 = 先着が決まらないと入れ替わる）
        ds.insts.push_back(MakeInst(blob.id, 0.0f, 0.0f, 0.0f, 0.3f, 0.2f, 1.4f, 900));
        ds.insts.push_back(MakeInst(blob.id, 0.0f, 0.0f, 0.0f, 0.3f, 0.2f, 1.4f, 901));
        ds.stable = true;
        RunFrame(g, sys, dt, ds, 1);                      // 長さの見積もり（前回の可視数）を作る
        const Frame a = RunFrame(g, sys, dt, ds, 1);
        const Frame b = RunFrame(g, sys, dt, ds, 2);
        size_t visDiff = 0;
        for (size_t i = 0; i < a.vis.size(); ++i) visDiff += (a.vis[i] != b.vis[i]) ? 1u : 0u;
        bool sorted = true;
        const size_t n1 = a.st.visibleP1;
        for (size_t i = 1; i < a.visible.size(); ++i)
        {
            if (i == n1) continue;   // 二相目の先頭（HZB 無しなので通常は空）
            const auto& p = a.visible[i - 1];
            const auto& q = a.visible[i];
            if (p.first > q.first || (p.first == q.first && p.second >= q.second)) sorted = false;
        }
        std::printf("(d) 決定論: 可視クラスタ %zu（ソート %u / 諦めたフェーズ %u）/ 2 回の可視性バッファの差 %zu 画素 / 可視リストが昇順 %s / リスト一致 %s\n",
                    a.visible.size(), a.st.sortedClusters, a.st.sortSkipped, visDiff, sorted ? "yes" : "no", a.visible == b.visible ? "yes" : "no");
        CHECK(a.st.stableOrder);
        CHECK(a.st.sortSkipped == 0);
        CHECK(a.st.sortedClusters == a.visible.size());
        CHECK(sorted);
        CHECK(a.visible == b.visible);
        CHECK(visDiff == 0);
        // 参考: 安定化なし（InterlockedAdd の到着順）では並びが揺れうる
        ds.stable = false;
        const Frame c = RunFrame(g, sys, dt, ds, 1);
        const Frame d2 = RunFrame(g, sys, dt, ds, 2);
        size_t visDiff2 = 0;
        for (size_t i = 0; i < c.vis.size(); ++i) visDiff2 += (c.vis[i] != d2.vis[i]) ? 1u : 0u;
        std::printf("    参考（stableOrder なし）: 2 回の可視性バッファの値の差 %zu 画素 / リスト一致 %s\n", visDiff2, c.visible == d2.visible ? "yes" : "no");
    }

    // ── (e) 両面材質は法線コーンで落とさない ────────────────────────────────────
    {
        Scene es = sc;
        es.cone = true;
        VgMaterialGpu m{};
        m.albedoSrv = m.normalSrv = m.metalRoughSrv = m.emissiveSrv = VG_NONE;
        m.roughness = 0.5f; m.uvScaleOffset[0] = m.uvScaleOffset[1] = 1.0f;
        m.flags = 0;
        CHECK(sys.SetAssetMaterials(blob.id, &m, 1));
        CHECK(sys.SetAssetMaterials(torus.id, &m, 1));
        const Frame single = RunFrame(g, sys, dt, es);
        m.flags = VG_MAT_DOUBLE_SIDED;
        CHECK(sys.SetAssetMaterials(blob.id, &m, 1));
        CHECK(sys.SetAssetMaterials(torus.id, &m, 1));
        const Frame dbl = RunFrame(g, sys, dt, es);
        std::printf("(e) 法線コーンで落としたクラスタ: 片面 %u → 両面 %u / 可視 %u → %u\n",
                    single.st.clustersCulledCone, dbl.st.clustersCulledCone, single.st.visibleClusters, dbl.st.visibleClusters);
        CHECK(single.st.clustersCulledCone > 0);
        CHECK(dbl.st.clustersCulledCone == 0);
        CHECK(dbl.st.visibleClusters >= single.st.visibleClusters);
    }

    std::printf("非有限値（NaN / Inf）: %zu 件\n", g_nonFinite);
    CHECK(g_nonFinite == 0);
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
            if (SUCCEEDED(g.info->GetMessage(i, m, &len)) && (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING))
            {
                ++bad;
                if (bad <= 10) std::printf("D3D12 メッセージ(sev=%d id=%d): %s\n", static_cast<int>(m->Severity), static_cast<int>(m->ID), m->pDescription);
            }
        }
        std::printf("D3D12 デバッグレイヤ: 保存メッセージ %llu 件 / エラー・警告 %llu 件\n", static_cast<unsigned long long>(nmsg), static_cast<unsigned long long>(bad));
        CHECK(bad == 0);
    }
    std::printf("VgResolveGpuTests: %d チェック / 失敗 %d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
