// AtmosphereCommon.hlsl — 物理ベース大気(A1 / Hillaire 2020)の共通部: 定数・媒質・LUT のパラメータ化・散乱積分
//
// ★C++ 側 src/renderer/atmosphere/AtmosphereShared.h の AtmosphereGpuParams とバイト単位で一致させること(static_assert が番人)。
// ★媒質モデル・位相関数・積分は CPU 参照 src/renderer/atmosphere/AtmosphereMath.h と同じ式(片方を直したら両方直す)。
//
// 座標: 惑星の中心を原点、y を上とする「大気空間」[km]。カメラは (0, R + h, 0)。ワールドの方向ベクトルはそのまま使える
//   (ワールドの +Y = 局所の上。惑星が球なので水平位置は関係しない)。
// LUT の値は「大気上端の太陽照度 = 1(rgb 白)」で正規化した放射輝度。使う側が gLightE(照度 RGB)を掛ける。
//   → LUT は太陽の色・強度・時刻の照度が変わっても作り直さなくてよい(向きとカメラ高度が変わったときだけ SkyView/AP を作り直す)。

#ifndef ATMOSPHERE_COMMON_HLSLI
#define ATMOSPHERE_COMMON_HLSLI

#define AT_PI 3.14159265358979323846

// LUT の解像度(C++ の AtmosphereRenderer と一致させること)
#define AT_T_W   256
#define AT_T_H   64
#define AT_MS_W  32
#define AT_MS_H  32
#define AT_SV_W  384
#define AT_SV_H  128
#define AT_AP_W  32
#define AT_AP_H  32
#define AT_AP_D  32

#define AT_FLAG_STARS    1u
#define AT_FLAG_MOON     2u
#define AT_FLAG_AP       4u
#define AT_FLAG_LIGHT_IS_MOON 8u

// ---------------------------------------------------------------------------
//  パラメータと LUT の置き場(2 通り)
//   ・通常(AT_BINDLESS 未定義): cbuffer AtmosphereCB(b0)+ t0..t4 / s0(AtmosphereRenderer の専用ルートシグネチャ)。
//   ・AT_BINDLESS(パストレーサーなど別のルートシグネチャから使う): 呼び出し側が `uint gAtSrvBase;`(AtmosphereRenderer::SrvBlockBase)と
//     `#define AT_SAMPLER <LINEAR CLAMP のサンプラ>` を先に定義しておく。パラメータは SRV ブロックの [6](ByteAddressBuffer)から
//     AtLoadParams() で static 変数へ読む(カーネルの先頭で 1 回呼ぶ)。LUT は ResourceDescriptorHeap[gAtSrvBase + 0..4]。
// ---------------------------------------------------------------------------
#ifdef AT_BINDLESS
static float3 gRayScat;      static float gRayExp;
static float3 gMieScat;      static float gMieExp;
static float3 gMieExt;       static float gMieG;
static float3 gOzoneExt;     static float gOzoneCenter;
static float3 gGroundAlbedo; static float gOzoneHalf;
static float  gBottomR;      static float gTopR;       static float gCamAlt;    static float gMsFactor;
static float3 gLightDir;     static float gSunAngRad;
static float3 gLightE;       static float gSkyScale;
static float3 gSunDir;       static float gApMaxKm;
static float3 gSunTopE;      static float gApStartKm;
static float3 gMoonTopE;     static float gMoonAngRad;
static float3 gPoleAxis;     static float gStarAngle;
static float3 gNightSky;     static float gApStrength;
static float3 gCamPos;       static uint  gFlags;
static float3 gMoonDir;      static float gPad0;
static float4x4 gInvViewProj;
static float4x4 gInvViewProjJ;

void AtLoadParams()
{
    ByteAddressBuffer b = ResourceDescriptorHeap[gAtSrvBase + 6];
    float4 r0 = asfloat(b.Load4(0));   float4 r1 = asfloat(b.Load4(16));  float4 r2 = asfloat(b.Load4(32));
    float4 r3 = asfloat(b.Load4(48));  float4 r4 = asfloat(b.Load4(64));  float4 r5 = asfloat(b.Load4(80));
    float4 r6 = asfloat(b.Load4(96));  float4 r7 = asfloat(b.Load4(112)); float4 r8 = asfloat(b.Load4(128));
    float4 r9 = asfloat(b.Load4(144)); float4 r10 = asfloat(b.Load4(160)); float4 r11 = asfloat(b.Load4(176));
    float4 r12 = asfloat(b.Load4(192)); float4 r13 = asfloat(b.Load4(208)); float4 r14 = asfloat(b.Load4(224));
    gRayScat = r0.xyz; gRayExp = r0.w;   gMieScat = r1.xyz; gMieExp = r1.w;   gMieExt = r2.xyz; gMieG = r2.w;
    gOzoneExt = r3.xyz; gOzoneCenter = r3.w;   gGroundAlbedo = r4.xyz; gOzoneHalf = r4.w;
    gBottomR = r5.x; gTopR = r5.y; gCamAlt = r5.z; gMsFactor = r5.w;
    gLightDir = r6.xyz; gSunAngRad = r6.w;   gLightE = r7.xyz; gSkyScale = r7.w;
    gSunDir = r8.xyz; gApMaxKm = r8.w;   gSunTopE = r9.xyz; gApStartKm = r9.w;
    gMoonTopE = r10.xyz; gMoonAngRad = r10.w;   gPoleAxis = r11.xyz; gStarAngle = r11.w;
    gNightSky = r12.xyz; gApStrength = r12.w;   gCamPos = r13.xyz; gFlags = asuint(r13.w);
    gMoonDir = r14.xyz; gPad0 = r14.w;
    gInvViewProj = float4x4(asfloat(b.Load4(240)), asfloat(b.Load4(256)), asfloat(b.Load4(272)), asfloat(b.Load4(288)));
    gInvViewProjJ = float4x4(asfloat(b.Load4(304)), asfloat(b.Load4(320)), asfloat(b.Load4(336)), asfloat(b.Load4(352)));
}
float4 AtLoadTrans(float2 uv)    { Texture2D<float4> t = ResourceDescriptorHeap[gAtSrvBase + 0]; return t.SampleLevel(AT_SAMPLER, uv, 0); }
float4 AtLoadMs(float2 uv)       { Texture2D<float4> t = ResourceDescriptorHeap[gAtSrvBase + 1]; return t.SampleLevel(AT_SAMPLER, uv, 0); }
float4 AtLoadSkyView(float2 uv)  { Texture2D<float4> t = ResourceDescriptorHeap[gAtSrvBase + 2]; return t.SampleLevel(AT_SAMPLER, uv, 0); }
float4 AtLoadApL(float3 uvw)     { Texture3D<float4> t = ResourceDescriptorHeap[gAtSrvBase + 3]; return t.SampleLevel(AT_SAMPLER, uvw, 0); }
float4 AtLoadApT(float3 uvw)     { Texture3D<float4> t = ResourceDescriptorHeap[gAtSrvBase + 4]; return t.SampleLevel(AT_SAMPLER, uvw, 0); }
#else
cbuffer AtmosphereCB : register(b0)
{
    float3 gRayScat;      float gRayExp;
    float3 gMieScat;      float gMieExp;
    float3 gMieExt;       float gMieG;
    float3 gOzoneExt;     float gOzoneCenter;
    float3 gGroundAlbedo; float gOzoneHalf;
    float  gBottomR;      float gTopR;       float gCamAlt;    float gMsFactor;
    float3 gLightDir;     float gSunAngRad;      // LUT に使う光源(太陽 or 月)へ向かう向き / 太陽の視半径
    float3 gLightE;       float gSkyScale;       // LUT に使う光源の上端照度 RGB(シーン単位)/ スカイパスの倍率
    float3 gSunDir;       float gApMaxKm;        // 実際の太陽へ向かう向き
    float3 gSunTopE;      float gApStartKm;      // 太陽円盤の上端照度 RGB(月への切替で絞られる)
    float3 gMoonTopE;     float gMoonAngRad;     // 月円盤の上端照度 RGB
    float3 gPoleAxis;     float gStarAngle;      // 天の北極の向き(ワールド)/ 星空の回転角
    float3 gNightSky;     float gApStrength;     // 夜空の下限輝度 RGB(シーン単位)/ AP の濃さ
    float3 gCamPos;       uint  gFlags;          // カメラのワールド位置[m]
    float3 gMoonDir;      float gPad0;           // 月へ向かう向き(ワールド)
    float4x4 gInvViewProj;    // ジッタなし・行ベクトル mul 用に転置済み(AP LUT のレイ方向)
    float4x4 gInvViewProjJ;   // ジッタあり(スカイ / AP 合成の画素レイ・深度復元)
};

Texture2D<float4>   gTransT  : register(t0);   // Transmittance LUT
Texture2D<float4>   gMsLut   : register(t1);   // Multi-scattering LUT
Texture2D<float4>   gSkyView : register(t2);   // Sky-View LUT
Texture3D<float4>   gApL     : register(t3);   // AP: rgb = in-scattering(照度 1 あたり)
Texture3D<float4>   gApT     : register(t4);   // AP: rgb = transmittance
SamplerState        gLinClamp : register(s0);

float4 AtLoadTrans(float2 uv)    { return gTransT.SampleLevel(gLinClamp, uv, 0); }
float4 AtLoadMs(float2 uv)       { return gMsLut.SampleLevel(gLinClamp, uv, 0); }
float4 AtLoadSkyView(float2 uv)  { return gSkyView.SampleLevel(gLinClamp, uv, 0); }
float4 AtLoadApL(float3 uvw)     { return gApL.SampleLevel(gLinClamp, uvw, 0); }
float4 AtLoadApT(float3 uvw)     { return gApT.SampleLevel(gLinClamp, uvw, 0); }
#endif

// ---------------------------------------------------------------------------
//  基本
// ---------------------------------------------------------------------------
float AtSqr(float x) { return x * x; }

// 原点中心・半径 r の球と半直線(o, d)の最初の交点までの距離。無ければ -1。
float AtRaySphereNearest(float3 o, float3 d, float r)
{
    float b = dot(o, d);
    float c = dot(o, o) - r * r;
    if (c >= 0.0 && b >= 0.0) return -1.0;   // 表面ちょうど(c=0)で上向きを「地面に当たる」と数えない
    float disc = b * b - c;
    if (disc < 0.0) return -1.0;
    float s = sqrt(disc);
    float t0 = -b - s, t1 = -b + s;
    if (t0 >= 0.0) return t0;
    if (t1 >= 0.0) return t1;
    return -1.0;
}

float AtRayleighPhase(float cosTheta) { return 3.0 / (16.0 * AT_PI) * (1.0 + cosTheta * cosTheta); }
float AtHgPhase(float g, float cosTheta)
{
    float d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * AT_PI * d * sqrt(d));
}

// LUT の端の半テクセルを避ける uv 変換(端の値がにじまない)
float AtFromUnitToSubUv(float u, float res) { return (u + 0.5 / res) * (res / (res + 1.0)); }
float AtFromSubUvToUnit(float u, float res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

struct AtMedium
{
    float3 scattering;
    float3 scatteringRay;
    float3 scatteringMie;
    float3 extinction;
};

AtMedium AtSampleMedium(float altKm)
{
    float h = max(altKm, 0.0);
    float dMie = exp(gMieExp * h);
    float dRay = exp(gRayExp * h);
    float dOzo = (gOzoneHalf > 0.0) ? saturate(1.0 - abs(h - gOzoneCenter) / gOzoneHalf) : 0.0;
    AtMedium m;
    m.scatteringRay = dRay * gRayScat;
    m.scatteringMie = dMie * gMieScat;
    m.scattering    = m.scatteringRay + m.scatteringMie;
    m.extinction    = dRay * gRayScat + dMie * gMieExt + dOzo * gOzoneExt;
    return m;
}

// ---------------------------------------------------------------------------
//  Transmittance LUT のパラメータ化(Bruneton 2017 と同じ)
//    u = 天頂角の余弦(大気の端までの距離で線形化)、v = 高度(地平線までの距離 ρ で線形)
// ---------------------------------------------------------------------------
void AtUvToTransmittanceParams(float2 uv, out float r, out float mu)
{
    float xMu = uv.x, xR = uv.y;
    float H = sqrt(max(0.0, gTopR * gTopR - gBottomR * gBottomR));
    float rho = H * xR;
    r = sqrt(rho * rho + gBottomR * gBottomR);
    float dMin = gTopR - r;
    float dMax = rho + H;
    float d = dMin + xMu * (dMax - dMin);
    mu = (d == 0.0) ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}

float2 AtTransmittanceParamsToUv(float r, float mu)
{
    r = clamp(r, gBottomR, gTopR);
    float H = sqrt(max(0.0, gTopR * gTopR - gBottomR * gBottomR));
    float rho = sqrt(max(0.0, r * r - gBottomR * gBottomR));
    float disc = r * r * (mu * mu - 1.0) + gTopR * gTopR;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dMin = gTopR - r;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-6);
    float xR = rho / max(H, 1e-6);
    return float2(saturate(xMu), saturate(xR));
}

// 点(半径 r)から天頂角の余弦 mu の向きへ大気の外まで抜ける透過率。地面に当たる向きは 0 を返す(呼び出し側で扱う)。
float3 AtTransmittance(float r, float mu)
{
    float2 uv = AtTransmittanceParamsToUv(r, mu);
    return AtLoadTrans(uv).rgb;
}

// 惑星の影による太陽の見え(0 = 完全に隠れる〜1 = 全部見える)。太陽の視半径ぶんだけ半影を持たせる(二値だとサンプルごとに明暗が跳んで帯になる)。
//   点の半径 r・光源方向の天頂角の余弦 mu で、太陽の中心が地平線(cos = -√(1-(R/r)²))に掛かる位置が境目。
float AtPlanetShadow(float r, float mu)
{
    float muH = -sqrt(max(0.0, 1.0 - AtSqr(gBottomR / max(r, gBottomR))));
    // 半影の幅: 太陽の視半径 α の数倍(見た目の滑らかさを優先。物理の半影は ±α)
    float w = max(gSunAngRad, 1e-4) * 3.0;
    return smoothstep(muH - w, muH + w, mu);
}

// 太陽(光源)方向の透過率。地面に隠れる(惑星の影)なら 0。
float3 AtTransmittanceToLight(float3 pos, float3 lightDir)
{
    float r = length(pos);
    float3 up = pos / r;
    float mu = dot(up, lightDir);
    float vis = AtPlanetShadow(r, mu);
    if (vis <= 0.0) return float3(0, 0, 0);
    return AtTransmittance(r, max(mu, -0.9999)) * vis;
}

// ---------------------------------------------------------------------------
//  Multi-scattering LUT の参照(u = 光源の天頂角の余弦、v = 高度)
// ---------------------------------------------------------------------------
float3 AtGetMultipleScattering(float r, float sunZenithCos)
{
    float2 uv = saturate(float2(sunZenithCos * 0.5 + 0.5, (r - gBottomR) / (gTopR - gBottomR)));
    uv = float2(AtFromUnitToSubUv(uv.x, AT_MS_W), AtFromUnitToSubUv(uv.y, AT_MS_H));
    return AtLoadMs(uv).rgb;
}

// ---------------------------------------------------------------------------
//  散乱の積分(視線に沿ったレイマーチ)
//   ・光源の照度は 1(LUT の正規化)。ground = true なら視線が地面に当たるとき地表の拡散反射を足す。
//   ・msMode: 0 = 通常(多重散乱 LUT を引く)/ 1 = 多重散乱 LUT の構築(等方位相・多重散乱項なし・MultiScatAs1 を集計)
// ---------------------------------------------------------------------------
struct AtScatterResult
{
    float3 L;
    float3 transmittance;
    float3 multiScatAs1;
};

AtScatterResult AtIntegrate(float3 worldPos, float3 worldDir, float3 lightDir, float sampleCountIni,
                            bool ground, bool variableSamples, bool mieRayPhase, float tMaxMax, bool msBuild,
                            bool flatAP, float tOverride)
{
    AtScatterResult res;
    res.L = 0; res.transmittance = 1; res.multiScatAs1 = 0;

    float tBottom = AtRaySphereNearest(worldPos, worldDir, gBottomR);
    float tTop = AtRaySphereNearest(worldPos, worldDir, gTopR);
    float tMax = 0.0;
    if (flatAP)
    {
        // エアリアルパースペクティブ: 地面(球)では止めず、指定の距離まで(地面より下は高度 0 として扱う)
        tMax = tOverride;
        if (tTop >= 0.0) tMax = min(tMax, tTop);
    }
    else
    {
        if (tBottom < 0.0) { if (tTop < 0.0) return res; tMax = tTop; }
        else { tMax = (tTop > 0.0) ? min(tTop, tBottom) : tBottom; }
    }
    tMax = min(tMax, tMaxMax);
    if (tMax <= 0.0) return res;

    float sampleCount = sampleCountIni;
    float sampleCountFloor = sampleCountIni;
    float tMaxFloor = tMax;
    if (variableSamples)
    {
        // 距離に応じてサンプル数を変える(参照実装と同じ: 0..100 km で ini..ini*2)
        float tKm = min(tMax, 100.0);
        sampleCount = lerp(sampleCountIni * 0.5, sampleCountIni, saturate(tKm * 0.01));
        sampleCountFloor = floor(sampleCount);
        tMaxFloor = tMax * sampleCountFloor / sampleCount;
    }
    float dt = tMax / sampleCount;

    float cosTheta = dot(lightDir, worldDir);
    float miePhase = mieRayPhase ? AtHgPhase(gMieG, cosTheta) : 1.0 / (4.0 * AT_PI);
    float rayPhase = mieRayPhase ? AtRayleighPhase(cosTheta) : 1.0 / (4.0 * AT_PI);

    float3 L = 0, throughput = 1, opticalDepth = 0;
    float3 msAs1 = 0;
    float t = 0.0;
    const float segT = 0.3;
    [loop]
    for (float s = 0.0; s < sampleCount; s += 1.0)
    {
        float dtSeg;
        if (variableSamples)
        {
            float t0 = s / sampleCountFloor;
            float t1 = (s + 1.0) / sampleCountFloor;
            t0 = t0 * t0; t1 = t1 * t1;
            t0 = tMaxFloor * t0;
            t1 = (t1 > 1.0) ? tMax : tMaxFloor * t1;
            t = t0 + (t1 - t0) * segT;
            dtSeg = t1 - t0;
        }
        else
        {
            t = tMax * (s + segT) / sampleCount;
            dtSeg = dt;
        }

        float3 P = worldPos + t * worldDir;
        float pR = length(P);
        float alt = pR - gBottomR;
        AtMedium med = AtSampleMedium(alt);

        float3 sampleOD = med.extinction * dtSeg;
        float3 sampleT = exp(-sampleOD);
        opticalDepth += sampleOD;

        float3 up = P / pR;
        float sunZenithCos = dot(lightDir, up);
        float3 tSun;
        if (flatAP)
        {
            // 球の影は使わない(高度 0 の面より下も「空の中」として扱う)。太陽が地平線の下でも透過率 LUT が 0 へ落ちる。
            tSun = AtTransmittance(max(pR, gBottomR + 1e-4), sunZenithCos) * AtPlanetShadow(pR, sunZenithCos);
        }
        else
        {
            tSun = AtTransmittanceToLight(P, lightDir);
        }

        float3 phaseTimesScattering = med.scatteringMie * miePhase + med.scatteringRay * rayPhase;

        float3 msLum = 0;
        if (!msBuild && gMsFactor > 0.0)
            msLum = AtGetMultipleScattering(max(pR, gBottomR), sunZenithCos) * gMsFactor;

        float3 S = tSun * phaseTimesScattering + msLum * med.scattering;

        if (msBuild)
        {
            float3 MS = med.scattering;
            float3 MSint = (MS - MS * sampleT) / max(med.extinction, 1e-9);
            msAs1 += throughput * MSint;
        }

        float3 Sint = (S - S * sampleT) / max(med.extinction, 1e-9);
        L += throughput * Sint;
        throughput *= sampleT;
    }

    if (ground && !flatAP && tMax == tBottom && tBottom > 0.0)
    {
        // 地表の拡散反射(ランバート)
        float3 P = worldPos + tBottom * worldDir;
        float pR = length(P);
        float3 up = P / pR;
        float ndl = saturate(dot(up, lightDir));
        float3 tSun = AtTransmittanceToLight(P, lightDir);
        L += throughput * tSun * ndl * gGroundAlbedo / AT_PI;
    }

    res.L = L;
    res.transmittance = throughput;
    res.multiScatAs1 = msAs1;
    return res;
}

// ---------------------------------------------------------------------------
//  Sky-View LUT のパラメータ化(地平線付近を細かく)
//    v(縦)= 視線の天頂角(地平線を v=0.5、地平線の近くほど細かい)、u(横)= 光源と視線の方位角の余弦
//    高度 h[km] から地平線の天頂角 π - β を出す(cosβ = √(h(2R+h)) / (R+h)。h の桁落ちを避けるため h から直接計算)
// ---------------------------------------------------------------------------
void AtSkyViewHorizon(float h, out float beta, out float zenithHorizonAngle)
{
    float vh2 = h * (2.0 * gBottomR + h);
    float viewR = gBottomR + h;
    float cosBeta = sqrt(max(vh2, 0.0)) / viewR;
    beta = acos(clamp(cosBeta, 0.0, 1.0));
    zenithHorizonAngle = AT_PI - beta;
}

void AtUvToSkyViewParams(float h, float2 uv, out float viewZenithCos, out float lightViewCos)
{
    uv = float2(AtFromSubUvToUnit(uv.x, AT_SV_W), AtFromSubUvToUnit(uv.y, AT_SV_H));
    float beta, zh;
    AtSkyViewHorizon(h, beta, zh);
    if (uv.y < 0.5)
    {
        float coord = 2.0 * uv.y;
        coord = 1.0 - coord;
        coord *= coord;
        coord = 1.0 - coord;
        viewZenithCos = cos(zh * coord);
    }
    else
    {
        float coord = uv.y * 2.0 - 1.0;
        coord *= coord;
        viewZenithCos = cos(zh + beta * coord);
    }
    float coordX = uv.x;
    coordX *= coordX;
    lightViewCos = -(coordX * 2.0 - 1.0);
}

float2 AtSkyViewParamsToUv(bool intersectGround, float viewZenithCos, float lightViewCos, float h)
{
    float beta, zh;
    AtSkyViewHorizon(h, beta, zh);
    float viewZenithAngle = acos(clamp(viewZenithCos, -1.0, 1.0));
    float2 uv;
    if (!intersectGround)
    {
        float coord = viewZenithAngle / max(zh, 1e-5);
        coord = 1.0 - coord;
        coord = sqrt(max(coord, 0.0));
        coord = 1.0 - coord;
        uv.y = coord * 0.5;
    }
    else
    {
        float coord = (viewZenithAngle - zh) / max(beta, 1e-5);
        coord = sqrt(max(coord, 0.0));
        uv.y = coord * 0.5 + 0.5;
    }
    float coordX = -lightViewCos * 0.5 + 0.5;
    coordX = sqrt(saturate(coordX));
    uv.x = coordX;
    return float2(AtFromUnitToSubUv(uv.x, AT_SV_W), AtFromUnitToSubUv(uv.y, AT_SV_H));
}

// ワールド方向 dir(単位)の空の放射輝度(照度 1 あたり)を Sky-View LUT から引く。カメラは (0, R + gCamAlt, 0)。
float3 AtSampleSkyView(float3 dir)
{
    float h = gCamAlt;
    float3 up = float3(0, 1, 0);
    float viewZenithCos = dot(dir, up);
    float3 side = cross(up, dir);
    float sideLen = length(side);
    float3 forward = (sideLen > 1e-6) ? cross(side / sideLen, up) : float3(1, 0, 0);
    float2 lightOnPlane = float2(dot(gLightDir, forward), dot(gLightDir, side / max(sideLen, 1e-6)));
    float lpl = length(lightOnPlane);
    lightOnPlane = (lpl > 1e-6) ? lightOnPlane / lpl : float2(1, 0);
    float lightViewCos = lightOnPlane.x;

    float3 pos = float3(0, gBottomR + h, 0);
    bool intersectGround = AtRaySphereNearest(pos, dir, gBottomR) >= 0.0;
    float2 uv = AtSkyViewParamsToUv(intersectGround, viewZenithCos, lightViewCos, h);
    return AtLoadSkyView(uv).rgb;
}

// ---------------------------------------------------------------------------
//  Aerial-Perspective(froxel)LUT の参照
//    slice k(0..31)の距離 = gApMaxKm · ((k+1)/32)²。距離 d[km] のときの (in-scattering, transmittance)。
//    d が最初のスライスより手前では原点(0, 1)との線形補間。
// ---------------------------------------------------------------------------
void AtSampleAp(float2 uv, float dKm, out float3 inscatter, out float3 trans)
{
    float sIdx = AT_AP_D * sqrt(saturate(dKm / max(gApMaxKm, 1e-4)));
    float w = clamp((sIdx - 0.5) / AT_AP_D, 0.5 / AT_AP_D, 1.0 - 0.5 / AT_AP_D);
    float3 L = AtLoadApL(float3(uv, w)).rgb;
    float3 T = AtLoadApT(float3(uv, w)).rgb;
    if (sIdx < 1.0)
    {
        L *= sIdx;
        T = lerp(float3(1, 1, 1), T, sIdx);
    }
    inscatter = L;
    trans = T;
}

// カメラ → 画素 uv(0..1)のワールド方向
float3 AtPixelDir(float2 uv, float4x4 invVP)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 farW  = mul(float4(ndc, 1.0, 1.0), invVP);
    float4 nearW = mul(float4(ndc, 0.0, 1.0), invVP);
    farW /= farW.w;
    nearW /= nearW.w;
    return normalize(farW.xyz - nearW.xyz);
}

#endif // ATMOSPHERE_COMMON_HLSLI
