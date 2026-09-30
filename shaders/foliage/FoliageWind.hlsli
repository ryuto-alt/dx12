// FoliageWind.hlsli - 植生 F1: 風による頂点変位（幹の曲げ + 葉のはばたき）。
//
// ★src/renderer/foliage/FoliageMath.h の WindDelta / WindStrengthAt と一対一（tests/foliage_gpu_test.cpp が GPU と CPU の一致を見る）。
// 時間は「フレームの時刻 + シーン風の位相オフセット」（決定論キャプチャでは固定される）。前フレームの時刻も渡され、
// 速度（モーションベクタ）は今フレームと前フレームの 2 回評価で作る（風でゆれる葉でもベクタが正しい）。
//
// 頂点色の規約（Crysis 流。GPU Gems 3 第 16 章）: R = はばたき（葉の縁）の重み / G = 葉ごとの位相 / B = 曲げの重み / A = 焼き込み AO。
//   ★PS へ渡す色にはこの頂点色を使わない（風データがアルベドに掛かってしまうため）。VS が AO だけを乗せる。
#ifndef FOLIAGE_WIND_HLSLI
#define FOLIAGE_WIND_HLSLI

#include "FoliageCommon.hlsli"

// フレーム共通の風（b1 の予約領域 [0..1] または深度系パスの専用 CBV）
struct FoliageWindFrame
{
    float2 dir;            // ワールド XZ の風向（正規化）
    float  speed;          // m/s
    float  gustStrength;   // 0..1
    float  gustFreq;
    float  turbulence;
    float  time;
    float  prevTime;
};
FoliageWindFrame FoliageMakeWindFrame(float4 A, float4 B)
{
    FoliageWindFrame f;
    f.dir = A.xy; f.speed = A.z; f.gustStrength = A.w;
    f.gustFreq = B.x; f.turbulence = B.y; f.time = B.z; f.prevTime = B.w;
    return f;
}

// レイヤー別（b0 の末尾）
struct FoliageWindLayer
{
    float bend;
    float flutter;
    float bendExp;
    float heightLocal;
};

float FWFrac(float x) { return x - floor(x); }
float FWHashLattice(int ix, int iy)
{
    return FoliageHash01((uint(ix) * 0x8da6b343u) ^ (uint(iy) * 0xd8163841u) ^ 0x1b873593u);
}
float FWValueNoise2(float x, float y)
{
    const float fx = floor(x), fy = floor(y);
    const int ix = int(fx), iy = int(fy);
    float ux = x - fx, uy = y - fy;
    ux = ux * ux * (3.0 - 2.0 * ux);
    uy = uy * uy * (3.0 - 2.0 * uy);
    const float a = FWHashLattice(ix, iy),     b = FWHashLattice(ix + 1, iy);
    const float c = FWHashLattice(ix, iy + 1), d = FWHashLattice(ix + 1, iy + 1);
    const float ab = a + (b - a) * ux;
    const float cd = c + (d - c) * ux;
    return ab + (cd - ab) * uy;
}
float FWTri(float x) { return abs(FWFrac(x + 0.5) * 2.0 - 1.0); }

float FoliageWindStrengthAt(float wx, float wz, float t, FoliageWindFrame f, float instPhase01)
{
    const float gc = (wx * f.dir.x + wz * f.dir.y) * 0.08 - t * f.gustFreq;
    const float cr = (-wx * f.dir.y + wz * f.dir.x) * 0.05;
    const float gust = FWValueNoise2(gc, cr);
    float s = f.speed * (1.0 + f.gustStrength * (gust * 2.0 - 1.0) * 0.85);
    const float turb = FWValueNoise2(t * 6.0 + instPhase01 * 17.0, instPhase01 * 5.0 + 0.37);
    s *= 1.0 + f.turbulence * (turb - 0.5) * 1.2;
    return max(s, 0.0);
}

// ワールド位置へ足す変位。引数の意味は FoliageMath.h の WindDelta を参照。
float3 FoliageWindDelta(float3 wp, float3 origin, float3 nrm, float localY, float3 vc,
                        float instPhase01, float windScale, float scale,
                        FoliageWindFrame f, FoliageWindLayer l, float t)
{
    if (f.speed <= 0.0 || (l.bend == 0.0 && l.flutter == 0.0)) return float3(0, 0, 0);
    const float s = FoliageWindStrengthAt(origin.x, origin.z, t, f, instPhase01) * windScale;
    const float h = saturate(localY / max(l.heightLocal, 1e-3));
    const float osc = 0.85 + 0.15 * sin(t * 1.7 + instPhase01 * kFoliageTwoPi);
    const float mag = l.bend * s * 0.01 * pow(h, l.bendExp) * osc * vc.z * l.heightLocal * scale;
    const float perp = sin(t * 1.1 + instPhase01 * kFoliageTwoPi + h * 1.5) * 0.3 * mag;
    const float3 off = float3(f.dir.x * mag - f.dir.y * perp, 0.0, f.dir.y * mag + f.dir.x * perp);
    const float3 r  = wp - origin;
    const float3 r2 = r + off;
    const float l1 = length(r);
    const float l2 = max(length(r2), 1e-4);
    float3 outD = r2 * (l1 / l2) - r;
    if (l.flutter != 0.0 && vc.x > 0.0)
    {
        const float ph = vc.y * kFoliageTwoPi + instPhase01 * 3.7;
        const float k1 = 3.0;
        const float tri = (FWTri(t * 1.975 * k1 + ph) + FWTri(t * 0.793 * k1 + ph * 1.31)
                         + FWTri(t * 0.375 * k1 + ph * 1.73) + FWTri(t * 0.193 * k1 + ph * 2.11)) * 0.25 * 2.0 - 1.0;
        const float amp = l.flutter * vc.x * scale * (0.2 + 0.8 * saturate(s / 8.0));
        outD += nrm * (tri * amp);
    }
    return outD;
}

#endif // FOLIAGE_WIND_HLSLI
