#include "renderer/AutoExposurePass.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace dx12e
{
// HLSL の AECB と一致（2 x float4 + int4 = 12 DWORD、ルート定数）
struct AECB
{
    float p0[4];   // minLog2, 1/(maxLog2-minLog2), dt, 適応速度
    float p1[4];   // EV補正, 総ピクセル数, _, _
    int   rect[4]; // left, top, width, height
};
static_assert(sizeof(AECB) == 12 * sizeof(float), "AECB must be 12 DWORDs");
static constexpr UINT kAECBNum32 = 12;

static constexpr u32 kHistBins = 256;

void AutoExposurePass::Initialize(GraphicsDevice& device, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Compute Root Signature: b0(12 DWORD) + t0(SRV table) + u0/u1(root UAV) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;  // t0

        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;  // b0
        params[0].Constants.Num32BitValues = kAECBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u0 = histogram
        params[2].Descriptor.ShaderRegister = 0;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;  // u1 = exposure
        params[3].Descriptor.ShaderRegister = 1;
        params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 4;
        desc.pParameters   = params;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- Compute PSO ×2 ---
    auto makeCS = [&](const std::wstring& cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"ExposureHistogram_CS.cso", m_psoHistogram);
    makeCS(L"ExposureAdapt_CS.cso",     m_psoAdapt);

    // --- バッファ（DEFAULT ヒープ・UAV）---
    auto makeBuf = [&](u64 size, Microsoft::WRL::ComPtr<ID3D12Resource>& out,
                       D3D12_RESOURCE_STATES initial)
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
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, initial, nullptr, IID_PPV_ARGS(&out)));
    };
    makeBuf(kHistBins * sizeof(u32), m_histBuf,     D3D12_RESOURCE_STATE_COPY_DEST);
    makeBuf(2 * sizeof(float),       m_exposureBuf, D3D12_RESOURCE_STATE_COPY_DEST);
    m_exposureState = D3D12_RESOURCE_STATE_COPY_DEST;

    // --- ゼロ初期化用アップロードバッファ（初回 Generate でコピー）---
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = kHistBins * sizeof(u32);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_zeroUpload)));
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        ThrowIfFailed(m_zeroUpload->Map(0, &readRange, &mapped));
        std::memset(mapped, 0, kHistBins * sizeof(u32));
        m_zeroUpload->Unmap(0, nullptr);
    }

    Logger::Info("AutoExposurePass initialized");
}

void AutoExposurePass::Generate(ID3D12GraphicsCommandList* cmd,
                                D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                                u32 rectLeft, u32 rectTop, u32 rectW, u32 rectH,
                                float dt, const PostProcessSettings& s)
{
    if (!m_psoHistogram || rectW == 0 || rectH == 0) return;

    auto barrier = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&](ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmd->ResourceBarrier(1, &b);
    };

    // 初回: 両バッファをゼロ初期化（未定義メモリのヒストグラム/露出を読まない）
    if (m_first)
    {
        // AE OFF のまま EnsureReadable が先に呼ばれていた場合は PSR になっているので COPY_DEST へ戻す
        if (m_exposureState != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            barrier(m_exposureBuf.Get(), m_exposureState, D3D12_RESOURCE_STATE_COPY_DEST);
            m_exposureState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        cmd->CopyBufferRegion(m_histBuf.Get(),     0, m_zeroUpload.Get(), 0, kHistBins * sizeof(u32));
        cmd->CopyBufferRegion(m_exposureBuf.Get(), 0, m_zeroUpload.Get(), 0, 2 * sizeof(float));
        barrier(m_histBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(m_exposureBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_exposureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dt = 1.0e9f;  // 初回は即適応（フェードインで暗闇から始まらない）
    }

    if (m_exposureState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        barrier(m_exposureBuf.Get(), m_exposureState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_exposureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    const float logMin = (std::min)(s.aeLogMin, s.aeLogMax - 0.1f);
    AECB cb{};
    cb.p0[0] = logMin;
    cb.p0[1] = 1.0f / (std::max)(s.aeLogMax - logMin, 0.1f);
    cb.p0[2] = dt;
    cb.p0[3] = (std::max)(s.aeSpeed, 0.01f);
    cb.p1[0] = s.aeEvComp;
    cb.p1[1] = static_cast<float>(rectW) * static_cast<float>(rectH);
    cb.rect[0] = static_cast<int>(rectLeft);
    cb.rect[1] = static_cast<int>(rectTop);
    cb.rect[2] = static_cast<int>(rectW);
    cb.rect[3] = static_cast<int>(rectH);

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kAECBNum32, &cb, 0);
    cmd->SetComputeRootDescriptorTable(1, sceneSrvGpu);
    cmd->SetComputeRootUnorderedAccessView(2, m_histBuf->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_exposureBuf->GetGPUVirtualAddress());

    // ① ヒストグラム集計（前フレームの CSAdapt によるクリア書き込みと順序保証）
    uavBarrier(m_histBuf.Get());
    cmd->SetPipelineState(m_psoHistogram.Get());
    cmd->Dispatch((rectW + 15) / 16, (rectH + 15) / 16, 1);

    // ② リダクション + 時間適応（①の書き込みを待つ）
    uavBarrier(m_histBuf.Get());
    cmd->SetPipelineState(m_psoAdapt.Get());
    cmd->Dispatch(1, 1, 1);

    m_first = false;
}

void AutoExposurePass::EnsureReadable(ID3D12GraphicsCommandList* cmd)
{
    if (!m_exposureBuf) return;
    if (m_exposureState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;

    // 初回 Generate 前（COPY_DEST のまま）でも読取状態にしておく（中身は 0 = 露出 0 倍。
    // その場合 uber 側のマスクも立っていないので実際には参照されない）。
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = m_exposureBuf.Get();
    b.Transition.StateBefore = m_exposureState;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    m_exposureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

} // namespace dx12e
