#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/ContactShadowSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class ConstantBuffer;

// 深度プリパスで完成したカメラ深度(R32_FLOAT SRV)を入力に、太陽方向へスクリーン空間
// レイマーチして近接遮蔽テクスチャ(R8_UNORM, 1=遮蔽なし)を作る。
// 構造は SSAOPass と同じ（専用 RootSignature / PSO / RT / パラメータ CB を自己完結で保持）。
// SSAO と違いブラーは掛けない（接地の細い影が潰れるため。ディザは PS 側の
// interleaved gradient noise で散らしてある）。
class ContactShadowPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す（ルートシグネチャ/RT/CB は不変）。
    void RecreatePipelines(GraphicsDevice& device);

    // sceneRT / depth と同タイミングで RT を作り直す（フル解像度）。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // 深度SRV(depthSrvGpu)を読み遮蔽テクスチャを生成し、その SRV index を返す。
    // 呼び出し側は事前に depth を PIXEL_SHADER_RESOURCE へ遷移しておくこと。
    // 戻り時、RT は PIXEL_SHADER_RESOURCE 状態。RT/ビューポートは呼び出し側で再設定すること。
    // lightDirWorld: 太陽の進行方向（PerFrame の lightDir と同じ。内部で「ライトへ向かう」
    //   ビュー空間ベクトルへ変換する）。
    // vpLeft/vpTop/vpW/vpH: ジオメトリが深度バッファへ描かれているサブ矩形（px）。
    // 未準備（PSO/RT 未生成）時は DescriptorHeap::kInvalidIndex を返す。
    u32  Generate(ID3D12GraphicsCommandList* cmd,
                  D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                  const ContactShadowSettings& s,
                  const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                  const DirectX::XMFLOAT3& lightDirWorld,
                  u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, u32 frameIndex);

    bool IsReady() const { return m_pso != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    std::unique_ptr<RenderTarget>   m_rt;       // R8_UNORM（1=遮蔽なし）
    std::unique_ptr<ConstantBuffer> m_paramCB;  // ContactShadowParams（フレーム多重化）
    // RT は native cmd で遷移するため RenderTarget の内部状態を使わず自前で追跡する。
    D3D12_RESOURCE_STATES m_rtState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    u32 m_width  = 0;
    u32 m_height = 0;
    std::wstring m_shaderDir;   // RecreatePipelines 用に保持
};

} // namespace dx12e
