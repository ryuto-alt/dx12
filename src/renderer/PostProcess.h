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

    // シェーダーホットリロード用。PSO のみ作り直す(ルートシグネチャ/CBは不変のため触らない)。
    // Initialize() が内部で1回呼ぶのに加え、ShaderManager の再生成コールバックからも呼ばれる。
    void RecreatePipelines(GraphicsDevice& device);

    // uber パスへの GPU 入力一式。無効な SRV スロットにも白ダミー等の有効なハンドルを
    // 渡すこと（参照はマスクビットでゲートされる）。*Ready=false なら該当合成をスキップ。
    struct Inputs
    {
        D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv{};     // シーン（または DoF/MB 処理後のシーン）
        D3D12_GPU_DESCRIPTOR_HANDLE bloomSrv{};     // ブルーム結果（ビューポートローカル 0..1）
        D3D12_GPU_DESCRIPTOR_HANDLE lutSrv{};       // 3D LUT ストリップ
        D3D12_GPU_DESCRIPTOR_HANDLE godraysSrv{};   // ゴッドレイ光条（シーンと同レイアウト）
        D3D12_GPU_DESCRIPTOR_HANDLE flareSrv{};     // レンズフレア（ビューポートローカル 0..1）
        D3D12_GPU_DESCRIPTOR_HANDLE distortSrv{};   // パーティクル歪みバッファ（シーンと同レイアウト）
        float                       lutSize    = 0.0f;  // LUT の N（<2 で無効）
        D3D12_GPU_VIRTUAL_ADDRESS   exposureVA = 0;     // 自動露出バッファ（0 で無効）
        bool bloomReady   = false;
        bool godraysReady = false;
        bool flareReady   = false;
        bool distortReady = false;   // 歪みパーティクルが存在するフレームのみ true
    };

    // 呼び出し側で対象 RTV と descriptor heap(srv) を先にバインドしておくこと。
    // uvOffset/uvScale はエディタのビューポート矩形に合わせたシーンテクスチャの参照範囲。
    // frameIndex はフレーム多重化 CB のスロット選択（同一フレーム内の複数 Apply も安全）。
    void Apply(ID3D12GraphicsCommandList* cmd,
               const Inputs& in,
               const PostProcessSettings& s,
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
    DXGI_FORMAT  m_outFormat = DXGI_FORMAT_UNKNOWN;  // RecreatePipelines 用に保持
    std::wstring m_shaderDir;                        // 同上
    u32 m_frameCount     = 3;
    u32 m_lastFrameIndex = 0xFFFFFFFFu;
    u32 m_applyIndex     = 0;
};

} // namespace dx12e
