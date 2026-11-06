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

// ローカル露出（UE5 の Local Exposure 相当）。シーン HDR（TAA / DoF / モーションブラー後・露出前）を入力に、
// 露出後の対数輝度から画素ごとの base を作り、明部/暗部のコントラストだけを縮めた色を
// シーンと同寸の RGBA16F へ書く。後段（ブルーム / uber）はこの出力を「シーン」として読む。
//   ① CSGrid      16x16 画素 = 1 柱のバイラテラルグリッド（輝度方向 32 段 = 0.5EV 刻み、範囲 -10..+6 EV）
//   ② CSDown      8x8 画素の対数輝度平均（1/8 縮小）
//   ③ CSGridBlur  グリッドの 3x3x3 ぼかし   ④ CSLumBlur  縮小輝度の 9x9 ガウス
//   ⑤ PS          base = lerp(グリッド trilinear, ぼかし輝度, leBlurredLumBlend) → 色に gain を掛ける
// 設定が OFF / 恒等（コントラスト・ディテールが全部 1）のときは NeedsRun() が false ＝ パスごと走らない。
// 解像度はシーン RT に追従（Resize）。バッファはルート SRV/UAV で渡し、ディスクリプタは出力 RT の分だけ。
class LocalExposurePass
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す（ルートシグネチャ / バッファ / RT は不変）。
    void RecreatePipelines(GraphicsDevice& device);

    // シーン RT と同じタイミングで呼ぶ（呼び出し側が GPU アイドルにしてある前提。他の Pass と同じ）。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // このフレームでパスを走らせる必要があるか（OFF・恒等なら false）。
    static bool NeedsRun(const PostProcessSettings& s);

    // sceneSrvGpu: NON_PIXEL | PIXEL の複合読取状態のシーン HDR。
    // exposureVA: 自動露出バッファ（AutoExposurePass が NON_PIXEL | PIXEL の読取状態にしてあること。0 なら無し）。
    // 戻り値 = 補正後シーン（複合読取状態）の SRV index。未準備 / バッファ不足なら kInvalidIndex（呼び出し側は素通し）。
    // 呼び出し後、RT / ビューポート / 記述子ヒープは呼び出し側で張り直すこと。
    u32 Apply(CommandList& cmd,
              D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
              D3D12_GPU_VIRTUAL_ADDRESS exposureVA,
              u32 sceneW, u32 sceneH,
              const PostProcessSettings& s);

    bool IsReady() const { return m_psoGrid != nullptr && m_psoApply != nullptr; }

private:
    void CreateBuffers(GraphicsDevice& device, u32 width, u32 height);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_csRootSig;   // b0 + t0(table) + t1(root SRV) + u0..u3(root UAV)
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gfxRootSig;  // b0 + t0(table) + t1/t2/t3(root SRV)
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoGrid;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDown;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoGridBlur;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoLumBlur;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoApply;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_gridRaw;    // uint2 x (gw*gh*32)        常に UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gridBlur;   // float2 x (gw*gh*32)       UAV ↔ PIXEL_SHADER_RESOURCE
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lumA;       // float x (lw*lh)           常に UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lumB;       // float x (lw*lh)           UAV ↔ PIXEL_SHADER_RESOURCE
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dummyExpo;  // 露出バッファ無しのときの t1（UPLOAD / GENERIC_READ）
    std::unique_ptr<RenderTarget> m_outRT;               // シーンと同寸 RGBA16F

    u32 m_width = 0, m_height = 0;     // 確保済みバッファが対応する最大のシーン寸法
    std::wstring m_shaderDir;
};

} // namespace dx12e
