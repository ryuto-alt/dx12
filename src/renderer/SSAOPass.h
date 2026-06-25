#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/SSAOSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class ConstantBuffer;

// 深度プリパスで完成したカメラ深度(R32_FLOAT SRV)を入力に、
// 半球カーネル SSAO → 4x4 ボックスブラー を実行して AO テクスチャ(R8_UNORM)を生成する。
// 専用 RootSignature / PSO / RT(AO, Blur) / パラメータ CB を自己完結で保持する。
class SSAOPass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // sceneRT / depth と同タイミングで AO/Blur RT を作り直す（フル解像度）。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // 深度SRV(depthSrvGpu)を読み AO→Blur を実行し、最終 AO の SRV index を返す。
    // 呼び出し側は事前に depth を PIXEL_SHADER_RESOURCE へ遷移しておくこと。
    // 戻り時、AO/Blur RT は PIXEL_SHADER_RESOURCE 状態。RT/ビューポートは呼び出し側で再設定すること。
    // vpLeft/vpTop/vpW/vpH: ジオメトリが深度バッファへ描かれているサブ矩形（px）。
    //   フル RT(m_width×m_height) の UV↔NDC をこの矩形基準で計算してエディタ視点でも歪まないようにする。
    // 未準備（PSO/RT 未生成）時は DescriptorHeap::kInvalidIndex を返す（呼び出し側で白ダミーへフォールバック）。
    u32  Generate(ID3D12GraphicsCommandList* cmd, DescriptorHeap* srvHeap,
                  D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                  const SSAOSettings& s, const DirectX::XMMATRIX& proj,
                  float nearZ, float farZ,
                  u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, u32 frameIndex);

    bool IsReady() const { return m_psoAO != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBlur;
    std::unique_ptr<RenderTarget>   m_aoRT;     // R8_UNORM（生 AO）
    std::unique_ptr<RenderTarget>   m_blurRT;   // R8_UNORM（ブラー済み）
    std::unique_ptr<ConstantBuffer> m_paramCB;  // SSAOParams（フレーム多重化）
    std::unique_ptr<ConstantBuffer> m_blurCB;   // BlurCB（フレーム多重化）
    DirectX::XMFLOAT4 m_kernel[16]{};           // 半球カーネル（初期化時に1度生成）
    // AO/Blur RT は native cmd で遷移するため RenderTarget の内部状態を使わず自前で追跡する。
    D3D12_RESOURCE_STATES m_aoState   = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES m_blurState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    u32 m_width  = 0;
    u32 m_height = 0;
};

} // namespace dx12e
