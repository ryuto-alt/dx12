#include "renderer/DecalSystem.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/Texture.h"
#include "renderer/ClusterMath.h"
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

// shaders/forward/ClusterCullDecals.hlsl の cbuffer DecalCullCB(b0) と完全一致（28 DWORD）。
struct DecalCullCB
{
    XMFLOAT4X4 invView;     //  0..15  転置済み（HLSL は mul(row, mat) 規約）
    u32        grid[4];     // 16..19  gridX, gridY, gridZ, clusterCount
    float      zParams[4];  // 20..23  zNear, zFarCluster, zFarCamera, 未使用
    float      misc[4];     // 24..27  1/proj_11, 1/proj_22, デカール数, 1クラスタ最大数
};
static_assert(sizeof(DecalCullCB) == 28 * sizeof(u32), "DecalCullCB must be 28 DWORDs");
constexpr UINT kDecalCullCBNum32 = 28;

constexpr u32 kThreadsPerGroup = 64;
constexpr u32 kGroupCount = (cluster::kClusterCount + kThreadsPerGroup - 1) / kThreadsPerGroup;

constexpr u64 kIndexBytes = static_cast<u64>(cluster::kClusterCount)
                          * DecalSystem::kMaxPerCluster * sizeof(u32);          // 216 KB
constexpr u64 kCountBytes = static_cast<u64>(cluster::kClusterCount) * sizeof(u32);  // 13.5 KB
constexpr u64 kDecalBytes = static_cast<u64>(DecalSystem::kMaxDecals) * 176;         // 44 KB

// フォワード PS からもデカールのカリング CS からも読まれるので、読み取り状態の合成にしておく
// （ClusteredLightCulling のカリング結果と同じ流儀）。
constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

} // namespace

void DecalSystem::Initialize(GraphicsDevice& device, DescriptorHeap* /*srvHeap*/,
                             const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_shaderDir = shaderDir;

    // --- compute ルートシグネチャ: b0(28 DWORD) + t0(root SRV) + u0/u1(root UAV) ---
    // DWORD: 28 + 2 + 2*2 = 34 / 64。
    {
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0
        params[0].Constants.Num32BitValues = kDecalCullCBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t0 = デカール
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        for (u32 i = 0; i < 2; ++i)
        {
            params[2 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u0,u1
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

    // --- DEFAULT ヒープのバッファ（ClusteredLightCulling の makeBuf と同形）---
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
    makeBuf(kIndexBytes, m_indexBuf);
    makeBuf(kCountBytes, m_countBuf);
    m_indexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_countState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // --- UPLOAD ヒープのデカールバッファ（フレームリング。永続 Map）---
    for (u32 f = 0; f < kFrameCount; ++f)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = kDecalBytes;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_decalBuf[f])));

        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        ThrowIfFailed(m_decalBuf[f]->Map(0, &readRange, &mapped));
        m_decalMapped[f] = static_cast<u8*>(mapped);
        // 未初期化のデカールを読まないようゼロ埋め（opacity=0 / atlasUV=0 なので寄与も 0）。
        std::memset(m_decalMapped[f], 0, static_cast<size_t>(kDecalBytes));
    }

    Logger::Info("DecalSystem initialized (max {} decals, {}/cluster, {:.2f} MB)",
                 kMaxDecals, kMaxPerCluster,
                 static_cast<double>(kIndexBytes + kCountBytes + kDecalBytes * kFrameCount)
                     / (1024.0 * 1024.0));
}

void DecalSystem::Shutdown()
{
    for (u32 f = 0; f < kFrameCount; ++f)
    {
        if (m_decalBuf[f] && m_decalMapped[f])
        {
            m_decalBuf[f]->Unmap(0, nullptr);
            m_decalMapped[f] = nullptr;
        }
        m_decalBuf[f].Reset();
    }
    m_indexBuf.Reset();
    m_countBuf.Reset();
    m_pso.Reset();
    m_rootSig.Reset();
}

void DecalSystem::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + L"ClusterCullDecals_CS.cso");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.CS = { bc.GetData(), bc.GetSize() };
    ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

u32 DecalSystem::Upload(const DecalGPU* decals, u32 count, u32 frameIndex)
{
    if (frameIndex >= kFrameCount || !m_decalMapped[frameIndex]) return 0;
    const u32 n = (std::min)(count, kMaxDecals);
    if (n > 0 && decals)
        std::memcpy(m_decalMapped[frameIndex], decals, static_cast<size_t>(n) * sizeof(DecalGPU));
    return n;
}

void DecalSystem::EnsureUavState(ID3D12GraphicsCommandList* cmd)
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

void DecalSystem::Cull(ID3D12GraphicsCommandList* cmd, FXMMATRIX view,
                       float proj11, float proj22,
                       float zNear, float zFarCluster, float zFarCamera,
                       u32 numDecals, u32 frameIndex)
{
    if (!IsReady() || !cmd || frameIndex >= kFrameCount) return;

    EnsureUavState(cmd);

    DecalCullCB cb{};
    // view → world。転置は HLSL の mul(row, mat) 規約のため。
    // ★transpose(view) の近似ではなく XMMatrixInverse で作る（スケール付きでも壊れない）。
    XMStoreFloat4x4(&cb.invView, XMMatrixTranspose(XMMatrixInverse(nullptr, view)));
    cb.grid[0] = cluster::kGridX;
    cb.grid[1] = cluster::kGridY;
    cb.grid[2] = cluster::kGridZ;
    cb.grid[3] = cluster::kClusterCount;
    cb.zParams[0] = zNear;
    cb.zParams[1] = zFarCluster;
    cb.zParams[2] = zFarCamera;
    cb.zParams[3] = 0.0f;
    cb.misc[0] = 1.0f / ((proj11 != 0.0f) ? proj11 : 1.0f);
    cb.misc[1] = 1.0f / ((proj22 != 0.0f) ? proj22 : 1.0f);
    cb.misc[2] = static_cast<float>((std::min)(numDecals, kMaxDecals));
    cb.misc[3] = static_cast<float>(kMaxPerCluster);

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kDecalCullCBNum32, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, m_decalBuf[frameIndex]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, m_indexBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_countBuf->GetGPUVirtualAddress());

    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch(kGroupCount, 1, 1);

    D3D12_RESOURCE_BARRIER b[2]{};
    for (u32 i = 0; i < 2; ++i)
    {
        b[i].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[i].Transition.StateAfter  = kReadState;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    b[0].Transition.pResource = m_indexBuf.Get();
    b[1].Transition.pResource = m_countBuf.Get();
    cmd->ResourceBarrier(2, b);
    m_indexState = kReadState;
    m_countState = kReadState;
}

void DecalSystem::EnsureReadable(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd || !m_indexBuf || !m_countBuf) return;

    D3D12_RESOURCE_BARRIER b[2]{};
    u32 n = 0;
    auto push = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& state)
    {
        if (state == kReadState) return;
        b[n].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition.pResource   = res;
        b[n].Transition.StateBefore = state;
        b[n].Transition.StateAfter  = kReadState;
        b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        state = kReadState;
        ++n;
    };
    push(m_indexBuf.Get(), m_indexState);
    push(m_countBuf.Get(), m_countState);
    if (n > 0) cmd->ResourceBarrier(n, b);
}

void DecalSystem::WriteSrvsInto(GraphicsDevice& device, DescriptorHeap& heap,
                                u32 blockStart, u32 frameIndex,
                                Texture* atlas)
{
    if (!m_indexBuf || blockStart == DescriptorHeap::kInvalidIndex) return;
    if (frameIndex >= kFrameCount) return;

    auto* dev = device.GetDevice();
    const u32 base = blockStart + kSrvOffset;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Format                  = DXGI_FORMAT_UNKNOWN;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement     = 0;
    srv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_NONE;

    // t18: StructuredBuffer<Decal>（フレーム毎に変わる）
    srv.Buffer.NumElements         = kMaxDecals;
    srv.Buffer.StructureByteStride = sizeof(DecalGPU);
    dev->CreateShaderResourceView(m_decalBuf[frameIndex].Get(), &srv, heap.GetCpuHandle(base + 0));

    // t19: StructuredBuffer<uint>（インデックスリスト）
    srv.Buffer.NumElements         = cluster::kClusterCount * kMaxPerCluster;
    srv.Buffer.StructureByteStride = sizeof(u32);
    dev->CreateShaderResourceView(m_indexBuf.Get(), &srv, heap.GetCpuHandle(base + 1));

    // t20: StructuredBuffer<uint>（クラスタ毎カウント）
    srv.Buffer.NumElements         = cluster::kClusterCount;
    srv.Buffer.StructureByteStride = sizeof(u32);
    dev->CreateShaderResourceView(m_countBuf.Get(), &srv, heap.GetCpuHandle(base + 2));

    // t21: Texture2D（アトラス）。★宛先へ直接 SRV を作る。
    //   シェーダ可視ヒープからのディスクリプタコピーは不正（CPU write-only なので
    //   コピー元として読めない）。以前ここは CopyDescriptorsSimple で、デバッグレイヤが
    //   id=654 を出していた＝t21 の中身は未定義だった。
    if (atlas) atlas->CreateSRV(device, heap.GetCpuHandle(base + 3));
}

} // namespace dx12e
