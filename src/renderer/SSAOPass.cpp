#include "renderer/SSAOPass.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/RenderTarget.h"
#include "graphics/Buffer.h"
#include "graphics/FrameResources.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstring>
#include <random>

namespace dx12e
{
using namespace DirectX;

// HLSL の cbuffer SSAOParams(b0) とバイト単位で一致させること。
struct SSAOParamsCB
{
    XMFLOAT4X4 proj;        // 64B
    XMFLOAT4X4 invProj;     // 64B
    XMFLOAT2   invRTSize;   // 8B
    float      radius;      // 4B
    float      bias;        // 4B  → 16B
    float      intensity;   // 4B
    float      power;       // 4B
    float      nearZ;       // 4B
    float      farZ;        // 4B  → 16B
    int        sampleCount; // 4B
    float      _pad0;       // 4B
    XMFLOAT2   _pad1;       // 8B  → 16B
    XMFLOAT4   kernel[16];  // 256B
};
static_assert(sizeof(SSAOParamsCB) % 16 == 0, "SSAOParamsCB must be 16B aligned");
static_assert(sizeof(SSAOParamsCB) == 432, "SSAOParamsCB layout mismatch with SSAO.hlsl");

// HLSL の cbuffer BlurCB(b0) と一致。
struct SSAOBlurCB
{
    XMFLOAT2 invRTSize;
    XMFLOAT2 _pad;
};
static_assert(sizeof(SSAOBlurCB) % 16 == 0, "SSAOBlurCB must be 16B aligned");

static constexpr DXGI_FORMAT kAOFormat = DXGI_FORMAT_R8_UNORM;

void SSAOPass::Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                          u32 width, u32 height, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    // --- 半球カーネル生成（[0,1) z 上向き、距離を内側へ寄せる）---
    {
        std::mt19937 rng(1337u);
        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
        std::uniform_real_distribution<float> distNP(-1.0f, 1.0f);
        for (int i = 0; i < 16; ++i)
        {
            XMVECTOR v = XMVectorSet(distNP(rng), distNP(rng), dist01(rng), 0.0f);
            v = XMVector3Normalize(v);
            v = XMVectorScale(v, dist01(rng));
            // 近距離側に多くサンプルを寄せる（accelerated interpolation）
            float scale = static_cast<float>(i) / 16.0f;
            scale = 0.1f + 0.9f * scale * scale;
            v = XMVectorScale(v, scale);
            XMStoreFloat4(&m_kernel[i], v);
            m_kernel[i].w = 0.0f;
        }
    }

    // --- RootSignature: t0(SRV table) + b0(CBV) + s0(point clamp) ---
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

    // --- PSO（共通: VB なしフルスクリーン三角形 / Depth OFF / Cull NONE / RT=R8_UNORM）---
    auto buildPso = [&](const std::wstring& vsFile, const std::wstring& psFile,
                        Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + vsFile);
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + psFile);

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
        pso.RTVFormats[0]         = kAOFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    buildPso(L"SSAO_VS.cso", L"SSAO_PS.cso",     m_psoAO);
    buildPso(L"SSAO_VS.cso", L"SSAOBlur_PS.cso", m_psoBlur);

    // --- AO / Blur RT（R8_UNORM, クリア=白=AOなし）---
    const float aoClear[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_aoRT = std::make_unique<RenderTarget>();
    m_aoRT->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kAOFormat, aoClear);
    m_blurRT = std::make_unique<RenderTarget>();
    m_blurRT->Initialize(device, rtvHeap, srvHeap, m_width, m_height, kAOFormat, aoClear);
    m_aoState   = D3D12_RESOURCE_STATE_RENDER_TARGET;  // 作成直後は RENDER_TARGET
    m_blurState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // --- パラメータ CB（フレーム多重化）---
    m_paramCB = std::make_unique<ConstantBuffer>();
    m_paramCB->Initialize(device, sizeof(SSAOParamsCB), FrameResources::kFrameCount);
    m_blurCB = std::make_unique<ConstantBuffer>();
    m_blurCB->Initialize(device, sizeof(SSAOBlurCB), FrameResources::kFrameCount);

    Logger::Info("SSAOPass initialized ({}x{})", m_width, m_height);
}

void SSAOPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    if (m_aoRT)   m_aoRT->Resize(device, width, height);
    if (m_blurRT) m_blurRT->Resize(device, width, height);
    m_aoState   = D3D12_RESOURCE_STATE_RENDER_TARGET;  // 再作成直後は RENDER_TARGET
    m_blurState = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

u32 SSAOPass::Generate(ID3D12GraphicsCommandList* cmd, DescriptorHeap* srvHeap,
                       D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                       const SSAOSettings& s, const DirectX::XMMATRIX& proj,
                       float nearZ, float farZ, u32 frameIndex)
{
    if (!m_psoAO || !m_aoRT) return 0;

    const float invW = 1.0f / static_cast<float>(m_width);
    const float invH = 1.0f / static_cast<float>(m_height);

    // ---- パラメータ CB 更新 ----
    SSAOParamsCB cb{};
    // HLSL は mul(row, M) なので転置して渡す（forward と同じ運用）。
    XMStoreFloat4x4(&cb.proj,    XMMatrixTranspose(proj));
    XMStoreFloat4x4(&cb.invProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
    cb.invRTSize   = {invW, invH};
    cb.radius      = s.radius;
    cb.bias        = s.bias;
    cb.intensity   = s.intensity;
    cb.power       = s.power;
    cb.nearZ       = nearZ;
    cb.farZ        = farZ;
    cb.sampleCount = (s.sampleCount >= 16) ? 16 : ((s.sampleCount <= 8) ? 8 : s.sampleCount);
    for (int i = 0; i < 16; ++i) cb.kernel[i] = m_kernel[i];
    m_paramCB->Update(&cb, sizeof(cb), frameIndex);

    SSAOBlurCB bcb{};
    bcb.invRTSize = {invW, invH};
    m_blurCB->Update(&bcb, sizeof(bcb), frameIndex);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    D3D12_RECT     sc{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    // リソースを target 状態へ遷移するヘルパ（自前で状態追跡）。
    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES next)
    {
        if (cur == next) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = cur;
        b.Transition.StateAfter  = next;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        cur = next;
    };

    // ===== Pass1: SSAO（depth → aoRT）=====
    {
        transition(m_aoRT->GetResource(), m_aoState, D3D12_RESOURCE_STATE_RENDER_TARGET);

        auto rtv = m_aoRT->GetRtv();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->SetPipelineState(m_psoAO.Get());
        cmd->SetGraphicsRootDescriptorTable(0, depthSrvGpu);
        cmd->SetGraphicsRootConstantBufferView(1, m_paramCB->GetGpuAddress(frameIndex));
        cmd->DrawInstanced(3, 1, 0, 0);

        transition(m_aoRT->GetResource(), m_aoState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (!s.blur)
        return m_aoRT->GetSrvIndex();

    // ===== Pass2: Blur（aoRT → blurRT）=====
    {
        transition(m_blurRT->GetResource(), m_blurState, D3D12_RESOURCE_STATE_RENDER_TARGET);

        auto rtv = m_blurRT->GetRtv();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->SetPipelineState(m_psoBlur.Get());
        cmd->SetGraphicsRootDescriptorTable(0, srvHeap->GetGpuHandle(m_aoRT->GetSrvIndex()));
        cmd->SetGraphicsRootConstantBufferView(1, m_blurCB->GetGpuAddress(frameIndex));
        cmd->DrawInstanced(3, 1, 0, 0);

        transition(m_blurRT->GetResource(), m_blurState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    return m_blurRT->GetSrvIndex();
}

} // namespace dx12e
