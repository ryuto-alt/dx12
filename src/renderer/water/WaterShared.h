#pragma once
// ===========================================================================
// WaterShared — C++ と HLSL（shaders/water/WaterCommon.hlsli の WD_* 定数）で共有する GPU データのレイアウト。
// ---------------------------------------------------------------------------
// 水のパラメータは StructuredBuffer<float4>（メイン RS の材質テーブルの 4 本目 = t24。DWORD 増分 0）に入れる。
//   [0 .. kFrameFloat4s)                       フレーム共通のヘッダ
//   [kFrameFloat4s + b*kBodyStride ...)        水域 b の 1 ブロック
// b0（ルート定数 24 DWORD）はドロー（リング 1 枚）ごとの値だけ: viewProj(16) + lvl0(4) + lvl1(4)。
// ★HLSL 側の定数と行ごとに一致させること。並べ替えたら WaterCommon.hlsli の WD_* も直す。
// ===========================================================================
#include <cstdint>
#include <cstring>

#include "renderer/water/WaterMath.h"

namespace dx12e::water
{

inline constexpr uint32_t kMaxBodies = 16;
inline constexpr uint32_t kFrameFloat4s = 12;
inline constexpr uint32_t kBodyStride = 42;    // float4 単位
inline constexpr uint32_t kDataFloat4s = kFrameFloat4s + kMaxBodies * kBodyStride;
inline constexpr uint32_t kDataBytes = kDataFloat4s * 16;

// ---- フレームヘッダ（float4 の添字）----
enum FrameSlot : uint32_t
{
    kFrInvVP = 0,      // 0..3   invViewProjJ（ジッタあり・非転置の行 4 本。HLSL では float4x4(rows) で組み立てて mul(v, M)）
    kFrVP = 4,         // 4..7   viewProjJ（同）
    kFrScreen = 8,     // (W, H, 1/W, 1/H)
    kFrProj = 9,       // (proj._33, proj._43, 未使用, 未使用)
    kFrCam = 10,       // (camPos.xyz, 水中のとき: その水域の添字 / 水中でなければ -1)
    kFrRange = 11,     // (near, far, 未使用, 未使用)
};

// ---- 水域ブロック（float4 の添字。base = kFrameFloat4s + b * kBodyStride）----
enum BodySlot : uint32_t
{
    kBdLevel = 0,      // (水面高, 波の本数, 時刻[秒], 形)
    kBdOrigin = 1,     // (ox, oz, ax, az)   ローカル原点のワールド XZ / ローカル x 軸のワールド向き
    kBdAxes = 2,       // (bx, bz, sizeX, sizeZ)   ローカル z 軸 / 矩形・楕円の全幅
    kBdInv = 3,        // (i00, i01, i10, i11)   ワールド → ローカル
    kBdFoam = 4,       // (foamCrest, foamShore, foamWidth, shoreFade)
    kBdExt = 5,        // (消散 RGB = 吸収 + 濁り, 濁り)
    kBdScatter = 6,    // (散乱色 RGB, ior)
    kBdOptics = 7,     // (roughness, refractionStrength, detailStrength, causticIntensity)
    kBdDetail = 8,     // (detailTile, detailSpeed, flowX, flowZ)
    kBdMisc = 9,       // (ssr(0/1), 多角形の点数, 波の最大鉛直変位, 未使用)
    kBdWaves = 10,     // 10 .. 33  波 12 本 x (A, B)
    kBdPoly = 34,      // 34 .. 41  多角形 16 点 x float2 = 8 float4
};

struct GpuFrameIn
{
    float invViewProjJ[16];   // 行ベクトル規約の行（非転置）
    float viewProjJ[16];
    float width = 0, height = 0;
    float projA = 0, projB = 0;
    float camPos[3] = {0, 0, 0};
    float underBody = -1.0f;
    float nearZ = 0.1f, farZ = 1000.0f;
};

inline void PackFrame(float* dst /* kFrameFloat4s * 4 */, const GpuFrameIn& f)
{
    std::memset(dst, 0, kFrameFloat4s * 16);
    std::memcpy(dst + kFrInvVP * 4, f.invViewProjJ, 64);
    std::memcpy(dst + kFrVP * 4, f.viewProjJ, 64);
    dst[kFrScreen * 4 + 0] = f.width;  dst[kFrScreen * 4 + 1] = f.height;
    dst[kFrScreen * 4 + 2] = f.width > 0 ? 1.0f / f.width : 0.0f;
    dst[kFrScreen * 4 + 3] = f.height > 0 ? 1.0f / f.height : 0.0f;
    dst[kFrProj * 4 + 0] = f.projA; dst[kFrProj * 4 + 1] = f.projB;
    dst[kFrCam * 4 + 0] = f.camPos[0]; dst[kFrCam * 4 + 1] = f.camPos[1]; dst[kFrCam * 4 + 2] = f.camPos[2];
    dst[kFrCam * 4 + 3] = f.underBody;
    dst[kFrRange * 4 + 0] = f.nearZ; dst[kFrRange * 4 + 1] = f.farZ;
}

// 1 水域ぶんを float4 単位で詰める（dst は kBodyStride * 4 float）。waves は GenerateWaves の結果。
inline void PackBody(float* dst, const WaterBody& b, const WaterFrame& fr, const std::vector<float>& poly, float time,
                     const Wave* waves, int waveCount)
{
    std::memset(dst, 0, kBodyStride * 16);
    auto set4 = [&](uint32_t slot, float x, float y, float z, float w)
    {
        dst[slot * 4 + 0] = x; dst[slot * 4 + 1] = y; dst[slot * 4 + 2] = z; dst[slot * 4 + 3] = w;
    };
    set4(kBdLevel, fr.level, static_cast<float>(waveCount), time, static_cast<float>(b.shape));
    set4(kBdOrigin, fr.ox, fr.oz, fr.ax, fr.az);
    set4(kBdAxes, fr.bx, fr.bz, b.size.x, b.size.y);
    set4(kBdInv, fr.i00, fr.i01, fr.i10, fr.i11);
    set4(kBdFoam, Clamp(b.foamCrest, 0, 1), Clamp(b.foamShore, 0, 1), std::max(b.foamWidth, 0.01f), std::max(b.shoreFade, 0.0f));
    float ext[3];
    ExtinctionRgb(b, ext);
    set4(kBdExt, ext[0], ext[1], ext[2], Clamp(b.turbidity, 0, 1));
    set4(kBdScatter, b.scatterColor.x, b.scatterColor.y, b.scatterColor.z, std::max(b.ior, 1.0001f));
    set4(kBdOptics, Clamp(b.roughness, 0.0f, 1.0f), b.refractionStrength, std::max(b.detailStrength, 0.0f), std::max(b.causticIntensity, 0.0f));
    set4(kBdDetail, std::max(b.detailTile, 0.05f), b.detailSpeed, b.flow.x, b.flow.y);
    set4(kBdMisc, b.ssr ? 1.0f : 0.0f, static_cast<float>(poly.size() / 2), MaxWaveHeight(waves, waveCount), 0.0f);
    for (int i = 0; i < waveCount && i < kMaxWaves; ++i)
    {
        set4(kBdWaves + i * 2, waves[i].dirX, waves[i].dirZ, waves[i].k, waves[i].omega);
        set4(kBdWaves + i * 2 + 1, waves[i].amp, waves[i].steep, waves[i].phase, 0.0f);
    }
    for (size_t i = 0; i + 1 < poly.size() && i < static_cast<size_t>(kMaxPolyPoints) * 2; i += 2)
    {
        const uint32_t p = static_cast<uint32_t>(i / 2);
        dst[(kBdPoly + p / 2) * 4 + (p % 2) * 2 + 0] = poly[i];
        dst[(kBdPoly + p / 2) * 4 + (p % 2) * 2 + 1] = poly[i + 1];
    }
}

}   // namespace dx12e::water
