// GPU 駆動のインスタンス群（設計 docs/SCENE_FORMAT_DESIGN.md §4.2 = 4-3）の単体テスト。
//
//   ① 純ロジック（GPU 不要）: InstanceGpuMath.h の CPU 参照
//      ・MakeRecord の中心 / 半径が BuildDrawList の式どおり
//      ・チャンクが落ちるなら中身の球は全部 SphereInPlanes でも落ちる（チャンクカリングが絵を変えない）
//      ・参照結果の並び = インスタンス番号の昇順（描画順が CPU 経路と同じになる条件）
//      ・LOD 閾値 / 影のテクセル LOD / lodBias / maxList への丸め
//   ② GPU（窓なしの D3D12 デバイス。無ければ SKIP）: shaders/instancegpu/InstanceCull.hlsl の compute を実行して
//      ・list ごとの集合 = CPU 参照（複数のカメラ x lodBias x texelWorld x 個数）。浮動小数の境界の差は 0.1% まで許容して数を出す
//      ・出力の並び = 番号の昇順 / 色 / 前フレームの表の中身 / 間接引数（IndexCount・InstanceCount・StartInstance）
//      ・同じ入力を 2 回 = バイト一致（決定論）/ 全カリング / 1 個 / 128・129 個（チャンク境界）/ 1 つのコマンドリストで複数ビュー
//   環境変数: DX12E_TEST_D3D_DEBUG=1 で D3D12 デバッグレイヤ（警告 / エラーが 1 件でもあれば失敗）。
#include "renderer/instancegpu/InstanceGpuCuller.h"
#include "renderer/instancegpu/InstanceGpuMath.h"

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
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dx12e;
using namespace dx12e::instgpu;
using namespace DirectX;
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

// ---------------------------------------------------------------------------
// シーンの生成
// ---------------------------------------------------------------------------
LocalBounds BoxBounds(f32 h) { LocalBounds b; b.mn = {-h, -h, -h}; b.mx = {h, h, h}; b.valid = true; return b; }

std::vector<XMFLOAT4X4> MakeWorlds(u32 n, f32 extent, u32 seed, bool coherent = false)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> pos(-extent, extent), sc(0.4f, 3.0f), rot(0.0f, 6.28f);
    std::vector<XMFLOAT4X4> w(n);
    for (u32 i = 0; i < n; ++i)
    {
        const f32 s = sc(rng);
        const XMMATRIX m = XMMatrixScaling(s, s * 1.2f, s) * XMMatrixRotationRollPitchYaw(rot(rng), rot(rng), rot(rng))
                         * XMMatrixTranslation(pos(rng), pos(rng) * 0.2f, pos(rng));
        XMStoreFloat4x4(&w[i], m);
    }
    // coherent: x 順に並べる（現実の配置は空間的にまとまっている。チャンクの AABB が締まり、チャンク棄却が効く）
    if (coherent) std::sort(w.begin(), w.end(), [](const XMFLOAT4X4& a, const XMFLOAT4X4& b) { return a._41 < b._41; });
    return w;
}

std::vector<InstanceRecord> MakeRecords(const std::vector<XMFLOAT4X4>& worlds, const LocalBounds& b)
{
    std::vector<InstanceRecord> r;
    r.reserve(worlds.size());
    for (const auto& w : worlds) r.push_back(MakeRecord(w, b));
    return r;
}

ViewParams MakeView(XMFLOAT3 eye, XMFLOAT3 target, f32 fovDeg, f32 farZ, u32 lodBias, f32 texelWorld, u32 maxList)
{
    const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovDeg), 16.0f / 9.0f, 0.1f, farZ);
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, view * proj);
    ViewParams v;
    PlanesFromViewProj(vp, v.planes);
    v.camPos[0] = eye.x; v.camPos[1] = eye.y; v.camPos[2] = eye.z;
    v.lodBias = lodBias;
    v.texelWorld = texelWorld;
    v.maxList = maxList;
    return v;
}

// ---------------------------------------------------------------------------
// ① 純ロジック
// ---------------------------------------------------------------------------
void TestMath()
{
    // MakeRecord: スケール 2・平行移動。BuildDrawList の式（半径 = max(1, 0.5|ext|) * 最大行長 * 1.25）
    {
        XMFLOAT4X4 w;
        XMStoreFloat4x4(&w, XMMatrixScaling(2, 2, 2) * XMMatrixTranslation(10, 20, 30));
        LocalBounds b = BoxBounds(1.0f);   // 対角の半分 = sqrt(3)
        const InstanceRecord r = MakeRecord(w, b);
        CHECK(std::fabs(r.center[0] - 10.0f) < 1e-5f && std::fabs(r.center[1] - 20.0f) < 1e-5f && std::fabs(r.center[2] - 30.0f) < 1e-5f);
        CHECK(std::fabs(r.radius - std::sqrt(3.0f) * 2.0f * 1.25f) < 1e-4f);
        // r0..r2 = transpose(world) の先頭 3 行（r0.w = 平行移動 x）
        CHECK(r.r0[0] == 2.0f && r.r0[3] == 10.0f && r.r1[1] == 2.0f && r.r1[3] == 20.0f && r.r2[2] == 2.0f && r.r2[3] == 30.0f);
        // 小さいメッシュは半径の下限 1.0
        LocalBounds sb = BoxBounds(0.1f);
        const InstanceRecord s = MakeRecord(w, sb);
        CHECK(std::fabs(s.radius - 1.0f * 2.0f * 1.25f) < 1e-4f);
        // AABB が無いメッシュ: 球は world 原点中心・半径 1 * ms * 1.25
        LocalBounds nb;
        const InstanceRecord n = MakeRecord(w, nb);
        CHECK(std::fabs(n.center[0] - 10.0f) < 1e-5f && std::fabs(n.radius - 2.0f * 1.25f) < 1e-4f);
        // 前フレーム表
        const PrevRecord p = MakePrevRecord(w);
        CHECK(p.p0[3] == 10.0f && p.p1[3] == 20.0f && p.p2[3] == 30.0f);
    }

    // チャンク: 連続 128 個ごと。最後は端数
    {
        const auto worlds = MakeWorlds(300, 50.0f, 1);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        const auto ch = BuildChunks(rec.data(), static_cast<u32>(rec.size()));
        CHECK(ch.size() == 3);
        CHECK(ch[0].first == 0 && ch[0].count == 128 && ch[1].first == 128 && ch[1].count == 128 && ch[2].first == 256 && ch[2].count == 44);
        for (const auto& c : ch)
            for (u32 i = 0; i < c.count; ++i)
            {
                const auto& r = rec[c.first + i];
                for (int k = 0; k < 3; ++k) CHECK(r.center[k] - r.radius >= c.mn[k] && r.center[k] + r.radius <= c.mx[k]);
            }
    }

    // チャンクが落ちるなら中身の球は全部落ちる（チャンクカリングが絵を変えない）+ 並びは番号の昇順
    {
        const auto worlds = MakeWorlds(20000, 400.0f, 7, true);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        const auto ch = BuildChunks(rec.data(), static_cast<u32>(rec.size()));
        const XMFLOAT3 eyes[] = {{0, 30, -500}, {100, 5, 100}, {-300, 80, 0}, {0, 500, 0}};
        const XMFLOAT3 tgts[] = {{0, 0, 0}, {0, 0, 0}, {50, 0, 20}, {0, 0, 1}};
        u32 rejectedChunks = 0, rejectedInside = 0, totalVisible = 0;
        for (int vi = 0; vi < 4; ++vi)
        {
            const ViewParams v = MakeView(eyes[vi], tgts[vi], 60.0f, 700.0f, 0, 0.0f, 4);
            for (const auto& c : ch)
            {
                if (ChunkInPlanes(v.planes, c)) continue;
                ++rejectedChunks;
                for (u32 i = 0; i < c.count; ++i)
                    if (SelectList(v, rec[c.first + i]) != kInvalidList) ++rejectedInside;
            }
            const CullResult cr = CullReference(rec.data(), ch, v);
            totalVisible += cr.Total();
            for (const auto& l : cr.lists)
                CHECK(std::is_sorted(l.begin(), l.end()));
            // チャンクカリング無しの全数判定と同じ集合
            u32 flat = 0;
            for (u32 i = 0; i < rec.size(); ++i) if (SelectList(v, rec[i]) != kInvalidList) ++flat;
            CHECK(flat == cr.Total());
        }
        CHECK(rejectedChunks > 0);
        CHECK(rejectedInside == 0);
        CHECK(totalVisible > 0);
    }

    // LOD の閾値（BuildDrawList と同じ 0.125 / 0.055 / 0.020 / 0.008）と丸め
    {
        // apparent = radius / dist
        CHECK(NominalLod(1.0f, 7.0f, 1.0f) == 0);    // 0.142
        CHECK(NominalLod(1.0f, 10.0f, 1.0f) == 1);   // 0.1
        CHECK(NominalLod(1.0f, 20.0f, 1.0f) == 2);   // 0.05
        CHECK(NominalLod(1.0f, 60.0f, 1.0f) == 3);   // 0.0166
        CHECK(NominalLod(1.0f, 200.0f, 1.0f) == 4);  // 0.005
        CHECK(NominalLod(1.0f, 0.0f, 1.0f) == 0);    // 距離 0 でも壊れない（max(dist, 1e-3)）
        ViewParams v = MakeView({0, 0, 0}, {0, 0, 1}, 90.0f, 1000.0f, 0, 0.0f, 4);
        InstanceRecord r{};
        r.center[2] = 20.0f; r.radius = 1.0f;   // LOD 2
        CHECK(SelectList(v, r) == 2);
        v.lodBias = 1;
        CHECK(SelectList(v, r) == 3);           // 影は 1 段粗い
        v.lodBias = 3;
        CHECK(SelectList(v, r) == 4);           // 上限 4 に丸め
        v.lodBias = 0; v.maxList = 1;
        CHECK(SelectList(v, r) == 1);           // maxList（モデルの LOD 数）に丸め
        // 影のテクセル LOD: 直径が 20 テクセル（< 32）なら LOD 4
        v.maxList = 4; v.texelWorld = 2.0f * 1.0f / 20.0f;
        r.center[2] = 5.0f;                     // 名目は LOD 0
        CHECK(SelectList(v, r) == 4);
        v.texelWorld = 2.0f * 1.0f / 100.0f;    // 100 テクセル（96..288）→ LOD 2
        CHECK(SelectList(v, r) == 2);
        v.texelWorld = 2.0f * 1.0f / 1000.0f;   // 1000 テクセル → 0
        CHECK(SelectList(v, r) == 0);
        // 視錐台の外
        r.center[2] = -50.0f;
        CHECK(SelectList(v, r) == kInvalidList);
    }
}

// ---------------------------------------------------------------------------
// ② GPU
// ---------------------------------------------------------------------------
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
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
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

ComPtr<ID3D12Resource> MakeReadback(Gpu& g, u64 bytes)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = std::max<u64>(bytes, 16); rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
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

std::vector<u8> ReadBack(Gpu& g, ID3D12Resource* res, u64 bytes, D3D12_RESOURCE_STATES state)
{
    ComPtr<ID3D12Resource> rb = MakeReadback(g, bytes);
    auto* c = g.Begin();
    Barrier(c, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c->CopyBufferRegion(rb.Get(), 0, res, 0, bytes);
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

u64 TransKey(f32 a, f32 b, f32 c)
{
    u32 x, y, z;
    std::memcpy(&x, &a, 4); std::memcpy(&y, &b, 4); std::memcpy(&z, &c, 4);
    u64 h = 1469598103934665603ull;
    for (u32 v : {x, y, z}) { h ^= v; h *= 1099511628211ull; }
    return h;
}

struct GpuOut
{
    std::vector<u8> stream, prev, args;
};

constexpr u32 kSubmeshes = 2;
std::vector<u32> IndexTable()
{
    // [submesh * 5 + list]
    return {3000, 1500, 600, 150, 30, 900, 450, 180, 45, 9};
}

// 1 つの群を作って 1 ビューをカリングし、ストリーム / 前フレーム / 引数を読み戻す。
bool RunGpuCull(Gpu& g, InstanceGpuCuller& C, const std::vector<InstanceRecord>& rec, const std::vector<PrevRecord>* prev,
                const CullView& v, GpuOut& out)
{
    std::string err;
    auto grp = C.CreateGroup(kSubmeshes, IndexTable(), &err);
    if (!grp) { std::printf("CreateGroup: %s\n", err.c_str()); return false; }
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto* c = g.Begin();
    if (!C.Upload(*grp, c, rec.data(), static_cast<u32>(rec.size()), prev ? prev->data() : nullptr, &keep, &err))
    { std::printf("Upload: %s\n", err.c_str()); g.Submit(); return false; }
    if (!C.Cull(c, *grp, v, 0, true)) { std::printf("Cull が失敗\n"); g.Submit(); return false; }
    g.Submit();
    out.stream = ReadBack(g, grp->VisibleResource(), static_cast<u64>(rec.size()) * 64u, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (v.writePrev && grp->VisiblePrevResource())
        out.prev = ReadBack(g, grp->VisiblePrevResource(), static_cast<u64>(rec.size()) * 48u, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    out.args = ReadBack(g, grp->ArgsResource(), static_cast<u64>(kSubmeshes) * kLists * 20u, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    // 読み戻しコピーされた個数（統計用）も args と一致するはず
    u32 counts[kLists] = {};
    const bool rbOk = InstanceGpuCuller::ReadCounters(*grp, 0, counts);
    CHECK(rbOk);
    if (rbOk)
        for (u32 l = 0; l < kLists; ++l)
        {
            u32 ic; std::memcpy(&ic, out.args.data() + static_cast<size_t>(l) * 20u + 4u, 4);
            CHECK(counts[l] == ic);
        }
    return true;
}

// GPU の出力と CPU 参照を比べる。許容: 境界の浮動小数差（集合の対称差が個数の 0.1% か 2 個のどちらか大きい方以下）。
void CompareWithReference(const char* label, const std::vector<InstanceRecord>& rec, const std::vector<PrevRecord>* prev,
                          const CullView& v, const GpuOut& go)
{
    const auto chunks = BuildChunks(rec.data(), static_cast<u32>(rec.size()));
    const CullResult ref = CullReference(rec.data(), chunks, v.view);

    std::unordered_map<u64, u32> byPos;
    byPos.reserve(rec.size() * 2);
    for (u32 i = 0; i < rec.size(); ++i) byPos[TransKey(rec[i].r0[3], rec[i].r1[3], rec[i].r2[3])] = i;

    auto argAt = [&](u32 s, u32 l, u32 k) { u32 x; std::memcpy(&x, go.args.data() + (static_cast<size_t>(s) * kLists + l) * 20u + k * 4u, 4); return x; };

    u32 gpuTotal = 0, diffs = 0, order = 0, badRows = 0, badColor = 0, badPrev = 0;
    u32 expectedStart = 0;
    for (u32 l = 0; l < kLists; ++l)
    {
        const u32 cnt = argAt(0, l, 1);
        const u32 start = argAt(0, l, 4);
        // 全サブメッシュで InstanceCount / StartInstance が同じ（同じ list を共有）+ IndexCount が表どおり
        for (u32 s = 0; s < kSubmeshes; ++s)
        {
            CHECK(argAt(s, l, 1) == cnt);
            CHECK(argAt(s, l, 4) == start);
            CHECK(argAt(s, l, 0) == IndexTable()[s * kLists + l]);
            CHECK(argAt(s, l, 2) == 0 && argAt(s, l, 3) == 0);
        }
        CHECK(start == expectedStart);   // list を連結した 1 本のストリーム
        expectedStart += cnt;
        gpuTotal += cnt;
        std::vector<u32> gpuIdx;
        gpuIdx.reserve(cnt);
        for (u32 k = 0; k < cnt; ++k)
        {
            const f32* row = reinterpret_cast<const f32*>(go.stream.data() + static_cast<size_t>(start + k) * 64u);
            auto it = byPos.find(TransKey(row[3], row[7], row[11]));
            if (it == byPos.end()) { ++badRows; gpuIdx.push_back(0xFFFFFFFFu); continue; }
            const u32 idx = it->second;
            gpuIdx.push_back(idx);
            const InstanceRecord& r = rec[idx];
            if (std::memcmp(row, r.r0, 16) != 0 || std::memcmp(row + 4, r.r1, 16) != 0 || std::memcmp(row + 8, r.r2, 16) != 0) ++badRows;
            if (std::memcmp(row + 12, v.color, 16) != 0) ++badColor;
            if (prev && !go.prev.empty())
            {
                const f32* pr = reinterpret_cast<const f32*>(go.prev.data() + static_cast<size_t>(start + k) * 48u);
                const PrevRecord& p = (*prev)[idx];
                if (std::memcmp(pr, p.p0, 16) != 0 || std::memcmp(pr + 4, p.p1, 16) != 0 || std::memcmp(pr + 8, p.p2, 16) != 0) ++badPrev;
            }
            else if (!prev && v.writePrev && !go.prev.empty())
            {
                // prev 無し = 現在と同じ（インスタンス表自身を読む）
                const f32* pr = reinterpret_cast<const f32*>(go.prev.data() + static_cast<size_t>(start + k) * 48u);
                if (std::memcmp(pr, r.r0, 16) != 0 || std::memcmp(pr + 4, r.r1, 16) != 0 || std::memcmp(pr + 8, r.r2, 16) != 0) ++badPrev;
            }
        }
        // 並び = 番号の昇順
        for (size_t k = 1; k < gpuIdx.size(); ++k)
            if (gpuIdx[k] != 0xFFFFFFFFu && gpuIdx[k - 1] != 0xFFFFFFFFu && gpuIdx[k] <= gpuIdx[k - 1]) ++order;
        // 集合の対称差
        std::vector<u32> a = gpuIdx, b = ref.lists[l];
        std::sort(a.begin(), a.end());
        std::vector<u32> sd;
        std::set_symmetric_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(sd));
        diffs += static_cast<u32>(sd.size());
    }
    const u32 tol = std::max(2u, static_cast<u32>(ref.Total() / 1000));
    std::printf("  %-34s N=%-7zu GPU 可視 %-7u CPU 可視 %-7u 差 %u / 並び違い %u / 行違い %u\n", label, rec.size(), gpuTotal, ref.Total(), diffs, order, badRows);
    CHECK(diffs <= tol);
    CHECK(order == 0);
    CHECK(badRows == 0);
    CHECK(badColor == 0);
    CHECK(badPrev == 0);
}

void TestGpu(Gpu& g, InstanceGpuCuller& C)
{
    struct Case { const char* name; u32 n; f32 extent; XMFLOAT3 eye, tgt; f32 fov, farZ; u32 bias; f32 texel; u32 maxList; bool prev; bool coherent; };
    const Case cases[] = {
        {"近距離・広角",                   5000,   120.0f, {0, 10, -200}, {0, 0, 0}, 100.0f, 600.0f, 0, 0.0f, 4, false, false},
        {"遠くから・LOD が粗くなる",       20000,  400.0f, {0, 80, -900}, {0, 0, 0}, 50.0f, 2500.0f, 0, 0.0f, 4, true, false},
        {"狭い視野・チャンク棄却が効く",   20000,  400.0f, {-300, 20, -100}, {-200, 0, 60}, 25.0f, 700.0f, 0, 0.0f, 4, false, true},
        {"影（lodBias 1・テクセル LOD）",  20000,  400.0f, {50, 300, 50}, {0, 0, 0}, 40.0f, 1500.0f, 1, 1.5f, 4, false, false},
        {"影・狭い視野・コヒーレント",     30000,  500.0f, {200, 400, 0}, {120, 0, 0}, 20.0f, 1200.0f, 1, 0.7f, 4, true, true},
        {"maxList 1（LOD の少ないモデル）", 8000,   200.0f, {0, 20, -300}, {0, 0, 0}, 70.0f, 900.0f, 0, 0.0f, 1, true, false},
        {"10 万個・前フレームつき",        100000, 800.0f, {-400, 60, -1100}, {0, 0, 0}, 60.0f, 3000.0f, 0, 0.0f, 4, true, false},
        {"10 万個・コヒーレント・狭い視野", 100000, 800.0f, {-700, 40, -300}, {-500, 0, 100}, 30.0f, 1500.0f, 0, 0.0f, 4, true, true},
    };
    for (const Case& cs : cases)
    {
        const auto worlds = MakeWorlds(cs.n, cs.extent, 11 + cs.n, cs.coherent);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        std::vector<PrevRecord> prevRecs;
        if (cs.prev)
        {
            prevRecs.reserve(cs.n);
            for (const auto& w : worlds)
            {
                XMFLOAT4X4 p = w;
                p._41 -= 0.25f;   // 少し動いた
                prevRecs.push_back(MakePrevRecord(p));
            }
        }
        CullView v;
        v.view = MakeView(cs.eye, cs.tgt, cs.fov, cs.farZ, cs.bias, cs.texel, cs.maxList);
        v.color[0] = 0.5f; v.color[1] = 0.25f; v.color[2] = 0.125f; v.color[3] = 1.0f;
        v.writePrev = true;   // prev 無しのケースは「現在と同じ」が出る
        GpuOut o;
        if (!RunGpuCull(g, C, rec, cs.prev ? &prevRecs : nullptr, v, o)) { ++g_failures; continue; }
        CompareWithReference(cs.name, rec, cs.prev ? &prevRecs : nullptr, v, o);
    }

    // チャンクの境界（1 / 127 / 128 / 129 / 256 / 257）
    for (u32 n : {1u, 127u, 128u, 129u, 256u, 257u})
    {
        const auto worlds = MakeWorlds(n, 30.0f, 100 + n);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        CullView v;
        v.view = MakeView({0, 5, -80}, {0, 0, 0}, 80.0f, 400.0f, 0, 0.0f, 4);
        v.writePrev = false;
        GpuOut o;
        if (!RunGpuCull(g, C, rec, nullptr, v, o)) { ++g_failures; continue; }
        char label[64];
        std::snprintf(label, sizeof(label), "境界 N=%u", n);
        CompareWithReference(label, rec, nullptr, v, o);
    }

    // 全カリング（カメラの背後）→ 全 list が 0 個・引数の InstanceCount = 0
    {
        const auto worlds = MakeWorlds(3000, 100.0f, 5);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        CullView v;
        v.view = MakeView({0, 0, 1000}, {0, 0, 2000}, 60.0f, 500.0f, 0, 0.0f, 4);
        GpuOut o;
        if (RunGpuCull(g, C, rec, nullptr, v, o))
        {
            u32 total = 0;
            for (u32 s = 0; s < kSubmeshes; ++s)
                for (u32 l = 0; l < kLists; ++l) { u32 x; std::memcpy(&x, o.args.data() + (static_cast<size_t>(s) * kLists + l) * 20u + 4u, 4); total += x; }
            CHECK(total == 0);
        }
    }

    // 決定論: 同じ入力を 2 回（別々の群）→ 可視ストリームの有効部分がバイト一致
    {
        const auto worlds = MakeWorlds(30000, 300.0f, 21);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        CullView v;
        v.view = MakeView({0, 30, -500}, {0, 0, 0}, 70.0f, 1500.0f, 0, 0.0f, 4);
        GpuOut a, b;
        const bool ok = RunGpuCull(g, C, rec, nullptr, v, a) && RunGpuCull(g, C, rec, nullptr, v, b);
        CHECK(ok);
        if (ok)
        {
            u32 total = 0;
            for (u32 l = 0; l < kLists; ++l) { u32 x; std::memcpy(&x, a.args.data() + static_cast<size_t>(l) * 20u + 4u, 4); total += x; }
            CHECK(total > 0);
            CHECK(std::memcmp(a.stream.data(), b.stream.data(), static_cast<size_t>(total) * 64u) == 0);
            CHECK(a.args == b.args);
            std::printf("  決定論: 可視 %u 個・2 回のバイト一致\n", total);
        }
    }

    // 1 つのコマンドリストで同じ群に複数ビューを続けて Cull（状態遷移 UAV / VB の往復）→ 最後のビューの結果になる。Upload し直しも
    {
        const auto worlds = MakeWorlds(10000, 200.0f, 33);
        const auto rec = MakeRecords(worlds, BoxBounds(1.0f));
        std::string err;
        auto grp = C.CreateGroup(kSubmeshes, IndexTable(), &err);
        CHECK(grp != nullptr);
        if (grp)
        {
            std::vector<ComPtr<ID3D12Resource>> keep;
            CullView v1, v2;
            v1.view = MakeView({0, 10, -300}, {0, 0, 0}, 60.0f, 800.0f, 0, 0.0f, 4);
            v2.view = MakeView({100, 200, 0}, {0, 0, 0}, 60.0f, 800.0f, 1, 0.8f, 4);
            v2.writePrev = true;
            auto* c = g.Begin();
            CHECK(C.Upload(*grp, c, rec.data(), static_cast<u32>(rec.size()), nullptr, &keep, &err));
            CHECK(InstanceGpuCuller::NeedsCull(*grp, v1));
            CHECK(C.Cull(c, *grp, v1, 0, false));
            CHECK(!InstanceGpuCuller::NeedsCull(*grp, v1));      // 同じビューは省ける
            CHECK(InstanceGpuCuller::NeedsCull(*grp, v2));
            CHECK(C.Cull(c, *grp, v2, 0, true));
            CHECK(!InstanceGpuCuller::NeedsCull(*grp, v2));
            g.Submit();
            GpuOut o;
            o.stream = ReadBack(g, grp->VisibleResource(), static_cast<u64>(rec.size()) * 64u, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            o.prev = ReadBack(g, grp->VisiblePrevResource(), static_cast<u64>(rec.size()) * 48u, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            o.args = ReadBack(g, grp->ArgsResource(), static_cast<u64>(kSubmeshes) * kLists * 20u, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            CompareWithReference("同一リストで 2 ビュー（最後が残る）", rec, nullptr, v2, o);

            // 次のコマンドリストでもう一度 v1（前のリストで VB 状態のまま）→ Upload し直し（個数が変わる）→ Cull
            c = g.Begin();
            CHECK(C.Cull(c, *grp, v1, 1, false));
            g.Submit();
            const auto worlds2 = MakeWorlds(4000, 150.0f, 34);
            const auto rec2 = MakeRecords(worlds2, BoxBounds(1.0f));
            c = g.Begin();
            CHECK(C.Upload(*grp, c, rec2.data(), static_cast<u32>(rec2.size()), nullptr, &keep, &err));
            CHECK(InstanceGpuCuller::NeedsCull(*grp, v1));       // Upload したら必ず再カリング
            CHECK(C.Cull(c, *grp, v1, 2, false));
            g.Submit();
            GpuOut o2;
            o2.stream = ReadBack(g, grp->VisibleResource(), static_cast<u64>(rec2.size()) * 64u, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            o2.args = ReadBack(g, grp->ArgsResource(), static_cast<u64>(kSubmeshes) * kLists * 20u, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            CompareWithReference("Upload し直し（個数が変わる）", rec2, nullptr, v1, o2);
        }
    }
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestMath();
    std::printf("instance_gpu_test: 純ロジック %d checks, %d failures\n", g_checks, g_failures);

#ifdef DX12E_SHADER_DIR
    const std::wstring shaderDir = std::wstring(L"" DX12E_SHADER_DIR);
#else
    const std::wstring shaderDir = L"shaders/";
#endif
    Gpu g;
    if (!g.Init()) { std::printf("SKIP instance_gpu_test(GPU 部分): D3D12 ハードウェアデバイスが無い\n"); return g_failures == 0 ? 0 : 1; }
    std::printf("adapter: %ls\n", g.adapterName.c_str());
    InstanceGpuCuller C;
    std::string err;
    if (!C.Initialize(g.dev.Get(), shaderDir, 3, &err)) { std::printf("SKIP instance_gpu_test(GPU 部分): %s\n", err.c_str()); return g_failures == 0 ? 0 : 1; }
    TestGpu(g, C);
    if (const u64 msgs = g.MessageCount()) { std::printf("FAIL: D3D12 デバッグレイヤのメッセージ %llu 件\n", static_cast<unsigned long long>(msgs)); ++g_failures; }
    std::printf("instance_gpu_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
