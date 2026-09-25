#pragma once

// ============================================================================
// シード付きの乱数（Brain ごとに 1 本）。splitmix64。
// ★Lua の math.random は Play のたびに種が変わる（エンジンは randomseed を呼ばない）ので、
//   決定論が要る AI は brain:random() を使う。同じ種なら同じ列。
// ============================================================================

#include "core/Types.h"

namespace dx12e
{
namespace ai
{

class Rng
{
public:
    explicit Rng(u64 seed = 1) { Seed(seed); }
    void Seed(u64 seed) { m_state = seed ^ 0x9E3779B97F4A7C15ull; }

    u64 Next()
    {
        u64 z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // [0, 1)
    f64 Uniform() { return static_cast<f64>(Next() >> 11) * (1.0 / 9007199254740992.0); }
    f64 Range(f64 lo, f64 hi) { return lo + (hi - lo) * Uniform(); }
    // [lo, hi]
    i64 Int(i64 lo, i64 hi)
    {
        if (hi <= lo) return lo;
        const u64 span = static_cast<u64>(hi - lo) + 1ull;
        return lo + static_cast<i64>(Next() % span);
    }
    u64 State() const { return m_state; }

private:
    u64 m_state = 0;
};

} // namespace ai
} // namespace dx12e
