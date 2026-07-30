#pragma once
//
// DdgiVolume — DDGI（Dynamic Diffuse Global Illumination）のプローブボリューム
//              計画09 Step 6。論文: Majercik et al., JCGT Vol.8 No.2 (2019)
//
// ★NVIDIA の RTXGI SDK は使わない（v2.0 で DDGI 削除・v1.x は休眠・独占ライセンス）。
//   SDK が提供するのは probe blending / relocation / classification だけで、
//   レイトレはもともとアプリ側の責任。このエンジンは inline RayQuery と
//   バインドレスのヒット読み取りが既にあるので、論文から自前実装するほうが速い。
//
// ★既定 OFF。OFF のとき絵は導入前と完全に一致する（このリポジトリの流儀）。
//
// 段階（途中で止めても価値が残るように刻む）:
//   Step 0 … プローブ格子 + レイトレ + 八面体アトラス                      （完了）
//   Step 1 … ライティングパスで拡散間接項として置き換え + シーン JSON へ保存（完了）
//   Step 1.5 … プローブが点光源 / スポットを拾う（屋内で効かせるのに必須） （完了）
//   Step 2 … 距離モーメント + Chebyshev 可視性テスト + ボーダーコピー       ← いまここ
//   Step 3 … 多重バウンス（プローブレイのヒット点で前フレームのプローブを引く）
//
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <memory>
#include <string>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class RenderTarget;

// シーン JSON に保存する設定。
struct DdgiSettings
{
    bool  enabled = false;          // ★既定 OFF
    // プローブ格子。屋内 1 部屋なら 8x4x8 = 256 個で十分。
    int   probeCountX = 8, probeCountY = 4, probeCountZ = 8;
    float spacing   = 2.0f;         // プローブ間隔(m)
    float originX = -8.0f, originY = 0.5f, originZ = -8.0f;   // 格子の最小コーナー
    float rayLength  = 30.0f;       // プローブレイの最大距離(m)
    float hysteresis = 0.97f;       // 履歴の保持率。大きいほど滑らかで応答が遅い
    float intensity  = 1.0f;
    float normalBias = 0.02f;       // 影レイ始点の法線オフセット(m)
    // 多重バウンスの強さ（段階3）。0 = 1 バウンスのみ＝段階2 までと同じ絵。
    // プローブレイのヒット点で【前フレームのプローブ】を引いて足す量。
    // ★1.0 を超えさせないこと。収束値は E/(1-ρ·b) の幾何級数なので、
    //   アルベド ρ とこの b の積が 1 に近づくと発散する（アルベド側も 0.9 で潰してある）。
    float bounceIntensity = 0.0f;
};

class DdgiVolume
{
public:
    // 1 プローブあたりのレイ本数。シェーダの DDGI_RAYS_PER_PROBE と一致させること。
    static constexpr u32 kRaysPerProbe   = 64;
    // 八面体タイルの内側テクセル数と、ボーダー込みのタイルサイズ。
    static constexpr u32 kIrradianceTexels = 6;
    static constexpr u32 kProbeTile        = kIrradianceTexels + 2;
    // 距離モーメント（Chebyshev 可視性）のタイル。★irradiance より高解像度にする。
    //   シェーダの DDGI_DISTANCE_TEXELS / DDGI_DISTANCE_TILE と一致させること。
    static constexpr u32 kDistanceTexels   = 14;
    static constexpr u32 kDistanceTile     = kDistanceTexels + 2;
    static constexpr u32 kMaxProbes        = 4096;

    bool Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();
    void RecreatePipelines(GraphicsDevice& device);   // シェーダーホットリロード用

    bool IsReady() const
    {
        return m_psoTrace != nullptr && m_psoBlend != nullptr && m_psoBlendDist != nullptr;
    }

    struct UpdateDesc
    {
        D3D12_GPU_VIRTUAL_ADDRESS tlas         = 0;
        D3D12_GPU_VIRTUAL_ADDRESS geometryInfo = 0;
        DirectX::XMFLOAT3 sunDir{0, -1, 0};      // 進行方向（PerFrame の lightDir と同じ）
        DirectX::XMFLOAT3 sunColor{1, 1, 1};
        float             sunIntensity = 1.0f;
        DirectX::XMFLOAT3 skyColor{0, 0, 0};     // envMap が無いときのミス放射輝度（フォールバック）
        // IBL の irradiance キューブの bindless index。0xFFFFFFFF で skyColor を使う。
        // ★フォワードの拡散 IBL と同じテクスチャを引かせることで、空が見えている面では
        //   DDGI の ON/OFF で値が変わらなくなる（遮蔽のある所のバウンスだけが差分になる）。
        u32               skyCubeSrvIndex = 0xFFFFFFFFu;
        // クラスタライト配列(t13 = StructuredBuffer<ClusterLight>)の bindless index と灯数。
        // ★t14/t15(クラスタのインデックス/カウント)は使わない。あれは画面空間のクラスタで、
        //   視錐台の外にあるプローブには対応するクラスタが無いため。灯数ぶん総当たりする。
        u32               lightSrvIndex = 0xFFFFFFFFu;
        u32               lightCount    = 0;
        u32               frameIndex = 0;
    };

    // プローブを 1 フレームぶん更新する。TLAS / GeometryInfo が無ければ何もしない。
    // ★テクスチャは「実際に ON にしたフレーム」で初めて確保する（OFF なら VRAM 消費ゼロ）。
    void Update(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                const DdgiSettings& s, const UpdateDesc& d);

    // irradiance アトラスの SRV index（可視化とライティングパスが読む）。
    u32 GetIrradianceSrvIndex() const;
    u32 GetDistanceSrvIndex() const;
    // 任意のディスクリプタ位置へ irradiance アトラスの SRV を作る（段階1）。
    // フォワードの t22 は slot11 テーブルの中なので、専用 index ではなくここへ書く必要がある。
    // アトラスが未確保なら false（呼び出し側が黒ダミーで埋める）。
    bool WriteIrradianceSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    // 距離モーメントアトラス（t23）。フォーマットが違うだけで扱いは irradiance と同じ。
    bool WriteDistanceSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    // 履歴を捨てる（シーン切替 / 設定変更）。次の更新で hysteresis 無しで埋め直す。
    void InvalidateHistory() { m_historyValid = false; }

    struct Stats
    {
        u32 probes   = 0;
        u32 raysCast = 0;
        u64 bytes    = 0;
    };
    const Stats& GetStats() const { return m_stats; }

private:
    bool EnsureResources(GraphicsDevice& device, const DdgiSettings& s);
    void CreateRootSignature(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTrace;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBlend;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBlendDist;

    // レイの結果（x = レイ番号 / y = プローブ番号）。rgb = 放射輝度 / a = ヒット距離。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_rayData;
    // 八面体 irradiance アトラス。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_irradiance;
    // 八面体 距離モーメントアトラス（.r=平均距離 / .g=二乗平均）。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distance;
    u32 m_rayDataUav    = 0xFFFFFFFFu;
    u32 m_irradianceUav = 0xFFFFFFFFu;   // = m_rayDataUav + 1
    u32 m_distanceUav   = 0xFFFFFFFFu;   // = m_rayDataUav + 2
    u32 m_irradianceSrv = 0xFFFFFFFFu;
    u32 m_distanceSrv   = 0xFFFFFFFFu;

    DescriptorHeap* m_srvHeap = nullptr;
    std::wstring    m_shaderDir;

    // 現在のリソースが対応している格子（変わったら作り直す）。
    u32  m_probesX = 0, m_probesY = 0, m_probesZ = 0;
    bool m_historyValid = false;
    // irradiance アトラスの現在のステート。段階1 でフォワード PS が読むようになったので
    // compute(UAV) と PS(SRV) を行き来する。遷移の面倒はこのクラスの中で完結させる。
    D3D12_RESOURCE_STATES m_irradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_distanceState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Stats m_stats;
};

} // namespace dx12e
