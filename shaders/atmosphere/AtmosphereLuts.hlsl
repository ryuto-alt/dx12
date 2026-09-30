// AtmosphereLuts.hlsl — 物理ベース大気(A1 / Hillaire 2020)の LUT を作る compute 群(cs_6_0)
//   TransmittanceCS   256x64   : 高度・天頂角 → 大気の端までの透過率(パラメータ変更時のみ)
//   MultiScatteringCS 32x32    : 高度・光源の天頂角 → 多重散乱の寄与(パラメータ変更時のみ)
//   SkyViewCS         192x108  : カメラ高度での全天の放射輝度(毎フレーム。光源の向き・高度が変わったときだけでもよい)
//   ApCS              32x32x32 : カメラ frustum の froxel ごとの in-scattering と透過率(毎フレーム)
//   SkyCubeCS         NxNx6    : SkyView から環境キューブ(IBL の元)へ(IBL 再ベイクのときだけ)
// LUT の値は「大気上端の光源照度 = 1」の正規化(AtmosphereCommon.hlsli)。
#include "AtmosphereCommon.hlsli"
#include "../ibl/IBLCommon.hlsli"
#include "AtmosphereSkyRadiance.hlsli"

RWTexture2D<float4>        gOutT    : register(u0);
RWTexture2D<float4>        gOutMs   : register(u1);
RWTexture2D<float4>        gOutSv   : register(u2);
RWTexture3D<float4>        gOutApL  : register(u3);
RWTexture3D<float4>        gOutApT  : register(u4);
RWTexture2DArray<float4>   gOutCube : register(u5);

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void TransmittanceCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= AT_T_W || id.y >= AT_T_H) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(AT_T_W, AT_T_H);
    float r, mu;
    AtUvToTransmittanceParams(uv, r, mu);

    float3 o = float3(0.0, r, 0.0);
    float3 d = float3(sqrt(saturate(1.0 - mu * mu)), mu, 0.0);
    float tTop = AtRaySphereNearest(o, d, gTopR);
    float3 od = 0;
    if (tTop > 0.0)
    {
        // 192 分割の中点則(40 分割 + 0.3 の位置だと地平線付近で最大 10% ずれた。LUT はパラメータ変更時にしか作らないので分割を増やしても安い)
        const float N = 192.0;
        float dt = tTop / N;
        [loop]
        for (float s = 0.0; s < N; s += 1.0)
        {
            float t = tTop * (s + 0.5) / N;
            float3 P = o + t * d;
            od += AtSampleMedium(length(P) - gBottomR).extinction * dt;
        }
    }
    gOutT[id.xy] = float4(exp(-od), 1.0);
}

// ---------------------------------------------------------------------------
groupshared float3 gsMsAs1[64];
groupshared float3 gsL[64];

[numthreads(1, 1, 64)]
void MultiScatteringCS(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    float2 uv = (float2(gid.xy) + 0.5) / float2(AT_MS_W, AT_MS_H);
    uv = float2(AtFromSubUvToUnit(uv.x, AT_MS_W), AtFromSubUvToUnit(uv.y, AT_MS_H));

    float cosSunZenith = uv.x * 2.0 - 1.0;
    float3 sunDir = float3(sqrt(saturate(1.0 - cosSunZenith * cosSunZenith)), cosSunZenith, 0.0);
    const float kPlanetOffset = 0.01;
    float viewR = gBottomR + saturate(uv.y + kPlanetOffset) * (gTopR - gBottomR - kPlanetOffset);
    float3 worldPos = float3(0.0, viewR, 0.0);

    // 球面を 8x8 の層別サンプルで一様に覆う 64 方向(1 スレッド = 1 方向)
    const float sqrtN = 8.0;
    float i = 0.5 + float(gtid.z / 8u);
    float j = 0.5 + float(gtid.z % 8u);
    float theta = 2.0 * AT_PI * i / sqrtN;
    float cosPhi = 1.0 - 2.0 * j / sqrtN;
    float sinPhi = sqrt(saturate(1.0 - cosPhi * cosPhi));
    float3 worldDir = float3(sinPhi * cos(theta), cosPhi, sinPhi * sin(theta));

    AtScatterResult r = AtIntegrate(worldPos, worldDir, sunDir, 20.0, true, false, false, 9.0e6, true, false, 0.0);

    const float sphereSolidAngle = 4.0 * AT_PI;
    gsMsAs1[gtid.z] = r.multiScatAs1 * sphereSolidAngle / (sqrtN * sqrtN);
    gsL[gtid.z]     = r.L * sphereSolidAngle / (sqrtN * sqrtN);
    GroupMemoryBarrierWithGroupSync();

    // 64 → 1 のリダクション(決定論: 固定順)
    [unroll]
    for (uint step = 32u; step > 0u; step >>= 1u)
    {
        if (gtid.z < step)
        {
            gsMsAs1[gtid.z] += gsMsAs1[gtid.z + step];
            gsL[gtid.z]     += gsL[gtid.z + step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gtid.z == 0u)
    {
        float3 msAs1 = gsMsAs1[0] * (1.0 / sphereSolidAngle);     // 論文の式 7: f_ms(球面の平均 = 等方位相をかけて 4π で割る)
        float3 inScat = gsL[0] * (1.0 / sphereSolidAngle);        // 論文の式 5: L_2ndorder(等方位相)
        float3 sumMs = 1.0 / (1.0 - msAs1);                       // 多重散乱の無限和(等比級数)
        gOutMs[gid.xy] = float4(inScat * sumMs, 1.0);
    }
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void SkyViewCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= AT_SV_W || id.y >= AT_SV_H) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(AT_SV_W, AT_SV_H);
    float viewZenithCos, lightViewCos;
    AtUvToSkyViewParams(gCamAlt, uv, viewZenithCos, lightViewCos);

    float sunZenithCos = gLightDir.y;
    float3 sunDir = float3(sqrt(saturate(1.0 - sunZenithCos * sunZenithCos)), sunZenithCos, 0.0);
    float3 worldPos = float3(0.0, gBottomR + gCamAlt, 0.0);
    float viewZenithSin = sqrt(saturate(1.0 - viewZenithCos * viewZenithCos));
    float3 worldDir = float3(viewZenithSin * lightViewCos, viewZenithCos,
                             viewZenithSin * sqrt(saturate(1.0 - lightViewCos * lightViewCos)));

    AtScatterResult r = AtIntegrate(worldPos, worldDir, sunDir, 64.0, true, true, true, 9.0e6, false, false, 0.0);
    gOutSv[id.xy] = float4(r.L, 1.0);
}

// ---------------------------------------------------------------------------
// AP: 1 スレッド = 1 本のレイ(画素方向)。手前から奥へ 32 スライスを順に積分し、各スライスの距離までの累積値を書く。
[numthreads(8, 8, 1)]
void ApCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= AT_AP_W || id.y >= AT_AP_H) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(AT_AP_W, AT_AP_H);
    float3 dir = AtPixelDir(uv, gInvViewProj);
    float3 pos0 = float3(0.0, gBottomR + gCamAlt, 0.0);

    float tTop = AtRaySphereNearest(pos0, dir, gTopR);
    if (tTop < 0.0) tTop = 0.0;

    float cosTheta = dot(gLightDir, dir);
    float miePhase = AtHgPhase(gMieG, cosTheta);
    float rayPhase = AtRayleighPhase(cosTheta);

    float3 L = 0, T = 1;
    float tPrev = 0.0;
    [loop]
    for (uint sl = 0u; sl < AT_AP_D; ++sl)
    {
        float fk = float(sl + 1u) / float(AT_AP_D);
        float tEnd = min(gApMaxKm * fk * fk, tTop);
        float seg = max(tEnd - tPrev, 0.0);
        if (seg > 0.0)
        {
            const uint steps = 4u;
            float dt = seg / float(steps);
            [unroll]
            for (uint k = 0u; k < steps; ++k)
            {
                float t = tPrev + (float(k) + 0.5) * dt;
                float3 P = pos0 + t * dir;
                float pR = length(P);
                AtMedium med = AtSampleMedium(pR - gBottomR);
                float3 sampleT = exp(-med.extinction * dt);
                float3 up = P / pR;
                float sunZenithCos = dot(gLightDir, up);
                float3 tSun = AtTransmittance(max(pR, gBottomR + 1e-4), sunZenithCos) * AtPlanetShadow(pR, sunZenithCos);
                float3 S = tSun * (med.scatteringMie * miePhase + med.scatteringRay * rayPhase);
                if (gMsFactor > 0.0)
                    S += AtGetMultipleScattering(max(pR, gBottomR), sunZenithCos) * gMsFactor * med.scattering;
                float3 Sint = (S - S * sampleT) / max(med.extinction, 1e-9);
                L += T * Sint;
                T *= sampleT;
            }
            tPrev = tEnd;
        }
        gOutApL[uint3(id.xy, sl)] = float4(L, 1.0);
        gOutApT[uint3(id.xy, sl)] = float4(T, 1.0);
    }
}

// ---------------------------------------------------------------------------
// 環境キューブ(IBL の元)。SkyView から引く。太陽円盤は含めない(直接光は平行光の担当)。夜空の下限を足す。
// mip 0 = 各テクセルの中心で 1 回、mip 1.. = 各テクセルを 2x2 でスーパーサンプルして直接評価する(mip 間の依存が無いので 1 リソースの別 mip を独立に書ける)。
// mip 付きにするのは、IBL の拡散(AtmosphereIrradiance.hlsl)とスペキュラの prefilter が mip を引いてサンプル数を減らす/ちらつきを抑えるため。
RWTexture2DArray<float4> gOutCubeMip : register(u6);   // mip 1.. の書き込み先(1 ディスパッチごとに 1 つの mip の UAV をバインドする)

cbuffer SkyCubeCB : register(b1)
{
    uint gCubeSize;    // この mip の面サイズ
    uint gCubeSS;      // 1 辺あたりのスーパーサンプル数(1 or 2)
    uint gCubePad1; uint gCubePad2;
};

float3 SkyCubeTexel(uint3 id)
{
    float3 sum = 0;
    const float n = float(gCubeSS);
    for (uint j = 0u; j < gCubeSS; ++j)
        for (uint i = 0u; i < gCubeSS; ++i)
        {
            float2 sub = (float2(i, j) + 0.5) / n;
            float2 uv = (float2(id.xy) + sub) / float(gCubeSize);
            sum += AtEnvRadiance(DirectionFromFaceUV(id.z, uv));
        }
    return min(sum / (n * n), 6.0e4);
}

[numthreads(8, 8, 1)]
void SkyCubeCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gCubeSize || id.y >= gCubeSize) return;
    gOutCube[id] = float4(SkyCubeTexel(id), 1.0);
}

[numthreads(8, 8, 1)]
void SkyCubeMipCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gCubeSize || id.y >= gCubeSize) return;
    gOutCubeMip[id] = float4(SkyCubeTexel(id), 1.0);
}
