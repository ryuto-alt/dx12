#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;

// 線形 HDR スクリーンショット（screenshot_final {format:"pfm"|"exr"}）用の読み出しパス（Q2）。
// ポスト(uber)へ入る直前のシーン色を RGBA32F のバッファへ写して readback へコピーする compute 1 本。
// 既定では 1 命令も記録しない（要求のあるフレームだけ Record を呼ぶ）。資源は Record のたびに作って Fetch で捨てる
// （撮影は低頻度なので常駐させない＝VRAM を食わない）。
class LinearCapturePass
{
public:
    void Initialize(GraphicsDevice& device, const std::wstring& shaderDir);
    // シェーダーホットリロード用。PSO のみ作り直す。
    void RecreatePipelines(GraphicsDevice& device);

    // srcSrvGpu = シーン HDR（読み取り状態。AutoExposure と同じ位置・同じ状態で呼ぶ）。w x h = そのテクスチャの寸法。
    // 呼び出し側は ID3D12DescriptorHeap（SRV ヒープ）を設定済みであること。
    bool Record(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd,
                D3D12_GPU_DESCRIPTOR_HANDLE srcSrvGpu, u32 w, u32 h, std::string& err);

    bool HasPending() const { return m_readback != nullptr; }

    // GPU 完了後（呼び出し側が WaitIdle 済み）に呼ぶ。rgb は上の行から順の RGB float（w*h*3）。呼ぶと資源を解放する。
    bool Fetch(std::vector<float>& rgb, u32& w, u32& h, std::string& err);
    void Discard();

    bool IsReady() const { return m_pso != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_outBuf;     // DEFAULT / UAV（RGBA32F x w*h）
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_readback;   // READBACK
    u32          m_w = 0, m_h = 0;
    std::wstring m_shaderDir;
};

} // namespace dx12e
