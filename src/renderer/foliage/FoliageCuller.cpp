#include "renderer/foliage/FoliageCuller.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/Logger.h"

using Microsoft::WRL::ComPtr;

namespace dx12e::foliage
{
namespace
{
// HLSL の cbuffer CullCB（shaders/foliage/FoliageCull.hlsl）と 1 バイトも違わないこと。
struct CullCbData
{
    f32 planes[6][4];
    f32 camDist[4];
    f32 lodDist[4];
    f32 paramsA[4];
    f32 paramsB[4];
    u32 flags[4];
    u32 lodCount[4];
    f32 layer0[4], layer1[4], layer2[4];
    u32 counts[4];
    u32 fin[4];
    u32 caps[4];
    f32 prevVP[16];
    f32 hzb[4];
};
static_assert(sizeof(CullCbData) == 368, "CullCB のレイアウトが HLSL とずれている");
constexpr u32 kCbBlock = 512;                 // 256 の倍数（CBV の配置制約）
constexpr u32 kCbRing  = 64u * kCbBlock;      // フレームあたり 64 ディスパッチ

std::vector<u8> ReadFileBytes(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, u64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                  D3D12_RESOURCE_STATES state)
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
    ComPtr<ID3D12Resource> r;
    // ★DEFAULT ヒープのバッファは常に COMMON で作られる（初期状態の指定は無視されて警告が出る）。呼び出し側が必要な遷移を明示する。
    if (heap == D3D12_HEAP_TYPE_DEFAULT) state = D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// DEFAULT バッファを作って data をアップロードする（COPY_DEST → afterState）。ステージングを keep へ積む。
ComPtr<ID3D12Resource> MakeDefaultWithData(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, const void* data, u64 bytes,
                                           D3D12_RESOURCE_STATES afterState, D3D12_RESOURCE_FLAGS flags,
                                           std::vector<ComPtr<ID3D12Resource>>& keep)
{
    ComPtr<ID3D12Resource> dst = MakeBuffer(dev, bytes, D3D12_HEAP_TYPE_DEFAULT, flags, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!dst) return nullptr;
    if (bytes > 0 && data)
    {
        ComPtr<ID3D12Resource> up = MakeBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up) return nullptr;
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(up->Map(0, &none, &mapped))) return nullptr;
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        up->Unmap(0, nullptr);
        cmd->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, bytes);
        keep.push_back(std::move(up));
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dst.Get();
    // コピーした場合は COMMON からの暗黙の昇格で COPY_DEST になっている。データが無いときは COMMON のまま。
    b.Transition.StateBefore = (bytes > 0 && data) ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = afterState;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    return dst;
}

void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES next)
{
    if (cur == next) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = next;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    cur = next;
}
void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cmd->ResourceBarrier(1, &b);
}
} // namespace

// ===========================================================================
struct FoliageCuller::Impl
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoReset, psoCount, psoScan, psoEmit, psoFinalize;
    // HZB 専用の小さなシェーダ可視ヒープ: [0] = HZB の SRV / [1] = ダミー（1x1 R32_FLOAT）
    ComPtr<ID3D12DescriptorHeap> heap;
    u32 descSize = 0;
    ComPtr<ID3D12Resource> dummyHzb;
    ID3D12Resource* hzbRes = nullptr;
    u32 hzbMips = 0;
    // 定数の upload リング（フレームごと）
    std::vector<ComPtr<ID3D12Resource>> cb;
    std::vector<u8*> cbMapped;
    u32 cbCursor = 0;
    u32 cbFrame = 0xFFFFFFFFu;
};

FoliageCuller::FoliageCuller() = default;
FoliageCuller::~FoliageCuller() { Shutdown(); }

FoliageCuller::Layer::~Layer() = default;

u32 FoliageCuller::Layer::ListBase(u32 view) const
{
    if (view == 0) return 0;
    return m_S * m_capMain + (view - 1) * m_S * m_capShadow;
}

D3D12_VERTEX_BUFFER_VIEW FoliageCuller::Layer::VisibleView(u32 view, u32 slot) const
{
    D3D12_VERTEX_BUFFER_VIEW v{};
    const u32 cap = CapOf(view);
    v.BufferLocation = m_visible->GetGPUVirtualAddress() + static_cast<u64>(ListBase(view) + slot * cap) * 64u;
    v.SizeInBytes = cap * 64u;
    v.StrideInBytes = 64u;
    return v;
}

bool FoliageCuller::Initialize(ID3D12Device* device, const std::wstring& shaderDir, u32 frameCount, std::string* err)
{
    Shutdown();
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!device) return fail("device が null");
    m_impl = std::make_unique<Impl>();
    Impl& I = *m_impl;
    I.device = device;
    m_frameCount = std::max(1u, frameCount);

    // ルートシグネチャ
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 2;   // t2 = HZB
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rp[9]{};
    auto rootCbv = [&](int i, UINT reg)
    {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[i].Descriptor = {reg, 0};
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    auto rootSrv = [&](int i, UINT reg)
    {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[i].Descriptor = {reg, 0};
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    auto rootUav = [&](int i, UINT reg)
    {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[i].Descriptor = {reg, 0};
        rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    rootCbv(0, 0);   // b0
    rootSrv(1, 0);   // t0 src
    rootSrv(2, 1);   // t1 chunks
    rootUav(3, 0);   // u0 visible
    rootUav(4, 1);   // u1 counters
    rootSrv(5, 3);   // t3 groupSlot
    rootUav(6, 2);   // u2 args
    rp[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[7].DescriptorTable = {1, &range};
    rp[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootUav(8, 3);   // u3 チャンクごとの個数 / プレフィックス和
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 9;
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
    if (!makePso(L"FoliageReset_CS.cso", I.psoReset)) return false;
    if (!makePso(L"FoliageCount_CS.cso", I.psoCount)) return false;
    if (!makePso(L"FoliageScan_CS.cso", I.psoScan)) return false;
    if (!makePso(L"FoliageEmit_CS.cso", I.psoEmit)) return false;
    if (!makePso(L"FoliageFinalize_CS.cso", I.psoFinalize)) return false;

    // HZB 専用ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 * m_frameCount;   // フレームスロットごとに 2 個（[slot*2] = HZB or ダミー / [slot*2+1] = 常にダミー）
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&I.heap)))) return fail("ヒープの作成に失敗");
    I.descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    {
        // ダミー HZB（1x1 R32_FLOAT = 1.0 = 何も遮蔽しない）。SRV だけあればよく、内容は読まれない（HZB 無効時は分岐で読まない）。
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc = {1, 0};
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&I.dummyHzb))))
            return fail("ダミー HZB の作成に失敗");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = I.heap->GetCPUDescriptorHandleForHeapStart();
        for (u32 i = 0; i < 2 * m_frameCount; ++i)
        {
            device->CreateShaderResourceView(I.dummyHzb.Get(), &sd, h);      // 全部ダミーで初期化（毎フレーム Cull が [slot*2] だけ書き直す）
            h.ptr += I.descSize;
        }
    }
    // 定数リング
    I.cb.resize(m_frameCount);
    I.cbMapped.resize(m_frameCount);
    for (u32 f = 0; f < m_frameCount; ++f)
    {
        I.cb[f] = MakeBuffer(device, kCbRing, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!I.cb[f]) return fail("定数バッファの作成に失敗");
        void* m = nullptr;
        D3D12_RANGE none{0, 0};
        I.cb[f]->Map(0, &none, &m);
        I.cbMapped[f] = static_cast<u8*>(m);
    }
    m_ready = true;
    return true;
}

void FoliageCuller::Shutdown()
{
    if (m_impl)
    {
        for (auto& c : m_impl->cb) if (c) c->Unmap(0, nullptr);
    }
    m_impl.reset();
    m_ready = false;
}

void FoliageCuller::SetHzbResource(ID3D12Resource* hzb, u32 mips)
{
    // ★記述子はここでは書かない。Cull() がフレームスロットごとの記述子へ「毎フレーム」書く。
    //   （以前は 1 個の共有記述子を「ポインタが変わったときだけ」書き換えていた。HZB が作り直されて
    //     同じアドレスに別のリソースが入ると更新が飛び、破棄済みリソースを指す記述子を読んで GPU がハングした。
    //     また、フライト中の前フレームが読んでいる記述子を上書きしてもいた。）
    if (!m_impl) return;
    m_impl->hzbRes = hzb;
    m_impl->hzbMips = mips;
}

std::unique_ptr<FoliageCuller::Layer> FoliageCuller::CreateLayer(const LayerGpuDesc& d, ID3D12GraphicsCommandList* cmd,
                                                                 std::vector<ComPtr<ID3D12Resource>>& keep, std::string* err)
{
    auto fail = [&](const char* m) -> std::unique_ptr<Layer> { if (err) *err = m; return nullptr; };
    if (!m_ready || !cmd) return fail("未初期化");
    if (d.numViews == 0 || d.numViews > 4 || d.slotsPerView == 0) return fail("ビュー数 / list 数が不正");
    if (d.groupSlot.size() != d.groupIndexCount.size()) return fail("groupSlot と groupIndexCount の数が違う");
    ID3D12Device* dev = m_impl->device.Get();
    auto L = std::make_unique<Layer>();
    L->m_instanceCount = d.instanceCount;
    L->m_chunkCount = d.chunkCount;
    L->m_numViews = d.numViews;
    L->m_S = d.slotsPerView;
    L->m_lodStride = d.lodStride;
    L->m_G = static_cast<u32>(d.groupSlot.size());
    L->m_capMain = std::max(16u, d.capMain);
    L->m_capShadow = std::max(16u, d.capShadow);
    for (u32 i = 0; i < d.instanceCount; ++i)
        L->m_maxScale = std::max(L->m_maxScale, std::fabs(UnpackScale(d.instances[i].rotScale)));

    const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    L->m_src = MakeDefaultWithData(dev, cmd, d.instances, static_cast<u64>(d.instanceCount) * sizeof(FoliageInstance), srvState,
                                   D3D12_RESOURCE_FLAG_NONE, keep);
    L->m_chunks = MakeDefaultWithData(dev, cmd, d.chunks, static_cast<u64>(d.chunkCount) * sizeof(FoliageChunk), srvState,
                                      D3D12_RESOURCE_FLAG_NONE, keep);
    L->m_groupSlot = MakeDefaultWithData(dev, cmd, d.groupSlot.data(), static_cast<u64>(d.groupSlot.size()) * 4u, srvState,
                                         D3D12_RESOURCE_FLAG_NONE, keep);
    if (!L->m_src || !L->m_chunks || !L->m_groupSlot) return fail("バッファの作成に失敗");

    const u64 visInstances = static_cast<u64>(L->m_S) * L->m_capMain
                           + static_cast<u64>(L->m_numViews - 1) * L->m_S * L->m_capShadow;
    L->m_visible = MakeBuffer(dev, visInstances * 64u, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    L->m_counters = MakeBuffer(dev, static_cast<u64>(L->m_numViews) * L->m_S * 4u, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // チャンクごと・list ごとの個数（Count が書き、Scan が排他的プレフィックス和にする）。作業用なのでビューごとに numViews * S * chunkCount
    L->m_chunkCounts = MakeBuffer(dev, static_cast<u64>(L->m_numViews) * L->m_S * std::max(1u, L->m_chunkCount) * 4u, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!L->m_visible || !L->m_counters || !L->m_chunkCounts) return fail("compact ストリームの作成に失敗（VRAM 不足の可能性）");
    {
        // COMMON → UAV（以後は Cull が状態を追跡する）
        D3D12_RESOURCE_BARRIER b[3]{};
        for (int i = 0; i < 3; ++i)
        {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.pResource = (i == 0 ? L->m_visible : (i == 1 ? L->m_counters : L->m_chunkCounts)).Get();
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmd->ResourceBarrier(3, b);
    }

    // 間接引数（DrawIndexedInstanced: IndexCount, InstanceCount, StartIndex, BaseVertex, StartInstance）。InstanceCount は毎フレーム CS が書く
    std::vector<u32> args(static_cast<size_t>(L->m_numViews) * L->m_G * 5u, 0u);
    for (u32 v = 0; v < L->m_numViews; ++v)
        for (u32 g = 0; g < L->m_G; ++g) args[(static_cast<size_t>(v) * L->m_G + g) * 5u] = d.groupIndexCount[g];
    L->m_args = MakeDefaultWithData(dev, cmd, args.data(), args.size() * 4u, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, keep);
    if (!L->m_args) return fail("間接引数の作成に失敗");

    L->m_readback.resize(m_frameCount);
    L->m_readbackPending.assign(m_frameCount, false);
    for (u32 f = 0; f < m_frameCount; ++f)
    {
        L->m_readback[f] = MakeBuffer(dev, static_cast<u64>(L->m_numViews) * L->m_S * 4u, D3D12_HEAP_TYPE_READBACK,
                                      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!L->m_readback[f]) return fail("読み戻しバッファの作成に失敗");
    }
    L->m_gpuBytes = static_cast<u64>(d.instanceCount) * 32u + static_cast<u64>(d.chunkCount) * 32u + visInstances * 64u
                  + args.size() * 4u + static_cast<u64>(L->m_numViews) * L->m_S * 4u
                  + static_cast<u64>(L->m_numViews) * L->m_S * std::max(1u, L->m_chunkCount) * 4u;
    L->m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    L->m_argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return L;
}

bool FoliageCuller::Cull(ID3D12GraphicsCommandList* cmd, Layer& L, const Affine34& layerWorld,
                         const std::vector<CullDispatch>& disp, u32 frameSlot, ID3D12DescriptorHeap* restoreHeap)
{
    if (!m_ready || !cmd || !L.m_visible) return false;
    Impl& I = *m_impl;
    frameSlot %= m_frameCount;
    if (I.cbFrame != frameSlot) { I.cbFrame = frameSlot; I.cbCursor = 0; }

    // ---- 状態: 書く前に UAV へ ----
    Transition(cmd, L.m_visible.Get(), L.m_visState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd, L.m_args.Get(), L.m_argsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {I.heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(I.rootSig.Get());
    // HZB テーブル（このフレームスロットの記述子。有効な HZB があればそれ、無ければダミーを毎フレーム書く）
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = I.heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(frameSlot) * 2u * I.descSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MostDetailedMip = 0;
        if (I.hzbRes && I.hzbMips > 0)
        {
            sd.Texture2D.MipLevels = I.hzbMips;
            I.device->CreateShaderResourceView(I.hzbRes, &sd, h);
        }
        else
        {
            sd.Texture2D.MipLevels = 1;
            I.device->CreateShaderResourceView(I.dummyHzb.Get(), &sd, h);
        }
    }
    D3D12_GPU_DESCRIPTOR_HANDLE hzbTable = I.heap->GetGPUDescriptorHandleForHeapStart();
    hzbTable.ptr += static_cast<UINT64>(frameSlot) * 2u * I.descSize;
    cmd->SetComputeRootDescriptorTable(7, hzbTable);
    cmd->SetComputeRootShaderResourceView(1, L.m_src->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, L.m_chunks->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, L.m_visible->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, L.m_counters->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(5, L.m_groupSlot->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, L.m_args->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, L.m_chunkCounts->GetGPUVirtualAddress());

    auto writeCb = [&](const CullDispatch& cd, u32 viewIdx) -> D3D12_GPU_VIRTUAL_ADDRESS
    {
        if (I.cbCursor + kCbBlock > kCbRing) return 0;   // 溢れたら以降のディスパッチは無し
        CullCbData c{};
        const CullParams& p = cd.p;
        std::memcpy(c.planes, p.planes, sizeof(c.planes));
        c.camDist[0] = p.camPos[0]; c.camDist[1] = p.camPos[1]; c.camDist[2] = p.camPos[2]; c.camDist[3] = p.cullDist;
        c.lodDist[0] = p.lodDist[0]; c.lodDist[1] = p.lodDist[1]; c.lodDist[2] = p.lodDist[2]; c.lodDist[3] = p.thinStart;
        c.paramsA[0] = p.lodFade; c.paramsA[1] = p.boundCenterY; c.paramsA[2] = p.boundRadius; c.paramsA[3] = p.radiusScale;
        const f32 margin = (p.boundRadius + std::fabs(p.boundCenterY)) * L.m_maxScale * p.layerScale * p.radiusScale;
        c.paramsB[0] = p.layerScale; c.paramsB[1] = margin; c.paramsB[2] = cd.rectInflatePx; c.paramsB[3] = 0.0f;
        const bool useHzb = cd.hzb && !p.shadowPass && I.hzbRes && I.hzbMips > 0;
        c.flags[0] = p.variantCount; c.flags[1] = p.shadowMaxLod;
        c.flags[2] = (p.shadowPass ? 1u : 0u) | (useHzb ? 2u : 0u);
        c.flags[3] = L.m_lodStride;
        for (int i = 0; i < 4; ++i) c.lodCount[i] = p.lodCount[i];
        for (int i = 0; i < 4; ++i) { c.layer0[i] = layerWorld.m[0][i]; c.layer1[i] = layerWorld.m[1][i]; c.layer2[i] = layerWorld.m[2][i]; }
        c.counts[0] = L.m_chunkCount;
        c.counts[1] = L.ListBase(viewIdx);
        c.counts[2] = L.CapOf(viewIdx);
        c.counts[3] = viewIdx * L.m_S;
        c.fin[0] = L.m_G; c.fin[1] = L.m_S; c.fin[2] = L.m_numViews;
        c.caps[0] = L.m_capMain; c.caps[1] = L.m_capShadow;
        {
            const DirectX::XMMATRIX t = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&cd.prevVP));
            DirectX::XMFLOAT4X4 tf;
            DirectX::XMStoreFloat4x4(&tf, t);
            std::memcpy(c.prevVP, &tf, 64);
        }
        c.hzb[0] = cd.hzbW; c.hzb[1] = cd.hzbH; c.hzb[2] = static_cast<f32>(cd.hzbMips); c.hzb[3] = 0.0f;
        std::memcpy(I.cbMapped[frameSlot] + I.cbCursor, &c, sizeof(c));
        const D3D12_GPU_VIRTUAL_ADDRESS va = I.cb[frameSlot]->GetGPUVirtualAddress() + I.cbCursor;
        I.cbCursor += kCbBlock;
        return va;
    };

    // ---- リセット（カウンタ 0）----
    {
        // fin だけ要る（numViews * S 個のカウンタを 0 にする）。CullCbData の先頭を作り直すより、最初のディスパッチ用 CB を流用する。
        CullDispatch dummy;
        if (!disp.empty()) dummy = disp[0];
        const D3D12_GPU_VIRTUAL_ADDRESS va = writeCb(dummy, 0);
        if (!va) return false;
        cmd->SetComputeRootConstantBufferView(0, va);
        cmd->SetPipelineState(I.psoReset.Get());
        const u32 n = L.m_numViews * L.m_S;
        cmd->Dispatch((n + 63) / 64, 1, 1);
        UavBarrier(cmd, L.m_counters.Get());
    }
    // ---- ビューごとのカリング（決定論的な compact: Count → Scan → Emit）----
    //   各ビューの CB は 3 段で使い回す（writeCb は 1 ビューにつき 1 回）。段の間は UAV バリア。
    std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, bool>> cbs;   // (va, 有効)
    for (const CullDispatch& cd : disp)
    {
        if (cd.view >= L.m_numViews) { cbs.push_back({0, false}); continue; }
        const D3D12_GPU_VIRTUAL_ADDRESS va = writeCb(cd, cd.view);
        cbs.push_back({va, va != 0 && L.m_chunkCount > 0});
    }
    const u32 nChunks = L.m_chunkCount;
    const u32 gx = std::max(1u, std::min(nChunks, 65535u));
    const u32 gy = std::max(1u, (nChunks + 65534u) / 65535u);
    auto pass = [&](ID3D12PipelineState* pso, auto&& dispatch)
    {
        cmd->SetPipelineState(pso);
        for (const auto& c : cbs)
        {
            if (!c.second) continue;
            cmd->SetComputeRootConstantBufferView(0, c.first);
            dispatch();
        }
        UavBarrier(cmd, L.m_chunkCounts.Get());
        UavBarrier(cmd, L.m_counters.Get());
    };
    pass(I.psoCount.Get(), [&] { cmd->Dispatch(gx, gy, 1); });
    pass(I.psoScan.Get(), [&] { cmd->Dispatch(L.m_S, 1, 1); });
    pass(I.psoEmit.Get(), [&] { cmd->Dispatch(gx, gy, 1); });
    UavBarrier(cmd, L.m_visible.Get());
    // ---- 引数の確定 ----
    {
        CullDispatch dummy;
        if (!disp.empty()) dummy = disp[0];
        const D3D12_GPU_VIRTUAL_ADDRESS va = writeCb(dummy, 0);
        if (!va) return false;
        cmd->SetComputeRootConstantBufferView(0, va);
        cmd->SetPipelineState(I.psoFinalize.Get());
        const u32 n = L.m_G * L.m_numViews;
        if (n > 0) cmd->Dispatch((n + 63) / 64, 1, 1);
    }
    UavBarrier(cmd, L.m_args.Get());

    // ---- 統計の読み戻しコピー（カウンタ → readback[frameSlot]）----
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = L.m_counters.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(L.m_readback[frameSlot].Get(), 0, L.m_counters.Get(), 0,
                              static_cast<u64>(L.m_numViews) * L.m_S * 4u);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
        L.m_readbackPending[frameSlot] = true;
    }

    // ---- 描画用の状態へ ----
    Transition(cmd, L.m_visible.Get(), L.m_visState, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(cmd, L.m_args.Get(), L.m_argsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    L.m_culledOnce = true;

    if (restoreHeap)
    {
        ID3D12DescriptorHeap* h[] = {restoreHeap};
        cmd->SetDescriptorHeaps(1, h);
    }
    return true;
}

bool FoliageCuller::ReadCounters(Layer& L, u32 frameSlot, LayerCounters& out)
{
    out = {};
    if (frameSlot >= L.m_readback.size() || !L.m_readbackPending[frameSlot]) return false;
    const u32 n = L.m_numViews * L.m_S;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(n) * 4};
    void* m = nullptr;
    if (FAILED(L.m_readback[frameSlot]->Map(0, &rr, &m))) return false;
    out.counts.assign(static_cast<const u32*>(m), static_cast<const u32*>(m) + n);
    D3D12_RANGE none{0, 0};
    L.m_readback[frameSlot]->Unmap(0, &none);
    out.valid = true;
    out.numViews = L.m_numViews;
    out.slotsPerView = L.m_S;
    return true;
}

} // namespace dx12e::foliage
