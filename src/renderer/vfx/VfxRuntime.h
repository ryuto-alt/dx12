#pragma once
// ===========================================================================
// UnoFX の実行時の窓口（W1-BD4）。renderer（VfxSystem）と、アプリ / MCP（core/mcp/ApplicationMcpLaneB.cpp）の間の口。
//   * Host: アプリが入れるコールバック（レジストリ・フレーム時間・品質プリセット・保留中の配置）。renderer は Application を知らない。
//   * 要求: vfx_seek などが積み、VfxSystem が次のフレームで消化する（メインスレッド専用。ロックなし）。
//   * 統計: VfxSystem が書き、vfx_stats が読む。
// ===========================================================================
#include <entt/entt.hpp>

#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e::vfx
{

struct Host
{
    void* user = nullptr;
    entt::registry* (*registry)(void* user) = nullptr;            // 今のシーンのレジストリ（無ければ nullptr）
    float           (*frameDt)(void* user) = nullptr;             // このフレームの実時間の dt（秒）。決定論キャプチャ中は 0
    void            (*quality)(void* user, float& emissionMul, u32& maxParticles) = nullptr;   // 品質プリセットの VFX の行（maxParticles 0 = 上限なし）
    void            (*pollPending)(void* user) = nullptr;         // 保留中の配置（pendingVfxPlace）を消化する
    // assets 相対パスのファイルを読む（プロジェクトに無ければエンジン同梱の assets から。空なら見つからない）。null なら vfs だけ。
    std::vector<u8> (*readAsset)(void* user, const std::string& rel) = nullptr;
};

struct SeekRequest
{
    entt::entity entity = entt::null;   // null = 全部
    double       t = 0.0;               // 秒
    bool         resume = false;        // true なら seek 後に再生を続ける。false なら止めたまま
};

struct InstanceStat
{
    u32    entity = 0;
    u32    emitter = 0;
    u32    cap = 0;
    u32    spawned = 0;       // 通し番号（これまでに放出した数）
    u32    alive = 0;         // GPU が数えた生存数（数フレーム遅れ。wave 集約のアトミック）
    double time = 0.0;
    bool   paused = false;
    bool   finished = false;
};

struct Stats
{
    bool   gpuReady = false;
    bool   gpuFailed = false;   // GPU リソース / パイプラインを作れなかった（lastError に理由）
    u32    poolUsed = 0;
    u32    poolCapacity = 0;
    u32    instances = 0;
    u64    vramBytes = 0;
    std::vector<InstanceStat> list;
    std::string lastError;
};

class Runtime
{
public:
    static Runtime& Get();

    void SetHost(const Host& h) { m_host = h; m_hasHost = true; }
    void ClearHost() { m_host = Host{}; m_hasHost = false; }
    bool HasHost() const { return m_hasHost && m_host.registry != nullptr; }
    const Host& GetHost() const { return m_host; }

    void RequestSeek(const SeekRequest& r) { m_seeks.push_back(r); }
    bool HasSeeks() const { return !m_seeks.empty(); }
    // アセットの世代。置いた（vfx_place / ダブルクリック）ときに進める。VfxSystem は世代が変わるとファイルの内容を見直す
    //   （編集した .uvfx の再読み込み。読めなかったものの再試行）。
    void BumpAssetGeneration() { ++m_assetGen; }
    u32  AssetGeneration() const { return m_assetGen; }
    // true なら保留中の配置はアプリが毎フレームの更新（VfxTick）で消化する。false の間は VfxSystem が従来どおり描画中に消化する
    void SetTickDriven(bool v) { m_tickDriven = v; }
    bool TickDriven() const { return m_tickDriven; }
    std::vector<SeekRequest> TakeSeeks() { std::vector<SeekRequest> v; v.swap(m_seeks); return v; }

    void PublishStats(Stats&& s) { m_stats = std::move(s); }
    const Stats& GetStats() const { return m_stats; }
    // GPU の初期化に失敗したことを統計へ即座に出す（VfxSystem が失敗した瞬間に 1 回呼ぶ。統計は初期値へ戻して gpuFailed / lastError だけ立てる。PublishStats は上書きなので、以後 gpuFailed を保つのは VfxSystem が作る Stats に m_gpuFailed / m_gpuError を写す側）。
    //   黙って無効にせず、vfx_stats の gpuFailed / lastError で外から見えるようにする（FU-E-19）。
    void ReportGpuFailure(const std::string& reason);
    // 失敗の 1 行説明（ログ・MCP のエラー文・UI 用）。失敗していなければ空。
    std::string DescribeGpuFailure() const;

private:
    Host m_host;
    bool m_hasHost = false;
    bool m_tickDriven = false;
    u32  m_assetGen = 0;
    std::vector<SeekRequest> m_seeks;
    Stats m_stats;
};

} // namespace dx12e::vfx
