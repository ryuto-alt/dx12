#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/PostProcessSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class CommandList;

// スクリーンスペース ゴッドレイ（GPU Gems 3 方式）。
// 深度から空マスク（半解像度）→ 太陽スクリーン位置への 64 タップ放射ブラー。
// 出力はシーンと同じ正規化 UV レイアウト（強度・フェード焼き込み済み）。uber が加算合成。
class GodRaysPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // depthSrvGpu: PIXEL_SHADER_RESOURCE 状態の深度(R32_FLOAT)。
    // sunUv: 太陽のスクリーンUV（フルRT正規化空間）。fade: 画面外/背面フェード 0..1。
    // 戻り値 = 光条テクスチャ(PIXEL_SHADER_RESOURCE)の SRV index。未準備は kInvalidIndex。
    u32 Generate(CommandList& cmd, DescriptorHeap* srvHeap,
                 D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                 float uvOfsX, float uvOfsY, float uvScaleX, float uvScaleY,
                 u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH,
                 float sunUvX, float sunUvY, float fade,
                 const DirectX::XMFLOAT3& sunColor,
                 const PostProcessSettings& s);

    bool IsReady() const { return m_psoMask != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBlur;
    std::unique_ptr<RenderTarget> m_maskRT;    // 半解像度 R11G11B10
    std::unique_ptr<RenderTarget> m_shaftRT;   // 半解像度 R11G11B10
    u32 m_width  = 0;
    u32 m_height = 0;
};

} // namespace dx12e
