#include "terrain/TerrainSplatMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e
{

namespace
{

// 決定的な value noise（ハッシュ + バイリニア）。境界を自然に散らすためだけに使うので安物で十分。
f32 Hash2(i32 x, i32 z)
{
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFF);
}

f32 ValueNoise(f32 x, f32 z)
{
    const f32 fx = std::floor(x), fz = std::floor(z);
    const i32 ix = static_cast<i32>(fx), iz = static_cast<i32>(fz);
    f32 tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const f32 a = Hash2(ix, iz),     b = Hash2(ix + 1, iz);
    const f32 c = Hash2(ix, iz + 1), d = Hash2(ix + 1, iz + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

f32 SmoothStep(f32 e0, f32 e1, f32 x)
{
    if (!(e1 > e0)) return (x >= e1) ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void WriteU32(std::vector<u8>& out, u32 v)
{
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>((v >> 8) & 0xFF));
    out.push_back(static_cast<u8>((v >> 16) & 0xFF));
    out.push_back(static_cast<u8>((v >> 24) & 0xFF));
}

u32 ReadU32(const u8* p)
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8)
         | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

} // namespace

u32 TerrainSplatMap::NormalizeSize(u32 size)
{
    // BC 圧縮も CPU ミップも 2 のべき乗が一番素直なので、近い 2 のべき乗へ丸める。
    u32 p = kMinSize;
    while (p < size && p < kMaxSize) p <<= 1;
    return std::clamp(p, kMinSize, kMaxSize);
}

void TerrainSplatMap::Create(u32 size)
{
    m_size = NormalizeSize(size);
    m_pixels.assign(static_cast<size_t>(m_size) * m_size * 4, 0);
    for (size_t i = 0; i < m_pixels.size(); i += 4)
        m_pixels[i] = 255;   // 全面レイヤー 0
    ++m_version;
}

u32 TerrainSplatMap::MipCount(u32 size)
{
    u32 n = 1;
    while (size > 1) { size >>= 1; ++n; }
    return n;
}

std::vector<std::vector<u8>> TerrainSplatMap::BuildMipChain() const
{
    std::vector<std::vector<u8>> chain;
    if (!IsValid()) return chain;

    chain.push_back(m_pixels);
    u32 w = m_size;
    while (w > 1)
    {
        const std::vector<u8>& src = chain.back();
        const u32 hw = w >> 1;
        std::vector<u8> dst(static_cast<size_t>(hw) * hw * 4);
        for (u32 y = 0; y < hw; ++y)
        {
            for (u32 x = 0; x < hw; ++x)
            {
                const size_t s0 = (static_cast<size_t>(y * 2) * w + x * 2) * 4;
                const size_t s1 = s0 + 4;
                const size_t s2 = s0 + static_cast<size_t>(w) * 4;
                const size_t s3 = s2 + 4;
                const size_t d  = (static_cast<size_t>(y) * hw + x) * 4;
                for (u32 c = 0; c < 4; ++c)
                {
                    const u32 sum = static_cast<u32>(src[s0 + c]) + src[s1 + c]
                                  + src[s2 + c] + src[s3 + c];
                    dst[d + c] = static_cast<u8>((sum + 2) / 4);
                }
            }
        }
        chain.push_back(std::move(dst));
        w = hw;
    }
    return chain;
}

i32 TerrainSplatMap::LocalToTexelX(f32 lx, f32 worldSize) const
{
    if (!IsValid() || !(worldSize > 0.0f)) return 0;
    const f32 u = (lx / worldSize) + 0.5f;
    return std::clamp(static_cast<i32>(u * static_cast<f32>(m_size)), 0,
                      static_cast<i32>(m_size) - 1);
}

i32 TerrainSplatMap::LocalToTexelZ(f32 lz, f32 worldSize) const
{
    if (!IsValid() || !(worldSize > 0.0f)) return 0;
    const f32 v = (lz / worldSize) + 0.5f;
    return std::clamp(static_cast<i32>(v * static_cast<f32>(m_size)), 0,
                      static_cast<i32>(m_size) - 1);
}

HeightField::Rect TerrainSplatMap::PaintCircle(f32 worldSize, f32 lx, f32 lz, f32 radius,
                                               u32 layer, f32 strength, f32 falloff)
{
    HeightField::Rect rect;
    if (!IsValid() || layer >= kLayers || !(worldSize > 0.0f) || !(radius > 0.0f))
        return rect;

    const f32 texelsPerMeter = static_cast<f32>(m_size) / worldSize;
    const f32 rTex = radius * texelsPerMeter;
    const f32 cx = ((lx / worldSize) + 0.5f) * static_cast<f32>(m_size);
    const f32 cz = ((lz / worldSize) + 0.5f) * static_cast<f32>(m_size);

    const i32 x0 = std::clamp(static_cast<i32>(std::floor(cx - rTex)), 0, static_cast<i32>(m_size) - 1);
    const i32 x1 = std::clamp(static_cast<i32>(std::ceil (cx + rTex)), 0, static_cast<i32>(m_size) - 1);
    const i32 z0 = std::clamp(static_cast<i32>(std::floor(cz - rTex)), 0, static_cast<i32>(m_size) - 1);
    const i32 z1 = std::clamp(static_cast<i32>(std::ceil (cz + rTex)), 0, static_cast<i32>(m_size) - 1);
    if (x1 < x0 || z1 < z0) return rect;

    const f32 fall = std::clamp(falloff, 0.0f, 1.0f);
    bool touched = false;

    for (i32 z = z0; z <= z1; ++z)
    {
        for (i32 x = x0; x <= x1; ++x)
        {
            const f32 dx = (static_cast<f32>(x) + 0.5f) - cx;
            const f32 dz = (static_cast<f32>(z) + 0.5f) - cz;
            const f32 d  = std::sqrt(dx * dx + dz * dz) / std::max(rTex, 1e-4f);
            if (d >= 1.0f) continue;

            // フォールオフ（中心が硬く、外周でなめらかに 0）。falloff=0 で円盤、1 でガウス風。
            const f32 w = (fall <= 0.0f) ? 1.0f
                        : (1.0f - SmoothStep(1.0f - fall, 1.0f, d));
            const f32 amount = std::clamp(strength * w, 0.0f, 1.0f);
            if (amount <= 0.0f) continue;

            const size_t i = (static_cast<size_t>(z) * m_size + static_cast<size_t>(x)) * 4;
            f32 c[kLayers];
            for (u32 k = 0; k < kLayers; ++k) c[k] = static_cast<f32>(m_pixels[i + k]) / 255.0f;

            // 対象レイヤーを amount ぶん 1.0 へ寄せ、残りは合計 1 になるよう比例縮小。
            const f32 target = c[layer] + (1.0f - c[layer]) * amount;
            f32 restSum = 0.0f;
            for (u32 k = 0; k < kLayers; ++k) if (k != layer) restSum += c[k];
            const f32 restScale = (restSum > 1e-5f) ? ((1.0f - target) / restSum) : 0.0f;

            for (u32 k = 0; k < kLayers; ++k)
            {
                const f32 v = (k == layer) ? target : c[k] * restScale;
                m_pixels[i + k] = static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            touched = true;
        }
    }

    if (!touched) return rect;
    rect.x0 = x0; rect.z0 = z0; rect.x1 = x1; rect.z1 = z1;
    ++m_version;
    return rect;
}

void TerrainSplatMap::AutoPaintFromHeightField(const HeightField& hf,
                                               const TerrainAutoPaintParams& params)
{
    if (!IsValid() || !hf.IsValid()) return;

    f32 minH = 0.0f, maxH = 0.0f;
    hf.MinMaxHeight(minH, maxH);

    f32 snow0 = params.snowHeightStart;
    f32 snow1 = params.snowHeightEnd;
    if (params.autoSnowHeight || !(snow1 > snow0))
    {
        const f32 range = std::max(maxH - minH, 1e-3f);
        snow0 = minH + range * 0.55f;
        snow1 = minH + range * 0.80f;
    }

    const f32 worldSize = hf.WorldSize();
    const f32 half      = hf.HalfSize();
    const f32 step      = worldSize / static_cast<f32>(m_size);

    for (u32 z = 0; z < m_size; ++z)
    {
        const f32 lz = -half + (static_cast<f32>(z) + 0.5f) * step;
        for (u32 x = 0; x < m_size; ++x)
        {
            const f32 lx = -half + (static_cast<f32>(x) + 0.5f) * step;

            const DirectX::XMFLOAT3 n = hf.SampleNormal(lx, lz);
            const f32 slope  = std::clamp(1.0f - n.y, 0.0f, 1.0f);
            const f32 height = hf.SampleHeight(lx, lz);

            // 境界を散らすノイズ（-1..+1 を noiseStrength でスケール）
            const f32 nz = (ValueNoise(lx * params.noiseScale, lz * params.noiseScale) - 0.5f)
                         * 2.0f * params.noiseStrength;

            const f32 rock = SmoothStep(params.rockSlopeStart, params.rockSlopeEnd, slope + nz);
            const f32 dirt = SmoothStep(params.dirtSlopeStart, params.dirtSlopeEnd, slope + nz * 0.5f)
                           * (1.0f - rock);
            f32 snow = SmoothStep(snow0, snow1, height + nz * (snow1 - snow0));
            snow *= 1.0f - SmoothStep(params.snowSlopeLimit * 0.7f, params.snowSlopeLimit, slope);
            snow *= 1.0f - rock;

            f32 w[kLayers];
            w[3] = std::clamp(snow, 0.0f, 1.0f);
            const f32 remain = 1.0f - w[3];
            w[2] = std::clamp(rock, 0.0f, 1.0f) * remain;
            w[1] = std::clamp(dirt, 0.0f, 1.0f) * remain;
            w[0] = std::max(remain - w[2] - w[1], 0.0f);

            f32 sum = 0.0f;
            for (u32 k = 0; k < kLayers; ++k) sum += w[k];
            if (sum <= 1e-5f) { w[0] = 1.0f; sum = 1.0f; }

            const size_t i = (static_cast<size_t>(z) * m_size + static_cast<size_t>(x)) * 4;
            for (u32 k = 0; k < kLayers; ++k)
                m_pixels[i + k] = static_cast<u8>(std::clamp(w[k] / sum, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    ++m_version;
}

std::vector<u8> TerrainSplatMap::Encode() const
{
    std::vector<u8> out;
    if (!IsValid()) return out;

    out.reserve(kHeaderSize + m_pixels.size());
    WriteU32(out, kMagic);
    WriteU32(out, kVersion);
    WriteU32(out, m_size);          // width
    WriteU32(out, m_size);          // height
    WriteU32(out, kLayers);
    WriteU32(out, 0);               // reserved 0
    WriteU32(out, 0);               // reserved 1
    WriteU32(out, 0);               // reserved 2
    out.insert(out.end(), m_pixels.begin(), m_pixels.end());
    return out;
}

bool TerrainSplatMap::Decode(const u8* data, size_t size)
{
    if (!data || size < kHeaderSize) return false;
    if (ReadU32(data) != kMagic) return false;
    if (ReadU32(data + 4) != kVersion) return false;

    const u32 w      = ReadU32(data + 8);
    const u32 h      = ReadU32(data + 12);
    const u32 layers = ReadU32(data + 16);
    if (w != h || w < kMinSize || w > kMaxSize || layers != kLayers) return false;

    const size_t need = static_cast<size_t>(w) * h * 4;
    if (size < kHeaderSize + need) return false;

    m_size = w;
    m_pixels.assign(data + kHeaderSize, data + kHeaderSize + need);
    ++m_version;
    return true;
}

} // namespace dx12e
