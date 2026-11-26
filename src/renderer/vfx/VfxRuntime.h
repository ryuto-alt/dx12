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
    std::vector<SeekRequest> TakeSeeks() { std::vector<SeekRequest> v; v.swap(m_seeks); return v; }

    void PublishStats(Stats&& s) { m_stats = std::move(s); }
    const Stats& GetStats() const { return m_stats; }

private:
    Host m_host;
    bool m_hasHost = false;
    std::vector<SeekRequest> m_seeks;
    Stats m_stats;
};

} // namespace dx12e::vfx
