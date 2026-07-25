#include "renderer/ContactShadowPass.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/Buffer.h"
#include "graphics/FrameResources.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{
using namespace DirectX;

// HLSL の cbuffer ContactShadowParams(b0) とバイト単位で一致させること。
struct ContactShadowParamsCB
{
    XMFLOAT4X4 proj;         // 64B
    XMFLOAT4X4 invProj;      // 64B
    XMFLOAT2   invRTSize;    // 8B
    float      rayLength;    // 4B
    float      thickness;    // 4B  → 16B
    float      bias;         // 4B
    float      intensity;    // 4B
    int        steps;        // 4B
    float      maxDistance;  // 4B  → 16B
    float      fadeDistance; // 4B
    XMFLOAT3   _pad0;        // 12B → 16B
    XMFLOAT4   viewport;     // 16B  xy=原点(px), zw=サイズ(px)
    XMFLOAT4   lightDirVS;   // 16B  xyz=ビュー空間でライトへ向かう単位ベクトル
};
static_assert(sizeof(ContactShadowParamsCB) % 16 == 0, "ContactShadowParamsCB must be 16B aligned");
static_assert(sizeof(ContactShadowParamsCB) == 208, "layout mismatch with ContactShadow.hlsl");

static constexpr DXGI_FORMAT kShadowFormat = DXGI_FORMAT_R8_UNORM;

void ContactShadowPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                                   u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- RootSignature: t0(SRV table) + b0(CBV) + s0(point clamp)。SSAOPass と同じ形。---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;  // t0

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;  // b0
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderRegister   = 0;  // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 2;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    // --- 遮蔽 RT（R8_UNORM, クリア=白=遮蔽なし）---
    const float clear[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_rt = std::make_unique<RenderTarget>();
    m_rt->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kShadowFormat, clear);
    m_rtState = D3D12_RESOURCE_STATE_RENDER_TARGET;  // 作成直後は RENDER_TARGET

    m_paramCB = std::make_unique<ConstantBuffer>();
    m_paramCB->Initialize(device, sizeof(ContactShadowParamsCB), FrameResources::kFrameCount);

    Logger::Info("ContactShadowPass initialized ({}x{})", m_width, m_height);
}

void ContactShadowPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"ContactShadow_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"ContactShadow_PS.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
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
    pso.RTVFormats[0]         = kShadowFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };

    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void ContactShadowPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    if (m_rt) m_rt->Resize(device, width, height);
    m_rtState = D3D12_RESOURCE_STATE_RENDER_TARGET;  // 再作成直後は RENDER_TARGET
}

u32 ContactShadowPass::Generate(ID3D12GraphicsCommandList* cmd,
                                D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                                const ContactShadowSettings& s,
                                const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                                const DirectX::XMFLOAT3& lightDirWorld,
                                u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, u32 frameIndex)
{
    // 未準備（PSO/RT 未生成）時は無効 index を返し、呼び出し側で白ダミー(1.0)へフォールバックさせる。
    if (!m_pso || !m_rt) return DescriptorHeap::kInvalidIndex;

    const float invW = 1.0f / static_cast<float>(m_width);
    const float invH = 1.0f / static_cast<float>(m_height);

    ContactShadowParamsCB cb{};
    // HLSL は mul(row, M) なので転置して渡す（SSAO/forward と同じ運用）。
    XMStoreFloat4x4(&cb.proj,    XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
    cb.invRTSize    = {invW, invH};
    cb.rayLength    = (s.rayLength    > 0.0f) ? s.rayLength    : 0.01f;
    cb.thickness    = (s.thickness    > 0.0f) ? s.thickness    : 0.01f;
    cb.bias         = (s.bias         > 0.0f) ? s.bias         : 0.0f;
    cb.intensity    = s.intensity;
    cb.steps        = (s.steps < 4) ? 4 : ((s.steps > 32) ? 32 : s.steps);
    cb.maxDistance  = s.maxDistance;
    cb.fadeDistance = (s.fadeDistance > 0.01f) ? s.fadeDistance : 0.01f;
    cb.viewport = {static_cast<float>(vpLeft), static_cast<float>(vpTop),
                   static_cast<float>(vpW), static_cast<float>(vpH)};

    // lightDir は「光の進行方向」なので、ライトへ向かうベクトルは -lightDir。
    // それをビュー空間へ回す（平行移動は無視したいので w=0 のベクトルとして変換する）。
    {
        XMVECTOR toLightWorld = XMVector3Normalize(
            XMVectorNegate(XMLoadFloat3(&lightDirWorld)));
        XMVECTOR toLightView = XMVector3Normalize(XMVector3TransformNormal(toLightWorld, view));
        XMFLOAT3 l{};
        XMStoreFloat3(&l, toLightView);
        cb.lightDirVS = {l.x, l.y, l.z, 0.0f};
    }
    m_paramCB->Update(&cb, sizeof(cb), frameIndex);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT     sc{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    // 自前で状態追跡して遷移する（SSAOPass と同じ理由）。
    auto transition = [&](D3D12_RESOURCE_STATES next)
    {
        if (m_rtState == next) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_rt->GetResource();
        b.Transition.StateBefore = m_rtState;
        b.Transition.StateAfter  = next;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_rtState = next;
    };

    transition(D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto rtv = m_rt->GetRtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootDescriptorTable(0, depthSrvGpu);
    cmd->SetGraphicsRootConstantBufferView(1, m_paramCB->GetGpuAddress(frameIndex));
    cmd->DrawInstanced(3, 1, 0, 0);

    transition(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    return m_rt->GetSrvIndex();
}

} // namespace dx12e
