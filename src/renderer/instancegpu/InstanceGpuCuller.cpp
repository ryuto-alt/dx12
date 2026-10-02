#include "renderer/instancegpu/InstanceGpuCuller.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>

#include "graphics/DeferredRelease.h"

using Microsoft::WRL::ComPtr;

namespace dx12e::instgpu
{
namespace
{
// HLSL の cbuffer CullCB（shaders/instancegpu/InstanceCull.hlsl）と 1 バイトも違わないこと。
struct CullCbData
{
    f32 planes[6][4];
    f32 cam[4];
    f32 color[4];
    u32 p0[4];   // instanceCount / chunkCount / lodBias / maxList
    f32 p1[4];   // texelWorld / lodScale / 0 / 0
    u32 p2[4];   // writePrev / submeshCount / prevStride / 0
};
static_assert(sizeof(CullCbData) == 176, "CullCB のレイアウトが HLSL とずれている");
constexpr u32 kCbBlock = 256;                 // CBV の配置制約（256 の倍数）
constexpr u32 kCbRing  = 8192u * kCbBlock;    // フレームあたり 8192 ビュー（2MB）

std::vector<u8> ReadFileBytes(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<u64>(bytes, 16);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    // ★DEFAULT ヒープのバッファは常に COMMON で作られる。UPLOAD は GENERIC_READ、READBACK は COPY_DEST。
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
    if (heap == D3D12_HEAP_TYPE_UPLOAD) st = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK) st = D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}
void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES next)
{
    if (!r || cur == next) return;
    Barrier(cmd, r, cur, next);
    cur = next;
}
void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cmd->ResourceBarrier(1, &b);
}

u64 HashBytes(const void* p, size_t n, u64 h = 1469598103934665603ull)
{
    const u8* q = static_cast<const u8*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= q[i]; h *= 1099511628211ull; }
    return h;
}

// 古いリソースの解放（DeferredRelease が有効ならフェンス完了後。無効なら即時＝呼び出し側が GPU を止めている前提）
void Retire(ComPtr<ID3D12Resource>& r)
{
    if (!r) return;
    DeferredRelease::Defer(std::move(r), nullptr);
    r.Reset();
}
} // namespace

// ===========================================================================
struct InstanceGpuCuller::Impl
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoReset, psoCount, psoScan, psoFinalize, psoEmit;
    std::vector<ComPtr<ID3D12Resource>> cb;   // フレームスロットごとの定数リング
    std::vector<u8*> cbMapped;
    u32 cbCursor = 0;
    u32 cbFrame = 0xFFFFFFFFu;
};

InstanceGpuCuller::InstanceGpuCuller() = default;
InstanceGpuCuller::~InstanceGpuCuller() { Shutdown(); }
// 群を捨てるとき、フライト中のフレームが読んでいるかもしれないバッファは DeferredRelease へ渡す（無効なら即時解放）。
InstanceGpuCuller::Group::~Group()
{
    for (ComPtr<ID3D12Resource>* r : std::initializer_list<ComPtr<ID3D12Resource>*>{&m_inst, &m_chunks, &m_prev, &m_indexCounts, &m_out, &m_outPrev, &m_chunkCounts, &m_counters, &m_args})
        Retire(*r);
    for (auto& rb : m_readback) Retire(rb);
}

D3D12_VERTEX_BUFFER_VIEW InstanceGpuCuller::Group::VisibleView() const
{
    D3D12_VERTEX_BUFFER_VIEW v{};
    if (!m_out) return v;
    v.BufferLocation = m_out->GetGPUVirtualAddress();
    v.SizeInBytes = m_count * 64u;
    v.StrideInBytes = 64u;
    return v;
}

D3D12_VERTEX_BUFFER_VIEW InstanceGpuCuller::Group::VisiblePrevView() const
{
    D3D12_VERTEX_BUFFER_VIEW v{};
    if (!m_outPrev) return v;
    v.BufferLocation = m_outPrev->GetGPUVirtualAddress();
    v.SizeInBytes = m_count * 48u;
    v.StrideInBytes = 48u;
    return v;
}

bool InstanceGpuCuller::Initialize(ID3D12Device* device, const std::wstring& shaderDir, u32 frameCount, std::string* err)
{
    Shutdown();
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!device) return fail("device が null");
    m_impl = std::make_unique<Impl>();
    Impl& I = *m_impl;
    I.device = device;
    m_frameCount = std::max(1u, frameCount);

    D3D12_ROOT_PARAMETER rp[10]{};
    auto root = [&](int i, D3D12_ROOT_PARAMETER_TYPE t, UINT reg)
    {
        rp[i].ParameterType = t;
        rp[i].Descriptor = {reg, 0};
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    root(0, D3D12_ROOT_PARAMETER_TYPE_CBV, 0);   // b0
    root(1, D3D12_ROOT_PARAMETER_TYPE_SRV, 0);   // t0 inst
    root(2, D3D12_ROOT_PARAMETER_TYPE_SRV, 1);   // t1 chunks
    root(3, D3D12_ROOT_PARAMETER_TYPE_SRV, 2);   // t2 prev
    root(4, D3D12_ROOT_PARAMETER_TYPE_SRV, 3);   // t3 indexCounts
    root(5, D3D12_ROOT_PARAMETER_TYPE_UAV, 0);   // u0 out
    root(6, D3D12_ROOT_PARAMETER_TYPE_UAV, 1);   // u1 outPrev
    root(7, D3D12_ROOT_PARAMETER_TYPE_UAV, 2);   // u2 chunkCounts
    root(8, D3D12_ROOT_PARAMETER_TYPE_UAV, 3);   // u3 counters
    root(9, D3D12_ROOT_PARAMETER_TYPE_UAV, 4);   // u4 args
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 10;
    rsd.pParameters = rp;
    ComPtr<ID3DBlob> blob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errBlob)))
        return fail("ルートシグネチャのシリアライズに失敗");
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&I.rootSig))))
        return fail("ルートシグネチャの作成に失敗");

    auto makePso = [&](const wchar_t* cso, ComPtr<ID3D12PipelineState>& out) -> bool
    {
        const std::vector<u8> bc = ReadFileBytes(shaderDir + cso);
        if (bc.empty()) { if (err) *err = "シェーダが読めない: " + std::filesystem::path(cso).string(); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = I.rootSig.Get();
        d.CS = {bc.data(), bc.size()};
        if (FAILED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)))) { if (err) *err = "PSO の作成に失敗"; return false; }
        return true;
    };
    if (!makePso(L"InstanceCullReset_CS.cso", I.psoReset)) return false;
    if (!makePso(L"InstanceCullCount_CS.cso", I.psoCount)) return false;
    if (!makePso(L"InstanceCullScan_CS.cso", I.psoScan)) return false;
    if (!makePso(L"InstanceCullFinalize_CS.cso", I.psoFinalize)) return false;
    if (!makePso(L"InstanceCullEmit_CS.cso", I.psoEmit)) return false;

    I.cb.resize(m_frameCount);
    I.cbMapped.resize(m_frameCount);
    for (u32 f = 0; f < m_frameCount; ++f)
    {
        I.cb[f] = MakeBuffer(device, kCbRing, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE);
        if (!I.cb[f]) return fail("定数バッファの作成に失敗");
        void* m = nullptr;
        D3D12_RANGE none{0, 0};
        I.cb[f]->Map(0, &none, &m);
        I.cbMapped[f] = static_cast<u8*>(m);
    }
    m_ready = true;
    return true;
}

void InstanceGpuCuller::Shutdown()
{
    if (m_impl)
        for (auto& c : m_impl->cb) if (c) c->Unmap(0, nullptr);
    m_impl.reset();
    m_ready = false;
}

std::unique_ptr<InstanceGpuCuller::Group> InstanceGpuCuller::CreateGroup(u32 submeshCount, const std::vector<u32>& indexCounts, std::string* err)
{
    auto fail = [&](const char* m) -> std::unique_ptr<Group> { if (err) *err = m; return nullptr; };
    if (!m_ready) return fail("未初期化");
    if (submeshCount == 0 || indexCounts.size() != static_cast<size_t>(submeshCount) * kLists) return fail("サブメッシュ数 / インデックス数表が不正");
    ID3D12Device* dev = m_impl->device.Get();
    auto G = std::make_unique<Group>();
    G->m_submeshCount = submeshCount;
    // インデックス数表は小さいので UPLOAD ヒープのまま SRV にする（GENERIC_READ で読める）
    G->m_indexCounts = MakeBuffer(dev, indexCounts.size() * 4u, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE);
    if (!G->m_indexCounts) return fail("バッファの作成に失敗");
    {
        void* m = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(G->m_indexCounts->Map(0, &none, &m))) return fail("バッファのマップに失敗");
        std::memcpy(m, indexCounts.data(), indexCounts.size() * 4u);
        G->m_indexCounts->Unmap(0, nullptr);
    }
    // 間接引数（サブメッシュ × list × 5 uint）と list ごとのカウンタ（個数に依らない固定サイズ）
    G->m_args = MakeBuffer(dev, static_cast<u64>(submeshCount) * kLists * 20u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    G->m_counters = MakeBuffer(dev, kLists * 4u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!G->m_args || !G->m_counters) return fail("バッファの作成に失敗");
    G->m_readback.resize(m_frameCount);
    G->m_readbackPending.assign(m_frameCount, false);
    for (u32 f = 0; f < m_frameCount; ++f)
    {
        G->m_readback[f] = MakeBuffer(dev, kLists * 4u, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE);
        if (!G->m_readback[f]) return fail("読み戻しバッファの作成に失敗");
    }
    G->m_argsState = D3D12_RESOURCE_STATE_COMMON;
    return G;
}

bool InstanceGpuCuller::Upload(Group& g, ID3D12GraphicsCommandList* cmd, const InstanceRecord* rec, u32 count, const PrevRecord* prev,
                               std::vector<ComPtr<ID3D12Resource>>* keepAlive, std::string* err)
{
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!m_ready || !cmd) return fail("未初期化");
    if (count == 0 || !rec) return fail("インスタンスが空");
    ID3D12Device* dev = m_impl->device.Get();
    const std::vector<ChunkRecord> chunks = BuildChunks(rec, count);

    auto stage = [&](ComPtr<ID3D12Resource>& dst, const void* data, u64 bytes) -> bool
    {
        ComPtr<ID3D12Resource> nd = MakeBuffer(dev, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> up = MakeBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE);
        if (!nd || !up) return false;
        void* m = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(up->Map(0, &none, &m))) return false;
        std::memcpy(m, data, static_cast<size_t>(bytes));
        up->Unmap(0, nullptr);
        cmd->CopyBufferRegion(nd.Get(), 0, up.Get(), 0, bytes);
        // COMMON → COPY_DEST は暗黙の昇格。コピー後に読み取り状態へ。
        Barrier(cmd, nd.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Retire(dst);
        dst = std::move(nd);
        if (keepAlive) keepAlive->push_back(std::move(up));
        else Retire(up);
        return true;
    };
    if (!stage(g.m_inst, rec, static_cast<u64>(count) * sizeof(InstanceRecord))) return fail("インスタンス表の作成に失敗（VRAM 不足の可能性）");
    if (!stage(g.m_chunks, chunks.data(), chunks.size() * sizeof(ChunkRecord))) return fail("チャンク表の作成に失敗");
    if (prev)
    {
        if (!stage(g.m_prev, prev, static_cast<u64>(count) * sizeof(PrevRecord))) return fail("前フレーム表の作成に失敗");
    }
    else
        Retire(g.m_prev);

    // 作業バッファ（個数に比例）。個数が変わったときだけ作り直す。
    if (!g.m_out || g.m_count != count)
    {
        Retire(g.m_out);
        Retire(g.m_outPrev);
        Retire(g.m_chunkCounts);
        g.m_out = MakeBuffer(dev, static_cast<u64>(count) * 64u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        g.m_chunkCounts = MakeBuffer(dev, static_cast<u64>(kLists) * chunks.size() * 4u, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!g.m_out || !g.m_chunkCounts) return fail("可視ストリームの作成に失敗（VRAM 不足の可能性）");
        g.m_outState = D3D12_RESOURCE_STATE_COMMON;
        g.m_outPrevState = D3D12_RESOURCE_STATE_COMMON;
        g.m_chunkCountsUav = false;
    }
    g.m_count = count;
    g.m_chunkCount = static_cast<u32>(chunks.size());
    g.m_lastCullKey = 0;
    g.m_gpuBytes = static_cast<u64>(count) * (64u + 64u + (g.m_prev ? 48u : 0u) + (g.m_outPrev ? 48u : 0u)) + chunks.size() * (32u + kLists * 4u);
    return true;
}

void InstanceGpuCuller::ClearPrev(Group& g)
{
    if (!g.m_prev) return;
    Retire(g.m_prev);
    g.m_lastCullKey = 0;
}

u64 InstanceGpuCuller::KeyOf(const CullView& v)
{
    u64 h = HashBytes(&v.view, sizeof(v.view));
    return HashBytes(v.color, sizeof(v.color), h) | 1ull;
}

bool InstanceGpuCuller::NeedsCull(const Group& g, const CullView& v)
{
    if (g.m_lastCullKey == 0 || g.m_lastCullKey != KeyOf(v)) return true;
    if (v.writePrev && !g.m_outPrevValid) return true;
    return false;
}

bool InstanceGpuCuller::Cull(ID3D12GraphicsCommandList* cmd, Group& g, const CullView& v, u32 frameSlot, bool countersReadback)
{
    if (!m_ready || !cmd || !g.HasData()) return false;
    Impl& I = *m_impl;
    ID3D12Device* dev = I.device.Get();
    frameSlot %= m_frameCount;
    if (I.cbFrame != frameSlot) { I.cbFrame = frameSlot; I.cbCursor = 0; }
    if (I.cbCursor + kCbBlock > kCbRing) return false;   // 定数リング切れ（1 フレーム 8192 ビュー超）

    // 前フレームのストリームが要るなら作業バッファを用意（初回だけ）
    if (v.writePrev && !g.m_outPrev)
    {
        g.m_outPrev = MakeBuffer(dev, static_cast<u64>(g.m_count) * 48u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!g.m_outPrev) return false;
        g.m_outPrevState = D3D12_RESOURCE_STATE_COMMON;
        g.m_gpuBytes += static_cast<u64>(g.m_count) * 48u;
    }
    const bool writePrev = v.writePrev && g.m_outPrev;

    // ---- 状態: 書く前に UAV へ ----
    // 作業バッファは COMMON で作られている。カウンタ / チャンク個数は以後ずっと UAV として扱う（読み戻しコピー時だけ一時的に別状態）。
    if (!g.m_chunkCountsUav)
    {
        Barrier(cmd, g.m_chunkCounts.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.m_chunkCountsUav = true;
    }
    if (!g.m_countersUav)
    {
        Barrier(cmd, g.m_counters.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.m_countersUav = true;
    }
    Transition(cmd, g.m_out.Get(), g.m_outState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (writePrev) Transition(cmd, g.m_outPrev.Get(), g.m_outPrevState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd, g.m_args.Get(), g.m_argsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ---- 定数 ----
    CullCbData c{};
    std::memcpy(c.planes, v.view.planes, sizeof(c.planes));
    c.cam[0] = v.view.camPos[0]; c.cam[1] = v.view.camPos[1]; c.cam[2] = v.view.camPos[2];
    std::memcpy(c.color, v.color, sizeof(c.color));
    c.p0[0] = g.m_count; c.p0[1] = g.m_chunkCount; c.p0[2] = v.view.lodBias; c.p0[3] = std::min(v.view.maxList, kLists - 1u);
    c.p1[0] = v.view.texelWorld; c.p1[1] = v.view.lodScale;
    c.p2[0] = writePrev ? 1u : 0u; c.p2[1] = g.m_submeshCount; c.p2[2] = g.m_prev ? 48u : 64u;
    std::memcpy(I.cbMapped[frameSlot] + I.cbCursor, &c, sizeof(c));
    const D3D12_GPU_VIRTUAL_ADDRESS cbVa = I.cb[frameSlot]->GetGPUVirtualAddress() + I.cbCursor;
    I.cbCursor += kCbBlock;

    cmd->SetComputeRootSignature(I.rootSig.Get());
    cmd->SetComputeRootConstantBufferView(0, cbVa);
    ID3D12Resource* prevSrc = g.m_prev ? g.m_prev.Get() : g.m_inst.Get();
    cmd->SetComputeRootShaderResourceView(1, g.m_inst->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, g.m_chunks->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(3, prevSrc->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(4, g.m_indexCounts->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(5, g.m_out->GetGPUVirtualAddress());
    // outPrev が無いときも有効なアドレスを渡す（書かない）。
    cmd->SetComputeRootUnorderedAccessView(6, (g.m_outPrev ? g.m_outPrev.Get() : g.m_out.Get())->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(7, g.m_chunkCounts->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, g.m_counters->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(9, g.m_args->GetGPUVirtualAddress());

    const u32 nChunks = g.m_chunkCount;
    const u32 gx = std::max(1u, std::min(nChunks, 65535u));
    const u32 gy = std::max(1u, (nChunks + 65534u) / 65535u);

    cmd->SetPipelineState(I.psoReset.Get());
    cmd->Dispatch(1, 1, 1);
    UavBarrier(cmd, g.m_counters.Get());
    cmd->SetPipelineState(I.psoCount.Get());
    cmd->Dispatch(gx, gy, 1);
    UavBarrier(cmd, g.m_chunkCounts.Get());
    cmd->SetPipelineState(I.psoScan.Get());
    cmd->Dispatch(kLists, 1, 1);
    UavBarrier(cmd, g.m_chunkCounts.Get());
    UavBarrier(cmd, g.m_counters.Get());
    cmd->SetPipelineState(I.psoFinalize.Get());
    cmd->Dispatch((g.m_submeshCount * kLists + 63) / 64, 1, 1);
    cmd->SetPipelineState(I.psoEmit.Get());
    cmd->Dispatch(gx, gy, 1);
    UavBarrier(cmd, g.m_out.Get());
    if (writePrev) UavBarrier(cmd, g.m_outPrev.Get());
    UavBarrier(cmd, g.m_args.Get());

    // ---- 統計の読み戻しコピー（カウンタ → readback[frameSlot]）----
    if (countersReadback)
    {
        Barrier(cmd, g.m_counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(g.m_readback[frameSlot].Get(), 0, g.m_counters.Get(), 0, kLists * 4u);
        Barrier(cmd, g.m_counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.m_readbackPending[frameSlot] = true;
    }

    // ---- 描画用の状態へ ----
    Transition(cmd, g.m_out.Get(), g.m_outState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (writePrev) Transition(cmd, g.m_outPrev.Get(), g.m_outPrevState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(cmd, g.m_args.Get(), g.m_argsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    g.m_outPrevValid = writePrev;
    g.m_culledOnce = true;
    g.m_lastCullKey = KeyOf(v);
    return true;
}

bool InstanceGpuCuller::ReadCounters(Group& g, u32 frameSlot, u32 outCounts[kLists])
{
    if (frameSlot >= g.m_readback.size() || !g.m_readbackPending[frameSlot]) return false;
    D3D12_RANGE rr{0, kLists * 4u};
    void* m = nullptr;
    if (FAILED(g.m_readback[frameSlot]->Map(0, &rr, &m))) return false;
    std::memcpy(outCounts, m, kLists * 4u);
    D3D12_RANGE none{0, 0};
    g.m_readback[frameSlot]->Unmap(0, &none);
    return true;
}

} // namespace dx12e::instgpu
