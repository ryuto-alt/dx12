#pragma once
//
// MeshPipelineStateBuilder ― メッシュシェーダ（AS / MS / PS）パイプライン用の PSO ストリーム対応（仮想ジオメトリ P3）。
//
//   ★既存の PipelineStateBuilder（固定の D3D12_GRAPHICS_PIPELINE_STATE_DESC = VS / PS / IA のみ）には 1 行も触れない。
//     メッシュシェーダは D3D12_GRAPHICS_PIPELINE_STATE_DESC に居場所が無く、PSO ストリーム
//     （D3D12_PIPELINE_STATE_STREAM_DESC。d3dx12 の CD3DX12_PIPELINE_MESH_STATE_STREAM）でしか作れない。
//   ★ヘッダオンリー・D3D12 デバイスだけで動く（GraphicsDevice に依存しない＝窓なしのヘッドレステストからも使える）。
//   ★使う側の前提: MeshShaderTier >= 1（GraphicsDevice::SupportsMeshShaders / OPTIONS7）。ID3D12Device2 が要る（Win10 2004+ の in-box）。
//
//   使い方:
//     MeshPipelineStateBuilder b;
//     b.SetRootSignature(rs).SetMeshShader(ms, msSize).SetPixelShader(ps, psSize)   // AS は任意
//      .SetRenderTargetFormat(DXGI_FORMAT_R32_UINT).SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
//      .SetDepthFunc(D3D12_COMPARISON_FUNC_LESS);
//     ComPtr<ID3D12PipelineState> pso = b.Build(device, &err);
//
#include <directx/d3d12.h>
#include <directx/d3dx12_pipeline_state_stream.h>
#include <wrl/client.h>

#include <climits>
#include <cstdio>
#include <string>

namespace dx12e
{

class MeshPipelineStateBuilder
{
public:
    MeshPipelineStateBuilder()
    {
        D3D12_BLEND_DESC& blend = m_desc.BlendState;
        blend.AlphaToCoverageEnable = FALSE;
        blend.IndependentBlendEnable = FALSE;
        blend.RenderTarget[0].BlendEnable = FALSE;
        blend.RenderTarget[0].LogicOpEnable = FALSE;
        blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_RASTERIZER_DESC& r = m_desc.RasterizerState;
        r.FillMode = D3D12_FILL_MODE_SOLID;
        r.CullMode = D3D12_CULL_MODE_NONE;          // 両面（フォワードと同じ）
        r.FrontCounterClockwise = FALSE;
        r.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        r.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        r.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        r.DepthClipEnable = TRUE;
        r.MultisampleEnable = FALSE;
        r.AntialiasedLineEnable = FALSE;
        r.ForcedSampleCount = 0;
        r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        D3D12_DEPTH_STENCIL_DESC& d = m_desc.DepthStencilState;
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        d.StencilEnable = FALSE;
        d.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        d.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        d.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        d.BackFace = d.FrontFace;

        m_desc.SampleMask = UINT_MAX;
        m_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        m_desc.NumRenderTargets = 1;
        m_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        m_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        m_desc.SampleDesc.Count = 1;
    }

    MeshPipelineStateBuilder& SetRootSignature(ID3D12RootSignature* rs) { m_desc.pRootSignature = rs; return *this; }
    MeshPipelineStateBuilder& SetAmplificationShader(const void* bc, SIZE_T size) { m_desc.AS = {bc, size}; return *this; }
    MeshPipelineStateBuilder& SetMeshShader(const void* bc, SIZE_T size) { m_desc.MS = {bc, size}; return *this; }
    MeshPipelineStateBuilder& SetPixelShader(const void* bc, SIZE_T size) { m_desc.PS = {bc, size}; return *this; }
    // 0 本（深度だけ / UAV だけ）も可。count は 0..8。
    MeshPipelineStateBuilder& SetRenderTargetFormats(UINT count, const DXGI_FORMAT* formats)
    {
        m_desc.NumRenderTargets = count;
        for (UINT i = 0; i < 8; ++i) m_desc.RTVFormats[i] = (i < count) ? formats[i] : DXGI_FORMAT_UNKNOWN;
        return *this;
    }
    MeshPipelineStateBuilder& SetRenderTargetFormat(DXGI_FORMAT f) { return SetRenderTargetFormats(1, &f); }
    MeshPipelineStateBuilder& SetDepthStencilFormat(DXGI_FORMAT f) { m_desc.DSVFormat = f; return *this; }
    MeshPipelineStateBuilder& SetDepthEnabled(bool on) { m_desc.DepthStencilState.DepthEnable = on ? TRUE : FALSE; return *this; }
    MeshPipelineStateBuilder& SetDepthWrite(bool on)
    {
        m_desc.DepthStencilState.DepthWriteMask = on ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        return *this;
    }
    MeshPipelineStateBuilder& SetDepthFunc(D3D12_COMPARISON_FUNC f) { m_desc.DepthStencilState.DepthFunc = f; return *this; }
    MeshPipelineStateBuilder& SetCullMode(D3D12_CULL_MODE m) { m_desc.RasterizerState.CullMode = m; return *this; }
    MeshPipelineStateBuilder& SetAlphaBlendEnabled(bool on)
    {
        auto& rt = m_desc.BlendState.RenderTarget[0];
        rt.BlendEnable = on ? TRUE : FALSE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        return *this;
    }

    // 失敗したら null と理由（err）。
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Build(ID3D12Device* device, std::string* err = nullptr) const
    {
        Microsoft::WRL::ComPtr<ID3D12Device2> dev2;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dev2))))
        {
            if (err) *err = "ID3D12Device2 が無い（メッシュシェーダの PSO ストリームが作れない）";
            return nullptr;
        }
        CD3DX12_PIPELINE_MESH_STATE_STREAM stream(m_desc);
        D3D12_PIPELINE_STATE_STREAM_DESC sd{};
        sd.SizeInBytes = sizeof(stream);
        sd.pPipelineStateSubobjectStream = &stream;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr = dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
        if (FAILED(hr))
        {
            if (err)
            {
                char b[64];
                std::snprintf(b, sizeof(b), "CreatePipelineState(mesh) 0x%08X", static_cast<unsigned>(hr));
                *err = b;
            }
            return nullptr;
        }
        return pso;
    }

private:
    D3DX12_MESH_SHADER_PIPELINE_STATE_DESC m_desc{};
};

} // namespace dx12e
