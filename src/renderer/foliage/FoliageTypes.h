#pragma once
// ===========================================================================
// 植生 F1: インスタンスデータ（CPU 側の実体）と 32B パック規約・チャンク構築。
// ---------------------------------------------------------------------------
// ★GPU にも D3D にも依存しない純ロジック。tests/foliage_test.cpp が直接リンクして検証する。
//   HLSL 側（shaders/foliage/FoliageCommon.hlsli）とバイトレイアウト・パック規約が一対一。
//   片方だけ直すと GPU が別のインスタンスを描くので、変えるときは必ず両方 + テストを更新すること。
//
// 1 インスタンス = 32 B（100 万で 32 MB）:
//   float3 position            レイヤーのローカル空間（レイヤーの Transform でワールドへ）
//   u32 rotScale               bit0..15 = yaw（u16、2π/65536 単位）/ bit16..31 = 一様スケール（IEEE half）
//   u32 tilt                   bit0..15 = tiltX（snorm16）/ bit16..31 = tiltZ（snorm16）。傾き = snorm × π/4
//   u32 color                  RGBA8（RGB = 色ばらつきの乗数 / A = 風の硬さのばらつき 0..1）
//   u32 seedType               bit0..23 = シード（風位相・間引きハッシュ）/ bit24..31 = 種別（variant 番号）
//   u32 params                 予約（bit0..7 = 風の強さ倍率 ×1/128。0 は 1.0 と同じ扱い）
// チャンク = 空間的にまとまった ≤128 個（AABB + 先頭添字 + 個数）。GPU は 1 チャンク = 1 スレッドグループで
// まずチャンクごと棄却してから個々のインスタンスを見る。
// ===========================================================================
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/Types.h"

namespace dx12e::foliage
{

constexpr u32 kMaxVariants       = 4;    // 種別（variant）の上限。variant ごとに LOD モデルを持つ
constexpr u32 kMaxLods           = 4;    // LOD の上限（LOD0..3）
constexpr u32 kChunkMaxInstances = 128;  // 1 チャンクの最大個数（= HLSL の numthreads）
constexpr u32 kFileVersion       = 1;

struct FoliageInstance
{
    f32 px = 0, py = 0, pz = 0;
    u32 rotScale = 0;
    u32 tilt     = 0;
    u32 color    = 0xFFFFFFFFu;
    u32 seedType = 0;
    u32 params   = 0;
};
static_assert(sizeof(FoliageInstance) == 32, "FoliageInstance は 32B（HLSL の FoliageInstance と一致）");

struct FoliageChunk
{
    f32 mn[3] = {0, 0, 0};
    u32 first = 0;
    f32 mx[3] = {0, 0, 0};
    u32 count = 0;
};
static_assert(sizeof(FoliageChunk) == 32, "FoliageChunk は 32B（HLSL の FoliageChunk と一致）");

// ---- パック / アンパック（GPU の FoliageCommon.hlsli と同じ規約）----
constexpr f32 kPi    = 3.14159265358979323846f;
constexpr f32 kTwoPi = 6.28318530717958647692f;
constexpr f32 kTiltMax = kPi * 0.25f;   // tilt の snorm 1.0 = 45°

inline u32 PackYawScale(f32 yawRad, f32 scale)
{
    f32 y = yawRad / kTwoPi;
    y -= std::floor(y);                                     // 0..1
    u32 yaw16 = static_cast<u32>(y * 65536.0f + 0.5f) & 0xFFFFu;
    const u16 half = DirectX::PackedVector::XMConvertFloatToHalf(scale);
    return yaw16 | (static_cast<u32>(half) << 16);
}
inline f32 UnpackYaw(u32 rotScale) { return static_cast<f32>(rotScale & 0xFFFFu) * (kTwoPi / 65536.0f); }
inline f32 UnpackScale(u32 rotScale)
{
    return DirectX::PackedVector::XMConvertHalfToFloat(static_cast<DirectX::PackedVector::HALF>(rotScale >> 16));
}

inline u32 PackSnorm16(f32 v)
{
    const f32 c = std::clamp(v, -1.0f, 1.0f);
    const i32 i = static_cast<i32>(std::lround(c * 32767.0f));
    return static_cast<u32>(static_cast<u16>(static_cast<i16>(i)));
}
inline f32 UnpackSnorm16(u32 bits16)
{
    const i16 s = static_cast<i16>(static_cast<u16>(bits16 & 0xFFFFu));
    return std::max(static_cast<f32>(s) / 32767.0f, -1.0f);
}
// tiltX/tiltZ はラジアン（±45° へクランプ）
inline u32 PackTilt(f32 tiltXRad, f32 tiltZRad)
{
    return PackSnorm16(tiltXRad / kTiltMax) | (PackSnorm16(tiltZRad / kTiltMax) << 16);
}
inline f32 UnpackTiltX(u32 t) { return UnpackSnorm16(t & 0xFFFFu) * kTiltMax; }
inline f32 UnpackTiltZ(u32 t) { return UnpackSnorm16(t >> 16) * kTiltMax; }

inline u32 PackColor8(f32 r, f32 g, f32 b, f32 a)
{
    auto q = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

inline u32 PackSeedType(u32 seed24, u32 type8) { return (seed24 & 0xFFFFFFu) | ((type8 & 0xFFu) << 24); }
inline u32 SeedOf(u32 seedType) { return seedType & 0xFFFFFFu; }
inline u32 TypeOf(u32 seedType) { return seedType >> 24; }

// 風の強さ倍率（params bit0..7）。0 は 1.0（既定）。
inline f32 WindScaleOf(u32 params)
{
    const u32 v = params & 0xFFu;
    return v == 0 ? 1.0f : static_cast<f32>(v) * (1.0f / 128.0f);
}

// ---- 決定論ハッシュ（HLSL の Hash と同一。lowbias32）----
inline u32 HashU32(u32 x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline f32 Hash01(u32 x) { return static_cast<f32>(HashU32(x) >> 8) * (1.0f / 16777216.0f); }

// インスタンス配列（チャンク順に並べ替え済み）+ チャンク表 + 全体境界。編集のたびに version が進む。
struct FoliageInstanceSet
{
    std::vector<FoliageInstance> instances;   // BuildChunks 後はチャンク順（先頭添字 = FoliageChunk::first）
    std::vector<FoliageChunk>    chunks;
    f32 boundsMin[3] = {0, 0, 0};
    f32 boundsMax[3] = {0, 0, 0};
    u32 version = 1;                          // GPU バッファを作り直すかの判定に使う（編集ごとに +1）

    u32 Count() const { return static_cast<u32>(instances.size()); }
    // instances を空間的にまとまった ≤kChunkMaxInstances 個ずつへ並べ替え、chunks / boundsMin/Max を作る。
    // 決定論: 同じ入力（順序込み）なら同じ並びになる（nth_element の同順位はハッシュで解く）。
    void Rebuild();
};

// ---- チャンク構築（二分割の kd 木。長辺の中央値で切る）----
inline void FoliageInstanceSet::Rebuild()
{
    chunks.clear();
    const u32 n = Count();
    if (n == 0)
    {
        for (int i = 0; i < 3; ++i) { boundsMin[i] = 0; boundsMax[i] = 0; }
        ++version;
        return;
    }
    struct Range { u32 begin, end; };
    std::vector<Range> stack;
    stack.push_back({0, n});
    // 深さ優先で「左 → 右」の順にチャンクを吐く（スタックには右を先に積む）
    chunks.reserve(n / 64 + 8);
    auto boundsOf = [&](u32 b, u32 e, f32 mn[3], f32 mx[3])
    {
        mn[0] = mn[1] = mn[2] = 3.4e38f;
        mx[0] = mx[1] = mx[2] = -3.4e38f;
        for (u32 i = b; i < e; ++i)
        {
            const FoliageInstance& s = instances[i];
            mn[0] = std::min(mn[0], s.px); mx[0] = std::max(mx[0], s.px);
            mn[1] = std::min(mn[1], s.py); mx[1] = std::max(mx[1], s.py);
            mn[2] = std::min(mn[2], s.pz); mx[2] = std::max(mx[2], s.pz);
        }
    };
    while (!stack.empty())
    {
        const Range r = stack.back();
        stack.pop_back();
        f32 mn[3], mx[3];
        boundsOf(r.begin, r.end, mn, mx);
        const u32 cnt = r.end - r.begin;
        if (cnt <= kChunkMaxInstances)
        {
            FoliageChunk c;
            std::memcpy(c.mn, mn, sizeof(mn));
            std::memcpy(c.mx, mx, sizeof(mx));
            c.first = r.begin;
            c.count = cnt;
            chunks.push_back(c);
            continue;
        }
        int axis = 0;
        f32 ext = mx[0] - mn[0];
        if (mx[1] - mn[1] > ext) { axis = 1; ext = mx[1] - mn[1]; }
        if (mx[2] - mn[2] > ext) { axis = 2; }
        // 個数が 128 の倍数になるよう「左 = 128 の倍数個」で切る（末端チャンクを満杯に近づける）
        u32 leftCnt = ((cnt / 2 + kChunkMaxInstances - 1) / kChunkMaxInstances) * kChunkMaxInstances;
        if (leftCnt >= cnt) leftCnt = cnt / 2;
        const u32 mid = r.begin + leftCnt;
        auto key = [axis](const FoliageInstance& a) -> f32 { return axis == 0 ? a.px : (axis == 1 ? a.py : a.pz); };
        std::nth_element(instances.begin() + r.begin, instances.begin() + mid, instances.begin() + r.end,
            [&](const FoliageInstance& a, const FoliageInstance& b)
            {
                const f32 ka = key(a), kb = key(b);
                if (ka != kb) return ka < kb;
                // 同値は seedType → 位置の残りで全順序にする（決定論）
                if (a.seedType != b.seedType) return a.seedType < b.seedType;
                if (a.px != b.px) return a.px < b.px;
                if (a.py != b.py) return a.py < b.py;
                return a.pz < b.pz;
            });
        stack.push_back({mid, r.end});
        stack.push_back({r.begin, mid});
    }
    boundsOf(0, n, boundsMin, boundsMax);
    ++version;
}

} // namespace dx12e::foliage
