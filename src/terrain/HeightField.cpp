#include "terrain/HeightField.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace dx12e
{

u32 HeightField::NormalizeResolution(u32 res)
{
    if (res < kMinResolution) res = kMinResolution;
    if (res > kMaxResolution) res = kMaxResolution;
    res -= (res % 4u);                                  // 4 の倍数へ切り下げ
    if (res < kMinResolution) res = kMinResolution;     // 切り下げで下限を割らないよう再クランプ
    return res;
}

void HeightField::Create(u32 resolution, f32 worldSize, f32 initialHeight)
{
    m_resolution = NormalizeResolution(resolution);
    m_worldSize  = (worldSize > 0.01f) ? worldSize : 0.01f;
    m_heights.assign(static_cast<size_t>(m_resolution) * m_resolution, initialHeight);
}

f32 HeightField::At(i32 ix, i32 iz) const
{
    if (!IsValid()) return 0.0f;
    const i32 n = static_cast<i32>(m_resolution);
    ix = std::clamp(ix, 0, n - 1);
    iz = std::clamp(iz, 0, n - 1);
    return m_heights[static_cast<size_t>(iz) * static_cast<size_t>(n) + static_cast<size_t>(ix)];
}

void HeightField::SetAt(i32 ix, i32 iz, f32 v)
{
    if (!IsValid()) return;
    const i32 n = static_cast<i32>(m_resolution);
    if (ix < 0 || iz < 0 || ix >= n || iz >= n) return;
    m_heights[static_cast<size_t>(iz) * static_cast<size_t>(n) + static_cast<size_t>(ix)] = v;
}

f32 HeightField::SampleHeight(f32 lx, f32 lz) const
{
    if (!IsValid()) return 0.0f;

    const f32 cell   = CellSize();
    const f32 half   = HalfSize();
    const f32 maxIdx = static_cast<f32>(m_resolution - 1);

    const f32 u = std::clamp((lx + half) / cell, 0.0f, maxIdx);
    const f32 v = std::clamp((lz + half) / cell, 0.0f, maxIdx);

    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const i32 x0 = static_cast<i32>(fu);
    const i32 z0 = static_cast<i32>(fv);
    const f32 fx = u - fu;
    const f32 fz = v - fv;

    const f32 h00 = At(x0,     z0);
    const f32 h10 = At(x0 + 1, z0);
    const f32 h01 = At(x0,     z0 + 1);
    const f32 h11 = At(x0 + 1, z0 + 1);

    return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz)
         + (h01 * (1.0f - fx) + h11 * fx) * fz;
}

XMFLOAT3 HeightField::SampleNormal(f32 lx, f32 lz) const
{
    if (!IsValid()) return XMFLOAT3{0.0f, 1.0f, 0.0f};

    const f32 cell = CellSize();
    const f32 hl = SampleHeight(lx - cell, lz);
    const f32 hr = SampleHeight(lx + cell, lz);
    const f32 hd = SampleHeight(lx, lz - cell);
    const f32 hu = SampleHeight(lx, lz + cell);

    XMFLOAT3 out{};
    XMStoreFloat3(&out, XMVector3Normalize(XMVectorSet(hl - hr, 2.0f * cell, hd - hu, 0.0f)));
    return out;
}

void HeightField::MinMaxHeight(f32& outMin, f32& outMax) const
{
    outMin = 0.0f;
    outMax = 0.0f;
    if (m_heights.empty()) return;
    outMin = outMax = m_heights[0];
    for (f32 h : m_heights)
    {
        outMin = (std::min)(outMin, h);
        outMax = (std::max)(outMax, h);
    }
}

bool HeightField::Raycast(const XMFLOAT3& origin, const XMFLOAT3& dir,
                          f32 maxDistance, f32& outT) const
{
    outT = 0.0f;
    if (!IsValid() || maxDistance <= 0.0f) return false;

    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-8f) return false;
    const f32 dx = dir.x / len, dy = dir.y / len, dz = dir.z / len;

    // 地形のバウンディングボックスへレイをクリップ（XZ はフットプリント、Y は高さの min/max）。
    f32 t0 = 0.0f;
    f32 t1 = maxDistance;
    auto clipSlab = [&t0, &t1](f32 o, f32 d, f32 mn, f32 mx) -> bool
    {
        if (std::fabs(d) < 1e-8f) return (o >= mn && o <= mx);
        f32 a = (mn - o) / d;
        f32 b = (mx - o) / d;
        if (a > b) { const f32 tmp = a; a = b; b = tmp; }
        t0 = (std::max)(t0, a);
        t1 = (std::min)(t1, b);
        return t1 >= t0;
    };

    const f32 half = HalfSize();
    f32 hMin = 0.0f, hMax = 0.0f;
    MinMaxHeight(hMin, hMax);

    if (!clipSlab(origin.x, dx, -half, half)) return false;
    if (!clipSlab(origin.z, dz, -half, half)) return false;
    // Y は少し余裕を持たせる（真上/真下からのレイを取りこぼさないため）
    if (!clipSlab(origin.y, dy, hMin - 1.0f, hMax + 1.0f)) return false;
    if (t1 < t0) return false;

    auto signedDist = [&](f32 t) -> f32
    {
        return (origin.y + dy * t) - SampleHeight(origin.x + dx * t, origin.z + dz * t);
    };

    // 開始点が既に地表より下なら、そこが交点（カメラが地中に潜っている等）。
    if (signedDist(t0) <= 0.0f) { outT = t0; return true; }

    // セル半分刻みで前進し、符号が反転した区間を二分探索で詰める。
    const f32 step = (std::max)(CellSize() * 0.5f, 0.01f);
    f32 tPrev = t0;
    f32 t     = t0 + step;
    while (true)
    {
        const bool last = (t >= t1);
        if (last) t = t1;

        if (signedDist(t) <= 0.0f)
        {
            f32 lo = tPrev, hi = t;
            for (int i = 0; i < 32; ++i)
            {
                const f32 mid = 0.5f * (lo + hi);
                if (signedDist(mid) > 0.0f) lo = mid; else hi = mid;
            }
            outT = hi;
            return true;
        }

        if (last) break;
        tPrev = t;
        t += step;
    }
    return false;
}

HeightField::Rect HeightField::FullRect() const
{
    Rect r;
    if (!IsValid()) return r;
    r.x0 = 0;
    r.z0 = 0;
    r.x1 = static_cast<i32>(m_resolution) - 1;
    r.z1 = static_cast<i32>(m_resolution) - 1;
    return r;
}

HeightField::Rect HeightField::ClampRect(const Rect& in) const
{
    Rect r;
    if (!IsValid() || !in.Valid()) return r;
    const i32 n = static_cast<i32>(m_resolution);
    r.x0 = (std::max)(0, in.x0);
    r.z0 = (std::max)(0, in.z0);
    r.x1 = (std::min)(n - 1, in.x1);
    r.z1 = (std::min)(n - 1, in.z1);
    if (!r.Valid()) return Rect{};
    return r;
}

HeightField::Rect HeightField::RectForCircle(f32 lx, f32 lz, f32 radius) const
{
    if (!IsValid() || radius <= 0.0f) return Rect{};
    const f32 cell = CellSize();
    const f32 half = HalfSize();

    Rect r;
    r.x0 = static_cast<i32>(std::floor((lx - radius + half) / cell));
    r.x1 = static_cast<i32>(std::ceil ((lx + radius + half) / cell));
    r.z0 = static_cast<i32>(std::floor((lz - radius + half) / cell));
    r.z1 = static_cast<i32>(std::ceil ((lz + radius + half) / cell));
    return ClampRect(r);
}

HeightField::Rect HeightField::UnionRect(const Rect& a, const Rect& b)
{
    if (!a.Valid()) return b;
    if (!b.Valid()) return a;
    Rect r;
    r.x0 = (std::min)(a.x0, b.x0);
    r.z0 = (std::min)(a.z0, b.z0);
    r.x1 = (std::max)(a.x1, b.x1);
    r.z1 = (std::max)(a.z1, b.z1);
    return r;
}

std::vector<u8> HeightField::Encode() const
{
    std::vector<u8> out;
    if (!IsValid()) return out;

    const u32 magic = kMagic;
    const u32 ver   = kVersion;
    const u32 res   = m_resolution;
    const f32 size  = m_worldSize;

    out.resize(kHeaderSize + m_heights.size() * sizeof(f32));
    std::memcpy(out.data() +  0, &magic, sizeof(u32));
    std::memcpy(out.data() +  4, &ver,   sizeof(u32));
    std::memcpy(out.data() +  8, &res,   sizeof(u32));
    std::memcpy(out.data() + 12, &size,  sizeof(f32));
    std::memcpy(out.data() + kHeaderSize, m_heights.data(), m_heights.size() * sizeof(f32));
    return out;
}

bool HeightField::Decode(const u8* data, size_t size)
{
    if (!data || size < kHeaderSize) return false;

    u32 magic = 0, ver = 0, res = 0;
    f32 worldSize = 0.0f;
    std::memcpy(&magic,     data +  0, sizeof(u32));
    std::memcpy(&ver,       data +  4, sizeof(u32));
    std::memcpy(&res,       data +  8, sizeof(u32));
    std::memcpy(&worldSize, data + 12, sizeof(f32));

    if (magic != kMagic || ver != kVersion) return false;
    if (res < kMinResolution || res > kMaxResolution) return false;
    // NormalizeResolution と同じ制約（Jolt HeightFieldShape のブロックサイズ倍数）を満たさない
    // ファイルは受け付けない。読めてしまうとコリジョンだけ生成に失敗して原因が分かりにくい。
    if (res % 4u != 0u) return false;
    if (!(worldSize > 0.0f)) return false;   // NaN も弾く

    const size_t count = static_cast<size_t>(res) * res;
    if (size < kHeaderSize + count * sizeof(f32)) return false;

    m_resolution = res;
    m_worldSize  = worldSize;
    m_heights.resize(count);
    std::memcpy(m_heights.data(), data + kHeaderSize, count * sizeof(f32));
    return true;
}

} // namespace dx12e
