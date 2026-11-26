#pragma once
// ===========================================================================
// UnoFX の放出の算術（W1-BD4。MOD-VFX-03）。ヘッダオンリー・GPU 非依存（tests/vfx_spawn_math_test.cpp）。
//   * 持ち越し式: 端数と、1 ステップの上限を超えた分は「失わず」次のステップへ持ち越す。
//   * サブフレーム補間: 1 ステップの n 個の放出は、ステップ内で等間隔（(k+0.5)/n）の時刻と位置に置く。
//   * 乱数は (seed, id, salt) のハッシュだけ（HLSL の VfxCommon.hlsli の Pcg / Hash01 と一致させること）。
// ===========================================================================
#include <algorithm>
#include <cmath>

#include "core/Types.h"
#include "renderer/vfx/VfxAsset.h"

namespace dx12e::vfx
{

struct SpawnClock
{
    double time      = 0.0;   // エフェクトの時刻（秒）
    double owed      = 0.0;   // 持ち越し（まだ放出していない個数。端数を含む）
    u32    spawned   = 0;     // 通し番号（次の粒子の id）。u32 の桁あふれは wrap する（slot = id % cap は cap が 2^32 を割り切るときだけ連続。そうでなければ約 43 億個で 1 回だけ slot が飛ぶ）
};

struct SpawnStep
{
    u32    count   = 0;   // このステップの放出数
    u32    firstId = 0;   // 最初の粒子の id（以降 +1 ずつ）
};

// 1 ステップ（dt 秒）ぶんの放出数を決めて clock を進める。
//   emissionMul: 品質プリセットの放出倍率。cap: そのインスタンスの粒子の上限（リングの大きさ）。
//   loop / duration: エフェクトの長さ。loop=false なら duration を超えたら放出しない（duration<=0 は無限）。
inline SpawnStep PlanSpawnStep(const SpawnDesc& s, bool loop, float duration, double dt,
                               float emissionMul, u32 cap, SpawnClock& clk)
{
    SpawnStep out;
    out.firstId = clk.spawned;
    const double t0 = clk.time;
    const double t1 = clk.time + dt;
    const double mul = std::max(0.0f, emissionMul);

    // 連続放出: [startDelay, end) との重なりの長さ × rate
    const double end = (!loop && duration > 0.0f) ? static_cast<double>(duration) : 1.0e300;
    const double lo = std::max(t0, static_cast<double>(s.startDelay));
    const double hi = std::min(t1, end);
    if (hi > lo) clk.owed += (hi - lo) * s.rate * mul;

    // バースト: [t0, t1) に入るもの。loop なら duration で折り返した時刻で数える。
    if (!s.bursts.empty())
    {
        const bool wrap = loop && duration > 0.0f;
        const double dur = duration;
        // 周回をまたぐ区間は周ごとに分けて数える（dt が duration より長くても取りこぼさない）
        double a = t0;
        while (a < t1)
        {
            const double cycleStart = wrap ? std::floor(a / dur) * dur : 0.0;
            const double cycleEnd   = wrap ? cycleStart + dur : 1.0e300;
            const double b = std::min(t1, cycleEnd);
            for (const BurstDesc& bd : s.bursts)
            {
                const double bt = cycleStart + bd.t;
                if (bt >= a && bt < b && (wrap ? bd.t < dur : true)) clk.owed += static_cast<double>(bd.count) * mul;
            }
            if (!wrap) break;
            a = b;
        }
    }

    double n = std::floor(clk.owed + 1.0e-9);
    n = std::max(0.0, n);
    const double perStepCap = static_cast<double>(std::min<u32>(kMaxEmitPerStep, std::max<u32>(cap, 1u)));
    if (n > perStepCap) n = perStepCap;
    clk.owed -= n;
    // 上限で詰まった分が無限に溜まらないよう、持ち越しの上限を決める（上限の 4 ステップぶん）
    const double maxBacklog = perStepCap * 4.0;
    if (clk.owed > maxBacklog) clk.owed = maxBacklog;
    if (clk.owed < 0.0) clk.owed = 0.0;

    out.count = static_cast<u32>(n);
    clk.spawned += out.count;
    clk.time = t1;
    return out;
}

// n 個のうち k 番目（0 始まり）の、ステップ内の位置 0..1（放出体の prev→cur の補間と、放出時刻に使う）。
inline float SubframeFraction(u32 k, u32 n) { return (static_cast<float>(k) + 0.5f) / static_cast<float>(n); }
// その粒子が放出された後、このステップの終わりまでに経過している時間（= 放出と同じステップで先に進める分）。
inline float SubframeAdvance(u32 k, u32 n, float stepDt) { return stepDt * (1.0f - SubframeFraction(k, n)); }

// ---- 乱数（HLSL の Pcg / Hash01 と同じ式）----
inline u32 Pcg(u32 v)
{
    const u32 state = v * 747796405u + 2891336453u;
    const u32 word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
inline u32 HashU(u32 seed, u32 id, u32 salt) { return Pcg(seed ^ Pcg(id + salt * 0x9E3779B9u)); }
inline float Hash01(u32 seed, u32 id, u32 salt) { return static_cast<float>(HashU(seed, id, salt) >> 8) * (1.0f / 16777216.0f); }

} // namespace dx12e::vfx
