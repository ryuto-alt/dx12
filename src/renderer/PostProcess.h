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
class ConstantBuffer;

// オフスクリーンのシーンテクスチャを入力に、フルスクリーン三角形で
// ポストエフェクトを適用してバックバッファへ出力する最終(uber)パス。
// ブルーム(BloomPass)・自動露出(AutoExposurePass)の結果もここで合成する。
// パラメータはルート定数ではなく CBV（フレーム×Apply回数で多重化）で渡す＝
// 64 DWORD のルート定数上限に縛られず自由に拡張できる。
class PostProcess
{
public:
    PostProcess();
    ~PostProcess();

    void Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat,
                    const std::wstring& shaderDir, u32 frameCount);

    // 呼び出し側で対象 RTV と descriptor heap(srv) を先にバインドしておくこと。
    // uvOffset/uvScale はエディタのビューポート矩形に合わせたシーンテクスチャの参照範囲。
    // bloomSrvGpu/lutSrvGpu: 無効時は白ダミー等の有効なハンドルを渡す（マスクで参照ゲート）。
    // bloomReady=false なら bloomOn でもブルーム合成しない（BloomPass 未実行フレーム対策）。
    // lutSize < 2 なら LUT 無効。exposureBufVA=0 なら自動露出無効。
    // frameIndex はフレーム多重化 CB のスロット選択（同一フレーム内の複数 Apply も安全）。
    void Apply(ID3D12GraphicsCommandList* cmd,
               D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
               D3D12_GPU_DESCRIPTOR_HANDLE bloomSrvGpu,
               D3D12_GPU_DESCRIPTOR_HANDLE lutSrvGpu,
               float lutSize,
               D3D12_GPU_VIRTUAL_ADDRESS exposureBufVA,
               const PostProcessSettings& s,
               bool bloomReady,
               float uvOffsetX, float uvOffsetY,
               float uvScaleX, float uvScaleY,
               float texelW, float texelH,
               float timeSeconds,
               u32 frameIndex);

    bool IsReady() const { return m_pso != nullptr; }

private:
    // 1 フレーム内で Apply が呼ばれる最大回数（メイン + カメラプレビュー + 予備）。
    static constexpr u32 kMaxAppliesPerFrame = 4;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    std::unique_ptr<ConstantBuffer> m_cb;   // frameCount × kMaxAppliesPerFrame スロット
    u32 m_frameCount     = 3;
    u32 m_lastFrameIndex = 0xFFFFFFFFu;
    u32 m_applyIndex     = 0;
};

} // namespace dx12e
