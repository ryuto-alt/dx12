// CloudMarch.hlsl — ボリュメトリッククラウド(Schneider 2015 / Hillaire 2016 系のレイマーチ。自前実装)
//
//   TraceCS     : 1/4 解像度の粗いグリッド(4x4 ブロックにつき 1 画素 × 毎フレーム別の画素 = 16 フレームで全画素を 1 周)を
//                 レイマーチする。結果 = 「既存の色に足す放射輝度 rgb」と「既存の色に掛ける透過率 a」(AP 込み)+ 雲までの平均距離。
//   ResolveCS   : フル解像度へ時間再投影(今フレームの画素 = 新しい値、ほかは前フレームの履歴を再投影 + 近傍クランプ)。
//   VSFull/PSComposite : sceneColor への合成。dst = src.rgb + dst * src.a(深度で「雲より手前の不透明物」は雲を掛けない)。
//
// 座標: 惑星中心原点・y 上の「大気空間」[km](AtmosphereCommon.hlsli と同じ)。カメラは (0, R + camAlt, 0)。
// 雲層は球殻 [R + base, R + base + thickness]。水平位置はワールドの x,z(= gCamPos + 大気空間の xz)でノイズを引くので、
// カメラが動いても雲は付いてこない。
// 光: 太陽(または月)へ数ステップの光線マーチ + Beer-Powder + 2 ローブ HG(+ 銀の縁)+ 多重散乱の近似(Wrenninge の octave 法)+ 空の環境光。
//     光の色は透過率 LUT(AtTransmittanceToLight)で決まるので、夕方は雲が赤くなる。単位は大気の gLightE(物理単位 / 従来単位の両方で破綻しない)。
#include "AtmosphereCommon.hlsli"

cbuffer CloudCB : register(b1)
{
    uint  gResX;  uint gResY;  uint gCoarseX;  uint gCoarseY;
    float4x4 gPrevViewProj;                       // 前フレームの viewProj(ジッタなし・mul(v, M) 用に転置済み)
    float3 gWindOffsetKm;  float gCoverage;       // 風で流れた距離(x,z は水平、y は使わない)
    float  gCloudType;  float gDensity;  float gBaseKm;  float gThickKm;
    float  gDetail;  float gScale;  float gMaxDistKm;  float gHistBlend;
    uint   gSteps;  uint gFrame;  uint gCFlags;  uint gCPad0;
};
#define CF_RESET 1u   // 履歴を使わない
#define CF_BURST 2u   // 16 サブフレームの一括生成中(その回の画素は新しい値をそのまま採用)

Texture3D<float4>  gShape   : register(t5);
Texture3D<float4>  gDetailT : register(t6);
Texture2D<float4>  gWeather : register(t7);
Texture2D<float4>  gTraceC  : register(t8);
Texture2D<float>   gTraceD  : register(t9);
Texture2D<float4>  gHistC   : register(t10);
Texture2D<float>   gHistD   : register(t11);
Texture2D<float>   gDepth   : register(t12);
SamplerState       gLinWrap : register(s1);

RWTexture2D<float4> gOutTraceC : register(u0);
RWTexture2D<float>  gOutTraceD : register(u1);
RWTexture2D<float4> gOutResC   : register(u2);
RWTexture2D<float>  gOutResD   : register(u3);

static const float kExtinction = 24.0;       // 密度 1・gDensity 1 のときの消散係数 [1/km](積雲の芯で 30〜100 /km)
static const float kShapeTileKm   = 12.0;    // 形状ノイズ 128^3 の 1 周期[km](× gScale)
static const float kDetailTileKm  = 1.6;     // 詳細ノイズ 32^3 の 1 周期[km]
static const float kWeatherTileKm = 90.0;    // 天気マップ 256^2 の 1 周期[km]

float Remap(float v, float lo, float hi, float nlo, float nhi) { return nlo + (v - lo) / (hi - lo) * (nhi - nlo); }
float Ign(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }   // Interleaved Gradient Noise(Jimenez 2014)

// ---------------------------------------------------------------------------
//  雲層との交差。camera O(大気空間)から単位方向 d。成功なら [t0, t1] [km]
// ---------------------------------------------------------------------------
bool CloudRange(float3 O, float3 d, out float t0, out float t1)
{
    t0 = 0.0; t1 = 0.0;
    float r = length(O);
    float r0 = gBottomR + gBaseKm, r1 = r0 + gThickKm;
    float b = dot(O, d);
    float oo = dot(O, O);
    float disc1 = b * b - (oo - r1 * r1);
    if (disc1 <= 0.0) return false;
    float s1 = sqrt(disc1);
    float o0 = -b - s1, o1 = -b + s1;
    float disc0 = b * b - (oo - r0 * r0);
    if (r < r0)
    {
        if (AtRaySphereNearest(O, d, gBottomR) >= 0.0) return false;   // 地面に当たる向き
        t0 = -b + sqrt(max(disc0, 0.0));                                // 内側の球から出る点
        t1 = o1;
    }
    else if (r < r1)
    {
        t0 = 0.0;
        t1 = o1;
        if (disc0 > 0.0) { float i0 = -b - sqrt(disc0); if (i0 > 0.0) t1 = i0; }
    }
    else
    {
        if (o0 < 0.0) return false;
        t0 = o0;
        t1 = o1;
        if (disc0 > 0.0) { float i0 = -b - sqrt(disc0); if (i0 > 0.0) t1 = i0; }
    }
    t1 = min(t1, t0 + gMaxDistKm);
    return t1 > t0;
}

// ---------------------------------------------------------------------------
//  密度
// ---------------------------------------------------------------------------
float HeightGradient(float hf, float type)
{
    float lo = saturate(Remap(hf, 0.0, 0.1, 0.0, 1.0));
    float st = lo * saturate(Remap(hf, 0.2, 0.3, 1.0, 0.0));
    float cu = lo * saturate(Remap(hf, 0.4, 0.7, 1.0, 0.0));
    float cb = lo * saturate(Remap(hf, 0.8, 1.0, 1.0, 0.0));
    return (type < 0.5) ? lerp(st, cu, type * 2.0) : lerp(cu, cb, (type - 0.5) * 2.0);
}

// wxz = ワールド水平位置[km]、h = 殻内の高さ[km](0 = 雲底)。useDetail = 詳細ノイズでの削り(光線マーチの遠いサンプルでは省く)
float CloudDensity(float2 wxz, float h, float hf, float detailFade, bool useDetail)
{
    float2 wuv = (wxz + gWindOffsetKm.xz * 0.5) / (kWeatherTileKm * gScale);
    float3 w = gWeather.SampleLevel(gLinWrap, wuv, 0).rgb;
    float cov = saturate(gCoverage + (w.r - 0.5) * 1.5);
    float type = saturate(gCloudType + (w.g - 0.5) * 0.7);
    float grad = HeightGradient(hf, type);
    if (grad <= 0.0 || cov <= 0.0) return 0.0;

    float3 sp = float3(wxz.x + gWindOffsetKm.x, h, wxz.y + gWindOffsetKm.z) / (kShapeTileKm * gScale);
    float4 sh = gShape.SampleLevel(gLinWrap, sp, 0);
    float lowFbm = sh.g * 0.625 + sh.b * 0.25 + sh.a * 0.125;
    float base = saturate(Remap(sh.r, -(1.0 - lowFbm), 1.0, 0.0, 1.0)) * grad;
    float thr = 1.0 - cov * 0.65;                    // 雲量が大きいほどしきい値が下がり、雲が広がる
    base = saturate((base - thr) / (1.0 - thr));
    if (base <= 0.0) return 0.0;

    if (useDetail && gDetail > 0.0 && detailFade > 0.0)
    {
        float3 dp = float3(wxz.x + gWindOffsetKm.x, h, wxz.y + gWindOffsetKm.z) / (kDetailTileKm * gScale);
        float4 dt = gDetailT.SampleLevel(gLinWrap, dp, 0);
        float hfFbm = dt.r * 0.625 + dt.g * 0.25 + dt.b * 0.125;
        hfFbm = lerp(hfFbm, 1.0 - hfFbm, saturate(hf * 8.0));          // 雲底はもこもこ、上へ行くほど薄く削れる
        base = saturate(Remap(base, hfFbm * 0.65 * gDetail * detailFade, 1.0, 0.0, 1.0));
    }
    return base;
}

// ---------------------------------------------------------------------------
//  位相関数: 2 ローブ HG(前方 + 後方)+ 太陽のまわりの銀の縁(g 0.93 の強い前方ローブ)。c = 多重散乱の octave の減衰(g を絞る)
// ---------------------------------------------------------------------------
float CloudPhase(float cosT, float c)
{
    float fwd = AtHgPhase(0.62 * c, cosT);
    float bwd = AtHgPhase(-0.28 * c, cosT);
    float silver = AtHgPhase(0.93 * c, cosT);
    return lerp(bwd, fwd, 0.65) + 0.22 * silver;
}

// ---------------------------------------------------------------------------
struct CloudRay { float3 L; float T; float dist; };

CloudRay MarchCloud(float3 O, float3 d, float jitter)
{
    CloudRay res;
    res.L = 0; res.T = 1; res.dist = gMaxDistKm;
    float t0, t1;
    if (!CloudRange(O, d, t0, t1)) return res;

    const float rBase = gBottomR + gBaseKm;
    const float len = t1 - t0;
    const float expo = lerp(1.0, 2.0, saturate((len - 8.0) / 40.0));   // 長い(地平線近くの)視線は手前を細かく
    const uint N = gSteps;
    const float cosT = dot(d, gLightDir);
    const float2 camXZ = gCamPos.xz * 0.001;

    // 環境光: 天頂の空(Sky-View LUT)× 光源の照度。下側は地面の照り返し
    float3 ambTop = AtSampleSkyView(float3(0, 1, 0)) * gLightE + gNightSky;
    float3 ambBot = gGroundAlbedo * gLightE * (saturate(gLightDir.y) * 0.5 / AT_PI) + ambTop * 0.15;

    float3 L = 0; float T = 1; float sumW = 0, sumD = 0;
    static const float kLightStep[5] = {0.07, 0.14, 0.28, 0.56, 1.1};
    [loop]
    for (uint i = 0; i < N; ++i)
    {
        float ua = pow(float(i) / N, expo), ub = pow(float(i + 1) / N, expo);
        float ta = t0 + len * ua, tb = t0 + len * ub;
        float dt = tb - ta;
        float t = lerp(ta, tb, jitter);
        float3 P = O + d * t;
        float pr = length(P);
        float h = pr - rBase;
        float hf = h / gThickKm;
        if (hf < 0.0 || hf > 1.0) continue;
        float2 wxz = P.xz + camXZ;
        float detailFade = saturate(1.0 - t / (gMaxDistKm * 0.35));
        float dens = CloudDensity(wxz, h, hf, detailFade, true);
        dens *= saturate((gMaxDistKm - t) / (gMaxDistKm * 0.25));        // 最遠端は薄く消す
        if (dens <= 0.002) continue;
        float sigma = dens * gDensity * kExtinction;

        // ---- 光源方向の光学的厚み ----
        float odSun = 0.0, acc = 0.0;
        [unroll]
        for (int k = 0; k < 5; ++k)
        {
            float ds = kLightStep[k];
            float3 Q = P + gLightDir * (acc + ds * 0.5);
            acc += ds;
            float qh = length(Q) - rBase;
            float qhf = qh / gThickKm;
            if (qhf < 0.0 || qhf > 1.0) continue;
            odSun += CloudDensity(Q.xz + camXZ, qh, qhf, 0.0, false) * gDensity * kExtinction * ds;
        }
        // 太陽方向のさらに遠い雲層の影(層を斜めに抜ける長い光路。粗い 1 サンプルで近似)
        {
            float3 Q = P + gLightDir * 2.4;
            float qh = length(Q) - rBase;
            float qhf = qh / gThickKm;
            if (qhf >= 0.0 && qhf <= 1.0)
                odSun += CloudDensity(Q.xz + camXZ, qh, qhf, 0.0, false) * gDensity * kExtinction * 1.6;
        }

        // ---- 多重散乱の近似(Wrenninge): a = 0.5, b = 0.25, c = 0.5 の 3 octave ----
        float3 sunT = AtTransmittanceToLight(P, gLightDir);
        float scat = 0.0, a = 1.0, b = 1.0, c = 1.0;
        [unroll]
        for (int o = 0; o < 3; ++o)
        {
            scat += a * CloudPhase(cosT, c) * exp(-odSun * b);
            a *= 0.5; b *= 0.25; c *= 0.5;     // 深い所ほど光が拡散して届く(b を小さくする)
        }
        // Beer-Powder: 光が当たる側の縁が暗くなりすぎないよう、向きによって弱く効かせる
        float powder = 1.0 - exp(-sigma * 0.2);
        scat *= lerp(1.0, saturate(powder * 2.0), 0.55 * saturate(0.5 - 0.5 * cosT));
        float3 sunLight = gLightE * sunT * scat;

        float3 amb = lerp(ambBot, ambTop, saturate(hf * 0.8 + 0.2)) * lerp(0.6, 1.0, exp(-odSun * 0.12));
        float3 E = sunLight + amb;

        float stepT = exp(-sigma * dt);
        float w = T * (1.0 - stepT);
        L += E * w;
        sumW += w; sumD += w * t;
        T *= stepT;
        if (T < 0.01) { T = 0.0; break; }
    }

    res.L = L; res.T = T;
    res.dist = (sumW > 1e-6) ? sumD / sumW : gMaxDistKm;
    return res;
}

static const int2 kBayerInv[16] = {
    int2(0,0), int2(2,2), int2(2,0), int2(0,2), int2(1,1), int2(3,3), int2(3,1), int2(1,3),
    int2(1,0), int2(3,2), int2(3,0), int2(1,2), int2(0,1), int2(2,3), int2(2,1), int2(0,3) };
static const int kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

[numthreads(8, 8, 1)]
void TraceCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gCoarseX || id.y >= gCoarseY) return;
    int2 pix = int2(id.xy) * 4 + kBayerInv[gFrame & 15u];
    pix = min(pix, int2(gResX - 1, gResY - 1));
    float2 uv = (float2(pix) + 0.5) / float2(gResX, gResY);
    float3 dir = AtPixelDir(uv, gInvViewProj);
    float3 O = float3(0, gBottomR + gCamAlt, 0);
    float jitter = frac(Ign(float2(pix)) + float(gFrame) * 0.61803398875);

    CloudRay r = MarchCloud(O, dir, jitter);

    // 遠景の空気遠近(AP): final = sky * T + Lap * (1 - T) + Tap * L
    float3 Lap = 0, Tap = 1;
    if ((gFlags & AT_FLAG_AP) != 0u && r.T < 0.9999)
    {
        AtSampleAp(uv, min(r.dist, gApMaxKm), Lap, Tap);
        Lap *= gLightE * gApStrength;
        Tap = lerp(float3(1, 1, 1), Tap, gApStrength);
    }
    float3 A = r.L * Tap + Lap * (1.0 - r.T);
    A = clamp(A, 0.0, 6.0e4);
    if (any(isnan(A)) || isnan(r.T)) { A = 0; r.T = 1; }
    gOutTraceC[id.xy] = float4(A, saturate(r.T));
    gOutTraceD[id.xy] = r.dist;
}

[numthreads(8, 8, 1)]
void ResolveCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gResX || id.y >= gResY) return;
    int2 q = int2(id.xy);
    int2 c = min(q >> 2, int2(gCoarseX - 1, gCoarseY - 1));
    float4 tv = gTraceC.Load(int3(c, 0));
    float td = gTraceD.Load(int3(c, 0));
    bool isNew = (kBayer[(q.y & 3) * 4 + (q.x & 3)] == int(gFrame & 15u));

    float2 uv = (float2(q) + 0.5) / float2(gResX, gResY);
    float3 dir = AtPixelDir(uv, gInvViewProj);

    float4 outC = tv;
    float outD = td;
    if ((gCFlags & CF_RESET) == 0u)
    {
        // 現在の画素の方向 × 近傍の粗い画素が見た雲までの距離 → ワールド点 → 前フレームの画面へ
        float3 Pw = gCamPos + dir * (td * 1000.0);
        float4 clip = mul(float4(Pw, 1.0), gPrevViewProj);
        float2 puv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
        bool valid = clip.w > 0.0 && all(puv > 0.0) && all(puv < 1.0);
        if (valid)
        {
            float4 hc = gHistC.SampleLevel(gLinClamp, puv, 0);
            float hd = gHistD.SampleLevel(gLinClamp, puv, 0);
            // 近傍(粗いグリッドの 3x3)の範囲へクランプしてゴーストを抑える
            float4 mn = tv, mx = tv;
            [unroll] for (int j = -1; j <= 1; ++j)
            [unroll] for (int i = -1; i <= 1; ++i)
            {
                int2 cc = clamp(c + int2(i, j), int2(0, 0), int2(gCoarseX - 1, gCoarseY - 1));
                float4 v = gTraceC.Load(int3(cc, 0));
                mn = min(mn, v); mx = max(mx, v);
            }
            float4 pad = (mx - mn) * 0.25 + float4(mx.rgb * 0.02, 0.02);
            hc = clamp(hc, mn - pad, mx + pad);
            // 粗いグリッドの双線形(ゆっくり履歴の誤差を戻す)
            float4 bil = gTraceC.SampleLevel(gLinClamp, uv, 0);
            float k = isNew ? (((gCFlags & CF_BURST) != 0u) ? 1.0 : 0.22) : gHistBlend;
            outC = lerp(hc, isNew ? tv : bil, k);
            outD = lerp(hd, td, k);
        }
    }
    outC.a = saturate(outC.a);
    gOutResC[id.xy] = float4(clamp(outC.rgb, 0.0, 6.0e4), outC.a);
    gOutResD[id.xy] = outD;
}

// ---------------------------------------------------------------------------
//  合成
// ---------------------------------------------------------------------------
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSComposite(VSOut i) : SV_TARGET
{
    int2 px = int2(i.pos.xy);
    if (gSteps == 17u)   // ★デバッグ表示(steps=17): 4 分割でノイズを見る
    {
        float2 q = frac(i.uv * 2.0);
        float v = 0;
        if (i.uv.y < 0.5) v = (i.uv.x < 0.5) ? gShape.SampleLevel(gLinWrap, float3(q, 0.3), 0).r : gShape.SampleLevel(gLinWrap, float3(q, 0.3), 0).g;
        else v = (i.uv.x < 0.5) ? gDetailT.SampleLevel(gLinWrap, float3(q, 0.3), 0).r : gWeather.SampleLevel(gLinWrap, q, 0).r;
        return float4(v, v, v, 0.0);
    }
    float4 c = gHistC.Load(int3(px, 0));      // 解決済みの雲(今フレームの出力)
    float d = gDepth.Load(int3(px, 0));
    if (d < 0.9999999)
    {
        // 不透明物が雲の入口より手前なら雲は掛けない(地面・建物)。奥(遠い山など)なら雲が手前にあるとして掛ける。
        float2 ndc = i.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
        float4 w = mul(float4(ndc, d, 1.0), gInvViewProjJ);
        float3 Pw = w.xyz / w.w;
        float geomKm = length(Pw - gCamPos) * 0.001;
        float3 dir = normalize(Pw - gCamPos);
        float t0, t1;
        float3 O = float3(0, gBottomR + gCamAlt, 0);
        if (!CloudRange(O, dir, t0, t1) || geomKm < t0) return float4(0, 0, 0, 1);
    }
    return float4(c.rgb, c.a);
}
