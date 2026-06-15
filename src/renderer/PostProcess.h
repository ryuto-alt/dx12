#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"
#include "renderer/PostProcessSettings.h"

namespace dx12e
{
class GraphicsDevice;

// オフスクリーンのシーンテクスチャを入力に、フルスクリーン三角形で
// ポストエフェクトを適用してバックバッファへ出力する単一パス。
class PostProcess
{
public:
    void Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat, const std::wstring& shaderDir);

    // 呼び出し側で対象 RTV と descriptor heap(srv) を先にバインドしておくこと。
    // uvOffset/uvScale はエディタのビューポート矩形に合わせたシーンテクスチャの参照範囲。
    void Apply(ID3D12GraphicsCommandList* cmd,
               D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
               const PostProcessSettings& s,
               float uvOffsetX, float uvOffsetY,
               float uvScaleX, float uvScaleY,
               float texelW, float texelH,
               float timeSeconds = 0.0f);

    bool IsReady() const { return m_pso != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
};

} // namespace dx12e
