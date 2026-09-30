#pragma once
// ===========================================================================
// PathTracerHost — DXR パストレーサー(リファレンスレンダー)の Application 側の糊。
// ---------------------------------------------------------------------------
// 本体は renderer/PathTracer(専用ルートシグネチャ・専用 TLAS)。ここは
//   ・ECS からのシーンのスナップショット(描画リスト → pt::SceneBuilder)
//   ・ジョブの状態機械(要求 → 準備 → 実行 → 仕上げ → 完了 / 失敗 / 中止)
//   ・フレームに埋め込んだ時間予算つきのディスパッチ(GPU 時間の上限 = frameBudgetMs)
//   ・出力(PFM / EXR / PNG / メタ JSON)
// を担当する。★既定 OFF: 要求が来るまで何も確保しない(遅延確保)。通常の描画経路には 1 ピクセルも触れない。
// 実装は ApplicationPathTracer.cpp、MCP は mcp/ApplicationMcpPathTracer.cpp、UI は editor/panels/PathTracerPanel。
// ===========================================================================
#include <directx/d3d12.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "renderer/PathTracer.h"
#include "renderer/pt/PtSceneBuilder.h"
#include "renderer/pt/PtUiState.h"

namespace dx12e
{

class Application;
class DescriptorHeap;

struct PtRequest
{
    uint32_t width = 1920, height = 1080;
    uint32_t spp = 256;
    uint32_t bounces = 8;
    uint32_t seed = 1;
    float    maxRadiance = 0.0f;          // 0 = クランプ無し
    float    frameBudgetMs = 12.0f;       // 1 フレームあたりの GPU 時間の上限
    double   maxSeconds = 0.0;            // 0 = 無制限。超えたらそこまでの結果を保存して終わる
    float    exposure = 1.0f;             // プレビュー PNG の露出
    std::string outputBase;               // 拡張子なしの基準パス。空 = 既定
    bool     writePfm = true, writeExr = false, writePng = true;
    // カメラ
    bool     useSceneCamera = true;
    float    camPos[3] = {0, 0, 0}, camTarget[3] = {0, 0, 1};
    float    fovDeg = 45.0f;
    float    lensRadius = 0.0f, focusDist = 10.0f;
    // 光輸送の切り替え
    bool     physicalFalloff = false;     // 点 / スポットを逆二乗にする(既定 = エンジンの saturate(1-d/r)^2)
    float    sunAngularRadiusDeg = -1.0f; // <0 = PCSS の設定に従う(OFF ならデルタ光)
    bool     russianRoulette = true;
    bool     forceLambert = false;        // 全材質を純ランバートにする(検証用)
    bool     noNormalMaps = false;
    bool     quantizeLikeForward = true;  // 材質値をフォワードと同じ 8bit 量子化にする(色 / 不透明度 / カットオフ / 発光)
    bool     background = true;           // カメラから見える空を出す(skybox の設定に従う)。false = 黒
    uint32_t tileSize = 256;
    uint32_t samplesPerDispatch = 1;
    std::string note;                     // メタ JSON にそのまま入れる自由メモ
};

struct PtSnapshotInfo
{
    uint32_t drawItems = 0;               // 走査した描画項目
    uint32_t instances = 0;               // TLAS に入ったインスタンス
    uint32_t skinnedInstances = 0;
    uint32_t skippedNoMesh = 0;           // メッシュ / VB / IB が無く飛ばした
    uint32_t skippedSkinned = 0;          // スキンドで変形後頂点を作れず飛ばした
    uint32_t vgProxies = 0;               // 仮想ジオメトリのプロキシ(低ポリ)でトレースしたインスタンス
    uint32_t customShader = 0;            // カスタムシェーダ材質(標準の PBR で近似)
    uint32_t terrainSplat = 0;            // スプラット地形(標準の PBR で近似)
    uint32_t textured = 0;                // アルベドテクスチャを持つインスタンス
    uint32_t materials = 0;
    uint32_t lights = 0;
    double   snapshotMs = 0.0;
};

struct PtHost
{
    enum class Phase { Idle, Requested, Preparing, Running, Finalizing, Done, Failed, Cancelled };

    std::unique_ptr<pt::PathTracer>   tracer;
    std::unique_ptr<pt::SceneBuilder> scene;
    bool        initTried = false;
    std::string initError;

    Phase       phase = Phase::Idle;
    uint32_t    jobId = 0;
    PtRequest   req;
    pt::JobDesc job;
    std::string error;
    std::string message;
    PtSnapshotInfo snap;
    pt::BuilderStats sceneStats;

    // 時刻 / 進捗
    std::chrono::steady_clock::time_point tRequest, tRunStart, tEnd;
    double      prepareMs = 0.0;
    double      runSeconds = 0.0;
    uint32_t    sampleTarget = 0;
    bool        cancelRequested = false;
    bool        saveOnCancel = false;
    bool        truncated = false;        // maxSeconds / キャンセルで目標 spp に届かず保存した
    int         sceneGeneration = 0;
    uint32_t    framesSinceBuild = 0;     // FinishBuild を積んでからのフレーム数(ステージング解放の待ち)
    bool        transientPending = false;
    std::vector<uint32_t> ownedSrvs;      // スキンドの写しに払い出した raw SRV(ジョブの終わりにヒープへ返す)
    DescriptorHeap*       ownedSrvHeap = nullptr;
    uint32_t    terminalFrames = 0;       // 終了後、専用シーンを返すまでのフレーム数
    double      lastMsPerUnit = 0.0;      // 直近ジョブの 1 ユニットあたり GPU ms(バッファ解放後も status に出す)
    uint32_t    settleFrames = 0;         // 準備前に待つフレーム数(アップロード完了待ち)
    // 結果
    std::vector<std::string> files;
    std::string outputBase;
    uint32_t    samplesDone = 0;
    pt::PtStatsOut stats;
    double      gpuMsTotal = 0.0;
    double      msPerSpp1080p = 0.0;
    std::string previewPath;              // 途中経過のプレビュー PNG(status の preview:true で更新)
    bool        previewRequested = false;
    std::string previewSizeNote;

    // ---- 公開 API(Application / MCP / UI から)----
    // 要求を受け付ける(実際の開始は次のフレーム)。失敗理由を err へ。
    bool Request(Application& app, const PtRequest& r, std::string* err);
    void Cancel(bool save);
    bool Busy() const { return phase == Phase::Requested || phase == Phase::Preparing || phase == Phase::Running || phase == Phase::Finalizing; }
    nlohmann::json Status(Application& app);
    // 実行中の累積を読み戻して <outputBase>.preview.png へ書く(GPU の完了待ちで数十 ms 止まる)。previewPath を更新。
    bool WritePreview(uint32_t* samples, std::string* err);
    // フレームごとに(PrepareFrame の TLAS 構築より後)呼ぶ。cmd はフレームのコマンドリスト。
    void Tick(Application& app, ID3D12GraphicsCommandList* cmd);
    // UI(EditorContext::ptUi)との同期。要求 / キャンセルを読み、進捗を書き戻す。
    void SyncUi(Application& app, pt::UiState& ui);
    // メッシュの解放 / シーンの切り替えなど、スナップショットが指すリソースが無効になる出来事。
    void Invalidate(const char* reason);
    void Shutdown();

private:
    bool EnsureTracer(Application& app);
    bool Snapshot(Application& app, ID3D12GraphicsCommandList* cmd, std::string* err);
    void Finalize(Application& app);
    void FailJob(const std::string& why);
    void ReleaseOwnedSrvs();
    std::string ResolveOutputBase(Application& app) const;
    static const char* PhaseName(Phase p);
};

} // namespace dx12e
