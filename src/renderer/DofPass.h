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

// 被写界深度（gather ボケ）。3 パス構成:
//   ①半解像度へ 色+CoC → ②ゴールデンアングル 32 タップ gather → ③フル解像度で合成。
// 出力 RT はシーンと同じ正規化 UV レイアウト（サブ矩形対応）で、uber の入力シーンとして使う。
class DofPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO(CoC/Gather/Composite)のみ作り直す(ルートシグネチャ/RTは不変)。
    void RecreatePipelines(GraphicsDevice& device);

    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // sceneSrvGpu/depthSrvGpu は PIXEL_SHADER_RESOURCE 状態で渡すこと。
    // projA=proj._33 / projB=proj._43（深度線形化）。
    // 戻り値 = ボケ合成済みフル解像度シーン(PIXEL_SHADER_RESOURCE)の SRV index。未準備は kInvalidIndex。
    u32 Apply(CommandList& cmd, DescriptorHeap* srvHeap,
              D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
              D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
              float uvOfsX, float uvOfsY, float uvScaleX, float uvScaleY,
              u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH,
              float projA, float projB,
              const PostProcessSettings& s);

    bool IsReady() const { return m_psoComposite != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCoc;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoGather;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoComposite;
    std::unique_ptr<RenderTarget> m_halfCoc;    // 半解像度 RGBA16F（色+CoC）
    std::unique_ptr<RenderTarget> m_halfBlur;   // 半解像度 RGBA16F（ボケ+量）
    std::unique_ptr<RenderTarget> m_outRT;      // フル解像度 RGBA16F
    u32 m_width  = 0;
    u32 m_height = 0;
    std::wstring m_shaderDir;   // RecreatePipelines 用に保持
};

} // namespace dx12e
