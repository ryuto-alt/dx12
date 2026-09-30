// WaterCommon.hlsli — 水面 W1 のリソース宣言と共通関数（描画パス WaterSurface.hlsl / WaterUnder.hlsl が include する）。
//
// ★水は専用のルートシグネチャで描く（メイン RS の DWORD は 1 つも使わない）。メイン RS の材質テーブルは PS 専用で VS が読めないため、
//   波のデータを VS が読める専用 RS にした。ライティング（b1 / CSM / スポット・ポイント影 / IBL / クラスタ）はメインと同じ
//   レジスタ番号で張り直す（Lighting.hlsli をそのまま include できる）。水の SRV ブロック（4 連続・VS/PS 可視）:
//     t0 = ディテール法線テクスチャ（RGBA8: R,G=傾き / B=泡ノイズ / A=コースティクス）
//     t1 = 不透明シーンの色のコピー（RGBA16F）   t2 = 不透明シーンの深度のコピー（R32F。標準 Z: 遠い = 1）
//     t3 = 水のパラメータ（StructuredBuffer<float4>。レイアウトは src/renderer/water/WaterShared.h と一致）
#ifndef WATER_COMMON_HLSLI
#define WATER_COMMON_HLSLI

#include "../forward/Lighting.hlsli"

Texture2DArray g_shadowMap : register(t4);
#include "../forward/ShadowPcss.hlsli"

TextureCube  g_irradianceMap  : register(t5);
TextureCube  g_prefilteredMap : register(t6);
Texture2D    g_brdfLUT        : register(t7);
SamplerState g_iblSampler     : register(s2);   // LINEAR CLAMP（mip 有）
SamplerState g_brdfSampler    : register(s3);

Texture2D<float4>        g_waterTex   : register(t0);
Texture2D<float4>        g_sceneColor : register(t1);
Texture2D<float>         g_sceneDepth : register(t2);
StructuredBuffer<float4> g_waterData  : register(t3);
SamplerState             g_sampler    : register(s0);   // ANISOTROPIC WRAP

// ---- ドロー（リング 1 枚）ごとの定数（b0。24 DWORD）----
cbuffer WaterDraw : register(b0)
{
    float4x4 gViewProj;   // ジッタあり（HLSL の mul(v, M) 用に転置して渡される）
    float4   gLvl0;       // (セルの大きさ, グリッド半幅, 中心 x, 中心 z)
    float4   gLvl1;       // (レベル, 穴の x ずれ, 穴の z ずれ, 水域の添字)
};

// ---- データレイアウト（WaterShared.h と一致）----
#define WD_FRAME   12
#define WD_STRIDE  42
#define WD_LEVEL   0
#define WD_ORIGIN  1
#define WD_AXES    2
#define WD_INV     3
#define WD_FOAM    4
#define WD_EXT     5
#define WD_SCATTER 6
#define WD_OPTICS  7
#define WD_DETAIL  8
#define WD_MISC    9
#define WD_WAVES   10
#define WD_POLY    34

static uint gBase = 0;    // 今の水域ブロックの先頭（float4 単位）

#define WATER_WAVE_A(i) g_waterData[gBase + WD_WAVES + (i) * 2]
#define WATER_WAVE_B(i) g_waterData[gBase + WD_WAVES + (i) * 2 + 1]
#include "WaterMath.hlsli"

float4x4 WaterFrameMat(uint row0)
{
    return float4x4(g_waterData[row0], g_waterData[row0 + 1], g_waterData[row0 + 2], g_waterData[row0 + 3]);
}

// ---- 画面 / 深度 ----
float LinearDepth(float d)
{
    const float4 pr = g_waterData[9];
    return pr.y / (d - pr.x);
}

// SV_Position の xy と深度からワールド座標（ジッタあり invViewProj）
float3 ReconWorld(float2 px, float d)
{
    const float4 sc = g_waterData[8];
    const float2 ndc = float2(px.x * sc.z * 2.0 - 1.0, 1.0 - px.y * sc.w * 2.0);
    const float4 w = mul(float4(ndc, d, 1.0), WaterFrameMat(0));
    return w.xyz / w.w;
}

// ワールド → SV_Position 相当のピクセル座標。w = view 空間の z（クリップ w）
float2 ProjectPx(float3 P, out float w)
{
    const float4 sc = g_waterData[8];
    const float4 c = mul(float4(P, 1.0), WaterFrameMat(4));
    w = c.w;
    const float2 ndc = c.xy / max(c.w, 1.0e-5);
    return float2((ndc.x * 0.5 + 0.5) * sc.x, (0.5 - ndc.y * 0.5) * sc.y);
}

// ---- 影（ForwardShade.hlsli の CalcShadow と同じ式。あのファイルは SSAO / SSR / デカール等の宣言を要求するので読まない）----
int WaterSelectCascade(float viewDepth)
{
    int c = NUM_CASCADES - 1;
    [unroll]
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (viewDepth <= cascadeSplitsView[i]) { c = i; break; }
    }
    return c;
}
float WaterShadow(float3 worldPos, float viewDepth, float2 svPos)
{
    if (cascadeSplitsView.x > 1.0e8) return 1.0;
    const int c = WaterSelectCascade(viewDepth);
    float shadow = SampleShadowCascadeCommon(g_shadowMap, c, worldPos, svPos, shadowParams.y);
    const float band = shadowParams.z;
    if (band > 0.0 && c < NUM_CASCADES - 1)
    {
        const float edge = cascadeSplitsView[c];
        const float t = saturate((edge - viewDepth) / max(band, 1.0e-4));
        if (t < 1.0) shadow = lerp(SampleShadowCascadeCommon(g_shadowMap, c + 1, worldPos, svPos, shadowParams.y), shadow, t);
    }
    return shadow;
}

// ---- 形（Lagrange 座標 x0 の内外判定）----
bool WaterInside(float2 xz)
{
    const float4 o = g_waterData[gBase + WD_ORIGIN];
    const float4 a = g_waterData[gBase + WD_AXES];
    const float4 iv = g_waterData[gBase + WD_INV];
    const int shape = (int)(g_waterData[gBase + WD_LEVEL].w + 0.5);
    if (shape == 0) return true;
    const float2 d = xz - o.xy;
    const float2 l = float2(d.x * iv.x + d.y * iv.z, d.x * iv.y + d.y * iv.w);
    if (shape == 1) return abs(l.x) <= 0.5 * a.z && abs(l.y) <= 0.5 * a.w;
    if (shape == 2)
    {
        const float2 r = 0.5 * max(a.zw, 1.0e-4);
        return dot(l / r, l / r) <= 1.0;
    }
    const int n = (int)(g_waterData[gBase + WD_MISC].y + 0.5);
    if (n < 3) return false;
    bool inside = false;
    [loop]
    for (int i = 0, j = n - 1; i < n; j = i++)
    {
        const float4 qi = g_waterData[gBase + WD_POLY + i / 2];
        const float4 qj = g_waterData[gBase + WD_POLY + j / 2];
        const float2 pi_ = (i & 1) ? qi.zw : qi.xy;
        const float2 pj_ = (j & 1) ? qj.zw : qj.xy;
        if (((pi_.y > l.y) != (pj_.y > l.y)) && (l.x < (pj_.x - pi_.x) * (l.y - pi_.y) / (pj_.y - pi_.y) + pi_.x)) inside = !inside;
    }
    return inside;
}

// ---- 環境光 ----
float3 WaterAmbientUp()
{
    if (hasIBL != 0u) return g_irradianceMap.SampleLevel(g_iblSampler, float3(0.0, 1.0, 0.0), 0).rgb * iblIntensity;
    return ambientStrength.xxx;
}

// 水面から水柱へ入る光（ランバートの radiance 規約: 拡散 = albedo * Ein）。sunShadow は太陽の可視性。
float3 WaterIncidentEnergy(float sunShadow, float ior)
{
    const float3 L = normalize(-lightDir);
    const float cosL = saturate(L.y);
    const float fs = WaterFresnel(cosL, ior);
    return lightColor * (sunShadow * cosL * (1.0 - fs) / PI) + WaterAmbientUp();
}

// ---- 画面空間の反射トレース（不透明シーンのコピーに対して。水面自身は含まない）----
//   戻り値 rgb = 反射先の放射輝度 / a = 信頼度。R は水面から外へ向かう方向。
float4 WaterTraceSSR(float3 P, float3 R, float2 px, float jitter)
{
    const float4 sc = g_waterData[8];
    const float kMaxDist = 90.0;
    const float3 P1 = P + R * kMaxDist;
    float4 c0 = mul(float4(P, 1.0), WaterFrameMat(4));
    float4 c1 = mul(float4(P1, 1.0), WaterFrameMat(4));
    const float nearW = 0.1;
    if (c0.w < nearW) return 0.0;
    if (c1.w < nearW) c1 = lerp(c0, c1, (c0.w - nearW) / max(c0.w - c1.w, 1.0e-5));
    const float2 s0 = float2((c0.x / c0.w * 0.5 + 0.5) * sc.x, (0.5 - c0.y / c0.w * 0.5) * sc.y);
    const float2 s1 = float2((c1.x / c1.w * 0.5 + 0.5) * sc.x, (0.5 - c1.y / c1.w * 0.5) * sc.y);
    const float k0 = 1.0 / c0.w, k1 = 1.0 / c1.w;
    const float len = length(s1 - s0);
    if (len < 2.0) return 0.0;
    const int steps = (int)clamp(len * 0.25, 10.0, 28.0);
    const float2 dS = (s1 - s0) / steps;
    const float  dK = (k1 - k0) / steps;
    float2 sp = s0 + dS * jitter;
    float  k = k0 + dK * jitter;
    float2 prevSp = s0;
    float  prevK = k0;
    bool hit = false;
    float2 hitPx = 0.0;
    [loop]
    for (int i = 0; i < steps; ++i)
    {
        sp += dS; k += dK;
        if (sp.x < 1.0 || sp.y < 1.0 || sp.x >= sc.x - 1.0 || sp.y >= sc.y - 1.0) break;
        const float zRay = 1.0 / k;
        const float dScene = g_sceneDepth.Load(int3(sp, 0));
        if (dScene < 0.99999)
        {
            const float zScene = LinearDepth(dScene);
            const float diff = zRay - zScene;
            const float thick = 0.35 + 0.045 * zRay;
            if (diff > 0.0 && diff < thick)
            {
                // 二分探索で交点を詰める（直前の点 = 手前 / 今の点 = 奥）
                float2 a = prevSp, b = sp;
                float ka = prevK, kb = k;
                [unroll]
                for (int r = 0; r < 4; ++r)
                {
                    const float2 m = 0.5 * (a + b);
                    const float km = 0.5 * (ka + kb);
                    const float dm = g_sceneDepth.Load(int3(m, 0));
                    const float zm = (dm < 0.99999) ? LinearDepth(dm) : 1.0e9;
                    if (1.0 / km > zm) { b = m; kb = km; } else { a = m; ka = km; }
                }
                hitPx = b;
                hit = true;
                break;
            }
        }
        prevSp = sp; prevK = k;
    }
    if (!hit) return 0.0;
    const float3 col = g_sceneColor.Load(int3(hitPx, 0)).rgb;
    // 画面端・レイの長さで信頼度を落とす
    const float2 e = min(hitPx, sc.xy - hitPx) / (0.08 * sc.xy);
    const float edge = saturate(min(e.x, e.y));
    return float4(min(col, 6.0e4), edge);
}

// 手続きの泡・コースティクスの値（ディテールテクスチャの B / A）
float WaterFoamNoise(float2 xz, float t, float2 flow, float tile)
{
    const float n1 = g_waterTex.SampleLevel(g_sampler, (xz - flow * t) / (tile * 0.55) + float2(0.13, 0.71), 0.0).b;
    const float n2 = g_waterTex.SampleLevel(g_sampler, (xz.yx - flow.yx * t * 0.7) / (tile * 0.21) + float2(0.5, 0.27), 0.0).b;
    return n1 * 0.6 + n2 * 0.4;
}

#endif // WATER_COMMON_HLSLI
