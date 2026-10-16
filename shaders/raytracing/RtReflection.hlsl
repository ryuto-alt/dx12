// RtReflection.hlsl — RT 反射（R2）。SSR の後段で、SSR の confidence が低い画素だけ RayQuery を撃つ。
//
// 4 パス（すべてフルスクリーン三角形の PS。ps_6_6 = RayQuery + バインドレス）:
//   TracePS     … ハーフ解像度。1 レイ / 画素。GGX の可視法線（VNDF）で反射レイを撃ち、ヒット点を
//                 RtHitShade.hlsli（DDGI と共通）で照らす。出力 = (rgb * conf, conf)（事前乗算）
//   TemporalPS  … 動きベクトルで履歴を再投影 + 3x3 の平均 ± σ へ分散クリップして指数平均
//   SpatialPS   … 深度・法線・粗さで重みを付ける 9 タップの空間フィルタ（粗さに比例した半径）
//   CompositePS … フル解像度へ深度・法線考慮でアップサンプルし、SSR（t16）と合成して t16 と同じ形式で出す
//
// ★出力は SSR と同じ形式（rgb = 反射放射輝度 / a = confidence）。フォワードは無改造:
//     prefiltered = lerp(prefiltered, ssrRgb, ssrConf)
//   に対し、合成後の (rgb, a) を  a = a_ssr + (1 - a_ssr) * a_rt,
//   rgb = (rgb_ssr * a_ssr + (1 - a_ssr) * rgb_rt * a_rt) / a  とすると、
//   lerp(p, rgb, a) = p (1 - a) + rgb_ssr a_ssr + (1 - a_ssr) rgb_rt a_rt  ＝ SSR → RT → 空 の順の over 合成になる。
// ★ミス（空へ抜けた / 最大距離超え / 面の裏へ向かうレイ）は conf = 0 ＝ 既存の段 4（空の prefiltered + DDGI の埋め）に任せる。
// ★ヒットの明るさの単位: シーン RT の単位のまま（物理単位モードでは nit）。DDGI の内部単位（half 溢れ防止の
//   gUnitScale 倍）には変換済み: RtHitShadeGiNew(extScale = 1, bounceScale = 1 / gUnitScale)。
#include "../post/FullscreenTri.hlsli"

cbuffer ReflParams : register(b1)
{
    float4x4 gInvViewProj;   // サブビューポート NDC → ワールド（転置済み: mul(row, M)）
    float4x4 gInvProj;       // 転置済み
    float4   gR0;            // xyz = カメラ位置 / w = zNear
    float4   gR1;            // x = roughnessMax / y = maxDistance(m) / z = レイ始点の法線バイアス(m) / w = intensity
    float4   gR2;            // xy = ビューポート原点(px) / zw = サイズ(px)
    float4   gR3;            // xy = ハーフ RT のサイズ(px) / zw = フル RT のサイズ(px)
    float4   gR4;            // x = フレーム連番 / y = SSR を撃たない閾値 / z = 履歴の有無(1/0) / w = G-Buffer 有効(1/0)
    float4   gR5;            // x = bounceScale（1 / gUnitScale）/ y = 空の倍率（flat 用）/ z = 1 で GI モード New のヒット shading / w = 空間フィルタの基準半径(px)
    uint4    gI0;            // depth / gbuffer / velocity / ssr の SRV index
    uint4    gI1;            // trace / history / filtered / probeData の SRV index
    uint4    gI2;            // x = ライトグリッド UAV の index / y = トレース解像度の縮小率（1 / 2）/ z = 空の prefiltered キューブの SRV（無ければ 0xFFFFFFFF）/ w = そのミップ数 - 1
};

#define gCameraPos     gR0.xyz
#define gRoughMax      gR1.x
#define gMaxDist       gR1.y
#define gNormalBias    gR1.z
#define gIntensity     gR1.w
#define gViewport      gR2
#define gHalfSize      gR3.xy
#define gFullSize      gR3.zw
#define gFrame         gR4.x
#define gSsrSkip       gR4.y
#define gHistValid     gR4.z
#define gGBufValid     gR4.w
#define gBounceScale   gR5.x
#define gSkyMul        gR5.y
#define gUseGiNew      gR5.z
#define gFilterRadius  gR5.w
#define gDepthSrv      gI0.x
#define gGBufSrv       gI0.y
#define gVelSrv        gI0.z
#define gSsrSrv        gI0.w
#define gTraceSrv      gI1.x
#define gHistSrv       gI1.y
#define gFilteredSrv   gI1.z
#define gProbeDataSrv  gI1.w
#define gGridUav       gI2.x
#define gSkySrv        gI2.z
#define gSkyMaxMip     ((float)gI2.w)
#define gScale         ((int)gI2.y)   // トレース解像度の縮小率（2 = ハーフ / 1 = フル）

RaytracingAccelerationStructure gTlas : register(t0);
SamplerState gLinearClamp : register(s1);

float4 RtProbeDataLoad(uint2 t)
{
    Texture2D<float4> pd = ResourceDescriptorHeap[gProbeDataSrv];
    return pd.Load(int3(int2(t), 0));
}
uint RtLightGridLoad(uint i)
{
    RWStructuredBuffer<uint> g = ResourceDescriptorHeap[gGridUav];
    return g[i];
}
#define DDGI_PROBEDATA_LOAD(t) RtProbeDataLoad(t)
#define RT_LIGHTGRID_AT(i) RtLightGridLoad(i)
#define RT_GEOMETRY_REGISTER t1
#include "RtHitShade.hlsli"

static const float RR_PI  = 3.14159265f;
static const float RR_2PI = 6.28318531f;

float RrSqr(float v) { return v * v; }

float3 RrOctDecode(float2 f)
{
    float3 n = float3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float  t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

float2 RrPixelToNdc(float2 px)
{
    float2 vpUV = (px - gViewport.xy) / max(gViewport.zw, 1e-6);
    return float2(vpUV.x * 2.0 - 1.0, 1.0 - vpUV.y * 2.0);
}

bool RrInsideViewport(float2 px)
{
    return px.x >= gViewport.x && px.x <= gViewport.x + gViewport.z
        && px.y >= gViewport.y && px.y <= gViewport.y + gViewport.w;
}

float3 RrWorldFromDepth(float2 px, float depth)
{
    float4 w = mul(float4(RrPixelToNdc(px), depth, 1.0), gInvViewProj);
    return w.xyz / w.w;
}

float RrViewZ(float2 px, float depth)
{
    float4 v = mul(float4(RrPixelToNdc(px), depth, 1.0), gInvProj);
    return v.z / v.w;
}

// Interleaved gradient noise（Jimenez）。フレームごとにずらして時間方向にも散らす。
float RrIgn(float2 px, float shift)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(px + shift, magic.xy)));
}

void RrBasis(float3 N, out float3 T, out float3 B)
{
    float3 up = (abs(N.y) < 0.99) ? float3(0, 1, 0) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// GGX の可視法線サンプリング（Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT 2018）。
// Ve = 接空間の視線ベクトル（z が法線）。alpha = roughness^2。戻り値 = 接空間のハーフベクトル。
float3 RrSampleVndf(float3 Ve, float alpha, float2 u)
{
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = (lensq > 0.0) ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 T2 = cross(Vh, T1);
    float  r   = sqrt(u.x);
    float  phi = RR_2PI * u.y;
    float  t1  = r * cos(phi);
    float  t2  = r * sin(phi);
    float  s   = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(0.0, Nh.z)));
}

// ---------------------------------------------------------------------------
// ① トレース（ハーフ解像度）
// ---------------------------------------------------------------------------
float4 TracePS(FSQuadVSOut i) : SV_TARGET
{
    const int2   hp = int2(i.pos.xy);
    const int2   fp = hp * gScale;                  // 担当するフル解像度の画素（左上）
    const float2 fpc = float2(fp) + 0.5;
    if (!RrInsideViewport(fpc)) return 0.0;
    if (gGBufValid < 0.5) return 0.0;

    Texture2D<float>  depthT = ResourceDescriptorHeap[gDepthSrv];
    const float d = depthT.Load(int3(fp, 0));
    if (d >= 1.0) return 0.0;                       // 空は反射しない

    Texture2D<float4> gbT = ResourceDescriptorHeap[gGBufSrv];
    const float4 gb = gbT.Load(int3(fp, 0));
    const float rough = gb.z;
    if (rough > gRoughMax) return 0.0;              // 粗い面は IBL に任せる

    // SSR が十分に当たっている画素は撃たない（SSR → RT の順の over 合成。ここを撃っても使われない）
    Texture2D<float4> ssrT = ResourceDescriptorHeap[gSsrSrv];
    if (saturate(ssrT.Load(int3(fp, 0)).a) >= gSsrSkip) return 0.0;

    const float3 P = RrWorldFromDepth(fpc, d);
    const float3 N = RrOctDecode(gb.xy);
    const float3 V = normalize(gCameraPos - P);
    if (dot(N, V) <= 1e-3) return 0.0;              // 裏を向いている面

    // ---- 反射方向 ----
    float3 R;
    if (rough < 0.04)
    {
        R = reflect(-V, N);                         // ほぼ鏡: サンプリングしない（ノイズゼロ）
    }
    else
    {
        float3 T, B;
        RrBasis(N, T, B);
        const float3 Ve = float3(dot(V, T), dot(V, B), dot(V, N));
        const float  sh = gFrame * 5.588238;
        const float2 u = float2(RrIgn(float2(fp), sh), RrIgn(float2(fp.yx) + 47.0, sh * 1.7 + 3.0));
        const float3 Hl = RrSampleVndf(Ve, rough * rough, u);
        const float3 H  = T * Hl.x + B * Hl.y + N * Hl.z;
        R = reflect(-V, H);
    }
    if (dot(R, N) <= 0.0) return 0.0;               // 面の裏へ向かうレイ（粗い面の斜めで起きる）。空に任せる

    // ---- トレース ----
    // 自己交差回避のバイアスは距離に応じて少し伸ばす（遠景の深度精度）
    const float  distCam = length(gCameraPos - P);
    const float3 origin  = P + N * (gNormalBias + distCam * 2e-4);
    RayDesc ray;
    ray.Origin = origin; ray.Direction = R; ray.TMin = 0.0; ray.TMax = max(gMaxDist, 1.0);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gTlas, 0, 0xFF, ray);
    RT_PROCEED_WITH_ALPHA(q, gLinearWrap);          // RT2: MASK(葉・柵)は α を見て抜く
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        // ミス = 空。フォワードと同じ空の prefiltered キューブ（ラフネスのミップ）を返す。
        // ★ここで返さないと、開いた天井・窓から見える空が「DDGI の埋め（ぼやけた部屋の平均色）」になって鏡に空が映らない。
        //   空の可視率が低い室内の鏡で破綻しないよう、キューブが無い（屋内で IBL 無し）ときだけ conf 0（＝段 4）。
        if (gSkySrv == 0xFFFFFFFFu) return 0.0;
        TextureCube<float4> sky = ResourceDescriptorHeap[gSkySrv];
        float3 skyRad = sky.SampleLevel(gLinearClamp, R, rough * gSkyMaxMip).rgb * gSkyMul;
        skyRad = min(max(skyRad, 0.0.xxx), 6.0e4.xxx);
        if (any(isnan(skyRad)) || any(isinf(skyRad))) return 0.0;
        float c = gIntensity;
        c *= 1.0 - smoothstep(gRoughMax * 0.6, gRoughMax, rough);
        c *= smoothstep(0.0, 0.2, dot(N, V));
        c = saturate(c);
        return float4(skyRad * c, c);
    }

    RtHitInfo h = RtLoadHit(q);
    if (!h.valid) return 0.0;
    if (dot(h.worldNormal, R) > 0.0) h.worldNormal = -h.worldNormal; // 裏面ヒットは法線を返して両面扱い

    float3 radiance;
    if (gUseGiNew > 0.5) radiance = RtHitShadeGiNew(h, R, false, 1.0, gBounceScale, true);
    else                 radiance = RtHitShadeFlat(h, 1.0, gSkyMul);
    radiance = min(max(radiance, 0.0.xxx), 6.0e4.xxx);
    if (any(isnan(radiance)) || any(isinf(radiance))) return 0.0;

    // ---- confidence ----
    const float t = q.CommittedRayT();
    float conf = gIntensity;
    conf *= 1.0 - smoothstep(gRoughMax * 0.6, gRoughMax, rough);        // 粗さの上限へ向けて連続に消す
    conf *= 1.0 - smoothstep(0.75 * gMaxDist, gMaxDist, t);             // 遠いヒットは空へ寄せる
    conf *= smoothstep(0.0, 0.2, dot(N, V));                            // 擦れすぎ
    conf = saturate(conf);
    return float4(radiance * conf, conf);
}

// ---------------------------------------------------------------------------
// ② 時間フィルタ（ハーフ解像度）。入力 trace / history（事前乗算）
// ---------------------------------------------------------------------------
float4 TemporalPS(FSQuadVSOut i) : SV_TARGET
{
    const int2 hp = int2(i.pos.xy);
    Texture2D<float4> traceT = ResourceDescriptorHeap[gTraceSrv];
    Texture2D<float4> histT  = ResourceDescriptorHeap[gHistSrv];
    const float4 cur = traceT.Load(int3(hp, 0));

    // 3x3 の平均と標準偏差（分散クリップ。Salvi 2016）
    float4 m1 = 0.0, m2 = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int2 tp = clamp(hp + int2(x, y), int2(0, 0), int2(gHalfSize) - 1);
        const float4 c = traceT.Load(int3(tp, 0));
        m1 += c; m2 += c * c;
    }
    m1 /= 9.0; m2 /= 9.0;
    const float4 sigma = sqrt(max(m2 - m1 * m1, 0.0));
    const float4 lo = m1 - 1.25 * sigma, hi = m1 + 1.25 * sigma;

    if (gHistValid < 0.5) return cur;

    // 動きベクトル（現 UV - 前 UV）。担当するフル解像度画素の値を使う
    Texture2D<float2> velT = ResourceDescriptorHeap[gVelSrv];
    const int2   fp = hp * gScale;
    const float2 vel = velT.Load(int3(fp, 0));
    const float2 uv  = (float2(fp) + 0.5) / gFullSize;
    const float2 puv = uv - vel;
    if (any(puv < 0.0) || any(puv > 1.0)) return cur;                  // 画面外から来た履歴は捨てる

    float4 hist = histT.SampleLevel(gLinearClamp, puv, 0);
    hist = clamp(hist, lo, hi);

    // 粗い面ほどノイズが大きいので履歴を強く効かせる。鏡面は遅れ（ゴースト）を嫌って弱める。
    Texture2D<float4> gbT = ResourceDescriptorHeap[gGBufSrv];
    const float rough = gbT.Load(int3(fp, 0)).z;
    const float alpha = lerp(0.35, 0.1, smoothstep(0.05, 0.4, rough));
    return lerp(hist, cur, alpha);
}

// ---------------------------------------------------------------------------
// ③ 空間フィルタ（ハーフ解像度）。入力 = 時間フィルタ後（履歴にはこの結果を入れない）
// ---------------------------------------------------------------------------
static const float2 kTap8[8] =
{
    float2( 0.7071,  0.7071), float2(-0.7071,  0.7071),
    float2(-0.7071, -0.7071), float2( 0.7071, -0.7071),
    float2( 1.0000,  0.0000), float2( 0.0000,  1.0000),
    float2(-1.0000,  0.0000), float2( 0.0000, -1.0000),
};

float4 SpatialPS(FSQuadVSOut i) : SV_TARGET
{
    const int2 hp = int2(i.pos.xy);
    Texture2D<float4> srcT = ResourceDescriptorHeap[gHistSrv];   // 時間フィルタの出力（この呼び出しでは hist 枠に入れて渡す）
    float4 sum = srcT.Load(int3(hp, 0));
    if (gGBufValid < 0.5) return sum;

    Texture2D<float>  depthT = ResourceDescriptorHeap[gDepthSrv];
    Texture2D<float4> gbT    = ResourceDescriptorHeap[gGBufSrv];
    const int2   fp = hp * gScale;
    const float  dc = depthT.Load(int3(fp, 0));
    if (dc >= 1.0) return sum;
    const float4 gc = gbT.Load(int3(fp, 0));
    const float3 Nc = RrOctDecode(gc.xy);
    const float2 fpc = float2(fp) + 0.5;
    const float  zc = RrViewZ(fpc, dc);
    const float3 Pc = RrWorldFromDepth(fpc, dc);

    // 粗い面ほど広くぼかす（ノイズが大きく、見た目もぼけているので）
    const float  radius = gFilterRadius * lerp(0.6, 2.0, saturate(gc.z / max(gRoughMax, 1e-3)));
    const float  ang = RR_2PI * RrIgn(float2(hp), gFrame * 3.17);
    const float2 rot = float2(cos(ang), sin(ang));

    float wsum = 1.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const float2 o = float2(kTap8[k].x * rot.x - kTap8[k].y * rot.y,
                                kTap8[k].x * rot.y + kTap8[k].y * rot.x) * radius;
        const int2 tp = hp + int2(round(o));
        if (any(tp < 0) || any(tp >= int2(gHalfSize))) continue;
        const int2 tf = tp * gScale;
        const float dt = depthT.Load(int3(tf, 0));
        if (dt >= 1.0) continue;
        const float4 gt = gbT.Load(int3(tf, 0));
        const float3 Nt = RrOctDecode(gt.xy);
        const float3 Pt = RrWorldFromDepth(float2(tf) + 0.5, dt);
        const float  zt = RrViewZ(float2(tf) + 0.5, dt);
        const float wz = exp(-abs(zt - zc) / max(0.05 * abs(zc), 1e-3));
        const float wn = RrSqr(RrSqr(max(0.0, dot(Nt, Nc))));
        const float3 dq = Pc - Pt;
        const float  d2 = dot(dq, dq);
        const float  pe = max(abs(dot(dq, Nt)), abs(dot(dq, Nc)));
        const float  wp = (d2 < 1e-4) ? 1.0 : RrSqr(max(0.0, 1.0 - 2.0 * pe / sqrt(d2)));
        const float  wr = exp(-abs(gt.z - gc.z) * 8.0);
        const float  wg = exp(-RrSqr(length(o) / max(0.9 * radius, 1e-3)));
        const float  w = wz * wn * wp * wr * wg;
        sum  += srcT.Load(int3(tp, 0)) * w;
        wsum += w;
    }
    return sum / max(wsum, 1e-5);
}

// ---------------------------------------------------------------------------
// ④ 合成 + アップサンプル（フル解像度）。出力 = t16 と同じ形式
// ---------------------------------------------------------------------------
float4 CompositePS(FSQuadVSOut i) : SV_TARGET
{
    const int2 px = int2(i.pos.xy);
    Texture2D<float4> ssrT = ResourceDescriptorHeap[gSsrSrv];
    const float4 ssr = ssrT.Load(int3(px, 0));
    if (gGBufValid < 0.5 || !RrInsideViewport(i.pos.xy)) return ssr;

    Texture2D<float>  depthT = ResourceDescriptorHeap[gDepthSrv];
    const float d = depthT.Load(int3(px, 0));
    if (d >= 1.0) return ssr;

    Texture2D<float4> gbT   = ResourceDescriptorHeap[gGBufSrv];
    Texture2D<float4> filtT = ResourceDescriptorHeap[gFilteredSrv];
    const float3 Nf = RrOctDecode(gbT.Load(int3(px, 0)).xy);
    const float  zf = RrViewZ(i.pos.xy, d);

    // ハーフ解像度の 2x2 を、フル画素の位置に応じたバイリニア重み × 深度 / 法線で持ち上げる
    const float2 hpf = i.pos.xy / (float)gScale - 0.5;          // ハーフ画素の連続座標（担当画素 = 2*hp の中心基準）
    const int2   h0  = int2(floor(hpf));
    const float2 fr  = hpf - float2(h0);
    float4 sum = 0.0;
    float  wsum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o  = int2(k & 1, k >> 1);
        const int2 tp = clamp(h0 + o, int2(0, 0), int2(gHalfSize) - 1);
        const int2 tf = tp * gScale;
        const float dt = depthT.Load(int3(tf, 0));
        if (dt >= 1.0) continue;
        const float3 Nt = RrOctDecode(gbT.Load(int3(tf, 0)).xy);
        const float  zt = RrViewZ(float2(tf) + 0.5, dt);
        const float wb = (o.x ? fr.x : 1.0 - fr.x) * (o.y ? fr.y : 1.0 - fr.y);
        const float wz = exp(-abs(zt - zf) / max(0.05 * abs(zf), 1e-3));
        const float wn = pow(saturate(dot(Nt, Nf)), 8.0);
        const float w = wb * wz * wn + 1e-5;
        sum  += filtT.Load(int3(tp, 0)) * w;
        wsum += w;
    }
    if (wsum <= 0.0) return ssr;
    const float4 rt = sum / wsum;                      // 事前乗算（rgb * conf, conf）
    const float  a  = saturate(rt.a);
    if (a <= 1e-4) return ssr;

    const float as = saturate(ssr.a);
    const float oa = as + (1.0 - as) * a;
    const float3 rgb = (ssr.rgb * as + (1.0 - as) * rt.rgb) / max(oa, 1e-4);
    return float4(rgb, oa);
}
