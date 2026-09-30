// WaterMath.hlsli — 水面 W1 の純関数（リソース宣言なし。CPU 参照 src/renderer/water/WaterMath.h と式を一致させる）。
//   波の評価（ゲルストナー）/ 誘電体のフレネル / スネルの屈折 / Beer-Lambert。
//   ★include 側が WATER_WAVE_A(i) / WATER_WAVE_B(i) を定義しておくこと（波データの読み出し先が違うため）:
//        A(i) = (dirX, dirZ, k, omega)   B(i) = (amp, steep, phase, 0)
#ifndef WATER_MATH_HLSLI
#define WATER_MATH_HLSLI

#define WATER_PI 3.14159265359

struct WaterGerstner
{
    float3 disp;        // 変位（x, y, z）
    float3 normal;      // 正規化済みの法線（デタッチ前）
    float  jac;         // ヤコビアン（1 未満 = 波頭）
    float  slopeVar;    // 解像できない波が持つ傾きの分散（ラフネスへ足す）
};

// n 本の波を (x0, z0) で評価する。
//   dispLod : 変位を落とす基準のワールド長（頂点間隔）。波長がこの 3 倍未満の波は変位を薄める。0 なら落とさない。
//   normLod : 法線を落とす基準のワールド長（1 画素の足跡）。波長がこの 2 倍未満の波は傾きを薄め、失った傾きを slopeVar に積む。0 なら落とさない。
WaterGerstner WaterEvalGerstner(uint n, float2 xz, float t, float2 flow, float dispLod, float normLod)
{
    WaterGerstner g;
    g.disp = 0.0; g.slopeVar = 0.0;
    float nx = 0.0, nz = 0.0, nyMinus = 0.0, jxx = 0.0, jzz = 0.0, jxz = 0.0;
    [loop]
    for (uint i = 0; i < n; ++i)
    {
        const float4 a = WATER_WAVE_A(i);
        const float4 b = WATER_WAVE_B(i);
        const float2 d = a.xy;
        const float k = a.z;
        const float lam = 2.0 * WATER_PI / k;
        float theta = k * dot(d, xz - flow * t) - a.w * t + b.z;
        float sn, cs;
        sincos(theta, sn, cs);
        float vf = 1.0;
        if (dispLod > 0.0) vf = saturate((lam / dispLod - 3.0) * (1.0 / 3.0));
        float nf = 1.0;
        if (normLod > 0.0) nf = saturate((lam / normLod - 2.0) * 0.5);
        const float qa = b.y * b.x * vf;
        g.disp.x += qa * d.x * cs;
        g.disp.z += qa * d.y * cs;
        g.disp.y += b.x * vf * sn;
        const float ka = k * b.x;
        // 法線 / ヤコビアンは変位の重み vf と別に nf で薄める（面の傾きは画素の足跡で決める）
        const float kan = ka * nf;
        nx -= d.x * kan * cs;
        nz -= d.y * kan * cs;
        nyMinus += b.y * kan * sn;
        jxx -= b.y * kan * d.x * d.x * sn;
        jzz -= b.y * kan * d.y * d.y * sn;
        jxz -= b.y * kan * d.x * d.y * sn;
        const float lost = ka * (1.0 - nf);
        g.slopeVar += 0.5 * lost * lost;
    }
    g.normal = normalize(float3(nx, 1.0 - nyMinus, nz));
    g.jac = (1.0 + jxx) * (1.0 + jzz) - jxz * jxz;
    return g;
}

// 誘電体境界のフレネル反射率（偏光平均）。cosI = 入射側の cos、eta = n2/n1。全反射で 1。
float WaterFresnel(float cosI, float eta)
{
    cosI = saturate(cosI);
    const float sinT2 = (1.0 - cosI * cosI) / (eta * eta);
    if (sinT2 >= 1.0) return 1.0;
    const float cosT = sqrt(1.0 - sinT2);
    const float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
    const float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// スネル。I = 表面へ向かう単位ベクトル、N = 入射側を向く法線、eta = n1/n2。全反射なら ok=false（戻り値は 0）。
float3 WaterRefract(float3 I, float3 N, float eta, out bool ok)
{
    const float cosi = -dot(I, N);
    const float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
    ok = (k >= 0.0);
    if (!ok) return 0.0;
    return eta * I + (eta * cosi - sqrt(k)) * N;
}

#endif // WATER_MATH_HLSLI
