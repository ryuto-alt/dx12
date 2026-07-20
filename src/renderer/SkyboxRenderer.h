#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include <DirectXMath.h>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;

// 全画面三角形で環境キューブを view ray でサンプルし背景を塗る。
// 自前 RootSig / PSO を持つ（メイン graphics RootSig を汚さない）。
// 深度テスト/書き込み OFF なので、不透明描画の前に塗って後続不透明が上書きする運用。
class SkyboxRenderer
{
public:
    void Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat, const std::wstring& shaderDirW);

    // シェーダーホットリロード用。PSO のみ作り直す(ルートシグネチャは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

    // 呼び出し側で対象 RTV と descriptor heap(srv) を先にバインドしておくこと。
    // envCubeSrvGpu は環境キューブの shader-visible SRV(GPU ハンドル)。
    // invViewProj は HLSL 行ベクトル mul 用に転置済みを渡すこと。
    void Render(ID3D12GraphicsCommandList* cmd,
                D3D12_GPU_DESCRIPTOR_HANDLE envCubeSrvGpu,
                const DirectX::XMFLOAT4X4& invViewProj,
                float intensity);

    bool IsReady() const { return m_pso != nullptr; }
    void Reset();

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;
    DXGI_FORMAT  m_rtvFormat = DXGI_FORMAT_UNKNOWN;
};

} // namespace dx12e
