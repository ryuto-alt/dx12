#include "renderer/LinearCapturePass.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>

namespace dx12e
{
namespace
{
struct LcCB
{
    u32 size[2];
    u32 pad[2];
};
static_assert(sizeof(LcCB) == 4 * sizeof(u32), "LcCB must be 4 DWORDs");
constexpr UINT kLcCBNum32 = 4;
}

void LinearCapturePass::Initialize(GraphicsDevice& device, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // ルートシグネチャ: b0(4 DWORD) + t0(SRV テーブル) + u0(ルート UAV)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = kLcCBNum32;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters   = params;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));

    m_shaderDir = shaderDir;
    RecreatePipelines(device);
}

void LinearCapturePass::RecreatePipelines(GraphicsDevice& device)
{
    auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + L"LinearCapture_CS.cso");
    if (bc.GetSize() == 0)
    {
        Logger::Warn("LinearCapture_CS.cso が読めません（線形 HDR スクリーンショットは無効）");
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.CS = { bc.GetData(), bc.GetSize() };
    ThrowIfFailed(device.GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

bool LinearCapturePass::Record(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd,
                               D3D12_GPU_DESCRIPTOR_HANDLE srcSrvGpu, u32 w, u32 h, std::string& err)
{
    if (!m_pso) { err = "LinearCapture_CS.cso is not loaded (build the engine shaders)"; return false; }
    if (w == 0 || h == 0) { err = "empty source"; return false; }
    Discard();

    auto* dev = device.GetDevice();
    const u64 bytes = static_cast<u64>(w) * h * 16ull;

    auto makeBuf = [&](D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                       Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heapType;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = bytes;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc       = {1, 0};
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = flags;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!makeBuf(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COMMON, m_outBuf)   // ★バッファは COMMON で作る(他の状態を指定するとデバッグレイヤが id=1328 の警告を出す)
        || !makeBuf(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_readback))
    {
        Discard();
        err = "GPU buffer allocation failed for the linear capture (" + std::to_string(bytes >> 20) + " MB x2)";
        return false;
    }

    LcCB cb{ { w, h }, { 0, 0 } };
    {
        D3D12_RESOURCE_BARRIER toUav{};
        toUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav.Transition.pResource   = m_outBuf.Get();
        toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &toUav);
    }
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRoot32BitConstants(0, kLcCBNum32, &cb, 0);
    cmd->SetComputeRootDescriptorTable(1, srcSrvGpu);
    cmd->SetComputeRootUnorderedAccessView(2, m_outBuf->GetGPUVirtualAddress());
    cmd->SetPipelineState(m_pso.Get());
    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = m_outBuf.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    cmd->CopyBufferRegion(m_readback.Get(), 0, m_outBuf.Get(), 0, bytes);

    m_w = w; m_h = h;
    return true;
}

bool LinearCapturePass::Fetch(std::vector<float>& rgb, u32& w, u32& h, std::string& err)
{
    if (!m_readback) { err = "no pending linear capture"; return false; }
    const u64 bytes = static_cast<u64>(m_w) * m_h * 16ull;
    void* mapped = nullptr;
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(bytes) };
    if (FAILED(m_readback->Map(0, &rr, &mapped))) { Discard(); err = "linear readback map failed"; return false; }
    const auto* src = static_cast<const float*>(mapped);
    const size_t n = static_cast<size_t>(m_w) * m_h;
    rgb.resize(n * 3);
    for (size_t i = 0; i < n; ++i)
    {
        rgb[i * 3 + 0] = src[i * 4 + 0];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + 2];
    }
    D3D12_RANGE wr{ 0, 0 };
    m_readback->Unmap(0, &wr);
    w = m_w; h = m_h;
    Discard();
    return true;
}

void LinearCapturePass::Discard()
{
    m_outBuf.Reset();
    m_readback.Reset();
    m_w = m_h = 0;
}

} // namespace dx12e
