#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

#include "core/Types.h"
#include "renderer/ClusterMath.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;

// クラスタードライティング（Forward+）のライトカリング。
//
// compute 2 パス:
//   ① ClusterBuild_CS  … 16x9x24=3456 クラスタの AABB を view 空間で構築（Z は指数分割）
//   ② ClusterCull_CS   … 球 vs AABB（スポットは円錐追加）でライトをクラスタへ割り当て
//
// 結果は固定ストライドのインデックスリスト + クラスタ毎カウント。フォワード PS は
// slot kSlotClusterSRV のディスクリプタテーブル（t13,t14,t15）から読む。
//
// ★このテーブルは AllocateBlock(7) で確保してある。t13,t14,t15 に続く 4 本は
//   デカール（計画06）が t18,t19,t20,t21 として相乗りするための予約枠。
//   後からテーブルを広げるとルートシグネチャの再作成が全 PSO に波及するので最初から 7 で取る。
//   予約枠は「必ず有効なディスクリプタ」で埋めておくこと（DATA_VOLATILE でも
//   ディスクリプタ自体は静的扱いで、テーブルを Set した時点で初期化済みが要求される）。
class ClusteredLightCulling
{
public:
    // グリッド定数は src/renderer/ClusterMath.h（= shaders/forward/ClusterCommon.hlsli）に集約。
    static constexpr u32 kClusterCount        = cluster::kClusterCount;
    static constexpr u32 kMaxLightsPerCluster = cluster::kMaxLightsPerCluster;
    static constexpr u32 kMaxSceneLights      = cluster::kMaxSceneLights;

    // ディスクリプタテーブルの本数（t13,t14,t15 + デカール予約 4 本）。
    static constexpr u32 kSrvTableSize = 7;
    // フレーム多重化数（FrameResources::kFrameCount と一致させること）。
    static constexpr u32 kFrameCount = 3;

    // shaders/forward/ClusterCommon.hlsli の ClusterLight と完全一致（64B）。
    struct LightGPU
    {
        DirectX::XMFLOAT3 position;    float range;
        DirectX::XMFLOAT3 color;       float type;        // 0=point / 1=spot
        DirectX::XMFLOAT3 direction;   float cosOuter;
        float             cosInner;    float sinOuter;
        float             shadowIndex; float _pad;
    };
    static_assert(sizeof(LightGPU) == 64, "ClusterLight must be 64 bytes (ClusterCommon.hlsli)");

    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();

    // シェーダーホットリロード用。PSO のみ作り直す。
    void RecreatePipelines(GraphicsDevice& device);

    // CPU が集めたライト列を UPLOAD リングへ書く。戻り値は実際に書けた灯数（kMaxSceneLights で頭打ち）。
    u32 UploadLights(const LightGPU* lights, u32 count, u32 frameIndex);

    // AABB 構築 + カリングを 1 回で。呼び出し後、index/count は PIXEL_SHADER_RESOURCE 状態。
    // view はジッタなしのビュー行列（非転置）。proj11/proj22 は投影行列の _11/_22。
    void Dispatch(ID3D12GraphicsCommandList* cmd, DirectX::FXMMATRIX view,
                  float proj11, float proj22,
                  float zNear, float zFarCluster, float zFarCamera,
                  u32 numLights, u32 frameIndex);

    // Dispatch をスキップしたフレームでも、テーブルをバインドする前に呼ぶ
    // （UAV → PIXEL_SHADER_RESOURCE 遷移。既に読取状態なら no-op）。
    void EnsureReadable(ID3D12GraphicsCommandList* cmd);

    // フォワードパスの kSlotClusterSRV へ渡す GPU ハンドル（テーブル先頭 = t13）。
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvTable(u32 frameIndex) const;
    // 上と同じものの SRV index 版（ダミーバインドしたい呼び出し側用）。
    u32 GetSrvTableIndex(u32 frameIndex) const;

    bool IsReady() const
    {
        if (!m_psoCull || !m_psoBuild) return false;
        for (u32 f = 0; f < kFrameCount; ++f)
            if (m_srvBlock[f] == 0xFFFFFFFFu) return false;
        return true;
    }

private:
    void EnsureUavState(ID3D12GraphicsCommandList* cmd);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBuild;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoCull;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_aabbBuf;    // DEFAULT / UAV のみ（PS からは読まない）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuf;   // DEFAULT / UAV + SRV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_countBuf;   // DEFAULT / UAV + SRV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightBuf[kFrameCount];   // UPLOAD リング
    u8*  m_lightMapped[kFrameCount] = {};

    DescriptorHeap* m_srvHeap = nullptr;
    u32  m_srvBlock[kFrameCount] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};

    D3D12_RESOURCE_STATES m_indexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_countState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    std::wstring m_shaderDir;
};

} // namespace dx12e
