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
#include <vector>

#include "core/Types.h"
#include "renderer/DdgiSchedule.h"

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
    // ---- GI S4（GI モード New だけが読む。Legacy は無視）----
    // true = カメラ追従のスクロール格子 + 2 カスケード。probeCountX/Y/Z = 1 カスケードの格子数、spacing = カスケード 0（近景）の間隔、
    //   originX/Y/Z は無視（窓はカメラに合わせて間隔単位でスナップして動く）。false = 従来の固定ボリューム（origin から spacing 刻み）。
    //   JSON のキーが無い = false（旧シーン互換）。新規シーンの既定は true（GiMigration）。
    bool  followCamera = false;
    float spacing1   = 2.0f;    // カスケード 1（遠景）の間隔(m)。followCamera のときだけ使う
    float budgetMs   = 1.0f;    // 1 フレームの DDGI の GPU 予算(ms)。実測から更新するプローブ数を決める（超えそうなら間引く）
};

// シーン単位の GI モード。Scene が保持し、保存 / 読込 / Undo を通る。
//   Legacy: 既存シーンの挙動。DDGI もフォワードも 1 ビットも変わらない（A5）。
//   New   : プローブ分類・再配置・空の可視率・ヒット面の多重バウンス（空の項の置き換え）と、
//           フォワードの環境光の DDGI 置換・鏡面遮蔽・SSGI のミス → DDGI。
enum class GiMode : u8
{
    Legacy = 0,
    New    = 1,
};

struct GiSettings
{
    GiMode mode = GiMode::Legacy;   // ★既定 Legacy（キーが無い = Legacy）
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
    // GiMode::New のバイアス類（プローブ間隔に対する比）。フォワードの b1 へも同じ式で渡す（Application）。
    static constexpr float kViewBiasRatio   = 0.3f;   // 補間の surface bias: 視線方向
    static constexpr float kNormalBiasRatio = 0.3f;   // 補間の surface bias: 法線方向
    static constexpr float kMinFrontRatio   = 0.15f;   // 再配置: 面からの最小距離
    static constexpr float kRelocLimit      = 0.45f;   // 再配置: オフセット上限（間隔比。RTXGI と同じ）

    bool Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, const std::wstring& shaderDir);
    void Shutdown();
    void RecreatePipelines(GraphicsDevice& device);   // シェーダーホットリロード用

    // GI モード New に要る分類 + 再配置の PSO が建っているか（無ければ Update は Legacy で動く）。
    bool SupportsGiNew() const { return m_psoProbeData != nullptr; }

    bool IsReady() const
    {
        // m_psoProbeData は GiMode::New だけが使う（無くても Legacy は動く）。
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
        u32               ringIndex  = 0;   // 飛行中のフレームの番号（0..2）。更新計画バッファのリング用
        // ---- GI モード New（GiMode::New）だけが読む。Legacy では全部無視される ----
        bool              giNew = false;
        // 空の【放射輝度】キューブ（パストレーサーと同じ envCube）の bindless index。ミスしたレイはこれを引く
        // （Legacy の skyCubeSrvIndex は irradiance キューブで、ミスの値としては二重にぼかしてある）。
        // 0xFFFFFFFF なら skyCubeSrvIndex → skyColor の順にフォールバック。
        u32               envCubeSrvIndex = 0xFFFFFFFFu;
        float             skyScale = 1.0f;   // ミスの放射輝度に掛ける（iblIntensity）。フォワード側は DDGI 項へ iblIntensity を掛けない
        // 検証用のビット（set_gi_mode の debugStage。保存しない）。0 = 全部（既定）/ bit0 = 旧の空の項 / bit1 = 再配置なし /
        // bit2 = 全プローブ有効（分類なし）。S2 の段階ごとの数値と切り分けのために残してある。
        u32               giStage = 0;
        // 物理ライティング単位（lightingUnits:1）。DDGI の内部は従来単位（太陽 ≈ 3）で持つ（half の溢れ防止）。
        // unitScale = 外部 → 内部の倍率（物理なら kClassicUnitScale、従来なら 1）。physicalUnits は点光源の減衰式の切替。
        float             unitScale = 1.0f;
        bool              physicalUnits = false;
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
    // プローブごとのデータ（xyz = 再配置オフセット(m) / w = 状態 0 無効・1 有効・2 有効になった直後）。
    // フォワードの t30。GiMode::New だけが読む。
    bool WriteProbeDataSrv(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    // いまのリソースが settings の格子に対応しているか。フォワードは「対応していない間は DDGI を読まない」
    // （リソースの作り直しはプローブ更新の中＝フォワードの b1 / ディスクリプタ書き込みより後ろだから）。
    bool MatchesGrid(const DdgiSettings& s) const;
    // 履歴を捨てる（シーン切替 / 設定変更）。次の更新で hysteresis 無しで埋め直す。
    void InvalidateHistory() { m_historyValid = false; m_requestResetAll = true; }

    // ---- GI S4: カメラ追従のスクロール格子 + 2 カスケード / 更新の間引き ----
    // フレームで 1 回、b1 を書く前に呼ぶ（主ビュー）。窓をカメラへ合わせ（はみ出した列だけ履歴を捨てる印を立てる）、
    // 更新リスト（リセット待ち + 予算ぶんのラウンドロビン）を作る。giNew=false なら何もしない（Legacy は従来の固定格子）。
    //   lastGpuMs: 直近に完了したフレームの DDGI の GPU 時間(ms)（GpuTimer。0 = 未計測）
    void PlanFrame(const DdgiSettings& s, bool giNew, const DirectX::XMFLOAT3& cam, float lastGpuMs);
    // フォワード / SSGI へ渡す格子の記述（PlanFrame の結果。GI モード New 用。固定ボリュームはカスケード 1・scroll 0）。
    struct SampleParams
    {
        DirectX::XMFLOAT4 origin0;    // xyz = カスケード 0 の窓の原点 / w = 1
        DirectX::XMFLOAT4 spacing0;   // xyz = カスケード 0 の間隔 / w = 法線バイアス（設定値）
        DirectX::XMFLOAT4 counts;     // xyz = 1 カスケードのプローブ数 / w = カスケード数
        DirectX::XMFLOAT4 c1;         // xyz = カスケード 1 の窓の原点 / w = カスケード 1 の間隔
        DirectX::XMFLOAT4 scroll0;    // xyz = カスケード 0 の記憶領域のずらし
        DirectX::XMFLOAT4 scroll1;
    };
    SampleParams GetSampleParams(const DdgiSettings& s) const;
    u32 CascadeCount() const { return m_planCascades; }

    struct Stats
    {
        u32 probes   = 0;          // 確保しているプローブ数（全カスケード）
        u32 raysCast = 0;          // このフレームに撃ったレイ数
        u64 bytes    = 0;
        // GI S4
        u32   cascades      = 1;
        u32   updatedProbes = 0;   // このフレームに更新したプローブ数（リセット待ち + ラウンドロビン）
        u32   resetProbes   = 0;   // うち履歴を捨てて埋め直したプローブ数（スクロール / 設定変更）
        u32   budgetProbes  = 0;   // 予算（プローブ数 / フレーム）
        float costUsPerProbe = 0;  // 1 プローブの GPU 時間の見積り(us)
        u32   lightGridCells = 0;
        float hysteresis0 = 0, hysteresis1 = 0;   // 実効ヒステリシス（間引きを考慮）
    };
    const Stats& GetStats() const { return m_stats; }

private:
    bool EnsureResources(GraphicsDevice& device, const DdgiSettings& s, u32 cascades);
    void BuildList(const DdgiSettings& s);
    // 古いリソース / ディスクリプタを GPU が使い終わるまで預かる（TDR 対策。作り直しの直前フレームがまだ走っている）。
    void RetireOldResources();
    void TickRetired();
    void CreateRootSignature(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoTrace;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBlend;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBlendDist;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoProbeData;   // GiMode::New: 分類 + 再配置
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoLightGrid;   // GiMode::New: ヒット点の点光源を絞るライトグリッド

    // レイの結果（x = レイ番号 / y = プローブ番号）。rgb = 放射輝度 / a = ヒット距離。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_rayData;
    // 八面体 irradiance アトラス。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_irradiance;
    // 八面体 距離モーメントアトラス（.r=平均距離 / .g=二乗平均）。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distance;
    u32 m_rayDataUav    = 0xFFFFFFFFu;   // UAV テーブルの先頭（u0..u4 の 5 連続）
    u32 m_irradianceUav = 0xFFFFFFFFu;   // = m_rayDataUav + 1
    u32 m_distanceUav   = 0xFFFFFFFFu;   // = m_rayDataUav + 2
    u32 m_irradianceSrv = 0xFFFFFFFFu;
    u32 m_distanceSrv   = 0xFFFFFFFFu;
    // プローブデータ（再配置オフセット + 状態）。u3 = m_rayDataUav + 3（UAV テーブルは 4 連続）。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_probeData;
    u32 m_probeDataUav  = 0xFFFFFFFFu;
    u32 m_probeDataSrv  = 0xFFFFFFFFu;
    D3D12_RESOURCE_STATES m_probeDataState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    GiMode m_lastGiMode = GiMode::Legacy;   // モードが変わったら履歴を捨てる（プローブの値の意味が変わる）

    // ライトグリッド（セルごとの影響灯リスト。UAV = u4）。u4 = m_rayDataUav + 4（UAV テーブルは 5 連続）。
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightGrid;
    u32 m_lightGridUav = 0xFFFFFFFFu;
    u32 m_lightGridCapCells = 0;
    // 更新計画バッファ（UPLOAD。ヘッダ + 更新リスト）。飛行中のフレーム数ぶんのリング。bindless SRV（cb の planSrvIndex）。
    static constexpr u32 kPlanRing        = 3;
    static constexpr u32 kPlanHeaderWords = 32;
    static constexpr u32 kMaxListEntries  = 2 * kMaxProbes;
    static constexpr u32 kLightGridMaxPerCell = 40;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_planBuf[kPlanRing];
    u8*  m_planMapped[kPlanRing] = {};
    u32  m_planSrv[kPlanRing] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    bool EnsurePlanBuffers(GraphicsDevice& device);

    // ---- 計画（PlanFrame の結果）----
    bool m_planGiNew = false;        // 今フレームは GI モード New（かつ PSO あり）
    bool m_planFollow = false;       // カメラ追従（2 カスケード）
    bool m_planReady = false;        // PlanFrame 済み・Update 未消化
    bool m_requestResetAll = false;  // InvalidateHistory が呼ばれた
    u32  m_planCascades = 1;
    int  m_counts[3] = {1, 1, 1};    // 1 カスケードの格子数
    ddgi::Window m_win[2];
    float m_spacing[2] = {1.0f, 1.0f};
    float m_origin[2][3] = {};
    std::vector<u8>  m_pending;      // プローブごとの「履歴を捨てて埋め直す」待ち（全カスケードぶん。通し番号で引く）
    u32  m_cursor[2] = {0, 0};       // ラウンドロビンの位置
    u32  m_budget = 0;               // 予算（プローブ数 / フレーム）
    float m_costPerProbe = 0.0f;     // ms
    u32  m_countHist[8] = {};        // 直近 8 フレームの更新プローブ数（GPU 計測の遅れ分を引くため）
    u32  m_planSeq = 0;
    std::vector<u32> m_list;         // 更新リスト（通し番号 | リセットビット）
    u32  m_listReset = 0;
    float m_hyst[2] = {0.97f, 0.97f};
    float m_lgOrigin[3] = {};
    float m_lgCell = 1.0f;
    int  m_lgDims[3] = {1, 1, 1};
    // 計画の鍵（変わったら全部リセット）
    struct PlanKey
    {
        int counts[3] = {0, 0, 0};
        u32 cascades = 0;
        float spacing0 = 0, spacing1 = 0;
        float origin[3] = {0, 0, 0};   // 固定ボリュームのときだけ
        bool follow = false;
        bool operator==(const PlanKey& o) const
        {
            return counts[0] == o.counts[0] && counts[1] == o.counts[1] && counts[2] == o.counts[2] && cascades == o.cascades
                && spacing0 == o.spacing0 && spacing1 == o.spacing1 && origin[0] == o.origin[0] && origin[1] == o.origin[1]
                && origin[2] == o.origin[2] && follow == o.follow;
        }
    } m_planKey;

    // 作り直しで退役させたディスクリプタ。GPU（最大 3 フレーム先行）が使い終わる数フレーム先まで保持してから返す
    // （リソース本体は DeferredRelease へ渡す）。
    struct Retired
    {
        u32 uavBlock = 0xFFFFFFFFu;     // FreeBlock(…, 5)
        u32 srv[3]   = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        u32 age      = 0;
    };
    std::vector<Retired> m_retired;

    DescriptorHeap* m_srvHeap = nullptr;
    std::wstring    m_shaderDir;

    // 現在のリソースが対応している格子（変わったら作り直す）。
    u32  m_probesX = 0, m_probesY = 0, m_probesZ = 0;
    u32  m_layoutCascades = 0;    // いまのリソースのカスケード数（アトラスの行数）
    bool m_justRecreated = false; // EnsureResources がこのフレームに作り直した
    bool m_historyValid = false;
    // irradiance アトラスの現在のステート。段階1 でフォワード PS が読むようになったので
    // compute(UAV) と PS(SRV) を行き来する。遷移の面倒はこのクラスの中で完結させる。
    D3D12_RESOURCE_STATES m_irradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_distanceState   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Stats m_stats;
};

} // namespace dx12e
