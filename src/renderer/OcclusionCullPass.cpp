#include "renderer/OcclusionCullPass.h"

#include "renderer/DrawItem.h"   // OcclusionBounds
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{
using namespace DirectX;

namespace
{
// HLSL の cbuffer HiZCullCB(b0) とバイト単位で一致させること。
struct HiZCullCB
{
    XMFLOAT4X4 viewProj;     // 64B
    XMFLOAT4   viewport;     // 16B  xy=原点(px) zw=サイズ(px)
    XMFLOAT4   hzbParams;    // 16B  xy=mip0 解像度 / z=ミップ数 / w=アイテム数
};
static_assert(sizeof(HiZCullCB) == 96, "HiZCullCB layout mismatch with HiZCull.hlsl");
constexpr u32 kCullCBNum32 = sizeof(HiZCullCB) / 4;

// HLSL の struct ItemBounds と一致（float4 x2）。
struct ItemBoundsGPU
{
    XMFLOAT4 aabbMin;
    XMFLOAT4 aabbMax;
};
static_assert(sizeof(ItemBoundsGPU) == 32, "ItemBoundsGPU layout mismatch with HiZCull.hlsl");

constexpr u32 kThreadsPerGroup = 64;   // HLSL の [numthreads(64,1,1)] と一致させること
constexpr u32 kStatsSlots      = 2;    // [0]=隠れていた数 / [1]=判定した数
constexpr u32 kMinCapacity     = 1024;

Microsoft::WRL::ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, u64 bytes,
                                                  D3D12_HEAP_TYPE heapType,
                                                  D3D12_RESOURCE_FLAGS flags,
                                                  D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heapType;

    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width            = bytes;
    d.Height           = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc       = {1, 0};
    d.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags            = flags;

    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    ThrowIfFailed(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state,
                                               nullptr, IID_PPV_ARGS(&res)));
    return res;
}
} // namespace

void OcclusionCullPass::Initialize(GraphicsDevice& device, const std::wstring& shaderDir)
{
    m_shaderDir = shaderDir;
    auto* dev = device.GetDevice();

    // --- RootSignature ---
    // b0(32bit定数 x24) + t0(ルートSRV=AABB) + t1(SRVテーブル=Hi-Z) + u0/u1(ルートUAV)
    // DWORD: 24 + 2 + 1 + 2 + 2 = 31 / 64。
    // ★Hi-Z はテクスチャなのでルートディスクリプタにできない（バッファ限定）＝テーブル。
    //   AABB / 可視性 / 統計はバッファなのでルートディスクリプタで済む（ヒープ確保が要らない）。
    {
        D3D12_DESCRIPTOR_RANGE hzbRange{};
        hzbRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        hzbRange.NumDescriptors     = 1;
        hzbRange.BaseShaderRegister = 1;   // t1

        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0
        params[0].Constants.Num32BitValues = kCullCBNum32;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;  // t0

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &hzbRange;

        params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 0;  // u0

        params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].Descriptor.ShaderRegister = 1;  // u1

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(params);
        desc.pParameters   = params;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    RecreatePipelines(device);

    // 統計バッファ + 0 クリア元 + 読み戻し。
    m_statsBuf = MakeBuffer(dev, kStatsSlots * sizeof(u32), D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_statsBuf->SetName(L"OcclusionStats");

    m_statsZero = MakeBuffer(dev, kStatsSlots * sizeof(u32), D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        void* p = nullptr;
        D3D12_RANGE nr{0, 0};
        ThrowIfFailed(m_statsZero->Map(0, &nr, &p));
        std::memset(p, 0, kStatsSlots * sizeof(u32));
        m_statsZero->Unmap(0, nullptr);
    }

    for (u32 f = 0; f < kFrameCount; ++f)
    {
        m_statsReadback[f] = MakeBuffer(dev, kStatsSlots * sizeof(u32), D3D12_HEAP_TYPE_READBACK,
                                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        m_statsPending[f] = false;
    }

    EnsureCapacity(device, kMinCapacity);
}

void OcclusionCullPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + L"HiZCull_CS.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.CS = { bc.GetData(), bc.GetSize() };
    ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void OcclusionCullPass::EnsureCapacity(GraphicsDevice& device, u32 count)
{
    if (count <= m_capacity) return;

    // 1.5 倍ずつ伸ばす（毎フレーム再確保しないため）。
    u32 cap = (m_capacity > 0) ? m_capacity : kMinCapacity;
    while (cap < count) cap = cap + cap / 2 + 1;

    auto* dev = device.GetDevice();

    for (u32 f = 0; f < kFrameCount; ++f)
    {
        m_boundsBuf[f] = MakeBuffer(dev, static_cast<u64>(cap) * sizeof(ItemBoundsGPU),
                                    D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        D3D12_RANGE nr{0, 0};
        ThrowIfFailed(m_boundsBuf[f]->Map(0, &nr, &p));
        m_boundsMapped[f] = static_cast<u8*>(p);
    }

    m_visBuf = MakeBuffer(dev, static_cast<u64>(cap) * kPredicateStride,
                          D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_visBuf->SetName(L"OcclusionVisibility");
    m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    m_capacity = cap;
    Logger::Info("オクルージョンカリング: 判定バッファを {} 件ぶん確保", cap);
}

void OcclusionCullPass::Dispatch(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                                 const std::vector<OcclusionBounds>& bounds, const Params& p,
                                 D3D12_GPU_DESCRIPTOR_HANDLE hzbSrvGpu, u32 frameIndex)
{
    m_lastSlotCount = 0;
    if (!IsReady() || !cmd || frameIndex >= kFrameCount) return;
    const u32 n = static_cast<u32>(bounds.size());
    if (n == 0) return;

    EnsureCapacity(device, n);
    if (!m_boundsMapped[frameIndex] || !m_visBuf) return;

    {
        auto* dst = reinterpret_cast<ItemBoundsGPU*>(m_boundsMapped[frameIndex]);
        for (u32 i = 0; i < n; ++i)
        {
            const OcclusionBounds& b = bounds[i];
            dst[i].aabbMin = XMFLOAT4(b.aabbMin.x, b.aabbMin.y, b.aabbMin.z, 0.0f);
            dst[i].aabbMax = XMFLOAT4(b.aabbMax.x, b.aabbMax.y, b.aabbMax.z, 0.0f);
        }
    }
    m_lastSlotCount = n;

    // 統計を 0 クリア（毎フレーム積み直す）。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_statsBuf.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_statsBuf.Get(), 0, m_statsZero.Get(), 0, kStatsSlots * sizeof(u32));
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
    }

    // 可視性バッファを UAV へ戻す（前フレーム末は PREDICATION 状態）。
    if (m_visState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_visBuf.Get();
        b.Transition.StateBefore = m_visState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    HiZCullCB cb{};
    // cbuffer の float4x4 は既定で列優先解釈される。XMStoreFloat4x4 は行優先で書くので、
    // 転置して入れると HLSL 側で元の行列に戻り、mul(row, mat) が期待通りに効く
    // （ClusteredLightCulling と同じ作法）。
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&p.viewProj)));
    cb.viewport  = XMFLOAT4(p.vpX, p.vpY, p.vpW, p.vpH);
    cb.hzbParams = XMFLOAT4(p.hzbW, p.hzbH, static_cast<f32>(p.mipCount), static_cast<f32>(n));

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kCullCBNum32, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, m_boundsBuf[frameIndex]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(2, hzbSrvGpu);
    cmd->SetComputeRootUnorderedAccessView(3, m_visBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, m_statsBuf->GetGPUVirtualAddress());
    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch((n + kThreadsPerGroup - 1) / kThreadsPerGroup, 1, 1);

    // 可視性バッファを述語として使える状態へ。
    // ★PREDICATION は INDIRECT_ARGUMENT の別名。ここを忘れるとデバッグレイヤが落とす。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_visBuf.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PREDICATION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_visState = D3D12_RESOURCE_STATE_PREDICATION;
    }

    // 統計を読み戻しへコピー（数フレーム遅れで CPU が拾う。表示専用）。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_statsBuf.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        cmd->CopyResource(m_statsReadback[frameIndex].Get(), m_statsBuf.Get());
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmd->ResourceBarrier(1, &b);
        m_statsPending[frameIndex] = true;
    }
}

void OcclusionCullPass::CollectStats(u32 frameIndex)
{
    if (frameIndex >= kFrameCount || !m_statsPending[frameIndex]) return;
    // このフレームスロットの GPU 作業は BeginFrame のフェンス待ちで完了済み
    // （GpuTimer の読み戻しと同じ前提）。
    void* p = nullptr;
    D3D12_RANGE rr{0, kStatsSlots * sizeof(u32)};
    if (FAILED(m_statsReadback[frameIndex]->Map(0, &rr, &p)) || !p) return;
    const u32* s = static_cast<const u32*>(p);
    m_statOccluded = s[0];
    m_statTested   = s[1];
    D3D12_RANGE nw{0, 0};
    m_statsReadback[frameIndex]->Unmap(0, &nw);
}

void OcclusionCullPass::Shutdown()
{
    for (u32 f = 0; f < kFrameCount; ++f)
    {
        if (m_boundsBuf[f] && m_boundsMapped[f]) m_boundsBuf[f]->Unmap(0, nullptr);
        m_boundsMapped[f] = nullptr;
        m_boundsBuf[f].Reset();
        m_statsReadback[f].Reset();
        m_statsPending[f] = false;
    }
    m_visBuf.Reset();
    m_statsBuf.Reset();
    m_statsZero.Reset();
    m_pso.Reset();
    m_rootSig.Reset();
    m_capacity = 0;
}

} // namespace dx12e
