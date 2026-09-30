#pragma once
// ===========================================================================
// PathTracer — DXR パストレーサー(地上真値レンダラ)。パリティ基盤 Q1a。
// ---------------------------------------------------------------------------
// 目的: 「UE に匹敵するグラフィック」の合否判定の基準になる、エンジン内の真値レンダラ
//       (Lumen も同様にパストレーサーとの比較で品質を語る)。フォワード + DDGI/SSGI/SSR との差が
//       「アルゴリズム差」だけになるよう、BSDF はフォワードの PBR と同じ式を使う(shaders/pt/PtCommon.hlsli)。
//
// 設計の柱
//   ・既定 OFF。通常の描画経路は 1 ピクセルも変えない(ジョブが走っている間だけ動く。遅延確保)。
//   ・専用ルートシグネチャ・専用 TLAS(pt::SceneBuilder)。D3D12 デバイス + キューだけで動く
//     (窓なしのテストからも使える)。バインドレス(ResourceDescriptorHeap[])のヒープは呼び出し側が渡す。
//   ・進行型: 累積バッファ(RGBA32F の和 + サンプル数)。画素 × サンプル番号 × シードで乱数が決まる＝決定論。
//   ・分割ディスパッチ: 1 ディスパッチ = 1 タイル × samplesPerDispatch サンプル。Record() は GPU 時間の予算
//     (ms)いっぱいまでしか積まない＝TDR とエディタのフリーズを避ける。
//
// 使い方(フレームに埋め込む場合)
//   BeginJob(...)                           … 累積バッファを確保。シーンは pt::SceneBuilder が組んだもの
//   毎フレーム Record(cmd, heap, budgetMs)   … 予算内でディスパッチを積む。積んだリストは【必ず】実行してから次の Record
//   Finished() が true になったら ReadbackAccum() → 画像出力(pt/PtImageIO.h)
// ===========================================================================
#include <directx/d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "renderer/pt/PtSceneBuilder.h"
#include "renderer/pt/PtShared.h"

namespace dx12e::pt
{

struct CameraDesc
{
    float pos[3]   = {0, 0, 0};
    float right[3] = {1, 0, 0};
    float up[3]    = {0, 1, 0};
    float fwd[3]   = {0, 0, 1};
    float tanHalfFovY = 0.41421356f;   // tan(45°/2)
    float aspect = 16.0f / 9.0f;
    float lensRadius = 0.0f;           // >0 で薄レンズ(被写界深度)
    float focusDist  = 10.0f;
};

struct SunDesc
{
    bool  enabled = false;
    float toLight[3] = {0, 1, 0};      // 太陽へ向かう単位ベクトル
    float tanRadius = 0.0f;            // 角半径の tan(0 = デルタ光)
    float E[3] = {0, 0, 0};            // 放射照度(フォワードの lightColor = 色 × 強度)
};

struct EnvDesc
{
    uint32_t cubeSrv = kNoIndex;       // 環境キューブ(TextureCube)の SRV 添字。無ければ一様
    float    uniform[3] = {0, 0, 0};   // キューブ無し時の一様な空の放射輝度
    float    lightScale = 1.0f;        // ライティング用(iblIntensity)
    float    bgScale    = 0.0f;        // カメラ直視の背景(skyboxIntensity。背景を出さないなら 0)
    bool     nee = true;               // 環境の NEE(空が明るいなら true。真っ黒なら false で省く)
    uint32_t atmosSrvBase = kNoIndex;  // 物理大気(A1)の SRV ブロック先頭(AtmosphereRenderer::SrvBlockBase)。有効なら従来の cubeSrv / uniform は使わない
};

struct JobDesc
{
    uint32_t width = 1920, height = 1080;
    uint32_t spp = 64;                 // 目標サンプル数/画素
    uint32_t bounces = 8;              // 散乱頂点の上限(1 = 直接光のみ)
    uint32_t seed = 1;
    float    maxRadiance = 0.0f;       // 1 サンプルの放射輝度の上限(0 = クランプ無し。GT の既定)
    uint32_t flags = kFlagRR | kFlagEnvNee;
    uint32_t rrStart = 3;              // ロシアンルーレットを始める深さ
    float    pixelFilter = 0.0f;       // 0 = 箱フィルタ(画素内一様)
    uint32_t tileSize = 256;           // タイル辺(画素)
    uint32_t samplesPerDispatch = 1;   // 1 ディスパッチで回すサンプル数
    bool     countStats = true;
    CameraDesc cam;
    SunDesc    sun;
    EnvDesc    env;
};

struct PtProgress
{
    uint32_t samplesDone = 0;          // 画素あたりの完了サンプル数(全タイルが揃った分)
    uint32_t samplesTarget = 0;
    double   fraction = 0.0;           // 0..1
    double   gpuMsTotal = 0.0;         // PT のディスパッチにかかった GPU 時間の累計(測定できた分)
    double   msPerUnit = 0.0;          // 1 ユニット(タイル × samplesPerDispatch)の推定 ms
    uint64_t dispatches = 0;
    bool     running = false;
    bool     finished = false;
};

struct PtStatsOut
{
    uint32_t nanSamples = 0;           // NaN / Inf で捨てたサンプル
    uint64_t paths = 0;                // 追跡した経路数
    uint32_t clampedSamples = 0;       // maxRadiance でクランプされたサンプル
    uint32_t rrTerminated = 0;         // ロシアンルーレットで打ち切られた経路
};

class PathTracer
{
public:
    PathTracer() = default;
    ~PathTracer() { Shutdown(); }
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    bool Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, const std::wstring& shaderDir, std::string* err);
    void Shutdown();
    bool IsReady() const { return m_pso != nullptr; }

    // ジョブ開始。累積バッファ / 統計バッファを確保し直す(前のジョブの結果は捨てる)。
    bool BeginJob(const JobDesc& desc, const SceneGpu& scene, std::string* err);
    // 予算(ms)いっぱいまでディスパッチを積む。積んだ数(ユニット)を返す。
    //   budgetMs <= 0 なら maxUnits 個(0 = 1 パス分)を積む(テスト用)。
    //   ★積んだコマンドリストは、次に Record を呼ぶ前に必ずキューへ実行させること(タイムスタンプの回収がその前提)。
    uint32_t Record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap, float budgetMs, uint32_t maxUnits = 0);

    void Cancel();
    bool Running() const { return m_state == State::Running; }
    bool Finished() const { return m_state == State::Finished; }
    bool HasJob() const { return m_state != State::Idle; }
    PtProgress GetProgress() const;
    const JobDesc& GetJob() const { return m_job; }

    // 累積結果(和とサンプル数)を GPU から読む。★GPU の完了を待つ(CPU ブロック)。積んだリストの実行後に呼ぶこと。
    // out = width × height × 4 の float(rgb = 和、a = サンプル数)。
    bool ReadbackAccum(std::vector<float>& out, std::string* err);
    // 平均(rgb = 和 / サンプル数)を width × height × 3 の float で返す。
    bool ReadbackAverage(std::vector<float>& rgb, uint32_t* samples, std::string* err);
    bool ReadbackStats(PtStatsOut& out);

    // 実行中のキューを同期でフラッシュする(テスト用: 積んだ分を全部終わらせる)。
    void WaitForGpu();
    // ジョブの GPU バッファ(累積 / 統計 / 読み戻し。1080p で約 35MB)を返す。終わったジョブの後始末。
    // ★GPU が使い終えてから呼ぶこと(内部で WaitForGpu する)。以後 ReadbackAccum は使えない。
    void ReleaseJobBuffers();

private:
    enum class State { Idle, Running, Finished };

    bool CreateRootSignatureAndPso(const std::wstring& shaderDir, std::string* err);
    void HarvestTimestamps();

    ID3D12Device5* m_device = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAtmos;   // 物理大気(A1)の空を引く版(PathTraceAtmos_CS.cso)
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    uint64_t m_fenceValue = 0;
    void* m_fenceEvent = nullptr;

    // ジョブ
    State m_state = State::Idle;
    JobDesc m_job;
    SceneGpu m_scene;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_accum, m_stats, m_jobCb;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;
    uint64_t m_readbackSize = 0;
    uint32_t m_tilesX = 0, m_tilesY = 0, m_passes = 0, m_samplesPerDispatch = 1;
    uint32_t m_passIndex = 0, m_tileIndex = 0;   // 次に積むユニット
    uint64_t m_dispatches = 0;

    // 時間予算(タイムスタンプ)
    static constexpr uint32_t kSlots = 8;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_queryReadback;
    struct Slot { uint32_t units = 0; uint64_t marker = 0; bool used = false; };
    Slot m_slots[kSlots];
    uint32_t m_slotCursor = 0;
    int m_prevSlot = -1;
    double m_msPerUnit = 0.0;
    double m_gpuMsTotal = 0.0;
    uint64_t m_tsFreq = 0;
};

} // namespace dx12e::pt
