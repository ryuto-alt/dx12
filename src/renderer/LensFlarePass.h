#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
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

// 疑似レンズフレア（Chapman 方式）。ブルームチェーンの縮小ミップを入力に
// ゴースト+ハロー+色収差を 1/4 解像度で生成する（ビューポートローカル 0..1）。
// 強度は焼き込み済みで uber が加算合成する。ブルームパスの実行が前提。
class LensFlarePass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // bloomMipSrvGpu: BloomPass の縮小ミップ（PIXEL_SHADER_RESOURCE 状態、ローカル 0..1）。
    // 戻り値 = フレアテクスチャ(PIXEL_SHADER_RESOURCE) の SRV index。未準備は kInvalidIndex。
    u32 Generate(CommandList& cmd, DescriptorHeap* srvHeap,
                 D3D12_GPU_DESCRIPTOR_HANDLE bloomMipSrvGpu,
                 const PostProcessSettings& s);

    bool IsReady() const { return m_pso != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    std::unique_ptr<RenderTarget> m_flareRT;   // 1/4 解像度 R11G11B10
    u32 m_width  = 0;
    u32 m_height = 0;
};

} // namespace dx12e
