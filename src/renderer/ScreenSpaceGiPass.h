#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/SsrSettings.h"
#include "renderer/SsgiSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class ConstantBuffer;
class CommandList;

// SSR（スクリーン空間反射）と SSGI（スクリーン空間 GI）を 1 クラスにまとめた自己完結パス。
//
// 分けずにまとめてあるのは、両者が
//   ・同じ入力（深度 / G-Buffer / 速度 / 前フレームカラー）
//   ・同じルートシグネチャ・同じ cbuffer・同じフルスクリーン三角形 VS
//   ・同じライフサイクル（Resize / 履歴破棄 / ホットリロード）
// を共有していて、分けると同期漏れの箇所が倍になるため（TaaPass が速度 RT と履歴 RT を
// 1 クラスに持っているのと同じ判断）。
//
// 入力の契約:
//   depth    … 深度プリパスで完成したカメラ深度（R32_FLOAT SRV）。透視カメラ限定。
//   gbuffer  … 深度+速度プリパスの RTV1（xy=oct(ワールド法線) z=rough w=metal）。
//   velocity … TaaPass の速度バッファ（ビューポートローカル UV 単位、現UV-前UV）。
//   前フレームカラー … CaptureSceneColor() が m_sceneRT からコピーしたリニア HDR。
//                      ★トーンマップ前・露出適用前。ここを変えると GI がフィードバックで暴れる。
//
// 出力: SSR = RGBA16F（rgb=反射放射輝度 a=confidence）/ SSGI = RGBA16F（rgb=間接放射照度）。
// どちらもフル解像度で、フォワード PS が Load(SV_Position.xy) で直読みする。
class ScreenSpaceGiPass
{
public:
    // 全ての中間 RT のフォーマット。G-Buffer（Application::kGBufferFormat）と同じ値。
    static constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す。
    void RecreatePipelines(GraphicsDevice& device);

    void Resize(GraphicsDevice& device, u32 width, u32 height);

    bool IsReady()    const { return m_psoSsrTrace != nullptr; }
    // 前フレームのシーンカラーを持っているか（初回フレーム / シーン切替直後は false）。
    bool HasHistory() const { return m_colorValid; }

    // 履歴を捨てる（リサイズ / シーンロード / Play↔Editor 遷移 / ビューポート矩形変更）。
    void InvalidateHistory() { m_colorValid = false; m_ssgiHistValid = false; }

    // ポストチェーンの直前に呼ぶ。このフレームの HDR シーンカラーを退避し、ハーフ版も作る。
    // sceneRT は COPY_SOURCE へ遷移されるので、呼び出し側は続けて必要な状態へ戻すこと。
    void CaptureSceneColor(CommandList& cmd, RenderTarget& sceneRT);

    struct GenerateDesc
    {
        const SsrSettings*  ssr  = nullptr;   // nullptr = SSR を生成しない
        const SsgiSettings* ssgi = nullptr;   // nullptr = SSGI を生成しない
        DirectX::XMMATRIX   view{};
        DirectX::XMMATRIX   proj{};
        float zNear = 0.1f, zFar = 1000.0f;
        u32   vpLeft = 0, vpTop = 0, vpW = 0, vpH = 0;   // フル RT 内のサブ矩形（px）
        u32   frameIndex = 0;
        bool  hasIbl = false;
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE irradianceSrv{};     // TextureCube（SSGI のミス時フォールバック）
    };

    // SSR / SSGI を生成し、それぞれの SRV index を返す（生成しなかった側は kInvalidIndex）。
    // 呼び出し側は深度と G-Buffer を PIXEL_SHADER_RESOURCE へ遷移しておくこと。
    // 戻り時、出力 RT は PIXEL_SHADER_RESOURCE。RT/ビューポート/RootSig は呼び出し側で再設定すること。
    void Generate(CommandList& cmd, const GenerateDesc& d, u32& outSsrSrv, u32& outSsgiSrv);

private:
    void CreateRootSignature(GraphicsDevice& device);
    void CreateTargets(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDownsample;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSsrTrace;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSsrUpsample;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSsgiTrace;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSsgiTemporal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSsgiUpsample;

    std::unique_ptr<RenderTarget> m_prevColor;      // フル。m_sceneRT の CopyResource 先
    std::unique_ptr<RenderTarget> m_prevColorHalf;  // ハーフ。ぼけた前フレームカラー
    std::unique_ptr<RenderTarget> m_ssrTrace;       // ハーフ
    std::unique_ptr<RenderTarget> m_ssrOut;         // フル（フォワードが読む）
    std::unique_ptr<RenderTarget> m_ssgiTrace;      // ハーフ
    std::unique_ptr<RenderTarget> m_ssgiHist[2];    // ハーフ ping-pong
    std::unique_ptr<RenderTarget> m_ssgiOut;        // フル（フォワードが読む）
    std::unique_ptr<ConstantBuffer> m_paramCB;

    DescriptorHeap* m_rtvHeap = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    u32  m_width  = 0;
    u32  m_height = 0;
    u32  m_halfW  = 0;
    u32  m_halfH  = 0;
    u32  m_histWrite     = 0;
    u32  m_frameCounter  = 0;   // SSGI の時間ジッタ用の連番
    bool m_colorValid    = false;   // 前フレームカラーが有効か
    bool m_ssgiHistValid = false;   // SSGI の時間蓄積履歴が有効か
    std::wstring m_shaderDir;
};

} // namespace dx12e
