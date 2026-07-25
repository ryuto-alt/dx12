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
    // MRT（複数レンダーターゲット）版。深度プリパスの DepthVelocityGBuffer モード用。
    // count は 0..8。既存の SetRenderTargetFormat（1本 or 0本）は温存してあるので、
    // 従来の呼び出し側は一切影響を受けない。
    PipelineStateBuilder& SetRenderTargetFormats(u32 count, const DXGI_FORMAT* formats);
    PipelineStateBuilder& SetDepthStencilFormat(DXGI_FORMAT format);
    PipelineStateBuilder& SetDepthEnabled(bool enabled);
    PipelineStateBuilder& SetDepthWrite(bool enabled);  // 深度テストは残しつつ書き込みのみ制御（半透明オーバーレイ用）
    PipelineStateBuilder& SetAlphaBlendEnabled(bool enabled);
    PipelineStateBuilder& SetAdditiveBlendEnabled(bool enabled);  // 加算合成（発光パーティクル用）
    PipelineStateBuilder& SetCullMode(D3D12_CULL_MODE mode);
    PipelineStateBuilder& SetDepthBias(i32 bias, f32 slopeScaledBias);
    PipelineStateBuilder& SetDepthFunc(D3D12_COMPARISON_FUNC func);  // 深度比較関数（プリパス併用で LESS_EQUAL 等）

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
