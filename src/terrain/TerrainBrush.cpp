#include "terrain/TerrainBrush.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace dx12e
{

namespace
{

constexpr f32 kPi = 3.14159265358979323846f;

// 32bit 整数ハッシュ → -1..1。決定的（同じ seed なら毎回同じ地形）。
f32 Hash2(i32 x, i32 z, u32 seed)
{
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return static_cast<f32>(h & 0x00FFFFFFu) * (2.0f / 16777215.0f) - 1.0f;
}

f32 SmoothStep01(f32 t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 2D バリューノイズ（格子点のハッシュ値を smoothstep で補間）。
f32 ValueNoise(f32 x, f32 z, u32 seed)
{
    const f32 fx = std::floor(x);
    const f32 fz = std::floor(z);
    const i32 ix = static_cast<i32>(fx);
    const i32 iz = static_cast<i32>(fz);
    const f32 tx = SmoothStep01(x - fx);
    const f32 tz = SmoothStep01(z - fz);

    const f32 a = Hash2(ix,     iz,     seed);
    const f32 b = Hash2(ix + 1, iz,     seed);
    const f32 c = Hash2(ix,     iz + 1, seed);
    const f32 d = Hash2(ix + 1, iz + 1, seed);

    return (a + (b - a) * tx) * (1.0f - tz) + (c + (d - c) * tx) * tz;
}

// ミラー無しの単発適用（ApplyTerrainBrush から呼ぶ実体）。
HeightField::Rect ApplyBrushAt(HeightField& hf, const TerrainBrushParams& p,
                               f32 cx, f32 cz, f32 dt, bool invert)
{
    const HeightField::Rect r = hf.RectForCircle(cx, cz, p.radius);
    if (!r.Valid()) return HeightField::Rect{};

    const i32 n    = static_cast<i32>(hf.Resolution());
    const f32 invR = 1.0f / p.radius;

    // Smooth は「元の高さ」を読みながら書くと結果が走査順に依存するのでコピーから読む。
    std::vector<f32> src;
    if (p.type == TerrainBrushType::Smooth) src = hf.Heights();

    for (i32 z = r.z0; z <= r.z1; ++z)
    {
        const f32 wz = hf.GridToLocalZ(z);
        for (i32 x = r.x0; x <= r.x1; ++x)
        {
            const f32 wx = hf.GridToLocalX(x);
            const f32 ddx = wx - cx;
            const f32 ddz = wz - cz;
            const f32 dist = std::sqrt(ddx * ddx + ddz * ddz);
            if (dist > p.radius) continue;

            const f32 w = TerrainBrushWeight(dist * invR, p.falloff);
            if (w <= 0.0f) continue;

            const f32 h = hf.At(x, z);
            f32 nh = h;

            switch (p.type)
            {
            case TerrainBrushType::Raise:
            case TerrainBrushType::Lower:
            {
                f32 sign = (p.type == TerrainBrushType::Raise) ? 1.0f : -1.0f;
                if (invert) sign = -sign;
                nh = h + sign * p.strength * w * dt;
                break;
            }
            case TerrainBrushType::Smooth:
            {
                f32 sum = 0.0f;
                i32 cnt = 0;
                for (i32 oz = -1; oz <= 1; ++oz)
                {
                    const i32 sz = std::clamp(z + oz, 0, n - 1);
                    for (i32 ox = -1; ox <= 1; ++ox)
                    {
                        const i32 sx = std::clamp(x + ox, 0, n - 1);
                        sum += src[static_cast<size_t>(sz) * static_cast<size_t>(n)
                                   + static_cast<size_t>(sx)];
                        ++cnt;
                    }
                }
                const f32 avg = sum / static_cast<f32>(cnt);
                const f32 k   = std::clamp(p.strength * w * dt * 0.5f, 0.0f, 1.0f);
                nh = h + (avg - h) * k;
                break;
            }
            case TerrainBrushType::Flatten:
            {
                const f32 k = std::clamp(p.strength * w * dt * 0.5f, 0.0f, 1.0f);
                nh = h + (p.flattenTarget - h) * k;
                break;
            }
            case TerrainBrushType::Noise:
            {
                const f32 nv = TerrainFbm(wx, wz, p.noiseFrequency, p.noiseOctaves,
                                          p.noiseRidged, p.noiseSeed);
                const f32 sign = invert ? -1.0f : 1.0f;
                nh = h + sign * nv * p.strength * w * dt;
                break;
            }
            default:
                break;
            }

            hf.SetAt(x, z, std::clamp(nh, p.minHeight, p.maxHeight));
        }
    }
    return r;
}

} // anonymous namespace

f32 TerrainBrushWeight(f32 distRatio, f32 falloff)
{
    if (distRatio >= 1.0f) return 0.0f;
    if (distRatio <= 0.0f) return 1.0f;
    const f32 soft = 1.0f - SmoothStep01(distRatio);   // 滑らかな釣鐘
    const f32 hard = 1.0f;                              // 縁まで一定（円柱）
    const f32 f    = std::clamp(falloff, 0.0f, 1.0f);
    return hard * (1.0f - f) + soft * f;
}

f32 TerrainFbm(f32 x, f32 z, f32 frequency, i32 octaves, f32 ridged, u32 seed)
{
    octaves = std::clamp(octaves, 1, 8);
    ridged  = std::clamp(ridged, 0.0f, 1.0f);

    f32 amp  = 1.0f;
    f32 freq = frequency;
    f32 sum  = 0.0f;
    f32 norm = 0.0f;

    for (i32 o = 0; o < octaves; ++o)
    {
        f32 nv = ValueNoise(x * freq, z * freq, seed + static_cast<u32>(o) * 7919u);
        if (ridged > 0.0f)
        {
            // |n| を折り返して尾根を立てる（Musgrave の ridged multifractal の骨だけ）
            const f32 rv = 1.0f - 2.0f * std::fabs(nv);
            nv = nv * (1.0f - ridged) + rv * ridged;
        }
        sum  += nv * amp;
        norm += amp;
        amp  *= 0.5f;
        freq *= 2.0f;
    }
    return (norm > 0.0f) ? (sum / norm) : 0.0f;
}

HeightField::Rect ApplyTerrainBrush(HeightField& hf, const TerrainBrushParams& p,
                                    f32 lx, f32 lz, f32 dt, bool invert)
{
    if (!hf.IsValid() || p.radius <= 0.0f || dt <= 0.0f) return HeightField::Rect{};

    if (p.type == TerrainBrushType::Erode)
    {
        return ApplyThermalErosion(hf, p.talusDeg, p.erodeIterations,
                                   hf.RectForCircle(lx, lz, p.radius));
    }

    // 対称ミラー（X / Z / 両方）。同じ筆を鏡像位置にも置くだけ。
    f32 cxs[4] = { lx, 0.0f, 0.0f, 0.0f };
    f32 czs[4] = { lz, 0.0f, 0.0f, 0.0f };
    int count = 1;
    if (p.mirrorX)               { cxs[count] = -lx; czs[count] =  lz; ++count; }
    if (p.mirrorZ)               { cxs[count] =  lx; czs[count] = -lz; ++count; }
    if (p.mirrorX && p.mirrorZ)  { cxs[count] = -lx; czs[count] = -lz; ++count; }

    HeightField::Rect total{};
    for (int i = 0; i < count; ++i)
        total = HeightField::UnionRect(total, ApplyBrushAt(hf, p, cxs[i], czs[i], dt, invert));
    return total;
}

HeightField::Rect ApplyThermalErosion(HeightField& hf, f32 talusDeg, i32 iterations,
                                      const HeightField::Rect& inRect)
{
    if (!hf.IsValid()) return HeightField::Rect{};

    HeightField::Rect r = inRect.Valid() ? hf.ClampRect(inRect) : hf.FullRect();
    if (!r.Valid()) return HeightField::Rect{};

    // 端の 1 セルぶんは隣から土砂が来るので、書き戻す範囲も 1 広げておく。
    HeightField::Rect work = r;
    work.x0 -= 1; work.z0 -= 1; work.x1 += 1; work.z1 += 1;
    work = hf.ClampRect(work);
    if (!work.Valid()) return HeightField::Rect{};

    iterations = std::clamp(iterations, 1, 200);
    const f32 talus = std::clamp(talusDeg, 1.0f, 85.0f);
    // 隣接セル間で許される最大高低差。これを超えたぶんだけ低い隣へ流す。
    const f32 maxDrop = std::tan(talus * kPi / 180.0f) * hf.CellSize();
    constexpr f32 kMoveRate = 0.5f;   // 超過分のうち一度に動かす割合

    const i32 w = work.Width();
    const i32 h = work.Height();
    std::vector<f32> delta(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);

    auto localIndex = [&](i32 gx, i32 gz) -> i32
    {
        if (gx < work.x0 || gx > work.x1 || gz < work.z0 || gz > work.z1) return -1;
        return (gz - work.z0) * w + (gx - work.x0);
    };

    for (i32 it = 0; it < iterations; ++it)
    {
        std::fill(delta.begin(), delta.end(), 0.0f);

        for (i32 z = work.z0; z <= work.z1; ++z)
        {
            for (i32 x = work.x0; x <= work.x1; ++x)
            {
                const f32 hc = hf.At(x, z);
                const i32 nx[4] = { x - 1, x + 1, x,     x     };
                const i32 nz[4] = { z,     z,     z - 1, z + 1 };

                f32 diff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                f32 dMax = 0.0f;
                f32 dSum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    diff[k] = hc - hf.At(nx[k], nz[k]);
                    if (diff[k] > maxDrop)
                    {
                        dSum += diff[k];
                        dMax  = (std::max)(dMax, diff[k]);
                    }
                }
                if (dSum <= 0.0f) continue;

                const f32 amount = kMoveRate * (dMax - maxDrop);
                if (amount <= 0.0f) continue;

                const i32 self = localIndex(x, z);
                for (int k = 0; k < 4; ++k)
                {
                    if (diff[k] <= maxDrop) continue;
                    const f32 move = amount * (diff[k] / dSum);
                    const i32 ni = localIndex(nx[k], nz[k]);
                    if (ni < 0) continue;   // work 矩形の外へは流さない（境界の質量は保存しない）
                    if (self >= 0) delta[static_cast<size_t>(self)] -= move;
                    delta[static_cast<size_t>(ni)] += move;
                }
            }
        }

        for (i32 z = work.z0; z <= work.z1; ++z)
            for (i32 x = work.x0; x <= work.x1; ++x)
            {
                const i32 li = localIndex(x, z);
                if (li < 0) continue;
                hf.SetAt(x, z, hf.At(x, z) + delta[static_cast<size_t>(li)]);
            }
    }

    return work;
}

TerrainGenParams MakeTerrainPreset(TerrainPreset preset, f32 worldSize, u32 seed)
{
    // 周波数は「地形の一辺に何個うねりが乗るか」で決める＝サイズを変えても見た目が保たれる。
    const f32 span = (worldSize > 1.0f) ? worldSize : 1.0f;
    TerrainGenParams p;
    p.seed = seed;

    switch (preset)
    {
    case TerrainPreset::Canyon:
        p.frequency   = 2.5f / span;
        p.octaves     = 5;
        p.amplitude   = 26.0f;
        p.ridged      = 0.85f;
        p.baseHeight  = 12.0f;
        p.edgeFalloff = 0.0f;
        p.valleyDepth = 1.6f;    // 低い所をさらに掘る＝切り立った谷
        break;
    case TerrainPreset::Mountains:
        p.frequency   = 3.5f / span;
        p.octaves     = 6;
        p.amplitude   = 70.0f;
        p.ridged      = 0.7f;
        p.baseHeight  = 0.0f;
        p.edgeFalloff = 0.55f;
        p.valleyDepth = 0.0f;
        break;
    case TerrainPreset::RollingHills:
    default:
        p.frequency   = 1.8f / span;
        p.octaves     = 4;
        p.amplitude   = 9.0f;
        p.ridged      = 0.0f;
        p.baseHeight  = 0.0f;
        p.edgeFalloff = 0.25f;
        p.valleyDepth = 0.0f;
        break;
    }
    return p;
}

void GenerateTerrain(HeightField& hf, const TerrainGenParams& p)
{
    if (!hf.IsValid()) return;

    const i32 n     = static_cast<i32>(hf.Resolution());
    const f32 half  = hf.HalfSize();
    const f32 edge  = std::clamp(p.edgeFalloff, 0.0f, 1.0f);
    const f32 valley = (std::max)(p.valleyDepth, 0.0f);

    for (i32 z = 0; z < n; ++z)
    {
        const f32 wz = hf.GridToLocalZ(z);
        for (i32 x = 0; x < n; ++x)
        {
            const f32 wx = hf.GridToLocalX(x);

            f32 v = TerrainFbm(wx, wz, p.frequency, p.octaves, p.ridged, p.seed);
            // fBm は -1..1。0..1 へ寄せてから振幅を掛ける（地面が水没しっぱなしにならない）。
            f32 hgt = (v * 0.5f + 0.5f) * p.amplitude + p.baseHeight;

            if (valley > 0.0f)
            {
                // 低い所ほど余計に掘る（v<0 の領域だけ深くなる）＝峡谷
                const f32 low = (std::max)(0.0f, -v);
                hgt -= low * low * p.amplitude * valley;
            }

            if (edge > 0.0f)
            {
                // 中心 1.0 → 外周 0.0 の滑らかな減衰（正方形の縁に沿って落とす）
                const f32 dx = std::fabs(wx) / half;
                const f32 dz = std::fabs(wz) / half;
                const f32 d  = (std::max)(dx, dz);
                const f32 k  = 1.0f - SmoothStep01((d - (1.0f - edge)) / (edge > 0.0f ? edge : 1.0f));
                hgt *= std::clamp(k, 0.0f, 1.0f);
            }

            hf.SetAt(x, z, hgt);
        }
    }
}

} // namespace dx12e
