#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

// GPU タイムスタンプ計測（パフォーマンス診断用）。
// フレームを固定スコープ（影/プリパス/メイン/…）で挟んでタイムスタンプを打ち、
// フレーム末に readback バッファへ Resolve、kFrames 周回後（フェンス完了保証済み）に読む。
// 結果は「約 kFrames フレーム前」の値だが、診断用途にはそれで十分。
// ponytail: スコープは固定 enum。動的な階層プロファイラは要るまで作らない。
class GpuTimer
{
public:
    enum Scope : u32
    {
        Total = 0,     // フレーム全体（cmdList 記録範囲）
        Shadows,       // スポット影 + ポイント影 + CSM
        DepthPrepass,  // 深度（+速度/G-Buffer）プリパスの描画そのものだけ（計画10 A2）
        PrepassSSAO,   // 上の DepthPrepass を含む「プリパス一式」（SSAO / コンタクトシャドウ / SSR/SSGI 生成込み）
        ClusterCull,   // クラスタ AABB 構築 + ライトカリング（compute 2 パス）
        Raytracing,    // DXR: BLAS の遅延構築 + TLAS の毎フレーム再構築（加速構造だけ）
        RtScreen,      // DXR: RT サン影 + RT-AO + RT デバッグ（スクリーン空間パス）
        RtReflection,  // DXR: RT 反射（トレース + 時間/空間フィルタ + SSR との合成。SSR の後段）
        Ddgi,          // DDGI: プローブのレイトレ + 八面体アトラスへのブレンド（compute 2 パス）
        ScreenSpaceGI, // SSR + SSGI（トレース/時間蓄積/アップサンプル + 前フレームカラー退避）
        VolumetricFog, // froxel フォグ（注入/散乱/Z積分の compute 3 パス + 合成）
        HiZ,           // Hi-Z 深度ピラミッド構築（compute。ミップ数ぶんのディスパッチ連鎖）
        MainScene,     // 不透明メッシュ（RenderSceneMeshes メインビュー）
        Particles,     // CPU/GPU パーティクル + 歪み
        PostFX,        // ブルーム/DoF/自動露出/uber 等ポスト一式
        UI,            // エディタアイコン/スプライト/ImGui/トランジション
        VgCull,        // 仮想ジオメトリ: GPU カリング（インスタンス / BVH 走査 / クラスタ / 二相 HZB。P2 は統計のみ）
        Clouds,        // ボリュメトリッククラウド: 粗いレイマーチ + 時間再投影(compute 2 パス)+ 合成（Particles の内数になる区間に重なる）
        CloudShadow,   // 雲の影: 影マップ(compute)+ 影マスクへの適用(全画面 PS。プリパス区間 PrepassSSAO の内数)
        RtShadowDenoise, // RT 影デノイザ: 時間蓄積 + 空間フィルタ（RtScreen の内数。トレースは含まない）
        Upscale,       // 時間方向アップスケーラ（DLSS の MV 準備 + 評価。PostFX の内数）
        VgShadow,      // 仮想ジオメトリ: 太陽の CSM へ VG のクラスタを深度だけで描く（Shadows の内数）
        // ---- W05-26: 新しい 30 本（番号は固定。対応するパスが無い間は常に 0 ms）----
        ShadowCsm,     // 影: 太陽の CSM（Shadows の内数）
        ShadowLocal,   // 影: スポット / ポイント（Shadows の内数）
        ScreenShadow,  // スクリーン空間の影（コンタクトシャドウ等。PrepassSSAO の内数）
        RtShadow,      // RT 太陽影（RtScreen の内数）
        RtLocalShadow, // RT ローカルライト影（RtScreen の内数）
        SsLighting,    // スクリーン空間ライティング（PrepassSSAO の内数）
        Skinning,      // スキニング（GPU スキン）
        Characters,    // キャラクター専用パス（髪・肌など）
        Transparent,   // 半透明
        Water,         // 水
        Foliage,       // 植生
        Atmosphere,    // 大気の空パス（Sky-View LUT + 空。AP / 雲は Particles 区間の中なので含まない）
        GodRays,       // ゴッドレイ（PostFX の内数）
        Decals,        // デカール
        VfxSim,        // VFX: シミュレーション
        VfxDraw,       // VFX: 描画
        VfxLighting,   // VFX: ライティング
        VfxShadow,     // VFX: 影
        Reactive,      // アップスケーラ用リアクティブマスク（PostFX の内数）
        Taa,           // TAA（PostFX の内数）
        Dof,           // 被写界深度（PostFX の内数）
        MotionBlur,    // モーションブラー（PostFX の内数）
        AutoExposure,  // 自動露出（PostFX の内数）
        LocalExposure, // ローカル露出（PostFX の内数）
        Bloom,         // ブルーム（PostFX の内数）
        LensFx,        // レンズ効果（PostFX の内数）
        ColorLut,      // カラー LUT（PostFX の内数）
        Uber,          // uber ポスト（PostFX の内数）
        EditorOverlay, // エディタのオーバーレイ（UI の内数）
        GameHud,       // ゲーム HUD（UI の内数）
        Count
    };
    static constexpr u32 kMaxScopes = 64;   // 64 ビットの書込みマスク（1ull << s）に収まる上限
    static_assert(Count <= kMaxScopes, "GpuTimer のスコープは 64 本まで（書込みマスクが u64）");

    // スコープの属性。parent は「この区間の中に入る」親（トップレベルは Total。Total 自身は Total）。
    // 子を持たないスコープが葉。alias は別名（大文字小文字を無視して FindScope で引ける）。
    struct ScopeInfo { const char* name; Scope parent; const char* alias; };
    static const ScopeInfo& Info(Scope s);
    static bool IsLeaf(Scope s);
    // 名前または別名からスコープを引く（大文字小文字を無視。旧名 volFog / 別名 fogScatter など）。
    static bool FindScope(const char* nameOrAlias, Scope& out);

    // ---- 葉の合計（予算の合計はここから出す）----
    // 各スコープの「葉としての寄与」: 構造上の葉は自分の時間、親は自分の作業時間 = 親 − Σ子（下限 0。
    // どの子スコープにも入っていない作業が親に残るため、それを "<親>.self" の合成の葉として数える）。
    // excludeMask に立っているスコープ（同一フレームに複数ビューで書かれたもの）とその子孫は数えず、
    // その時間は親の self に残す（2 つ目以降のビューで区間が上書きされるため入れ子が信用できない）。
    // この規則なら、子が未計装でも過小計上にならず、重なり（二重計上）だけが Total を超える。
    static u64   ExpandExcludeMask(u64 excludeMask);   // 子孫まで広げる
    static float LeafMs(Scope s, const float* ms, u64 expandedExclude = 0);
    static float LeafSumMs(const float* ms, u64 excludeMask = 0);   // Total の self を含む。excludeMask は広げる前の値
    struct LeafEntry { Scope scope; bool self; float ms; };          // self=true は "<name>.self"
    // 出力すべき葉の一覧（除外されたスコープは出さない）。out は kMaxScopes 件以上の配列。戻り値は件数。
    static u32   LeafEntries(const float* ms, u64 excludeMask, LeafEntry* out);

    // 1 フレームぶんのタイムスタンプの検査（GPU を使わない純関数。NewFrame が呼び、テストも直接呼ぶ）。
    // beginTs/endTs は各スコープの Begin/End のタイムスタンプ（Count 本）、writtenMask は End まで打ったスコープ、
    // multiMask は同一フレームに 2 回以上 Begin されたスコープ。freq は tick/秒。msOut に各スコープの ms を書く。
    struct FrameCheck
    {
        u32   nestViolations = 0;     // 子の区間が親の区間の外に出た数（複数書込みのスコープは除外）
        u64   nestViolatedMask = 0;   // 逸脱した子のビット
        float leafSumMs = 0.0f;
        float totalMs = 0.0f;
        bool  leafSumExceeded = false;   // 葉の合計 > Total + kLeafSumToleranceMs
    };
    static FrameCheck CheckFrame(const u64* beginTs, const u64* endTs, u64 writtenMask, u64 multiMask,
                                 double freq, float* msOut);
    static const char* Name(u32 s);

    static constexpr u32 kFrames = 3;   // FrameResources::kFrameCount と一致させる

    // queue はタイムスタンプ周波数の取得用。失敗時は m_valid=false のまま（全 API が no-op）。
    void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);

    // フレーム先頭（フェンス待ち後）に呼ぶ。同スロットの前回結果を読み戻し、書込みマスクをリセット。
    void NewFrame(u32 frameIndex);
    void Begin(ID3D12GraphicsCommandList* cmd, Scope s);
    void End(ID3D12GraphicsCommandList* cmd, Scope s);
    // フレーム末（cmdList Close 前）に呼ぶ。今フレームで Begin/End したスコープだけ Resolve する。
    void Resolve(ID3D12GraphicsCommandList* cmd);

    bool  IsValid() const { return m_valid; }
    // 直近の完了フレームの計測値(ms)。未計測スコープは 0。
    float GetMs(Scope s) const { return (s < Count) ? m_ms[s] : 0.0f; }

    // 実行時の検査の結果（NewFrame が読み戻しのたびに調べる）。
    //  - 子の区間が親の区間（Begin/End のタイムスタンプ）の外に出た回数
    //  - 葉の合計が Total + 0.1ms を超えた回数
    u32 NestViolations()    const { return m_nestViolations; }
    u32 LeafSumViolations() const { return m_leafViolations; }
    // 直近に読み戻したフレームで、同一フレームに複数回 Begin されたスコープ（複数ビュー）。入れ子と葉から外す。
    u64 MultiWriteMask()    const { return m_multiLast; }

    // ---- 全スコープぶんの履歴 / ベンチ集計（Application.h の PerfFrame は 24 本固定で凍結のため、ここで持つ）----
    static constexpr u32 kHistory = 240;   // Application::kPerfHistory と同じ
    void RecordHistory();                                    // 現在の m_ms を履歴に積む（フレームに 1 回）
    u32  HistoryCount() const { return m_histCount < kHistory ? m_histCount : kHistory; }
    // 直近 window フレームの平均を out[Count] へ。window は HistoryCount に丸める。戻り値は実際に使った数。
    u32  AverageHistory(u32 window, float* out) const;
    void BenchReset();
    void BenchAdd();                                         // 現在の m_ms を集計へ足す
    u32  BenchAverage(float* out) const;                     // 集計の平均を out[Count] へ。戻り値は集計フレーム数
    static constexpr float kLeafSumToleranceMs = 0.1f;

private:
    u32 QueryIndex(u32 frame, u32 scope, u32 beginEnd) const
    {
        return (frame * Count + scope) * 2 + beginEnd;
    }

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_readback;   // u64 × kFrames×Count×2
    u64   m_freq = 0;
    bool  m_valid = false;
    u32   m_frame = 0;                  // NewFrame で受けた現在スロット
    u64   m_writtenMask[kFrames] = {};  // スロットごとの「このフレームで End まで打った」スコープビット（1ull << s）
    u64   m_beganMask[kFrames] = {};    // スロットごとの「Begin を打った」ビット（2 回目の Begin を検出する）
    u64   m_multiMask[kFrames] = {};    // スロットごとの「2 回以上 Begin された」ビット
    u64   m_multiLast = 0;
    float m_ms[Count] = {};
    u32   m_nestViolations = 0;
    u32   m_leafViolations = 0;
    u64   m_nestWarned = 0;             // 親子の逸脱を警告したスコープ（1 スコープ 1 回だけ）
    float m_hist[kHistory][Count] = {};
    u32   m_histCount = 0;              // 積んだ延べ数（リング位置 = % kHistory）
    double m_benchSum[Count] = {};
    u32   m_benchN = 0;
};

} // namespace dx12e
