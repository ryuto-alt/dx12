#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <memory>
#include <string>

#include "core/Types.h"
#include "renderer/VolumetricFogSettings.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class ConstantBuffer;

// froxel（frustum-aligned voxel）ボリュメトリックフォグ。
//
// パス構成:
//   ① FogInject_CS    … 媒質（σ_s / σ_t）を 3D テクスチャへ注入
//   ② FogScatter_CS   … 太陽 + CSM（+ クラスタライト）の in-scattering と時間再投影
//   ③ FogIntegrate_CS … Z 方向の解析積分（Hillaire / Frostbite SIGGRAPH 2015）
//   ④ FogComposite    … フルスクリーン三角形 1 枚をブレンドでシーン RT へ合成
//
// ★このクラスは RTV も DSV も 1 個も消費しない。合成は
//   dst.rgb = src.rgb + dst.rgb * src.a（SrcBlend=ONE / DestBlend=SRC_ALPHA）で
//   既存のシーン RT へ直接書くため。メインのルートシグネチャにも
//   PerFrameConstants(b1) にも一切触れない ＝ カスタムシェーダにも自動でフォグがかかる。
//
// 諸元は shaders/fog/FogCommon.hlsli と一致させること（kFroxelX/Y/Z ↔ FROXEL_X/Y/Z）。
class VolumetricFogPass
{
public:
    // ★shaders/fog/FogCommon.hlsli の FROXEL_X/Y/Z と一致させること。
    static constexpr u32 kFroxelX = 160;
    static constexpr u32 kFroxelY = 90;
    static constexpr u32 kFroxelZ = 64;

    // BuildVolumes に渡すビュー依存パラメータ。Application::Render がその場で組む。
    struct ViewParams
    {
        DirectX::XMMATRIX view{};          // ジッタなしのビュー行列（非転置）
        DirectX::XMMATRIX prevViewProj{};  // 前フレームの viewProj（ジッタなし・非転置）
        float proj11 = 1.0f;               // 投影行列の _11
        float proj22 = 1.0f;               // 投影行列の _22
        DirectX::XMFLOAT3 cameraPos{};
        DirectX::XMFLOAT3 sunDir{};        // 光が進む方向（PerFrame の lightDir と同義）
        DirectX::XMFLOAT3 sunColor{};      // intensity 乗算済み
        // CSM。cascadeViewProj は Application::m_cascadeViewProj と同じ「非転置」で渡すこと。
        const DirectX::XMFLOAT4X4* cascadeViewProj = nullptr;  // 4 本
        DirectX::XMFLOAT4 cascadeSplits{};
        DirectX::XMFLOAT4 shadowParams{};
        // クラスタードライティング（Step F5）。clusterGrid.w == 0 なら点光源散乱は走らない。
        DirectX::XMFLOAT4 clusterParams{};
        DirectX::XMFLOAT4 clusterGrid{};
        u32   numLights        = 0;      // シーン総灯数（統計用。ループ上限はクラスタのカウント）
        u32   maxPerCluster    = 128;    // cluster::kMaxLightsPerCluster
        // スポット影。★Application の FrameConstants::spotShadowMatrix と同じ「転置済み」を渡すこと
        //   （未使用スロットは単位行列で埋まっている）。cascadeViewProj だけが非転置なので注意。
        const DirectX::XMFLOAT4X4* spotShadowMatrixTransposed = nullptr;  // 4 本
        float spotShadowTexel  = 0.0f;   // 1/kSpotShadowMapSize
        float pointShadowNear  = 0.1f;
        // ビューポート矩形。★どのフォグシェーダも読んでいない（FogCommon.hlsli:45 の gRect は
        //   宣言だけ）。CB を 1 本で済ませるために枠だけ残っている状態で、
        //   以前ここには「合成時に使う」と書いてあったが嘘だった。
        u32 vpLeft = 0, vpTop = 0, vpW = 1, vpH = 1;
        float nearZ = 0.1f, farZ = 1000.0f;
        // ディスクリプタ（すべて m_srvHeap 上・すでにバインド済みのヒープを前提）
        D3D12_GPU_DESCRIPTOR_HANDLE csmSrv{};            // t0  : Texture2DArray（CSM）
        D3D12_GPU_DESCRIPTOR_HANDLE clusterSrv{};        // t3-5: クラスタライト/インデックス/カウント
        D3D12_GPU_DESCRIPTOR_HANDLE punctualShadowSrv{}; // t6,7: スポット影配列 / ポイント影キューブ配列
    };

    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();

    // シェーダーホットリロード用。PSO のみ作り直す（ルートシグネチャ/ボリューム/CB は不変）。
    void RecreatePipelines(GraphicsDevice& device);

    // カメラのテレポート/シーン切替時に呼ぶ。次の 2 フレームは時間再投影を使わない。
    void InvalidateHistory() { m_warmup = 2; }

    // ①〜③（compute）。呼び出し後、積分済みボリュームは PIXEL_SHADER_RESOURCE 状態。
    // ★compute は PSO を graphics と共有するので、呼び出し側は直後に
    //   グラフィクスのルートシグネチャ / PSO / ディスクリプタヒープを再設定すること。
    void BuildVolumes(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                      const VolumetricFogSettings& s, const ViewParams& v, u32 frameIndex);

    // ④（graphics）。シーン RT がバインドされ、深度が PIXEL_SHADER_RESOURCE の状態で呼ぶ。
    // DSV は不要（ブレンドで書くだけ）。呼び出し側は直後に RootSig/PSO/トポロジを再設定すること。
    void Composite(ID3D12GraphicsCommandList* cmd, D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                   u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, u32 frameIndex);

    bool IsReady() const { return m_psoInject && m_psoScatter && m_psoIntegrate && m_psoComposite; }

    // 決定論キャプチャ（#31）用。時間ジッタの位相を 0 へ戻す。毎フレーム呼べばジッタが固定され、
    // 時間再投影の履歴が不動点へ収束する＝同じ設定で撮れば同じ絵になる。
    void ResetTemporalPhase() { m_frameCounter = 0; }
    // 3D テクスチャが確保済みか（＝一度でも enabled で BuildVolumes が走ったか）。
    // 「今フレーム BuildVolumes を呼んだか」は呼び出し側がローカルのフラグで持つこと。
    bool VolumesAllocated() const { return m_integrated != nullptr; }

private:
    // enabled になった最初のフレームで 3D テクスチャ 4 枚（28MB）を確保する。
    bool EnsureVolumes(GraphicsDevice& device);
    void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRS;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_compositeRS;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoInject;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoScatter;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoIntegrate;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoComposite;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_media;       // .rgb=σ_s .a=σ_t
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scatter[2];  // .rgb=in-scattered radiance .a=σ_t（ピンポン）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_integrated;  // .rgb=累積 in-scattering .a=transmittance

    D3D12_RESOURCE_STATES m_mediaState      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_scatterState[2] = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    D3D12_RESOURCE_STATES m_integratedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // SRV ブロック（8 本連続）。レンジ (t1,t2) を組み替えるためにペアを複製してある:
    //   [0]=media [1]=scatter0 / [2]=media [3]=scatter1 / [4]=scatter0 [5]=scatter1 / [6]=integrated [7]=integrated
    u32 m_srvBlock = 0xFFFFFFFFu;
    // UAV ブロック（4 本連続）: [0]=media [1]=scatter0 [2]=scatter1 [3]=integrated
    u32 m_uavBlock = 0xFFFFFFFFu;

    std::unique_ptr<ConstantBuffer> m_fogCB;
    DescriptorHeap* m_srvHeap = nullptr;
    std::wstring    m_shaderDir;

    u32  m_cur     = 0;   // scatter のピンポン
    u32  m_warmup  = 2;   // >0 の間は履歴を使わない（両スロットが埋まるまで）
    u64  m_frameCounter = 0;
};

} // namespace dx12e
