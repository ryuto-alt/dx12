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

// カメラモーションブラー（velocity buffer 不要の深度再構成方式）。
// 深度 + 逆viewProj でワールド位置を復元し、前フレーム viewProj で再投影した速度方向に
// N タップ平均する。出力 RT はシーンと同じ正規化 UV レイアウトで、uber の入力シーンとして使う。
class MotionBlurPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す(ルートシグネチャ/RTは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // invViewProj / prevViewProj は転置済み（HLSL mul(M, v) 規約）で渡すこと。
    // 戻り値 = ブラー済みフル解像度シーン(複合読取状態)の SRV index。未準備は kInvalidIndex。
    u32 Apply(CommandList& cmd, DescriptorHeap* srvHeap,
              D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
              D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
              const DirectX::XMFLOAT4X4& invViewProjT,
              const DirectX::XMFLOAT4X4& prevViewProjT,
              float uvOfsX, float uvOfsY, float uvScaleX, float uvScaleY,
              u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH,
              const PostProcessSettings& s);

    bool IsReady() const { return m_pso != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    std::unique_ptr<RenderTarget> m_outRT;   // フル解像度 RGBA16F
    u32 m_width  = 0;
    u32 m_height = 0;
    std::wstring m_shaderDir;   // RecreatePipelines 用に保持
};

} // namespace dx12e
