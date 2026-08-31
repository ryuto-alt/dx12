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
    // ★#16: シーンは RT 全面に描かれるので、可視サブ矩形の UV も描画先の矩形も要らなくなった
    //   （半解像度チェーンを vpLeft/2, vpW/2 と整数半減で作るオフバイワンも同時に消えた）。
    // proj22 = proj._22（= 1/tan(fovY/2)）。dofFocalLength=0 のとき、ここから
    //   f(mm) = (センサ高 24mm / 2) * proj22
    // で焦点距離を導く＝【画角と焦点距離が必ず一致する】ので、絞り(F値)だけで絵が決まる。
    // focusDistOverride > 0 なら dofFocusDist の代わりにそれを合焦距離に使う
    //   （dofFocusName のエンティティまでのビュー距離。呼び出し側が解決する）。
    u32 Apply(CommandList& cmd, DescriptorHeap* srvHeap,
              D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
              D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
              float projA, float projB, float proj22,
              const PostProcessSettings& s,
              float focusDistOverride = -1.0f);

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
