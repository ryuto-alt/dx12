#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/TaaSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;
class CommandList;

// TAA（テンポラルアンチエイリアス）の一切合切を持つ自己完結パス。
//   - 速度バッファ(R16G16_FLOAT)。深度プリパスを MRT 化して同時に書く RT。
//   - 履歴バッファ 2 枚（ping-pong）。TAA の出力がそのまま次フレームの履歴になる。
//   - 解決 PSO / デバッグ可視化 PSO / ハルトンジッタ列。
//
// 速度 RT と履歴を 1 クラスにまとめてあるのは、分けるとリサイズと状態遷移の同期漏れが
// 起きやすいため（MotionBlurPass / ContactShadowPass と同じ流儀）。
class TaaPass
{
public:
    // 速度バッファのフォーマット。ビューポートローカル UV 単位で「現UV - 前UV」を書く。
    // 1920px 幅で 1px/frame = 5.2e-4 UV。fp16 の相対精度 2^-11 で絶対誤差 0.0005px＝十分。
    // R16G16_SNORM は 1LSB = 0.06px 誤差になり履歴がズレ続けるので不可。
    static constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16_FLOAT;

    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, DXGI_FORMAT historyFormat, DXGI_FORMAT debugRtFormat,
                    const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す。
    void RecreatePipelines(GraphicsDevice& device);

    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // 履歴を捨てる（リサイズ / シーンロード / Play↔Editor 遷移 / 初回）。
    // 忘れると「Play 開始直後に前のシーンが半透明で残る」という気味の悪いバグになる。
    void InvalidateHistory() { m_historyValid = false; }

    bool IsReady() const { return m_velocityRT != nullptr; }
    bool IsResolveReady() const { return m_resolvePso != nullptr && m_history[0] != nullptr; }

    // ---- 速度バッファ + G-Buffer（深度プリパスの MRT）----
    // RTV0=速度RT / RTV1=G-Buffer / DSV=呼び出し側の深度 をバインドし、速度RTを 0 でクリアする。
    // 深度と G-Buffer のクリア/遷移は呼び出し側の責任（G-Buffer の所有は Application）。
    // ★ 速度 PSO は常に RTV 2 本で作られている。gbufferRtv を省略すると PSO 不一致になるので
    //   必ず有効なハンドルを渡すこと（00-COORDINATION §5.5）。
    void BeginVelocity(CommandList& cmd, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                       D3D12_CPU_DESCRIPTOR_HANDLE gbufferRtv,
                       u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH);
    // 速度RTを PIXEL_SHADER_RESOURCE へ遷移する。
    void EndVelocity(CommandList& cmd);
    u32  GetVelocitySrvIndex() const;

    // ---- 解決 ----
    // sceneSrv（トーンマップ前の HDR）を履歴と合成して新しい履歴 RT へ書き、その SRV index を返す。
    // 未準備なら DescriptorHeap::kInvalidIndex（呼び出し側は入力をそのまま使うこと）。
    // depthSrv は事前に PIXEL_SHADER_RESOURCE へ遷移しておくこと。
    // invViewProjT / prevViewProjT はどちらも「ジッタなし」行列の転置。
    // ★#16: シーンは RT 全面に描かれるので、可視サブ矩形の UV は要らなくなった。
    u32 Resolve(CommandList& cmd,
                D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
                D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
                const DirectX::XMFLOAT4X4& invViewProjT,
                const DirectX::XMFLOAT4X4& prevViewProjT,
                u32 sceneW, u32 sceneH,
                const TaaSettings& s);

    // ---- デバッグ可視化 ----
    // 現在バインドされている RT（バックバッファ想定）へ速度バッファを塗る。
    // 静止時に全面 (0.5,0.5,0.5) の均一グレーになるのが「ジッタが正しく除去されている」証拠。
    // ★これだけは**表示側**の矩形を受け取る（バックバッファへ描くため）。
    void DrawVelocityDebug(CommandList& cmd,
                           D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv,
                           u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, float scale);

    // ---- ジッタ ----
    // ハルトン列を1つ進めて、現フレームの NDC ジッタを返す。ラスタライズする投影行列に
    // clip 空間の平行移動として後乗算すること（proj * XMMatrixTranslation(jx, jy, 0)）。
    DirectX::XMFLOAT2 NextJitterNdc(u32 sampleCount, u32 vpW, u32 vpH, float jitterScale);
    void ResetJitter() { m_jitterIndex = 0; }

    // ラディカル逆関数（ハルトン列）。出典: https://alextardif.com/TAA.html
    static float Halton(u32 i, u32 b);

private:
    void CreateResolveRootSignature(GraphicsDevice& device);
    void CreateDebugRootSignature(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_resolveRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_resolvePso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_debugRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugPso;

    std::unique_ptr<RenderTarget> m_velocityRT;   // R16G16_FLOAT（現UV - 前UV）
    std::unique_ptr<RenderTarget> m_history[2];   // ping-pong。m_historyWrite が今フレームの書き先
    u32  m_historyWrite  = 0;
    bool m_historyValid  = false;

    u32 m_width  = 0;
    u32 m_height = 0;
    u32 m_jitterIndex = 0;

    DescriptorHeap* m_srvHeap = nullptr;   // 履歴/速度 SRV の GPU ハンドル解決用

    DXGI_FORMAT  m_historyFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    DXGI_FORMAT  m_debugRtFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::wstring m_shaderDir;
};

} // namespace dx12e
