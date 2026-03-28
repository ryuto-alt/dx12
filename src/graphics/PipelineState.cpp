#include "graphics/PipelineState.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

// ===========================================================================
// PipelineStateBuilder
// ===========================================================================
PipelineStateBuilder::PipelineStateBuilder()
{
    // BlendState デフォルト（不透明）
    D3D12_BLEND_DESC& blend = m_desc.BlendState;
    blend.AlphaToCoverageEnable  = FALSE;
    blend.IndependentBlendEnable = FALSE;
    blend.RenderTarget[0].BlendEnable           = FALSE;
    blend.RenderTarget[0].LogicOpEnable         = FALSE;
    blend.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend             = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // RasterizerState
    D3D12_RASTERIZER_DESC& raster = m_desc.RasterizerState;
    raster.FillMode              = D3D12_FILL_MODE_SOLID;
    raster.CullMode              = D3D12_CULL_MODE_BACK;
    raster.FrontCounterClockwise = FALSE;
    raster.DepthBias             = D3D12_DEFAULT_DEPTH_BIAS;
    raster.DepthBiasClamp        = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    raster.SlopeScaledDepthBias  = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    raster.DepthClipEnable       = TRUE;
    raster.MultisampleEnable     = FALSE;
    raster.AntialiasedLineEnable = FALSE;
    raster.ForcedSampleCount     = 0;
    raster.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    // DepthStencilState
    D3D12_DEPTH_STENCIL_DESC& depth = m_desc.DepthStencilState;
    depth.DepthEnable      = TRUE;
    depth.DepthWriteMask   = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc        = D3D12_COMPARISON_FUNC_LESS;
    depth.StencilEnable    = FALSE;
    depth.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
    depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

    depth.FrontFace.StencilFailOp      = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp      = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc        = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.BackFace = depth.FrontFace;

    // その他デフォルト
    m_desc.SampleMask            = UINT_MAX;
    m_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    m_desc.NumRenderTargets      = 1;
    m_desc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    m_desc.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    m_desc.SampleDesc.Count      = 1;
    m_desc.SampleDesc.Quality    = 0;
}

PipelineStateBuilder& PipelineStateBuilder::SetRootSignature(ID3D12RootSignature* rs)
{
    m_desc.pRootSignature = rs;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetVertexShader(const void* bytecode, SIZE_T size)
{
    m_desc.VS.pShaderBytecode = bytecode;
    m_desc.VS.BytecodeLength  = size;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetPixelShader(const void* bytecode, SIZE_T size)
{
    m_desc.PS.pShaderBytecode = bytecode;
    m_desc.PS.BytecodeLength  = size;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetInputLayout(const D3D12_INPUT_ELEMENT_DESC* elements, u32 count)
{
    m_desc.InputLayout.pInputElementDescs = elements;
    m_desc.InputLayout.NumElements        = count;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetRenderTargetFormat(DXGI_FORMAT format)
{
    m_desc.RTVFormats[0] = format;
    m_desc.NumRenderTargets = (format == DXGI_FORMAT_UNKNOWN) ? 0 : 1;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetDepthStencilFormat(DXGI_FORMAT format)
{
    m_desc.DSVFormat = format;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetDepthEnabled(bool enabled)
{
    m_desc.DepthStencilState.DepthEnable = enabled ? TRUE : FALSE;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetAlphaBlendEnabled(bool enabled)
{
    auto& rt = m_desc.BlendState.RenderTarget[0];
    rt.BlendEnable    = enabled ? TRUE : FALSE;
    rt.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp        = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha  = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetCullMode(D3D12_CULL_MODE mode)
{
    m_desc.RasterizerState.CullMode = mode;
    return *this;
}

PipelineStateBuilder& PipelineStateBuilder::SetDepthBias(i32 bias, f32 slopeScaledBias)
{
    m_desc.RasterizerState.DepthBias = bias;
    m_desc.RasterizerState.SlopeScaledDepthBias = slopeScaledBias;
    m_desc.RasterizerState.DepthBiasClamp = 0.0f;
    return *this;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateBuilder::Build(GraphicsDevice& device)
{
    DX_ASSERT(m_desc.pRootSignature, "Root signature must be set before building PSO");

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&m_desc, IID_PPV_ARGS(&pso)));

    Logger::Info("PipelineState created");
    return pso;
}

// ===========================================================================
// PipelineState
// ===========================================================================
void PipelineState::Initialize(GraphicsDevice& device, PipelineStateBuilder& builder)
{
    m_pso = builder.Build(device);
}

} // namespace dx12e
