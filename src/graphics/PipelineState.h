#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;

// ---------------------------------------------------------------------------
// PipelineStateBuilder  (Builder パターン)
// ---------------------------------------------------------------------------
class PipelineStateBuilder
{
public:
    PipelineStateBuilder();

    PipelineStateBuilder& SetRootSignature(ID3D12RootSignature* rs);
    PipelineStateBuilder& SetVertexShader(const void* bytecode, SIZE_T size);
    PipelineStateBuilder& SetPixelShader(const void* bytecode, SIZE_T size);
    PipelineStateBuilder& SetInputLayout(const D3D12_INPUT_ELEMENT_DESC* elements, u32 count);
    PipelineStateBuilder& SetRenderTargetFormat(DXGI_FORMAT format);
    PipelineStateBuilder& SetDepthStencilFormat(DXGI_FORMAT format);
    PipelineStateBuilder& SetDepthEnabled(bool enabled);
    PipelineStateBuilder& SetAlphaBlendEnabled(bool enabled);
    PipelineStateBuilder& SetCullMode(D3D12_CULL_MODE mode);
    PipelineStateBuilder& SetDepthBias(i32 bias, f32 slopeScaledBias);

    Microsoft::WRL::ComPtr<ID3D12PipelineState> Build(GraphicsDevice& device);

private:
    D3D12_GRAPHICS_PIPELINE_STATE_DESC m_desc{};
};

// ---------------------------------------------------------------------------
// PipelineState
// ---------------------------------------------------------------------------
class PipelineState
{
public:
    void Initialize(GraphicsDevice& device, PipelineStateBuilder& builder);

    ID3D12PipelineState* Get() const { return m_pso.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
};

} // namespace dx12e
