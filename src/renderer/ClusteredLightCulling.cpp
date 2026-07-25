#include "renderer/ClusteredLightCulling.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstring>

using namespace DirectX;

namespace dx12e
{
namespace
{

// shaders/forward/ClusterCommon.hlsli の cbuffer ClusterCB(b0) と完全一致（28 DWORD）。
struct ClusterCB
{
    XMFLOAT4X4 viewMat;    //  0..15  転置済み（HLSL は mul(row, mat) 規約）
    u32        gridDims[4];// 16..19  gridX, gridY, gridZ, clusterCount
    float      zParams[4]; // 20..23  zNear, zFarCluster, zFarCamera, 未使用
    float      misc[4];    // 24..27  1/proj_11, 1/proj_22, 灯数, 1クラスタ最大灯数
};
static_assert(sizeof(ClusterCB) == 28 * sizeof(u32), "ClusterCB must be 28 DWORDs");
constexpr UINT kClusterCBNum32 = 28;

// dispatch のスレッドグループ数（HLSL の [numthreads(64,1,1)] と一致させること）。
constexpr u32 kThreadsPerGroup = 64;
constexpr u32 kGroupCount = (cluster::kClusterCount + kThreadsPerGroup - 1) / kThreadsPerGroup;

constexpr u64 kAabbBytes  = static_cast<u64>(cluster::kClusterCount) * 32;               // 108 KB
constexpr u64 kIndexBytes = static_cast<u64>(cluster::kClusterCount)
                          * cluster::kMaxLightsPerCluster * sizeof(u32);                 // 1.69 MB
constexpr u64 kCountBytes = static_cast<u64>(cluster::kClusterCount) * sizeof(u32);      // 13.5 KB
constexpr u64 kLightBytes = static_cast<u64>(cluster::kMaxSceneLights) * 64;             // 64 KB

} // namespace

void ClusteredLightCulling::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                                       const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_srvHeap  = srvHeap;
    m_shaderDir = shaderDir;

    // --- compute ルートシグネチャ: b0(28 DWORD) + t0(root SRV) + u0/u1/u2(root UAV) ---
    // DWORD: 28 + 2 + 2*3 = 36 / 64。
    {
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0
        params[0].Constants.Num32BitValues = kClusterCBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t0 = ライト
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        for (u32 i = 0; i < 3; ++i)
        {
            params[2 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u0,u1,u2
            params[2 + i].Descriptor.ShaderRegister = i;
            params[2 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        }

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

    // --- DEFAULT ヒープのバッファ（AutoExposurePass::Initialize の makeBuf と同形）---
    auto makeBuf = [&](u64 size, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = size;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    makeBuf(kAabbBytes,  m_aabbBuf);
    makeBuf(kIndexBytes, m_indexBuf);
    makeBuf(kCountBytes, m_countBuf);
    m_indexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_countState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // --- UPLOAD ヒープのライトバッファ（フレームリング。永続 Map）---
    for (u32 f = 0; f < kFrameCount; ++f)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = kLightBytes;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightBuf[f])));

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        ThrowIfFailed(m_lightBuf[f]->Map(0, &readRange, &mapped));
        m_lightMapped[f] = static_cast<u8*>(mapped);
        // 未初期化のライトを読まないようゼロ埋め（type=0/range=0 なので減衰も 0）。
        std::memset(m_lightMapped[f], 0, static_cast<size_t>(kLightBytes));
    }

    // --- SRV ディスクリプタ（7 本連続 × 3 フレーム）---
    // [0]=t13 ライト（フレーム毎に変わる） [1]=t14 インデックスリスト [2]=t15 カウント
    // [3..6] = デカール（計画06）の t18..t21 予約枠。
    //   FLAG_NONE/DATA_VOLATILE のレンジは「ディスクリプタ自体は静的」扱いで、
    //   テーブルを Set した時点で全数が初期化済みであることが要求される
    //   （未初期化だとデバッグレイヤが "All descriptors ... must be initialized" で落とす）。
    //   なのでカウントバッファの SRV を仮で複製して埋めておく。デカール実装時に上書きすること。
    for (u32 f = 0; f < kFrameCount; ++f)
    {
        // 枯渇時は AllocateBlock 自身が Logger::Error + throw で fail-fast する（DescriptorHeap.cpp）。
        // 念のためセンチネルも見て、取れていなければ IsReady() が false になるようにしておく。
        const u32 base = m_srvHeap->AllocateBlock(kSrvTableSize);
        if (base == DescriptorHeap::kInvalidIndex)
        {
            Logger::Error("ClusteredLightCulling: SRV ディスクリプタの確保に失敗（ヒープ枯渇）");
            m_srvBlock[f] = 0xFFFFFFFFu;
            return;
        }
        m_srvBlock[f] = base;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format                  = DXGI_FORMAT_UNKNOWN;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement     = 0;
        srv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_NONE;

        // t13: StructuredBuffer<ClusterLight>
        srv.Buffer.NumElements         = cluster::kMaxSceneLights;
        srv.Buffer.StructureByteStride = sizeof(LightGPU);
        dev->CreateShaderResourceView(m_lightBuf[f].Get(), &srv, m_srvHeap->GetCpuHandle(base + 0));

        // t14: StructuredBuffer<uint>（インデックスリスト）
        srv.Buffer.NumElements         = cluster::kClusterCount * cluster::kMaxLightsPerCluster;
        srv.Buffer.StructureByteStride = sizeof(u32);
        dev->CreateShaderResourceView(m_indexBuf.Get(), &srv, m_srvHeap->GetCpuHandle(base + 1));

        // t15: StructuredBuffer<uint>（クラスタ毎カウント）
        srv.Buffer.NumElements         = cluster::kClusterCount;
        srv.Buffer.StructureByteStride = sizeof(u32);
        dev->CreateShaderResourceView(m_countBuf.Get(), &srv, m_srvHeap->GetCpuHandle(base + 2));

        // [3..6] デカール予約枠（有効なディスクリプタで埋めるだけ。誰も読まない）
        for (u32 i = 3; i < kSrvTableSize; ++i)
            dev->CreateShaderResourceView(m_countBuf.Get(), &srv, m_srvHeap->GetCpuHandle(base + i));
    }

    Logger::Info("ClusteredLightCulling initialized ({}x{}x{}={} clusters, {} lights/cluster, {} scene lights, {:.1f} MB)",
                 cluster::kGridX, cluster::kGridY, cluster::kGridZ, cluster::kClusterCount,
                 cluster::kMaxLightsPerCluster, cluster::kMaxSceneLights,
                 static_cast<double>(kAabbBytes + kIndexBytes + kCountBytes + kLightBytes * kFrameCount)
                     / (1024.0 * 1024.0));
}

void ClusteredLightCulling::Shutdown()
{
    for (u32 f = 0; f < kFrameCount; ++f)
    {
        if (m_lightBuf[f] && m_lightMapped[f])
        {
            m_lightBuf[f]->Unmap(0, nullptr);
            m_lightMapped[f] = nullptr;
        }
        if (m_srvHeap && m_srvBlock[f] != 0xFFFFFFFFu)
        {
            m_srvHeap->FreeBlock(m_srvBlock[f], kSrvTableSize);
            m_srvBlock[f] = 0xFFFFFFFFu;
        }
        m_lightBuf[f].Reset();
    }
    m_aabbBuf.Reset();
    m_indexBuf.Reset();
    m_countBuf.Reset();
    m_psoBuild.Reset();
    m_psoCull.Reset();
    m_rootSig.Reset();
    m_srvHeap = nullptr;
}

void ClusteredLightCulling::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto makeCS = [&](const std::wstring& cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"ClusterBuild_CS.cso", m_psoBuild);
    makeCS(L"ClusterCull_CS.cso",  m_psoCull);
}

u32 ClusteredLightCulling::UploadLights(const LightGPU* lights, u32 count, u32 frameIndex)
{
    if (frameIndex >= kFrameCount || !m_lightMapped[frameIndex]) return 0;
    const u32 n = (std::min)(count, cluster::kMaxSceneLights);
    if (n > 0 && lights)
        std::memcpy(m_lightMapped[frameIndex], lights, static_cast<size_t>(n) * sizeof(LightGPU));
    return n;
}

void ClusteredLightCulling::EnsureUavState(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER b[2]{};
    u32 n = 0;
    auto push = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& state)
    {
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return;
        b[n].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition.pResource   = res;
        b[n].Transition.StateBefore = state;
        b[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ++n;
    };
    push(m_indexBuf.Get(), m_indexState);
    push(m_countBuf.Get(), m_countState);
    if (n > 0) cmd->ResourceBarrier(n, b);
}

void ClusteredLightCulling::Dispatch(ID3D12GraphicsCommandList* cmd, FXMMATRIX view,
                                     float proj11, float proj22,
                                     float zNear, float zFarCluster, float zFarCamera,
                                     u32 numLights, u32 frameIndex)
{
    if (!IsReady() || !cmd || frameIndex >= kFrameCount) return;

    EnsureUavState(cmd);

    ClusterCB cb{};
    XMStoreFloat4x4(&cb.viewMat, XMMatrixTranspose(view));   // HLSL は列優先 mul(row, mat)
    cb.gridDims[0] = cluster::kGridX;
    cb.gridDims[1] = cluster::kGridY;
    cb.gridDims[2] = cluster::kGridZ;
    cb.gridDims[3] = cluster::kClusterCount;
    cb.zParams[0]  = zNear;
    cb.zParams[1]  = zFarCluster;
    cb.zParams[2]  = zFarCamera;
    cb.zParams[3]  = 0.0f;
    // proj._11 / proj._22 は 0 になり得ない（XMMatrixPerspectiveFovLH）。念のため 0 除算を避ける。
    cb.misc[0] = 1.0f / ((proj11 != 0.0f) ? proj11 : 1.0f);
    cb.misc[1] = 1.0f / ((proj22 != 0.0f) ? proj22 : 1.0f);
    cb.misc[2] = static_cast<float>((std::min)(numLights, cluster::kMaxSceneLights));
    cb.misc[3] = static_cast<float>(cluster::kMaxLightsPerCluster);

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kClusterCBNum32, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, m_lightBuf[frameIndex]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, m_aabbBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_indexBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, m_countBuf->GetGPUVirtualAddress());

    // ① クラスタ AABB 構築
    cmd->SetPipelineState(m_psoBuild.Get());
    cmd->Dispatch(kGroupCount, 1, 1);

    // ② ライト割り当て（①の AABB 書き込みを待つ）
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = m_aabbBuf.Get();
        cmd->ResourceBarrier(1, &b);
    }
    cmd->SetPipelineState(m_psoCull.Get());
    cmd->Dispatch(kGroupCount, 1, 1);

    // PS が読める状態へ
    {
        D3D12_RESOURCE_BARRIER b[2]{};
        for (u32 i = 0; i < 2; ++i)
        {
            b[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        b[0].Transition.pResource = m_indexBuf.Get();
        b[1].Transition.pResource = m_countBuf.Get();
        cmd->ResourceBarrier(2, b);
        m_indexState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_countState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void ClusteredLightCulling::EnsureReadable(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd || !m_indexBuf || !m_countBuf) return;

    D3D12_RESOURCE_BARRIER b[2]{};
    u32 n = 0;
    auto push = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& state)
    {
        if (state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;
        b[n].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition.pResource   = res;
        b[n].Transition.StateBefore = state;
        b[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ++n;
    };
    push(m_indexBuf.Get(), m_indexState);
    push(m_countBuf.Get(), m_countState);
    if (n > 0) cmd->ResourceBarrier(n, b);
}

D3D12_GPU_DESCRIPTOR_HANDLE ClusteredLightCulling::GetSrvTable(u32 frameIndex) const
{
    const u32 f = (frameIndex < kFrameCount) ? frameIndex : 0;
    if (!m_srvHeap || m_srvBlock[f] == 0xFFFFFFFFu) return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    return m_srvHeap->GetGpuHandle(m_srvBlock[f]);
}

u32 ClusteredLightCulling::GetSrvTableIndex(u32 frameIndex) const
{
    const u32 f = (frameIndex < kFrameCount) ? frameIndex : 0;
    return m_srvBlock[f];
}

} // namespace dx12e
