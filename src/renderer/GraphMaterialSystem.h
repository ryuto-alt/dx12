#pragma once
// ============================================================================
// GraphMaterialSystem — マテリアルグラフ（G2b / G2c）のランタイム
//
//   .dxmat の "graph" キーを持つ材質（グラフ材質）を、フォワード描画へ載せる。設計: docs/MATERIAL_GRAPH_DESIGN.md §3 / §7 G2b・G2c、
//   仕様と規則: docs/MATGRAPH_G2B.md・docs/MATGRAPH_G2C.md。
//
//   ┌ MaterialAssetManager::Entry（.dxmat。graphPath / graphParams を持つ）
//   │     └ Resolve() が Instance を作る（.dxmat 1 個 = Instance 1 個）
//   │           ├ .dxmg を読んで CompileGraph（CPU・数 ms）→ CompileResult（HLSL・スロット表）
//   │           ├ パラメータプールに【レコード】（float4 × slotCount）を確保 → 値とテクスチャの SRV 添字を書く
//   │           └ 同じ HLSL ハッシュの ShaderVariant を共有（ワーカーが DXC + PSO を作る。ディスクキャッシュあり）
//   └ 描画側（ApplicationRender.cpp の drawEntity）: DrawParams（PSO・recordBase・プール SRV 添字）を b2 へ詰めて描く
//
//   ★非同期・旧版維持: HLSL が変わる編集は新しい Build を pending に置き、DXC + PSO が終わるまで旧 Build（active）で描き続ける。
//     失敗してもエラーだけ保持して描画は止めない（初回で失敗したら従来の 4 枚テクスチャの代理材質 = 単色プロキシで描く）。
//   ★値の編集（定数 / パラメータ / テクスチャの差し替え / .dxmat の params）は再コンパイルしない。レコードを書き直すだけ。
//   ★プールは CPU の影（vector<float>）を正とし、フレームごとの 3 本の UPLOAD バッファへ変更があったときだけ複写する。
//   ★グラフ材質は影・深度プリパス・速度・パストレ・DXR では従来の 4 枚組の代理（MaterialAssetManager::Entry の代理ブロック =
//     白 / 既定法線 / 既定 MR / 黒）で描かれる = 単色プロキシ。半透明・マスクの別エントリは G3b（未実装）。VG 対象外（設計書 §7.4）。
//   ★スレッド: 描画スレッド（メイン）専用。ワーカー（DXC + PSO）とはキューだけで渡す。
//
//   ★G2c の追加:
//     ・ワーカーは複数本（論理コアの 1/8、2〜4 本）・BELOW_NORMAL 優先度。ジョブは優先度つきキュー（ユーザーが待っている高速版 / プレビュー /
//       サムネイルが先・背景の最適化版は後。最適化版の同時実行数は workers-1 まで）。
//     ・段階コンパイル（SetStagedCompile）: Forward 種別の構造変更を「-Od の高速版」と「最適化版」の 2 ジョブで出す。高速版が先に出来れば
//       それで描き始め、最適化版が出来たら差し替える（PSO は墓場経由）。順序は段階で決める: 遅れて届いた古い / 低い段階は捨てる（StageGate）。
//       ディスクキャッシュに最適化版があれば高速版を省く。
//     ・種別（Kind）: Forward = シーン / PreviewLite = プレビュー球（UNO_SHADE_LITE: 影・クラスタ・GI をコンパイル時に外した同じライティング）/
//       NodeLdr / NodeRaw = ノード内サムネイル（部分グラフ出力。ライティングなし・RGBA8 / RGBA32F）。
//     ・古い編集の間引き: 未完了の pending が置き換わるとき、他に誰も使っていない未着手のジョブは取り消す。
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"
#include "renderer/matgraph/Compiler.h"
#include "renderer/matgraph/ParamPool.h"
#include "renderer/matgraph/PreviewLogic.h"
#include "renderer/matgraph/ShaderCache.h"
#include "resource/MaterialAssetManager.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;
class ResourceManager;
class RootSignature;
class ShaderRuntimeCompiler;

class GraphMaterialSystem
{
public:
    static constexpr u32 kFrames       = 3;
    static constexpr u32 kPoolCapacity = 32768;   // float4（= 512 KB × 3 フレーム）。1 材質は slotCount 個（典型 10〜40）

    // パイプラインの種別（インスタンス / 変種の種類）
    enum class Kind : u8
    {
        Forward     = 0,   // シーンのフォワード PS（完全なライティング。RGBA16F + D32・less / lequal）
        PreviewLite = 1,   // エディタのプレビュー用（UNO_SHADE_LITE。RGBA16F + D32・less だけ）
        NodeLdr     = 2,   // ノード内サムネイル（部分グラフ出力。RGBA8 へガンマ表示）
        NodeRaw     = 3,   // 同上（数値テスト用。RGBA32F へ値そのまま）
    };

    struct InitDesc
    {
        GraphicsDevice*  device = nullptr;
        RootSignature*   rootSignature = nullptr;   // メイン RS（IsBindless() = true が前提）
        ResourceManager* resources = nullptr;
        DescriptorHeap*  srvHeap = nullptr;
        DXGI_FORMAT      colorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
        DXGI_FORMAT      depthFormat = DXGI_FORMAT_D32_FLOAT;
        bool             allowCompile = true;       // エディタ = true。ゲームモード（DXC 無し）= false（キャッシュ / pak だけ）
        bool             pollFiles = true;          // .dxmg の更新時刻を見てホットリロード（エディタのみ）
        std::string      engineVersion;             // ディスクキャッシュのキー
        std::string      cacheDir;                  // 空 = %LOCALAPPDATA%\UnoEngine\shadercache\<版>（UNO_SHADERCACHE_DIR で差し替え可）
        bool             stagedCompile = false;     // G2c: Forward 種別の構造変更を -Od の高速版 → 最適化版の 2 段にする（エディタ = true）
        int              workerCount = 0;           // 0 = 自動（論理コアの 1/8 を 2〜4 に丸める。環境変数 UNO_MATGRAPH_WORKERS で上書き）
    };

    GraphMaterialSystem();
    ~GraphMaterialSystem();
    GraphMaterialSystem(const GraphMaterialSystem&) = delete;
    GraphMaterialSystem& operator=(const GraphMaterialSystem&) = delete;

    // false = バインドレス非対応 / プール確保失敗 など（以後 Resolve は常に false = 従来の代理材質で描く）
    bool Initialize(const InitDesc& d);
    void Shutdown();
    bool IsAvailable() const { return m_available; }

    // 段階コンパイルの切り替え（Initialize の stagedCompile を後から変える。既に投入済みのジョブには効かない）
    void SetStagedCompile(bool on) { m_staged = on; }
    bool StagedCompile() const { return m_staged; }
    int  WorkerCount() const { return static_cast<int>(m_workers.size()); }

    // ---- 描画側 ---------------------------------------------------------------------------
    struct DrawParams
    {
        ID3D12PipelineState* pso = nullptr;
        u32 recordBase = 0;      // b2 DWORD0
        u32 poolSrvIndex = 0;    // b2 DWORD1（このフレームのプールの SRV 添字）
    };
    // グラフ材質の描画に必要なものを返す。false = まだ使えない / 失敗 → 従来の代理材質（単色プロキシ）で描くこと。
    // lequal: 深度プリパス併用時の LESS_EQUAL 版。cmd はテクスチャ読み込み用。
    bool Resolve(const std::string& dxmatRel, const MaterialAssetManager::Entry& entry,
                 ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out);

    // ---- エディタのプレビュー（G2c）----
    // Entry（.dxmat のキャッシュ）を介さず、材質のデータを直接渡す版。エディタのプレビュー（保存前の材質・保存前のグラフ）が使う。
    //   instanceKey = 任意の一意な名前（実在しなくてよい。例 "__preview__/<guid>"）。dataSerial = data を変えるたびに +1 して渡す
    //   （Entry::loadSerial と同じ役割。変わると次の呼び出しでレコードだけ書き直す）。data.graphPath がグラフの場所（実在しなくてよい: SetInstanceGraphText で本文を渡す）。
    bool ResolveData(const std::string& instanceKey, const MaterialAssetData& data, u32 dataSerial,
                     ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out);
    // 保存前のグラフ本文（正準形の .dxmg テキスト）をインスタンスへ押し込む。以後そのインスタンスは .dxmg のファイルを読まず、この本文でビルドする
    // （ファイルのホットリロードは効かない）。空文字で解除してファイルへ戻す。次のフレームで作り直され、HLSL が同じなら値の更新だけで済む。
    void SetInstanceGraphText(const std::string& instanceKey, const std::string& dxmgText);

    // ★G2c: インスタンスの種別（最初の SetInstanceCompiled / Resolve より前に。既定は Forward）
    void SetInstanceKind(const std::string& instanceKey, Kind kind);
    // ★G2c: エディタが CPU で作った CompileResult をそのまま渡す（.dxmg の読み込みも CompileGraph も省く。部分グラフ出力 = ノード内サムネイルもこれ）。
    //   HLSL のハッシュが同じなら値（レコード）の更新だけ・違えば新しい Build を pending に置く。null で解除。
    void SetInstanceCompiled(const std::string& instanceKey, std::shared_ptr<const matgraph::CompileResult> cr);
    // ★G2c: 注入したインスタンスの描画（材質データは持たない）。false = まだ使えない / 失敗 / 未登録
    bool ResolveInstance(const std::string& instanceKey, ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out);
    // ★G2c: インスタンスを捨てる（プレビューの一時キー / サムネイルの後始末）
    void RemoveInstance(const std::string& instanceKey);

    // 描画リストのソート用: この .dxmat が使う HLSL ハッシュ（ロードしない。未解決 / グラフ材質でなければ 0）
    u64 PeekShaderHash(const std::string& dxmatRel) const;

    // 1 フレームに 1 回（Render の頭）。ワーカーの結果の取り込み・pending → active の切り替え・ホットリロード・GC
    void Update(f32 dt, u32 frameIndex, ID3D12GraphicsCommandList* cmd);
    // 変更があればこのフレームのプールの UPLOAD バッファへ複写する（Resolve / Update が内部で呼ぶ。描画パスの頭でも呼んでよい）
    void SyncPool(u32 frameIndex);

    // ---- 状態（エディタ / MCP が読む）----------------------------------------------------
    struct InstanceStatus
    {
        std::string dxmat;              // 正規化した .dxmat のパス
        std::string graph;              // .dxmg のパス
        Kind        kind = Kind::Forward;
        bool        hasActive = false;  // 描画に使える Build がある
        bool        compiling = false;  // pending の DXC / PSO 待ち（この間は旧版で描いている）
        bool        failed = false;     // 直近のビルドが失敗（旧版があればそのまま描いている）
        int         level = 0;          // 描いている版の段階: 0 = 無し / 1 = -Od の高速版 / 2 = 最適化版
        bool        optimizing = false; // 高速版で描いていて、最適化版を裏で作っている最中
        u64         activeHash = 0;     // 描いている HLSL のハッシュ
        u64         pendingHash = 0;
        u32         slotCount = 0;
        u32         textureCount = 0;
        u32         recordBase = 0;
        u32         generation = 0;     // 構造の変更（新しい HLSL ハッシュ）を受け付けた回数
        u32         valueUpdates = 0;   // 再コンパイル無しでレコードを書き直した回数
        std::string errorLog;           // DXC / PSO の全文（失敗したときだけ）
        std::vector<matgraph::Diagnostic> diagnostics;   // グラフの診断 + DXC エラー（nodeId 逆引き済み）
        double      codegenMs = 0, dxcMs = 0, psoMs = 0;
        bool        cacheHit = false;   // 直近のビルドが DXC を呼ばずにディスクキャッシュだけで済んだ
        // ★G2c の実測点（steady_clock）。Build 生成（構造変更を受け付けた）から:
        double      firstUsableMs = 0;  // 描ける版（高速版 or 最適化版）が出来るまで（直近の Promote）
        double      finalMs = 0;        // 最適化版が出来るまで（最適化版が無いなら firstUsableMs と同じ）
        double      fastDxcMs = 0, fastPsoMs = 0, optDxcMs = 0, optPsoMs = 0;   // 段階ごとの所要
        // ワーカーの進み具合（UI の「生成中…」表示用）: 0 = 待機（キュー）/ 1 = DXC / 2 = PSO / 3 = 完了。
        //   pendingPhase = いま作っている新しい版（compiling のとき）/ optPhase = 高速版で描きながら作っている最適化版（optimizing のとき）
        int         pendingPhase = 0;
        int         optPhase = 0;
    };
    std::vector<InstanceStatus> ListInstances() const;
    bool GetInstanceStatus(const std::string& dxmatRel, InstanceStatus& out) const;

    struct Counters
    {
        u32 instances = 0, variants = 0, poolUsed = 0, poolCapacity = 0;
        u32 codegen = 0;             // CompileGraph の回数（グラフ → HLSL）
        u32 dxcCompiles = 0;         // DXC を実際に呼んだ回数（キャッシュヒットは数えない）
        u32 fastCompiles = 0;        // うち -Od の高速版
        u32 optCompiles = 0;         // うち最適化版（-Od なし）
        u32 cacheHits = 0, cacheMisses = 0, cacheStores = 0;
        u32 psoCreates = 0;
        u32 valueUpdates = 0;        // 再コンパイル無しの値更新
        u32 jobsQueued = 0, jobsDone = 0, jobsFailed = 0;
        u32 jobsCancelled = 0;       // 古い編集の間引きで取り消した未着手のジョブ
        u32 staleIgnored = 0;        // 遅れて届いた古い / 低い段階の結果（捨てた）
        u32 fallbackDraws = 0;       // Resolve が false を返した回数（代理材質で描いた）
        u32 workers = 0;
        std::string cacheDir;
    };
    Counters GetCounters() const;

    // エンジン側の事情による警告（未対応の blendMode / shadingModel・グラフに無い params 名）。MCP の validate も同じ文面を返す
    static std::vector<matgraph::Diagnostic> CollectEngineDiagnostics(const matgraph::CompileResult& cr, const MaterialAssetData* data);
    // .dxmg を書き換えたことを知らせる（更新時刻の 0.5 秒ポーリングを待たずに次のフレームで作り直す）
    void NotifyGraphFileChanged(const std::string& graphRel);

    // .dxmg / .dxmat の再読み込みを求める（MCP compile force / ホットリロード）。force = キャッシュも DXC の結果も捨てて作り直す
    bool Rebuild(const std::string& dxmatRel, bool force);
    // すべてのグラフ材質を作り直す（エンジンの HLSL ヘッダを編集した後など）
    void RebuildAll(bool force);

    // ---- 配布ビルド ----
    // assets 内の全グラフ材質（.dxmat の graph キー）の HLSL を DXC で DXIL にして返す（PSO は作らない。ディスクキャッシュを使う）。
    //   relPath は pak のキー（"shaders/graph/<HLSL ハッシュ>_PS.cso" と共通の "shaders/graph/vs.cso"）。1 つでも失敗したら false + errors
    //   （BuildGame はビルドを止める = 壊れたグラフを出荷しない）。ゲームモードの GraphMaterialSystem（allowCompile=false）は同じ名前で pak から読んで PSO を作る。
    struct BakedBlob { std::string relPath; std::vector<u8> bytes; };
    bool BakeForBuild(std::vector<BakedBlob>& out, std::vector<std::string>& errors);

    // 完了待ち（テスト / MCP wait:true 用）。timeoutMs 以内に pending が 0 になれば true。メインスレッドから呼ぶと Update を回す
    bool WaitIdle(int timeoutMs, ID3D12GraphicsCommandList* cmd);
    bool HasPending() const;

    // テスト / 診断用: 描画に使っている（active）レコードの中身（float4 × slotCount の並び）を返す
    bool DebugReadRecord(const std::string& dxmatRel, std::vector<float>& out) const;

    // デバッグ / テスト用: ワーカーを一時停止して「コンパイル中の旧版維持」を再現する（ms 待ってからジョブを処理する）
    void DebugSetWorkerDelayMs(int ms) { m_workerDelayMs = ms; }
    // デバッグ / テスト用: 指定した段階（1 = 高速版 / 2 = 最適化版）の結果の公開を ms 遅らせる（段階の結果が順不同に届く状況を作る）
    void DebugSetStageDelayMs(int stage, int ms) { if (stage == 1) m_stage1DelayMs = ms; else if (stage == 2) m_stage2DelayMs = ms; }

private:
    struct PoolRecord { u32 base = 0; u32 count = 0; bool valid = false; };
    using Clock = std::chrono::steady_clock;

    struct Variant
    {
        enum class State : u8 { Compiling, Ready, Failed };
        u64   hash = 0;
        Kind  kind = Kind::Forward;
        State state = State::Compiling;
        matgraph::StageGate gate;                    // 受理した最高の段階（0 / 1 / 2）。遅れて届いた古い結果を捨てる
        bool  optQueued = false;                     // 最適化版のジョブを投入した（level 1 で描きながら待っている）
        std::shared_ptr<std::atomic<int>> phaseFast = std::make_shared<std::atomic<int>>(0);   // ワーカーの進み具合（0 待機 / 1 DXC / 2 PSO / 3 完了）
        std::shared_ptr<std::atomic<int>> phaseOpt = std::make_shared<std::atomic<int>>(0);
        Microsoft::WRL::ComPtr<ID3D12PipelineState> less, lequal;
        std::string errorLog;
        std::vector<matgraph::Diagnostic> mapped;    // DXC エラーの nodeId 逆引き
        double dxcMs = 0, psoMs = 0;                 // 直近に受理した段階の所要
        double fastDxcMs = 0, fastPsoMs = 0, optDxcMs = 0, optPsoMs = 0;
        double firstUsableMs = 0, finalMs = 0;       // createdAt から
        Clock::time_point createdAt = Clock::now();
        bool   cacheHit = false;
        u64    lastUsedFrame = 0;
    };

    struct Build
    {
        std::shared_ptr<const matgraph::CompileResult> cr;
        PoolRecord rec;
        std::shared_ptr<Variant> variant;
        Clock::time_point createdAt = Clock::now();
    };

    struct Instance
    {
        std::string key;                    // 正規化 .dxmat
        std::string graphRel;
        std::string memText;                // 空でなければ .dxmg のファイルではなくこの本文でビルドする（SetInstanceGraphText）
        std::shared_ptr<const matgraph::CompileResult> injected;   // 空でなければ CompileGraph も省いてこれを使う（SetInstanceCompiled）
        Kind kind = Kind::Forward;
        MaterialAssetData data;             // 直近の Resolve で見た .dxmat の中身
        u32  dxmatSerial = 0xFFFFFFFFu;
        bool graphDirty = true;             // .dxmg を読み直す
        bool paramsDirty = false;           // レコードだけ書き直す
        bool built = false;
        std::filesystem::file_time_type graphMtime{};
        std::unique_ptr<Build> active, pending;
        // 状態
        std::vector<matgraph::Diagnostic> diags;
        std::string errorLog;
        bool   failed = false;
        u32    generation = 0, valueUpdates = 0;
        double codegenMs = 0;
        double firstUsableMs = 0, finalMs = 0;   // 直近の Promote（描ける版が出来るまで）/ 最適化版が出来るまで
        u64    lastUsedFrame = 0;
        u64    activeHash = 0;              // PeekShaderHash 用（active の hash。無ければ pending / 0）
    };

    struct Job
    {
        u64  hash = 0;
        Kind kind = Kind::Forward;
        std::string hlsl;
        bool force = false;
        int  stage = 2;                     // 1 = -Od の高速版 / 2 = 最適化版（キャッシュ優先）
        std::shared_ptr<std::atomic<int>> phase;   // 進み具合の書き込み先（Variant::phaseFast / phaseOpt）
    };
    struct JobResult
    {
        u64  hash = 0;
        Kind kind = Kind::Forward;
        int  level = 2;                     // この結果の段階（高速版のジョブでも、最適化版がキャッシュにあれば 2 になる）
        bool ok = false;
        std::string errorLog;
        std::vector<matgraph::Diagnostic> mapped;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> less, lequal;
        double dxcMs = 0, psoMs = 0;
        bool cacheHit = false;
    };

    static u64 VKey(u64 hash, Kind kind) { return hash ^ (static_cast<u64>(kind) * 0x9E3779B97F4A7C15ull); }

    // ---- メインスレッド --------------------------------------------------------------------
    void RebuildInstance(Instance& inst, ID3D12GraphicsCommandList* cmd);
    bool LoadAndCompile(Instance& inst, std::shared_ptr<const matgraph::CompileResult>& outCr);
    void RepackBuild(Instance& inst, Build& b, ID3D12GraphicsCommandList* cmd);
    void FreeRecord(PoolRecord& r);
    bool AllocRecord(u32 count, PoolRecord& out);
    void Promote(Instance& inst);
    void ReleaseBuild(std::unique_ptr<Build>& b);         // レコードを墓場へ・他に使う人のいない未完了の変種は取り消す
    void CancelVariant(const std::shared_ptr<Variant>& v);
    void DrainResults();
    void CollectGarbage();
    std::shared_ptr<Variant> GetOrCreateVariant(const matgraph::CompileResult& cr, Kind kind, bool force);
    void EnqueueJob(Job j);
    static std::string NormalizeKey(const std::string& rel);
    InstanceStatus MakeStatus(const Instance& inst) const;
    Instance& InstanceFor(const std::string& key);
    bool ResolveCommon(Instance& inst, ID3D12GraphicsCommandList* cmd, u32 frameIndex, bool lequal, DrawParams& out);

    // ---- ワーカー ---------------------------------------------------------------------------
    void WorkerMain(int index);
    void ProcessJob(const Job& job, ShaderRuntimeCompiler& compiler, const std::string& dxcVersion, JobResult& out);
    bool CompileBlobs(const Job& job, ShaderRuntimeCompiler& compiler, const std::string& dxcVersion, JobResult& out,
                      std::vector<u8>& vs, std::vector<u8>& ps);
    bool BuildPsos(Kind kind, const std::vector<u8>& vs, const std::vector<u8>& ps, JobResult& out);
    static int JobPriority(const Job& j) { return (j.kind != Kind::Forward || j.stage == 1) ? 0 : 1; }

    // ---- 状態 -------------------------------------------------------------------------------
    InitDesc m_desc;
    bool m_available = false;
    bool m_staged = false;
    u64  m_frame = 0;
    f32  m_pollTimer = 0.0f;

    // プール（CPU の影 + フレームごとの UPLOAD バッファ）
    std::vector<float> m_shadow;                                    // kPoolCapacity * 4
    matgraph::ParamPoolAllocator m_alloc;
    u64 m_poolVersion = 1;
    u64 m_syncedVersion[kFrames] = {0, 0, 0};
    u32 m_syncedHigh[kFrames] = {0, 0, 0};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_poolBuf[kFrames];
    u8*  m_poolMapped[kFrames] = {nullptr, nullptr, nullptr};
    u32  m_poolSrv[kFrames] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};

    struct GraveRecord { PoolRecord rec; u64 frame; };
    std::vector<GraveRecord> m_graveRecords;
    struct GraveVariant { std::shared_ptr<Variant> v; u64 frame; };
    std::vector<GraveVariant> m_graveVariants;
    struct GravePso { Microsoft::WRL::ComPtr<ID3D12PipelineState> a, b; u64 frame; };   // 段階の差し替えで置き換わった PSO（GPU が使い終わるまで）
    std::vector<GravePso> m_gravePsos;

    std::map<std::string, std::unique_ptr<Instance>> m_instances;
    std::unordered_map<u64, std::shared_ptr<Variant>> m_variants;   // VKey(hash, kind)
    std::unordered_map<std::string, u64> m_peek;                    // dxmat key → active hash（const な PeekShaderHash 用）
    std::shared_ptr<matgraph::ShaderDiskCache> m_cache;
    mutable Counters m_counters;

    // ワーカー
    std::vector<std::thread> m_workers;
    int m_maxOptJobs = 1;                                           // 最適化版の同時実行数の上限（workers-1・最低 1）
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    matgraph::PriorityJobQueue<Job> m_jobs;
    std::deque<JobResult> m_results;
    bool m_stop = false;
    int  m_runningOpt = 0;                                          // m_mutex で守る: 実行中の最適化版ジョブ
    std::atomic<int> m_workerDelayMs{0};
    std::atomic<int> m_stage1DelayMs{0}, m_stage2DelayMs{0};
    std::atomic<u32> m_inFlight{0};
    std::atomic<u32> m_dxcCompiles{0}, m_fastCompiles{0}, m_optCompiles{0}, m_psoCreates{0};
    bool m_forceNext = false;                                       // 次の構造ビルドはキャッシュを使わず DXC からやり直す
};

} // namespace dx12e
