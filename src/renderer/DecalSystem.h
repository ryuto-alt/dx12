#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;

// クラスタードフォワードデカール（計画06 D1〜D5）。
//
// 方式: デカールを計画02 のクラスタグリッド（16x9x24）へビニングし、フォワード PS の
// 「マテリアル確定直後・ライティング開始前」で albedo/normal/roughness/metallic を書き換える。
// deferred decals と違って G-Buffer を要求しないので、前方レンダラのままで
// 「正しくライティングされる法線マップ付きデカール」が成立する。
//   出典: turanszkij (Wicked Engine), "Forward+ decal rendering"
//
// ★ルートシグネチャは 1 DWORD も増えない。kSlotClusterSRV（slot 11）のテーブルには
//   計画02 の時点でレンジ2（t18..t21）が確保済みで、そこへ SRV を差し込むだけ。
//   ディスクリプタブロックの所有者は ClusteredLightCulling のまま（+3..+6 の 4 本を借りる）。
class DecalSystem
{
public:
    // ★shaders/forward/DecalCommon.hlsli の DECAL_MAX_SCENE / DECAL_MAX_PER_CLUSTER と一致させること。
    static constexpr u32 kMaxDecals     = 256;
    static constexpr u32 kMaxPerCluster = 16;
    // フレーム多重化数（FrameResources::kFrameCount と一致させること）。
    static constexpr u32 kFrameCount    = 3;
    // ClusteredLightCulling の SRV ブロック内でデカールが借りる先頭オフセット（t18 の位置）。
    static constexpr u32 kSrvOffset     = 3;

    // 176B。shaders/forward/DecalCommon.hlsli の struct Decal と完全一致。
    struct DecalGPU
    {
        DirectX::XMFLOAT4X4 invWorld;       //   0  world → デカールローカル（単位立方体）
        DirectX::XMFLOAT4   atlasUV;        //  64  カラー矩形 (u0,v0,du,dv)
        DirectX::XMFLOAT4   atlasUVNormal;  //  80  法線矩形。.z <= 0 で法線マップ無し
        DirectX::XMFLOAT3   tint;      float opacity;         //  96
        DirectX::XMFLOAT3   axisW;     float normalStrength;  // 112  投影軸（ローカル +Y のワールド方向）
        DirectX::XMFLOAT3   tangentW;  float roughness;       // 128  ローカル +X のワールド方向 / <0=変更しない
        DirectX::XMFLOAT3   emissive;  float metallic;        // 144  <0=変更しない
        float cosAngleFade; float fadeEdge; float _pad0; float _pad1;  // 160
    };
    static_assert(sizeof(DecalGPU) == 176, "Decal must be 176 bytes (DecalCommon.hlsli)");

    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();

    // シェーダーホットリロード用。PSO のみ作り直す。
    void RecreatePipelines(GraphicsDevice& device);

    // CPU が集めたデカール列（sortOrder 昇順にソート済み）を UPLOAD リングへ書く。
    // 戻り値は実際に書けた個数（kMaxDecals で頭打ち）。
    u32 Upload(const DecalGPU* decals, u32 count, u32 frameIndex);

    // クラスタへのビニング。呼び出し後、インデックス/カウントは PS/CS から読める状態。
    // view はジッタなしのビュー行列（非転置）。z パラメータはクラスタライティングと同じもの。
    void Cull(ID3D12GraphicsCommandList* cmd, DirectX::FXMMATRIX view,
              float proj11, float proj22,
              float zNear, float zFarCluster, float zFarCamera,
              u32 numDecals, u32 frameIndex);

    // Cull をスキップしたフレームでも、テーブルをバインドする前に呼ぶ（UAV → 読取状態）。
    void EnsureReadable(ID3D12GraphicsCommandList* cmd);

    // ClusteredLightCulling の SRV ブロック（7 本）の +3..+6 へ t18..t21 を書き込む。
    // atlasSrvCpu はデカールアトラスの SRV（未設定なら 1x1 ダミーを渡すこと。
    // 未初期化のディスクリプタがあるとテーブルを Set した時点でデバッグレイヤが落とす）。
    void WriteSrvsInto(GraphicsDevice& device, DescriptorHeap& heap,
                       u32 blockStart, u32 frameIndex,
                       D3D12_CPU_DESCRIPTOR_HANDLE atlasSrvCpu);

    bool IsReady() const { return m_pso != nullptr && m_indexBuf != nullptr; }

private:
    void EnsureUavState(ID3D12GraphicsCommandList* cmd);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuf;   // DEFAULT / UAV + SRV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_countBuf;   // DEFAULT / UAV + SRV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_decalBuf[kFrameCount];   // UPLOAD リング
    u8* m_decalMapped[kFrameCount] = {};

    D3D12_RESOURCE_STATES m_indexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_countState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    std::wstring m_shaderDir;
};

} // namespace dx12e
