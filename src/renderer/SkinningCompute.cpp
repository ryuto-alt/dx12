#include "renderer/SkinningCompute.h"

#include "graphics/GraphicsDevice.h"
#include "renderer/Mesh.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{
namespace
{

// shaders/skinning/SkinCompute.hlsl の cbuffer SkinCB(b0) と完全一致（4 DWORD）。
struct SkinCB
{
    u32 vertexCount;
    u32 srcStride;
    u32 pad0;
    u32 pad1;
};
static_assert(sizeof(SkinCB) == 4 * sizeof(u32), "SkinCB layout mismatch");

constexpr u32 kSkinCBNum32 = sizeof(SkinCB) / sizeof(u32);

// 使われなくなった出力バッファをこのフレーム数だけ寝かせてから解放する。
// 毎フレーム作り直すとアロケータが暴れるので、少し保持してから捨てる。
constexpr u32 kRetireAfterFrames = 60;

} // namespace

void SkinningCompute::Initialize(GraphicsDevice& device, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_shaderDir = shaderDir;

    // --- compute ルートシグネチャ: b0(4 DWORD) + t0/t1(root SRV) + u0(root UAV) ---
    // DWORD: 4 + 2 + 2 + 2 = 10 / 64。
    // ★PBR のルートシグネチャ（61/64 DWORD、残り 3）とは別物なので、あちらの予算は減らない。
    {
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0
        params[0].Constants.Num32BitValues = kSkinCBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t0 = 元頂点
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t1 = ボーン行列
        params[2].Descriptor.ShaderRegister = 1;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;   // u0 = 変形後頂点
        params[3].Descriptor.ShaderRegister = 0;
        params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

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
    Logger::Info("SkinningCompute initialized");
}

void SkinningCompute::Shutdown()
{
    m_entries.clear();
    m_pso.Reset();
    m_rootSig.Reset();
}

void SkinningCompute::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + L"SkinCompute_CS.cso");
    if (bc.GetSize() == 0)
    {
        Logger::Warn("SkinCompute_CS.cso を読めません（compute スキニングは無効）");
        m_pso.Reset();
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.CS = { bc.GetData(), bc.GetSize() };
    ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void SkinningCompute::BeginFrame()
{
    ++m_frame;
    m_stats = {};
    for (auto& [key, e] : m_entries)
        e.writtenThisFrame = false;
}

void SkinningCompute::EndFrame()
{
    // 長く使われていないエントリを捨てる（エンティティ削除・シーン切替の後始末）。
    for (auto it = m_entries.begin(); it != m_entries.end(); )
    {
        if (m_frame - it->second.lastUsedFrame > kRetireAfterFrames)
            it = m_entries.erase(it);
        else
            ++it;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS SkinningCompute::Skin(ID3D12GraphicsCommandList* cmd,
                                                GraphicsDevice& device,
                                                u64 key, const Mesh& mesh,
                                                D3D12_GPU_VIRTUAL_ADDRESS bonesAddress,
                                                u64 poseHash)
{
    if (!cmd || !m_pso || bonesAddress == 0)
        return 0;

    const auto& vbView = mesh.GetVertexBuffer().GetView();
    if (vbView.StrideInBytes == 0 || vbView.SizeInBytes == 0)
        return 0;
    const u32 vertexCount = vbView.SizeInBytes / vbView.StrideInBytes;
    if (vertexCount == 0)
        return 0;

    Entry& e = m_entries[key];

    // ポーズが前フレームと同じなら何もしない。バッファは前フレームの内容のまま正しく、
    // 状態も NON_PIXEL_SHADER_RESOURCE のままなので AS ビルドにそのまま使える。
    if (e.buffer && e.vertexCount == vertexCount && e.poseHash == poseHash && poseHash != 0)
    {
        e.lastUsedFrame    = m_frame;
        e.writtenThisFrame = false;
        ++m_stats.skipped;
        return e.buffer->GetGPUVirtualAddress();
    }

    // 頂点数が変わったら作り直す（LOD 変更ではなくメッシュ差し替え時）。
    if (!e.buffer || e.vertexCount != vertexCount)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = static_cast<u64>(vertexCount) * kOutStride;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        Microsoft::WRL::ComPtr<ID3D12Resource> buf;
        const HRESULT hr = device.GetDevice()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buf));
        if (FAILED(hr))
        {
            Logger::Warn("compute スキニングの出力バッファを確保できません（{} 頂点）", vertexCount);
            m_entries.erase(key);
            return 0;
        }
        e.buffer      = std::move(buf);
        e.vertexCount = vertexCount;
        e.state       = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // 前フレームに AS ビルド入力へ遷移させたままなので UAV へ戻す。
    if (e.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = e.buffer.Get();
        b.Transition.StateBefore = e.state;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        e.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    SkinCB cb{};
    cb.vertexCount = vertexCount;
    cb.srcStride   = vbView.StrideInBytes;

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kSkinCBNum32, &cb, 0);
    cmd->SetComputeRootShaderResourceView(1, vbView.BufferLocation);
    cmd->SetComputeRootShaderResourceView(2, bonesAddress);
    cmd->SetComputeRootUnorderedAccessView(3, e.buffer->GetGPUVirtualAddress());
    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch((vertexCount + 63) / 64, 1, 1);

    e.lastUsedFrame     = m_frame;
    e.writtenThisFrame  = true;
    e.poseHash          = poseHash;

    ++m_stats.meshes;
    m_stats.vertices += vertexCount;
    m_stats.bytes    += static_cast<u64>(vertexCount) * kOutStride;

    return e.buffer->GetGPUVirtualAddress();
}

void SkinningCompute::TransitionForAccelerationStructureBuild(ID3D12GraphicsCommandList* cmd)
{
    if (!cmd)
        return;

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(m_entries.size());
    for (auto& [key, e] : m_entries)
    {
        if (!e.writtenThisFrame || e.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = e.buffer.Get();
        b.Transition.StateBefore = e.state;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(b);
        e.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (!barriers.empty())
        cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

} // namespace dx12e
