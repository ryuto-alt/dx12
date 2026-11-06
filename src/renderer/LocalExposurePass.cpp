#include "renderer/LocalExposurePass.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/CommandList.h"
#include "resource/ShaderCompiler.h"
#include "renderer/PhotometricMath.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e
{
namespace
{
constexpr DXGI_FORMAT kLEFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// HLSL の LECB と一致（5 x float4 = 20 DWORD、ルート定数）
struct LECB
{
    float p0[4];     // 定数側の露出倍率, 露出バッファも掛けるか, グリッド輝度下限 log2, 1EV あたりのビン数
    u32   dim0[4];   // W, H, gridW, gridH
    u32   dim1[4];   // lumW, lumH, _, _
    float le0[4];    // highlight, shadow, detail, blend
    float le1[4];    // middle(log2), gain 下限(EV), gain 上限(EV), _
};
static_assert(sizeof(LECB) == 20 * sizeof(u32), "LECB must be 20 DWORDs");
constexpr UINT kLECBNum32 = 20;

constexpr u32   kBins      = 32;       // 輝度方向の段数（HLSL の kBins と一致）
constexpr float kGridMinEv = -10.0f;   // グリッドが覆う log2 輝度（露出後）。-10..+6 EV を 0.5EV 刻み
constexpr float kGridMaxEv = 6.0f;
constexpr float kGainLimit = 8.0f;     // 1 画素に掛ける補正の上限 ±8EV

inline u32 DivUp(u32 a, u32 b) { return (a + b - 1u) / b; }

// PostProcess::Apply と同じ規則で「uber が色に掛ける露出倍率」を再現する。
// 戻り値 scalar = 定数側の倍率、useBuf = 自動露出バッファ[0] も掛けるか。
void ResolveExposure(const PostProcessSettings& s, bool haveExposureBuf, float& scalar, bool& useBuf)
{
    const float legacyGain = (s.enabled && s.exposureOn) ? s.exposure : 1.0f;
    if (s.exposureMode == 1)
    {
        scalar = legacyGain * photo::ManualExposureScale(s.ev100, s.evComp);
        useBuf = false;
    }
    else if (s.exposureMode == 2)
    {
        scalar = legacyGain;
        useBuf = haveExposureBuf;
    }
    else
    {
        scalar = legacyGain;
        useBuf = s.enabled && s.autoExposureOn && haveExposureBuf;
    }
}
} // namespace

bool LocalExposurePass::NeedsRun(const PostProcessSettings& s)
{
    return s.enabled && s.localExposureOn
        && (s.leHighlightContrast != 1.0f || s.leShadowContrast != 1.0f || s.leDetailStrength != 1.0f);
}

void LocalExposurePass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                                   u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;
    m_shaderDir = shaderDir;

    // --- Compute Root Signature: b0(20) + t0(table) + t1(root SRV) + u0..u3(root UAV) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;   // t0

        D3D12_ROOT_PARAMETER params[7]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = kLECBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;   // t1 = 露出バッファ
        params[2].Descriptor.ShaderRegister = 1;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        for (UINT i = 0; i < 4; ++i)   // u0..u3
        {
            params[3 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[3 + i].Descriptor.ShaderRegister = i;
            params[3 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(params);
        desc.pParameters   = params;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_csRootSig)));
    }

    // --- Graphics Root Signature: b0(20) + t0(table) + t1/t2/t3(root SRV)。サンプラ無し（Load のみ）---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = kLECBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        for (UINT i = 0; i < 3; ++i)   // t1 = 露出 / t2 = ぼかし後グリッド / t3 = ぼかし後縮小輝度
        {
            params[2 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[2 + i].Descriptor.ShaderRegister = 1 + i;
            params[2 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(params);
        desc.pParameters   = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_gfxRootSig)));
    }

    RecreatePipelines(device);

    // --- 露出バッファ無しのときのダミー（値 1.0。p0.y=0 なので読まれないが、ルート SRV は有効な VA が要る）---
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = 16;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_dummyExpo)));
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        ThrowIfFailed(m_dummyExpo->Map(0, &readRange, &mapped));
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::memcpy(mapped, one, sizeof(one));
        m_dummyExpo->Unmap(0, nullptr);
    }

    CreateBuffers(device, m_width, m_height);

    const float clearBlack[4] = {0, 0, 0, 1};
    m_outRT = std::make_unique<RenderTarget>();
    m_outRT->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kLEFormat, clearBlack);

    Logger::Info("LocalExposurePass initialized");
}

void LocalExposurePass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto makeCS = [&](const wchar_t* cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_csRootSig.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"LocalExposureGrid_CS.cso",     m_psoGrid);
    makeCS(L"LocalExposureDown_CS.cso",     m_psoDown);
    makeCS(L"LocalExposureGridBlur_CS.cso", m_psoGridBlur);
    makeCS(L"LocalExposureLumBlur_CS.cso",  m_psoLumBlur);

    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"LocalExposure_PS.cso");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_gfxRootSig.Get();
    pso.VS = { vs.GetData(), vs.GetSize() };
    pso.PS = { ps.GetData(), ps.GetSize() };
    pso.InputLayout = { nullptr, 0 };
    pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask            = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = kLEFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };
    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoApply)));
}

void LocalExposurePass::CreateBuffers(GraphicsDevice& device, u32 width, u32 height)
{
    auto* dev = device.GetDevice();
    const u64 cells = static_cast<u64>(DivUp(width, 16)) * DivUp(height, 16) * kBins;
    const u64 lums  = static_cast<u64>(DivUp(width, 8)) * DivUp(height, 8);

    auto makeBuf = [&](u64 bytes, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = bytes;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        out.Reset();
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };
    makeBuf(cells * 8u, m_gridRaw);    // uint2
    makeBuf(cells * 8u, m_gridBlur);   // float2
    makeBuf(lums * 4u,  m_lumA);       // float
    makeBuf(lums * 4u,  m_lumB);
    m_width  = width;
    m_height = height;
}

void LocalExposurePass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    CreateBuffers(device, width, height);
    if (m_outRT) m_outRT->Resize(device, width, height);
}

u32 LocalExposurePass::Apply(CommandList& cmd,
                             D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                             D3D12_GPU_VIRTUAL_ADDRESS exposureVA,
                             u32 sceneW, u32 sceneH,
                             const PostProcessSettings& s)
{
    if (!IsReady() || !m_outRT || sceneW == 0 || sceneH == 0)
        return DescriptorHeap::kInvalidIndex;
    // 確保済みより大きい寸法は扱えない（バッファは Resize でしか作り直さない）→ 素通し。
    if (sceneW > m_width || sceneH > m_height)
        return DescriptorHeap::kInvalidIndex;

    auto* native = cmd.GetNative();
    const u32 gw = DivUp(sceneW, 16), gh = DivUp(sceneH, 16);
    const u32 lw = DivUp(sceneW, 8),  lh = DivUp(sceneH, 8);

    float expoScalar = 1.0f;
    bool  useBuf     = false;
    ResolveExposure(s, exposureVA != 0, expoScalar, useBuf);

    const float binsPerEv = static_cast<float>(kBins) / (kGridMaxEv - kGridMinEv);
    LECB cb{};
    cb.p0[0] = expoScalar;
    cb.p0[1] = useBuf ? 1.0f : 0.0f;
    cb.p0[2] = kGridMinEv;
    cb.p0[3] = binsPerEv;
    cb.dim0[0] = sceneW; cb.dim0[1] = sceneH; cb.dim0[2] = gw; cb.dim0[3] = gh;
    cb.dim1[0] = lw;     cb.dim1[1] = lh;
    cb.le0[0] = std::clamp(s.leHighlightContrast, 0.0f, 4.0f);
    cb.le0[1] = std::clamp(s.leShadowContrast,    0.0f, 4.0f);
    cb.le0[2] = std::clamp(s.leDetailStrength,    0.0f, 4.0f);
    cb.le0[3] = std::clamp(s.leBlurredLumBlend,   0.0f, 1.0f);
    cb.le1[0] = std::log2(0.18f) + std::clamp(s.leMiddleGreyBias, -8.0f, 8.0f);
    cb.le1[1] = -kGainLimit;
    cb.le1[2] =  kGainLimit;

    const D3D12_GPU_VIRTUAL_ADDRESS expoVa = (useBuf && exposureVA != 0)
        ? exposureVA : m_dummyExpo->GetGPUVirtualAddress();

    auto uavBarrierAll = [&]()
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;   // 全 UAV
        native->ResourceBarrier(1, &b);
    };
    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        native->ResourceBarrier(1, &b);
    };

    // ---- compute: ①② → ③④ ----
    native->SetComputeRootSignature(m_csRootSig.Get());
    native->SetComputeRoot32BitConstants(0, kLECBNum32, &cb, 0);
    native->SetComputeRootDescriptorTable(1, sceneSrvGpu);
    native->SetComputeRootShaderResourceView(2, expoVa);
    native->SetComputeRootUnorderedAccessView(3, m_gridRaw->GetGPUVirtualAddress());
    native->SetComputeRootUnorderedAccessView(4, m_gridBlur->GetGPUVirtualAddress());
    native->SetComputeRootUnorderedAccessView(5, m_lumA->GetGPUVirtualAddress());
    native->SetComputeRootUnorderedAccessView(6, m_lumB->GetGPUVirtualAddress());

    uavBarrierAll();   // 前フレームの読み（③④）との順序
    native->SetPipelineState(m_psoGrid.Get());
    native->Dispatch(gw, gh, 1);
    native->SetPipelineState(m_psoDown.Get());
    native->Dispatch(lw, lh, 1);
    uavBarrierAll();
    native->SetPipelineState(m_psoGridBlur.Get());
    native->Dispatch(DivUp(gw, 8), DivUp(gh, 8), kBins);
    native->SetPipelineState(m_psoLumBlur.Get());
    native->Dispatch(DivUp(lw, 8), DivUp(lh, 8), 1);
    uavBarrierAll();

    // ---- graphics: ⑤ ----
    transition(m_gridBlur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(m_lumB.Get(),     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_outRT->Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_outRT->GetRtv();
    native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd.SetViewportAndScissor(sceneW, sceneH);

    native->SetGraphicsRootSignature(m_gfxRootSig.Get());
    native->SetPipelineState(m_psoApply.Get());
    native->SetGraphicsRoot32BitConstants(0, kLECBNum32, &cb, 0);
    native->SetGraphicsRootDescriptorTable(1, sceneSrvGpu);
    native->SetGraphicsRootShaderResourceView(2, expoVa);
    native->SetGraphicsRootShaderResourceView(3, m_gridBlur->GetGPUVirtualAddress());
    native->SetGraphicsRootShaderResourceView(4, m_lumB->GetGPUVirtualAddress());
    native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native->IASetVertexBuffers(0, 0, nullptr);
    native->IASetIndexBuffer(nullptr);
    native->DrawInstanced(3, 1, 0, 0);

    transition(m_gridBlur.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(m_lumB.Get(),     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 後段（ブルーム / uber / 自動露出ではない）がシーンとして読むので複合読取状態へ
    m_outRT->Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                           | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return m_outRT->GetSrvIndex();
}

} // namespace dx12e
