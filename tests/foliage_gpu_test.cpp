// 植生 F1: GPU カリング（shaders/foliage/FoliageCull.hlsl）と CPU 参照（renderer/foliage/FoliageMath.h）の一致テスト。
//
//   窓なしの D3D12 デバイス（ハードウェアアダプタ）で compute だけを実行する。
//   GPU が無い / .cso が無い環境（CI）では「SKIP」を出して 0 で終わる（落とさない）。
//
//   ・GPU の list ごとの集合 = CPU 参照（EvaluateInstance）: 距離 / 錐台 / 間引き / LOD / クロスフェード（複数のカメラ x パラメータ）。
//     浮動小数の境界の 0.1% 以内の差は許容して数を記録する。行列 / ディザ値 / 色 / シードの中身も比べる。
//   ・影ビュー（shadowPass）/ 主ビューと同時に別ビューへ出せる / 容量超過（overflow）で落ちない・個数 = 容量に丸まる / 空 / 全カリング。
//   ・間接引数の InstanceCount = min(カウンタ, 容量)。同じ入力を 2 回 = 同じ集合（決定論）。
//   ・HZB: 手前に壁のある HZB を渡すと、CPU 参照（SphereOccludedByHiZ）と同じ集合が隠れる。壁が無ければ 1 つも隠れない。
//   ・風: FoliageWind.hlsli の変位（GPU）= FoliageMath.h の WindDelta（CPU）。
//   環境変数: DX12E_TEST_D3D_DEBUG=1 で D3D12 デバッグレイヤ（DX12E_TEST_D3D_GBV=1 で GPU ベース検証）。警告 / エラーが 1 件でもあれば失敗。
#include "renderer/foliage/FoliageCuller.h"
#include "renderer/foliage/FoliageMath.h"

#include <Windows.h>
#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::foliage;
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
    // デバッグレイヤの警告 / エラー件数（0 でなければテスト失敗）
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

ComPtr<ID3D12Resource> MakeBuf(Gpu& g, u64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = std::max<u64>(bytes, 16); rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) rd.Flags = D3D12_RESOURCE_FLAG_NONE;
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

// バッファを CPU へ読む（state = リソースの現在の状態。読み終わったら元の状態へ戻す）
std::vector<u8> ReadBack(Gpu& g, ID3D12Resource* res, u64 bytes, D3D12_RESOURCE_STATES state, u64 offset = 0)
{
    ComPtr<ID3D12Resource> rb = MakeBuf(g, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto* c = g.Begin();
    Barrier(c, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c->CopyBufferRegion(rb.Get(), 0, res, offset, bytes);
    Barrier(c, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    g.Submit();
    std::vector<u8> out(bytes);
    void* m = nullptr; D3D12_RANGE rr{0, static_cast<SIZE_T>(bytes)};
    rb->Map(0, &rr, &m);
    std::memcpy(out.data(), m, bytes);
    D3D12_RANGE none{0, 0};
    rb->Unmap(0, &none);
    return out;
}

// HZB（R32_FLOAT・全ミップ）を CPU の値で作る。NON_PIXEL_SHADER_RESOURCE 状態で返す。
struct HzbCpu { u32 w, h, mips; std::vector<std::vector<f32>> data; };
HzbCpu MakeHzbFromDepth(u32 w, u32 h, const std::vector<f32>& depth0)
{
    HzbCpu z; z.w = w; z.h = h;
    z.data.push_back(depth0);
    u32 cw = w, ch = h;
    while (cw > 1 || ch > 1)
    {
        const u32 nw = std::max(1u, cw / 2), nh = std::max(1u, ch / 2);
        std::vector<f32> nd(static_cast<size_t>(nw) * nh);
        const auto& src = z.data.back();
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
            {
                f32 m = 0;
                for (u32 dy = 0; dy < 2; ++dy)
                    for (u32 dx = 0; dx < 2; ++dx)
                    {
                        const u32 sx = std::min(cw - 1, x * 2 + dx), sy = std::min(ch - 1, y * 2 + dy);
                        m = std::max(m, src[static_cast<size_t>(sy) * cw + sx]);
                    }
                nd[static_cast<size_t>(y) * nw + x] = m;
            }
        z.data.push_back(std::move(nd));
        cw = nw; ch = nh;
    }
    z.mips = static_cast<u32>(z.data.size());
    return z;
}
ComPtr<ID3D12Resource> MakeHzbTexture(Gpu& g, const HzbCpu& h)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = h.w; td.Height = h.h; td.DepthOrArraySize = 1;
    td.MipLevels = static_cast<UINT16>(h.mips); td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc = {1, 0};
    ComPtr<ID3D12Resource> tex;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(h.mips);
    std::vector<UINT> rows(h.mips); std::vector<UINT64> rowBytes(h.mips);
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&td, 0, h.mips, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    ComPtr<ID3D12Resource> up = MakeBuf(g, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    u8* m = nullptr; D3D12_RANGE none{0, 0};
    up->Map(0, &none, reinterpret_cast<void**>(&m));
    for (u32 mip = 0; mip < h.mips; ++mip)
    {
        const u32 mw = std::max(1u, h.w >> mip);
        for (u32 y = 0; y < rows[mip]; ++y)
            std::memcpy(m + fp[mip].Offset + static_cast<size_t>(y) * fp[mip].Footprint.RowPitch, &h.data[mip][static_cast<size_t>(y) * mw], mw * 4);
    }
    up->Unmap(0, nullptr);
    auto* c = g.Begin();
    for (u32 mip = 0; mip < h.mips; ++mip)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[mip];
        c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    Barrier(c, tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.Submit();
    return tex;
}

std::vector<FoliageInstance> MakeInstances(u32 n, f32 extent, u32 seed, u32 variants)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-extent, extent), s(0.6f, 1.8f), yaw(0, 6.28f), tilt(-0.3f, 0.3f);
    std::vector<FoliageInstance> v;
    v.reserve(n);
    for (u32 i = 0; i < n; ++i)
    {
        FoliageInstance in;
        in.px = u(rng); in.py = u(rng) * 0.05f; in.pz = u(rng);
        in.rotScale = PackYawScale(yaw(rng), s(rng));
        in.tilt = PackTilt(tilt(rng), tilt(rng));
        in.color = PackColor8(0.5f + 0.5f * (i % 7) / 7.0f, 1, 1, 1);
        in.seedType = PackSeedType(i & 0xFFFFFF, rng() % variants);   // seed = 通し番号（GPU の出力から元のインスタンスを引ける）
        in.params = 64 + (rng() & 63);
        v.push_back(in);
    }
    return v;
}

struct Case
{
    const char* name;
    f32 cam[3]; f32 look[3];
    f32 cullDist, lodD0, lodD1, lodD2, thinStart, lodFade;
    u32 variants; u32 lods;
    f32 layerScale;   // レイヤーの一様スケール
    f32 layerYaw;
    bool wideFrustum; // true = 全方位（錐台無効）
};

DirectX::XMFLOAT4X4 MakeVP(const f32 cam[3], const f32 look[3], f32 nearZ = 0.1f, f32 farZ = 500.0f)
{
    using namespace DirectX;
    const XMMATRIX vp = XMMatrixLookAtLH(XMVectorSet(cam[0], cam[1], cam[2], 1), XMVectorSet(look[0], look[1], look[2], 1), XMVectorSet(0, 1, 0, 0))
                      * XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.2f, 16.0f / 9.0f, nearZ, farZ);
    XMFLOAT4X4 f; XMStoreFloat4x4(&f, vp);
    return f;
}

// 期待される 1 つの出力
struct Expect { u32 slot; f32 lo, hi; u32 seed; };

void CpuExpected(const std::vector<FoliageInstance>& inst, const Affine34& layer, const CullParams& p, u32 lodStride,
                 std::map<u32, std::vector<Expect>>& bySeed, std::vector<u32>& borderline)
{
    for (const auto& in : inst)
    {
        const EmitInfo e = EvaluateInstance(in, layer, p);
        for (u32 k = 0; k < e.count; ++k)
            bySeed[SeedOf(in.seedType)].push_back({e.variant * lodStride + e.lod[k], e.lo[k], e.hi[k], SeedOf(in.seedType)});
        // 境界付近（0.1% 以内）か
        bool border = false;
        const f32 d = e.dist;
        auto isNear = [&](f32 v, f32 t) { return std::fabs(v - t) <= 1e-3f * std::max(1.0f, t); };
        if (isNear(d, p.cullDist)) border = true;
        for (int i = 0; i < 3; ++i)
        {
            if (isNear(d, p.lodDist[i])) border = true;
            if (p.lodFade > 0) { if (isNear(d, p.lodDist[i] * (1 - p.lodFade)) || isNear(d, p.lodDist[i] * (1 + p.lodFade))) border = true; }
        }
        {
            const f32 keep = ThinKeepProb(d, p.cullDist, p.thinStart);
            const f32 h = Hash01(ThinHashKey(SeedOf(in.seedType)));
            if (std::fabs(keep - h) < 2e-3f || std::fabs(keep - h - kThinBand) < 2e-3f) border = true;
        }
        {
            f32 c[3]; f32 r; EvaluateInstance(in, layer, p, c, &r);
            for (int i = 0; i < 6; ++i)
            {
                const f32 dd = p.planes[i][0] * c[0] + p.planes[i][1] * c[1] + p.planes[i][2] * c[2] + p.planes[i][3];
                if (std::fabs(dd + r) < 2e-3f * std::max(1.0f, r)) border = true;
            }
        }
        if (border) borderline.push_back(SeedOf(in.seedType));
    }
}

struct GpuList { u32 slot; f32 rows[12]; u32 color, seedPk; f32 lo, hi; };

// visible バッファの view / slot から count 個を読む
std::vector<GpuList> ReadList(Gpu& g, FoliageCuller::Layer& L, u32 view, u32 slot, u32 count)
{
    std::vector<GpuList> out;
    if (count == 0) return out;
    const u64 bytes = static_cast<u64>(count) * 64;
    const u64 off = static_cast<u64>(L.ListBase(view) + slot * L.CapOf(view)) * 64;
    auto raw = ReadBack(g, L.VisibleResource(), bytes, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, off);
    for (u32 i = 0; i < count; ++i)
    {
        GpuList e{};
        e.slot = slot;
        const u8* p = raw.data() + static_cast<size_t>(i) * 64;
        std::memcpy(e.rows, p, 48);
        std::memcpy(&e.color, p + 48, 4);
        std::memcpy(&e.seedPk, p + 52, 4);
        std::memcpy(&e.lo, p + 56, 4);
        std::memcpy(&e.hi, p + 60, 4);
        out.push_back(e);
    }
    return out;
}

struct RunOut { std::vector<u32> counts; std::vector<u32> argCounts; };

RunOut RunCull(Gpu& g, FoliageCuller& C, FoliageCuller::Layer& L, const Affine34& layer, const std::vector<CullDispatch>& ds)
{
    // フレームスロットを回す（定数リングはスロットごとに 64 ブロック。エンジンと同じ使い方）
    static u32 s_slot = 0;
    const u32 slot = (s_slot++) % 3;
    auto* c = g.Begin();
    const bool ok = C.Cull(c, L, layer, ds, slot, nullptr);
    g.Submit();
    CHECK(ok);
    RunOut r;
    LayerCounters lc;
    if (FoliageCuller::ReadCounters(L, slot, lc) && lc.valid) r.counts = lc.counts;
    // 間接引数
    const u64 bytes = static_cast<u64>(L.NumViews()) * L.GroupCount() * 20;
    auto raw = ReadBack(g, L.ArgsResource(), bytes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    for (u32 i = 0; i < L.NumViews() * L.GroupCount(); ++i)
    {
        u32 a[5]; std::memcpy(a, raw.data() + static_cast<size_t>(i) * 20, 20);
        r.argCounts.push_back(a[1]);
    }
    return r;
}

void RunCase(Gpu& g, FoliageCuller& C, const Case& cs, u32 n)
{
    const u32 S = cs.variants * cs.lods;
    auto inst = MakeInstances(n, 300.0f, 100 + n, cs.variants);
    FoliageInstanceSet set; set.instances = inst; set.Rebuild();

    LayerGpuDesc d;
    d.instances = set.instances.data(); d.instanceCount = set.Count();
    d.chunks = set.chunks.data(); d.chunkCount = static_cast<u32>(set.chunks.size());
    d.numViews = 3; d.lodStride = cs.lods; d.slotsPerView = S; d.capMain = n; d.capShadow = n;   // 容量 = n（溢れない）
    for (u32 v = 0; v < cs.variants; ++v) for (u32 l = 0; l < cs.lods; ++l) { d.groupSlot.push_back(v * cs.lods + l); d.groupIndexCount.push_back(300 + v * 10 + l); }
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto* c0 = g.Begin();
    std::string err;
    auto L = C.CreateLayer(d, c0, keep, &err);
    g.Submit();
    CHECK(L != nullptr);
    if (!L) { std::printf("  CreateLayer: %s\n", err.c_str()); return; }
    keep.clear();

    // レイヤー Transform
    DirectX::XMFLOAT4X4 w;
    DirectX::XMStoreFloat4x4(&w, DirectX::XMMatrixScaling(cs.layerScale, cs.layerScale, cs.layerScale) * DirectX::XMMatrixRotationY(cs.layerYaw)
                                 * DirectX::XMMatrixTranslation(3, 1, -2));
    const Affine34 layer = AffineFromWorld(w);

    CullParams p;
    p.cullDist = cs.cullDist; p.lodDist[0] = cs.lodD0; p.lodDist[1] = cs.lodD1; p.lodDist[2] = cs.lodD2;
    p.thinStart = cs.thinStart; p.lodFade = cs.lodFade; p.boundCenterY = 1.2f; p.boundRadius = 2.5f; p.radiusScale = 1.15f;
    p.layerScale = MaxColumnLength(layer);
    p.variantCount = cs.variants;
    for (u32 v = 0; v < kMaxVariants; ++v) p.lodCount[v] = cs.lods;
    p.camPos[0] = cs.cam[0]; p.camPos[1] = cs.cam[1]; p.camPos[2] = cs.cam[2];
    if (cs.wideFrustum) { for (int i = 0; i < 6; ++i) { p.planes[i][0] = 0; p.planes[i][1] = 1; p.planes[i][2] = 0; p.planes[i][3] = 1e7f; } }
    else PlanesFromViewProj(MakeVP(cs.cam, cs.look), p.planes);

    CullDispatch main; main.view = 0; main.p = p;
    CullParams ps = p; ps.shadowPass = true; ps.shadowMaxLod = 1; ps.cullDist = std::min(60.0f, p.cullDist);
    ps.thinStart = 0.999f;
    // 影ビューは別の錐台（太陽風: 上から見下ろす）
    {
        const f32 sc[3] = {cs.cam[0], cs.cam[1] + 80.0f, cs.cam[2]}, sl[3] = {cs.cam[0], 0, cs.cam[2] + 1.0f};
        PlanesFromViewProj(MakeVP(sc, sl, 1.0f, 300.0f), ps.planes);
    }
    CullDispatch shadow; shadow.view = 1; shadow.p = ps;

    const RunOut ro = RunCull(g, C, *L, layer, {main, shadow});
    CHECK(ro.counts.size() == 3 * S);
    if (ro.counts.size() != 3 * S) return;

    // ---- 主ビュー: list ごとの集合を比べる ----
    std::map<u32, std::vector<Expect>> exp;
    std::vector<u32> border;
    CpuExpected(inst, layer, p, cs.lods, exp, border);
    std::set<u32> borderSet(border.begin(), border.end());
    u32 expTotal = 0, gpuTotal = 0, mism = 0, mismBorder = 0, badRows = 0, badDither = 0;
    std::map<u32, std::vector<Expect>> gpuBySeed;
    for (u32 slot = 0; slot < S; ++slot)
    {
        const u32 cnt = ro.counts[slot];
        gpuTotal += cnt;
        CHECK(cnt <= n);
        auto list = ReadList(g, *L, 0, slot, cnt);
        for (const auto& e : list)
        {
            const u32 seed = e.seedPk & 0xFFFFFF;
            gpuBySeed[seed].push_back({slot, e.lo, e.hi, seed});
            // 行列: CPU の ComposeInstance と比べる（seed = 通し番号 → set の並び替え後の実体を引く）
        }
    }
    for (const auto& [seed, v] : exp) expTotal += static_cast<u32>(v.size());
    // 集合比較（seed ごとに slot / lo / hi の多重集合）
    std::set<u32> allSeeds;
    for (const auto& [s, v] : exp) allSeeds.insert(s);
    for (const auto& [s, v] : gpuBySeed) allSeeds.insert(s);
    for (u32 s : allSeeds)
    {
        auto a = exp.count(s) ? exp[s] : std::vector<Expect>{};
        auto b = gpuBySeed.count(s) ? gpuBySeed[s] : std::vector<Expect>{};
        auto key = [](const Expect& e) { return e.slot; };
        std::sort(a.begin(), a.end(), [&](auto& x, auto& y) { return key(x) < key(y); });
        std::sort(b.begin(), b.end(), [&](auto& x, auto& y) { return key(x) < key(y); });
        bool same = a.size() == b.size();
        if (same)
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].slot != b[i].slot) same = false;
                else
                {
                    const f32 tol = 2e-3f;
                    if (std::fabs(a[i].lo - b[i].lo) > tol || std::fabs(a[i].hi - b[i].hi) > tol) ++badDither;
                }
            }
        if (!same) { ++mism; if (borderSet.count(s)) ++mismBorder; }
    }
    std::printf("  [%s] n=%u: CPU 出力 %u / GPU 出力 %u / 不一致 %u（うち境界付近 %u）\n", cs.name, n, expTotal, gpuTotal, mism, mismBorder);
    CHECK(mism == mismBorder || mism - mismBorder <= std::max<u32>(2u, n / 5000));   // 境界以外の不一致は 0.02% 以下
    CHECK(badDither <= std::max<u32>(2u, expTotal / 5000));
    CHECK(gpuTotal > 0);

    // ---- 出力順: (チャンク順, チャンク内の添字順) = 配列順と完全に一致する（実行順に依存しない決定論的 compact）----
    {
        std::vector<std::vector<u32>> want(S), got(S);
        for (const auto& in : set.instances)
        {
            const u32 seed = SeedOf(in.seedType);
            if (borderSet.count(seed)) continue;
            const EmitInfo e = EvaluateInstance(in, layer, p);
            for (u32 k = 0; k < e.count; ++k) want[e.variant * cs.lods + e.lod[k]].push_back(seed);
        }
        u32 badSlots = 0;
        for (u32 slot = 0; slot < S; ++slot)
        {
            for (const auto& e : ReadList(g, *L, 0, slot, ro.counts[slot]))
            {
                const u32 seed = e.seedPk & 0xFFFFFF;
                if (!borderSet.count(seed)) got[slot].push_back(seed);
            }
            if (want[slot] != got[slot]) ++badSlots;
        }
        std::printf("  [%s] 出力順（チャンク順 + 添字順）: 一致しない list %u / %u\n", cs.name, badSlots, S);
        CHECK(badSlots == 0);
    }

    // 行列 / 色 / 風倍率: 出力のうち一部を CPU の ComposeInstance と比べる
    {
        // seed → 元インスタンス
        std::map<u32, FoliageInstance> bySeed;
        for (const auto& in : set.instances) bySeed[SeedOf(in.seedType)] = in;
        u32 checked = 0, bad = 0;
        for (u32 slot = 0; slot < S && checked < 4000; ++slot)
        {
            auto list = ReadList(g, *L, 0, slot, std::min(ro.counts[slot], 1500u));
            for (const auto& e : list)
            {
                const u32 seed = e.seedPk & 0xFFFFFF;
                const FoliageInstance& in = bySeed[seed];
                const Affine34 m = ComposeInstance(in, layer);
                bool ok = true;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c)
                        if (std::fabs(e.rows[r * 4 + c] - m.m[r][c]) > 2e-3f * std::max(1.0f, std::fabs(m.m[r][c]))) ok = false;
                if (e.color != in.color) ok = false;
                if ((e.seedPk >> 24) != (in.params & 0xFF)) ok = false;
                ++checked;
                if (!ok) ++bad;
            }
        }
        CHECK(checked > 0 && bad == 0);
        badRows = bad;
    }
    (void)badRows;

    // ---- 影ビュー ----
    {
        std::map<u32, std::vector<Expect>> se; std::vector<u32> sb;
        CpuExpected(inst, layer, ps, cs.lods, se, sb);
        std::set<u32> sbs(sb.begin(), sb.end());
        u32 sExp = 0, sGpu = 0;
        for (const auto& [s, v] : se) sExp += static_cast<u32>(v.size());
        std::map<u32, u32> gpuSeedSlot;   // seed → slot
        u32 smism = 0, smismB = 0;
        for (u32 slot = 0; slot < S; ++slot)
        {
            const u32 cnt = ro.counts[S + slot];
            sGpu += cnt;
            auto list = ReadList(g, *L, 1, slot, cnt);
            for (const auto& e : list) gpuSeedSlot[e.seedPk & 0xFFFFFF] = slot;
        }
        for (const auto& [s, v] : se)
        {
            auto it = gpuSeedSlot.find(s);
            if (it == gpuSeedSlot.end() || it->second != v[0].slot) { ++smism; if (sbs.count(s)) ++smismB; }
        }
        for (const auto& [s, sl] : gpuSeedSlot) if (!se.count(s)) { ++smism; if (sbs.count(s)) ++smismB; }
        std::printf("  [%s] 影ビュー: CPU %u / GPU %u / 不一致 %u（境界 %u）\n", cs.name, sExp, sGpu, smism, smismB);
        CHECK(smism == smismB || smism - smismB <= std::max<u32>(2u, n / 5000));
        // 影ビューには LOD > shadowMaxLod が出ない・クロスフェードのディザが無い
        for (u32 slot = 0; slot < S; ++slot)
            if (slot % cs.lods > ps.shadowMaxLod) CHECK(ro.counts[S + slot] == 0);
    }

    // ---- 間接引数の InstanceCount = min(counter, cap) ----
    for (u32 v = 0; v < 2; ++v)
        for (u32 gi = 0; gi < L->GroupCount(); ++gi)
        {
            const u32 slot = d.groupSlot[gi];
            CHECK(ro.argCounts[v * L->GroupCount() + gi] == std::min(ro.counts[v * S + slot], n));
        }
    // 3 番目のビュー（ディスパッチしていない）は 0
    for (u32 gi = 0; gi < L->GroupCount(); ++gi) CHECK(ro.argCounts[2 * L->GroupCount() + gi] == 0);

    // ---- 決定論: 同じ入力を 2 回 → 同じ個数 ----
    // ★個数だけでなく「並びも含めたバイト列」が 2 回で一致する（以前の原子 Append は並びが実行ごとに変わった）
    std::vector<std::vector<GpuList>> first;
    for (u32 v = 0; v < 2; ++v)
        for (u32 slot = 0; slot < S; ++slot) first.push_back(ReadList(g, *L, v, slot, ro.counts[v * S + slot]));
    const RunOut ro2 = RunCull(g, C, *L, layer, {main, shadow});
    CHECK(ro2.counts == ro.counts);
    u32 diffLists = 0, idx = 0;
    for (u32 v = 0; v < 2; ++v)
        for (u32 slot = 0; slot < S; ++slot)
        {
            const auto again = ReadList(g, *L, v, slot, ro2.counts[v * S + slot]);
            const auto& a = first[idx++];
            if (a.size() != again.size() || (!a.empty() && std::memcmp(a.data(), again.data(), a.size() * sizeof(GpuList)) != 0)) ++diffLists;
        }
    CHECK(diffLists == 0);
}

void TestOverflow(Gpu& g, FoliageCuller& C)
{
    auto inst = MakeInstances(20000, 30.0f, 5, 1);
    FoliageInstanceSet set; set.instances = inst; set.Rebuild();
    LayerGpuDesc d;
    d.instances = set.instances.data(); d.instanceCount = set.Count();
    d.chunks = set.chunks.data(); d.chunkCount = static_cast<u32>(set.chunks.size());
    d.numViews = 2; d.lodStride = 1; d.slotsPerView = 1; d.capMain = 1000; d.capShadow = 500;
    d.groupSlot = {0}; d.groupIndexCount = {36};
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto* c0 = g.Begin();
    auto L = C.CreateLayer(d, c0, keep, nullptr);
    g.Submit();
    CHECK(L != nullptr);
    if (!L) return;
    CullParams p;
    p.cullDist = 500; p.thinStart = 0.999f; p.lodFade = 0; p.boundRadius = 1; p.variantCount = 1; p.lodCount[0] = 1;
    for (int i = 0; i < 6; ++i) { p.planes[i][1] = 1; p.planes[i][3] = 1e7f; }
    CullDispatch a; a.view = 0; a.p = p;
    CullDispatch b; b.view = 1; b.p = p; b.p.shadowPass = true;
    const RunOut ro = RunCull(g, C, *L, Affine34{}, {a, b});
    CHECK(ro.counts[0] == 20000u && ro.counts[1] == 20000u);          // 溢れても数え続ける
    CHECK(ro.argCounts[0] == 1000u && ro.argCounts[1] == 500u);       // 描画数は容量に丸まる
    // 容量分の中身は有効（seed が範囲内で重複しない）
    auto list = ReadList(g, *L, 0, 0, 1000);
    std::set<u32> seeds;
    bool ok = true;
    for (const auto& e : list) { const u32 s = e.seedPk & 0xFFFFFF; if (s >= 20000u) ok = false; seeds.insert(s); }
    CHECK(ok && seeds.size() == 1000u);
    std::printf("  [overflow] 20000 → 主 %u / 影 %u に丸まった\n", ro.argCounts[0], ro.argCounts[1]);
}

void TestEmptyAndCulled(Gpu& g, FoliageCuller& C)
{
    // 全カリング（カメラ距離を極小に）
    auto inst = MakeInstances(5000, 100.0f, 9, 1);
    FoliageInstanceSet set; set.instances = inst; set.Rebuild();
    LayerGpuDesc d;
    d.instances = set.instances.data(); d.instanceCount = set.Count();
    d.chunks = set.chunks.data(); d.chunkCount = static_cast<u32>(set.chunks.size());
    d.numViews = 1; d.lodStride = 2; d.slotsPerView = 2; d.capMain = 6000;
    d.groupSlot = {0, 1}; d.groupIndexCount = {36, 12};
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto* c0 = g.Begin();
    auto L = C.CreateLayer(d, c0, keep, nullptr);
    g.Submit();
    CHECK(L != nullptr);
    if (!L) return;
    CullParams p;
    p.cullDist = 0.5f; p.boundRadius = 1; p.variantCount = 1; p.lodCount[0] = 2;
    for (int i = 0; i < 6; ++i) { p.planes[i][1] = 1; p.planes[i][3] = 1e7f; }
    p.camPos[0] = 5000;   // 全部遠い
    CullDispatch a; a.view = 0; a.p = p;
    RunOut ro = RunCull(g, C, *L, Affine34{}, {a});
    CHECK(ro.counts[0] == 0 && ro.counts[1] == 0 && ro.argCounts[0] == 0 && ro.argCounts[1] == 0);
    // 全可視
    p.cullDist = 1e6f; p.camPos[0] = 0; p.thinStart = 0.999f; p.lodDist[0] = 1e5f; p.lodDist[1] = 2e5f; p.lodDist[2] = 3e5f; p.lodFade = 0;
    a.p = p;
    ro = RunCull(g, C, *L, Affine34{}, {a});
    CHECK(ro.counts[0] == 5000u && ro.counts[1] == 0);
    // ディスパッチ無し（空のリスト）でもカウンタは 0 に戻る
    ro = RunCull(g, C, *L, Affine34{}, {});
    CHECK(ro.counts[0] == 0 && ro.argCounts[0] == 0);
    // インスタンス 0 個のレイヤー
    LayerGpuDesc e = d;
    e.instances = nullptr; e.instanceCount = 0; e.chunks = nullptr; e.chunkCount = 0;
    auto* c1 = g.Begin();
    auto L0 = C.CreateLayer(e, c1, keep, nullptr);
    g.Submit();
    CHECK(L0 != nullptr);
    if (L0) { ro = RunCull(g, C, *L0, Affine34{}, {a}); CHECK(ro.counts[0] == 0 && ro.argCounts[0] == 0); }
}

void TestHzb(Gpu& g, FoliageCuller& C)
{
    using namespace DirectX;
    const u32 W = 640, H = 360;
    f32 cam[3] = {0, 3, -40}, look[3] = {0, 2, 0};
    const XMFLOAT4X4 vpF = MakeVP(cam, look);
    // 深度: 画面の左半分（x < W/2）に手前 12 m の壁（NDC 深度）、右半分は空（1.0）
    auto ndcZ = [&](f32 dist)
    {
        const XMFLOAT4X4 v = vpF;
        const XMVECTOR p = XMVectorSet(cam[0], cam[1], cam[2] + dist, 1);   // カメラの前方 dist（LookAt の向きは +z 寄り）
        const XMVECTOR c = XMVector4Transform(p, XMLoadFloat4x4(&v));
        return XMVectorGetZ(c) / XMVectorGetW(c);
    };
    const f32 wallZ = ndcZ(12.0f);
    std::vector<f32> depth(static_cast<size_t>(W) * H, 1.0f);
    for (u32 y = 0; y < H; ++y) for (u32 x = 0; x < W / 2; ++x) depth[static_cast<size_t>(y) * W + x] = wallZ;
    const HzbCpu hz = MakeHzbFromDepth(W, H, depth);
    ComPtr<ID3D12Resource> tex = MakeHzbTexture(g, hz);

    auto inst = MakeInstances(30000, 60.0f, 77, 1);
    for (auto& in : inst) in.pz += 30.0f;   // 壁（12m）より奥に置く
    FoliageInstanceSet set; set.instances = inst; set.Rebuild();
    LayerGpuDesc d;
    d.instances = set.instances.data(); d.instanceCount = set.Count();
    d.chunks = set.chunks.data(); d.chunkCount = static_cast<u32>(set.chunks.size());
    d.numViews = 1; d.lodStride = 1; d.slotsPerView = 1; d.capMain = 40000;
    d.groupSlot = {0}; d.groupIndexCount = {36};
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto* c0 = g.Begin();
    auto L = C.CreateLayer(d, c0, keep, nullptr);
    g.Submit();
    CHECK(L != nullptr);
    if (!L) return;
    CullParams p;
    p.cullDist = 400; p.thinStart = 0.999f; p.lodFade = 0; p.boundRadius = 1.0f; p.boundCenterY = 0.5f; p.variantCount = 1; p.lodCount[0] = 1;
    p.lodDist[0] = 1e5f; p.lodDist[1] = 2e5f; p.lodDist[2] = 3e5f;
    PlanesFromViewProj(vpF, p.planes);
    p.camPos[0] = cam[0]; p.camPos[1] = cam[1]; p.camPos[2] = cam[2];
    CullDispatch a; a.view = 0; a.p = p;

    // HZB なし
    C.SetHzbResource(nullptr, 0);
    RunOut off = RunCull(g, C, *L, Affine34{}, {a});
    // HZB あり
    C.SetHzbResource(tex.Get(), hz.mips);
    a.hzb = true; a.prevVP = vpF; a.hzbW = static_cast<f32>(W); a.hzbH = static_cast<f32>(H); a.hzbMips = hz.mips; a.rectInflatePx = 4.0f;
    RunOut on = RunCull(g, C, *L, Affine34{}, {a});
    CHECK(on.counts[0] < off.counts[0]);   // 壁の裏が隠れた

    // CPU 参照
    auto hzbAt = [&](u32 mip, i32 x, i32 y) -> f32
    {
        const u32 mw = std::max(1u, W >> mip);
        return hz.data[mip][static_cast<size_t>(y) * mw + x];
    };
    u32 expVisible = 0, expHidden = 0;
    std::set<u32> cpuVisible;
    for (const auto& in : set.instances)
    {
        f32 c[3], r;
        const EmitInfo e = EvaluateInstance(in, Affine34{}, p, c, &r);
        if (e.count == 0) continue;
        if (SphereOccludedByHiZ(c, r, vpF, static_cast<f32>(W), static_cast<f32>(H), hz.mips, a.rectInflatePx, hzbAt)) { ++expHidden; continue; }
        ++expVisible;
        cpuVisible.insert(SeedOf(in.seedType));
    }
    auto list = ReadList(g, *L, 0, 0, on.counts[0]);
    std::set<u32> gpuVisible;
    for (const auto& e : list) gpuVisible.insert(e.seedPk & 0xFFFFFF);
    u32 diff = 0;
    for (u32 s : cpuVisible) if (!gpuVisible.count(s)) ++diff;
    for (u32 s : gpuVisible) if (!cpuVisible.count(s)) ++diff;
    std::printf("  [hzb] HZB なし %u / あり %u（CPU 参照: 可視 %u, 隠れ %u）/ 集合の差 %u\n", off.counts[0], on.counts[0], expVisible, expHidden, diff);
    CHECK(diff <= std::max<u32>(3u, off.counts[0] / 2000));
    CHECK(expHidden > 100);

    // 壁の無い HZB（全面 1.0）では 1 つも隠れない
    std::vector<f32> clear(static_cast<size_t>(W) * H, 1.0f);
    const HzbCpu hz2 = MakeHzbFromDepth(W, H, clear);
    ComPtr<ID3D12Resource> tex2 = MakeHzbTexture(g, hz2);
    C.SetHzbResource(tex2.Get(), hz2.mips);
    RunOut none = RunCull(g, C, *L, Affine34{}, {a});
    CHECK(none.counts[0] == off.counts[0]);
    // 手前の壁が近すぎる（カメラが壁の内側 = 判定不能）でも消さない: 深度 0（全面が最近）でも「箱の最近点 > 0」= 隠れるが、
    // 画面外のインスタンス（w<=0 や矩形外）は消えない
    C.SetHzbResource(nullptr, 0);
}

void TestWindGpu(Gpu& g, const std::wstring& shaderDir)
{
    // FoliageWindTest_CS を読んで、N 件の風変位を CPU の WindDelta と比べる
    std::ifstream f(shaderDir + L"FoliageWindTest_CS.cso", std::ios::binary);
    if (!f) { std::printf("  SKIP wind: FoliageWindTest_CS.cso が無い\n"); return; }
    std::vector<char> bc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    D3D12_DESCRIPTOR_RANGE none{};
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor = {0, 0};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[1].Descriptor = {0, 0};
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 3; rsd.pParameters = rp;
    (void)none;
    ComPtr<ID3DBlob> blob, err;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
    ComPtr<ID3D12RootSignature> rs;
    g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rs.Get(); pd.CS = {bc.data(), bc.size()};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { CHECK(false); return; }

    const u32 N = 4096;
    std::mt19937 rng(5);
    std::uniform_real_distribution<f32> u01(0, 1), pos(-200, 200);
    std::vector<f32> in(static_cast<size_t>(N) * 32);
    struct Ref { f32 wp[3], origin[3], nrm[3], y, vc[3], phase, ws, sc, t; WindFrame f; WindLayer l; };
    std::vector<Ref> refs(N);
    for (u32 i = 0; i < N; ++i)
    {
        Ref& r = refs[i];
        for (int k = 0; k < 3; ++k) { r.origin[k] = pos(rng); }
        r.origin[1] = 0;
        r.y = u01(rng) * 8.0f;
        r.wp[0] = r.origin[0] + (u01(rng) - 0.5f) * 3; r.wp[1] = r.y; r.wp[2] = r.origin[2] + (u01(rng) - 0.5f) * 3;
        f32 nn[3] = {u01(rng) - 0.5f, u01(rng) - 0.5f, u01(rng) - 0.5f};
        const f32 nl = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) + 1e-3f;
        for (int k = 0; k < 3; ++k) r.nrm[k] = nn[k] / nl;
        for (int k = 0; k < 3; ++k) r.vc[k] = u01(rng);
        r.phase = u01(rng); r.ws = 0.5f + u01(rng); r.sc = 0.5f + u01(rng) * 2; r.t = u01(rng) * 200.0f;
        const f32 ang = u01(rng) * 6.28f;
        r.f.dirX = std::cos(ang); r.f.dirZ = std::sin(ang); r.f.speed = 1 + u01(rng) * 12; r.f.gustStrength = u01(rng);
        r.f.gustFreq = u01(rng); r.f.turbulence = u01(rng); r.f.time = r.t; r.f.prevTime = r.t - 0.016f;
        r.l.bend = u01(rng) * 2; r.l.flutter = u01(rng) * 0.3f; r.l.bendExp = 1 + u01(rng) * 2; r.l.heightLocal = 2 + u01(rng) * 8;
        f32* o = in.data() + static_cast<size_t>(i) * 32;
        std::memcpy(o + 0, r.wp, 12); std::memcpy(o + 3, r.origin, 12); std::memcpy(o + 6, r.nrm, 12);
        o[9] = r.y; std::memcpy(o + 10, r.vc, 12); o[13] = r.phase; o[14] = r.ws; o[15] = r.sc; o[16] = r.t;
        o[20] = r.f.dirX; o[21] = r.f.dirZ; o[22] = r.f.speed; o[23] = r.f.gustStrength;
        o[24] = r.f.gustFreq; o[25] = r.f.turbulence; o[26] = r.f.time; o[27] = r.f.prevTime;
        o[28] = r.l.bend; o[29] = r.l.flutter; o[30] = r.l.bendExp; o[31] = r.l.heightLocal;
    }
    ComPtr<ID3D12Resource> upIn = MakeBuf(g, in.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* m = nullptr; D3D12_RANGE nr{0, 0};
    upIn->Map(0, &nr, &m); std::memcpy(m, in.data(), in.size() * 4); upIn->Unmap(0, nullptr);
    ComPtr<ID3D12Resource> gIn = MakeBuf(g, in.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    // UAV 出力（16B ストライド）
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = N * 16; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> gOut;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gOut));
    ComPtr<ID3D12Resource> cb = MakeBuf(g, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    u32 cbd[4] = {N, 0, 0, 0};
    cb->Map(0, &nr, &m); std::memcpy(m, cbd, 16); cb->Unmap(0, nullptr);
    auto* c = g.Begin();
    c->CopyBufferRegion(gIn.Get(), 0, upIn.Get(), 0, in.size() * 4);
    Barrier(c, gIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    c->SetComputeRootSignature(rs.Get());
    c->SetPipelineState(pso.Get());
    c->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    c->SetComputeRootShaderResourceView(1, gIn->GetGPUVirtualAddress());
    c->SetComputeRootUnorderedAccessView(2, gOut->GetGPUVirtualAddress());
    c->Dispatch((N + 63) / 64, 1, 1);
    g.Submit();
    auto raw = ReadBack(g, gOut.Get(), N * 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f64 maxErr = 0, maxMag = 0;
    u32 bad = 0;
    for (u32 i = 0; i < N; ++i)
    {
        const Ref& r = refs[i];
        f32 want[3];
        WindDelta(r.wp, r.origin, r.nrm, r.y, r.vc, r.phase, r.ws, r.sc, r.f, r.l, r.t, want);
        f32 got[3]; std::memcpy(got, raw.data() + static_cast<size_t>(i) * 16, 12);
        f64 e3 = 0;
        for (int k = 0; k < 3; ++k) e3 = std::max(e3, static_cast<f64>(std::fabs(got[k] - want[k])));
        maxErr = std::max(maxErr, e3);
        maxMag = std::max(maxMag, static_cast<f64>(std::fabs(want[0]) + std::fabs(want[2])));
        if (e3 > 2e-3) ++bad;
    }
    std::printf("  [wind] %u 件: 最大誤差 %.2e / 最大変位 %.3f m / 誤差 > 2e-3: %u 件\n", N, maxErr, maxMag, bad);
    CHECK(maxMag > 0.05);
    CHECK(bad <= N / 500);    // 三角関数の実装差を許容（0.2% 未満）
    CHECK(maxErr < 0.05);
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 途中で落ちても出力が残るように
#ifdef DX12E_SHADER_DIR
    const std::wstring shaderDir = std::wstring(L"" DX12E_SHADER_DIR);
#else
    const std::wstring shaderDir = L"shaders/";
#endif
    Gpu g;
    if (!g.Init()) { std::printf("SKIP foliage_gpu_test: D3D12 ハードウェアデバイスが無い\n"); return 0; }
    std::printf("adapter: %ls\n", g.adapterName.c_str());
    FoliageCuller C;
    std::string err;
    if (!C.Initialize(g.dev.Get(), shaderDir, 3, &err)) { std::printf("SKIP foliage_gpu_test: %s\n", err.c_str()); return 0; }

    const Case cases[] = {
        {"近距離・全方位", {0, 4, 0}, {0, 2, 10}, 250, 25, 60, 120, 0.6f, 0.10f, 2, 3, 1.0f, 0.0f, true},
        {"前方の錐台・クロスフェード無し", {10, 6, -50}, {0, 2, 0}, 200, 20, 50, 100, 0.5f, 0.0f, 1, 4, 1.0f, 0.0f, false},
        {"回転 + スケール 2 のレイヤー", {-20, 5, 30}, {30, 1, 0}, 300, 30, 80, 160, 0.7f, 0.12f, 3, 3, 2.0f, 1.2f, false},
        {"遠くから", {0, 60, -400}, {0, 0, 0}, 500, 60, 150, 300, 0.4f, 0.08f, 2, 4, 0.5f, 0.6f, false},
    };
    for (const Case& cs : cases)
    {
        RunCase(g, C, cs, 60000);
    }
    RunCase(g, C, cases[0], 1);       // 1 個
    RunCase(g, C, cases[1], 130);     // 2 チャンク
    RunCase(g, C, cases[2], 200000);  // 大きめ
    TestOverflow(g, C);
    TestEmptyAndCulled(g, C);
    TestHzb(g, C);
    TestWindGpu(g, shaderDir);

    if (const u64 msgs = g.MessageCount()) { std::printf("FAIL: D3D12 デバッグレイヤのメッセージ %llu 件\n", static_cast<unsigned long long>(msgs)); ++g_failures; }
    std::printf("foliage_gpu_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
