#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>

#include "core/Types.h"
#include "renderer/PostProcessSettings.h"

namespace dx12e
{
class GraphicsDevice;

// 自動露出（eye adaptation）。
// compute 2 パス: ①シーンHDR のログ輝度 256bin ヒストグラム ②加重平均→時間適応→露出倍率。
// 露出値は GPU 内の StructuredBuffer<float>（[0]=倍率, [1]=適応済み平均輝度）に書かれ、
// PostProcess(uber) がルート SRV で直接読む（CPU readback なし）。
class AutoExposurePass
{
public:
    void Initialize(GraphicsDevice& device, const std::wstring& shaderDir);

    // シェーダーホットリロード用。PSO のみ作り直す(ルートシグネチャ/バッファは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

    // シーンRT は NON_PIXEL_SHADER_RESOURCE を含む読み取り状態にしておくこと。
    // rect*: シーンRT内の測光サブ矩形（px）。dt: フレーム時間（初回は内部で即適応）。
    void Generate(ID3D12GraphicsCommandList* cmd,
                  D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                  u32 rectLeft, u32 rectTop, u32 rectW, u32 rectH,
                  float dt, const PostProcessSettings& s);

    // uber パスが露出バッファを PS で読む前に呼ぶ（UAV→PIXEL_SHADER_RESOURCE 遷移）。
    // Generate をスキップしたフレームでも呼んで良い（状態が既に読取なら no-op）。
    void EnsureReadable(ID3D12GraphicsCommandList* cmd);

    D3D12_GPU_VIRTUAL_ADDRESS GetExposureBufferVA() const
    {
        return m_exposureBuf ? m_exposureBuf->GetGPUVirtualAddress() : 0;
    }

    bool IsReady() const { return m_psoHistogram != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoHistogram;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAdapt;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_histBuf;      // 256 x uint (UAV)
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_exposureBuf;  // 2 x float (UAV/SRV)
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_zeroUpload;   // 初回ゼロ初期化用
    D3D12_RESOURCE_STATES m_exposureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool m_first = true;
    std::wstring m_shaderDir;   // RecreatePipelines 用に保持
};

} // namespace dx12e
