// AtmosphereSkyRadiance.hlsli — 空の 1 方向の放射輝度(Sky-View LUT + 太陽円盤 + 月 + 星 + 夜空の下限)。
// 空パス(AtmosphereSky.hlsl)とパストレーサー(shaders/pt/。AT_BINDLESS 版の AtmosphereCommon.hlsli)が同じ式を使う。
// ★先に AtmosphereCommon.hlsli をインクルードしておくこと。
#ifndef ATMOSPHERE_SKY_RADIANCE_HLSLI
#define ATMOSPHERE_SKY_RADIANCE_HLSLI

// ---- 星: 方向を格子に切ってハッシュで 1 セル 1 個の点を置く(外部アセット無し)----
float3 AtHash33(float3 p)
{
    uint3 q = uint3(int3(p)) * uint3(1597334673u, 3812015801u, 2798796415u);
    q = (q.x ^ q.y ^ q.z) * uint3(1597334673u, 3812015801u, 2798796415u);
    return float3(q) * (1.0 / float(0xffffffffu));
}

float3 AtStars(float3 dir)
{
    // 天の北極まわりに回す(時刻で星空が回る)
    float3 ax = normalize(gPoleAxis);
    float c = cos(gStarAngle), s = sin(gStarAngle);
    float3 d = dir * c + cross(ax, dir) * s + ax * dot(ax, dir) * (1.0 - c);
    const float kCells = 110.0;
    float3 q = d * kCells;
    float3 cell = floor(q);
    float3 h = AtHash33(cell + 17.0);
    float3 h2 = AtHash33(cell + 91.0);
    float3 center = cell + 0.2 + 0.6 * h;
    float dist = length(q - center);
    float mag = h2.x;                       // 明るさの分布(ほとんどが暗い)
    float bright = pow(mag, 6.0);
    float present = step(0.86, h2.y);       // 約 14% のセルにだけ星がある
    float radius = 0.16 + 0.10 * bright;
    float spot = saturate(1.0 - dist / radius);
    spot *= spot;
    float3 tint = lerp(float3(1.0, 0.82, 0.68), float3(0.72, 0.84, 1.0), h.z);   // 色温度のばらつき
    return tint * (present * spot * (4.0 + 60.0 * bright));    // [nit]
}

// 環境(IBL / パストレーサーのライティング)としての空の放射輝度: 太陽円盤・月・星を含まない(直接光は平行光の担当)。gSkyScale は掛けない。
float3 AtEnvRadiance(float3 dir)
{
    return AtSampleSkyView(dir) * gLightE + gNightSky;
}

// 背景として見える空(スカイパス)。gSkyScale(= skyLuminanceScale × skyboxIntensity)を掛ける。
float3 SkyRadiance(float3 dir)
{
    float3 cam = float3(0.0, gBottomR + gCamAlt, 0.0);
    float3 sky = AtSampleSkyView(dir) * gLightE * gSkyScale + gNightSky;

    bool hitsGround = AtRaySphereNearest(cam, dir, gBottomR) >= 0.0;
    if (!hitsGround)
    {
        float camR = length(cam);
        // ---- 太陽円盤 ----
        float cosSun = cos(gSunAngRad);
        float cosT = dot(dir, gSunDir);
        if (cosT > cosSun && any(gSunTopE > 0.0))
        {
            float3 tSun = AtTransmittance(camR, gSunDir.y);
            float omega = 2.0 * AT_PI * (1.0 - cosSun);
            // 縁を滑らかに(視半径の外側 6% で 0 へ)。周縁減光を弱く付ける。
            float x = acos(clamp(cosT, -1.0, 1.0)) / gSunAngRad;     // 0(中心)〜1(縁)
            float edge = saturate((1.0 - x) / 0.06);
            float limb = 1.0 - 0.45 * (1.0 - sqrt(saturate(1.0 - x * x)));
            sky += gSunTopE * tSun * (edge * limb / omega);
        }
        // ---- 月(円盤 + 縁の柔らかさ。位相は無い)----
        if ((gFlags & AT_FLAG_MOON) != 0u && any(gMoonTopE > 0.0))
        {
            float cosMoonR = cos(gMoonAngRad);
            float cosM = dot(dir, gMoonDir);
            if (cosM > cosMoonR)
            {
                float3 tMoon = AtTransmittance(camR, gMoonDir.y);
                float omega = 2.0 * AT_PI * (1.0 - cosMoonR);
                float x = acos(clamp(cosM, -1.0, 1.0)) / gMoonAngRad;
                float edge = saturate((1.0 - x) / 0.08);
                // 月面の模様(海)を低周波の陰影で。太陽側の位相は付けない(満月固定)。
                float3 rel = dir - gMoonDir;
                float pat = 0.82 + 0.18 * sin(rel.x * 900.0 + 1.3) * sin(rel.z * 700.0 + 0.4);
                sky += gMoonTopE * tMoon * (edge * pat * 0.55 / omega);
            }
        }
        // ---- 星(夜だけ。太陽の照度が落ちるほど見える。地平線に近いほど大気で減光)----
        if ((gFlags & AT_FLAG_STARS) != 0u)
        {
            float3 tUp = AtTransmittance(camR, max(dir.y, 0.02));
            // 昼夜の判定は太陽の高度で決める(物理単位でも従来単位でも同じに効く)。地平線の -6°〜+3° あたりで星が消える。
            float dayW = saturate((gSunDir.y + 0.10) / 0.16);
            float sunLevel = 1.0 - dayW;
            sky += AtStars(dir) * tUp * sunLevel;
        }
    }
    return sky;
}

#endif // ATMOSPHERE_SKY_RADIANCE_HLSLI
