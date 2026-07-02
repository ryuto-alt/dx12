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

// 物理ベースブルーム（CoD:AW / Jimenez 2014 方式）。
// シーンHDR(R16G16B16A16_FLOAT) を入力に、半解像度から 6 段のダウンサンプル（13タップ、
// 初段のみ Karis average + ソフトニー抽出）→ 5 段のテントアップサンプル
// （ブレンドファクタ=radius で下段へ合成）を実行し、mip0(半解像度) の SRV を返す。
// 最終合成は PostProcess(uber) 側で col += bloom * intensity。
class BloomPass
{
public:
    static constexpr u32 kMips = 6;  // 1/2, 1/4, ... 1/64

    void Initialize(GraphicsDevice& device, DescriptorHeap* rtvHeap, DescriptorHeap* srvHeap,
                    u32 width, u32 height, const std::wstring& shaderDir);

    // sceneRT と同タイミングでチェーン RT を作り直す。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // sceneSrvGpu: PIXEL_SHADER_RESOURCE 状態のシーンHDR。
    // uvOfs/uvScale: シーンRT内の可視サブ矩形（エディタのビューポート対応）。
    // 戻り値 = ブルーム結果(mip0, PIXEL_SHADER_RESOURCE 状態) の SRV index。
    // 未準備なら DescriptorHeap::kInvalidIndex。RT/ビューポートは呼び出し側で再設定すること。
    u32 Generate(CommandList& cmd, DescriptorHeap* srvHeap,
                 D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                 float uvOfsX, float uvOfsY, float uvScaleX, float uvScaleY,
                 float sceneTexelW, float sceneTexelH,
                 const PostProcessSettings& s);

    bool IsReady() const { return m_psoDown != nullptr; }

    // チェーンの任意ミップの SRV index（レンズフレア等の入力用）。
    // Generate 直後、mip1..kMips-1 は PIXEL_SHADER_RESOURCE 状態。
    u32 GetMipSrvIndex(u32 mip) const;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDown;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoUp;    // 加算合成（BLEND_FACTOR）
    std::unique_ptr<RenderTarget> m_mips[kMips];
    u32 m_width  = 0;
    u32 m_height = 0;
};

} // namespace dx12e
