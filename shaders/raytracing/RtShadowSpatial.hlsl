// RtShadowSpatial.hlsl — RT 影デノイザ 3/3: 半影の幅に比例した空間フィルタ（SVGF 風 joint bilateral）
//
// 入力  t5 = 今フレームのトレース（RG16F: y=遮蔽物までの距離。半影の幅の見積もりにだけ使う）
//       t7 = 時間蓄積の出力（RGBA16F: x=平均, y=2 次モーメント, z=履歴の長さ n, w=ビュー Z）
// 出力  R8_UNORM の可視率。**今の影マスク(t11)をそのまま置き換える**＝フォワードは無変更。
//
// 半径の決め方:
//   半影の幅(px) = 2 · t · tan(太陽の角半径) ÷（その深度での 1 画素のワールド幅）
//   半径 = clamp(半影幅 · 0.5, 1, 16) px、履歴が短いほど大きく（× (1 + 3/(n+1))、最大 24px）。
//   t は 17 点（中心＋半径 4px/12px の 8 点ずつ）の「遮蔽されたレイの距離」の平均。
//   t を時間蓄積しない代わりに空間で集めている（履歴の 4 チャンネルは使い切っているため）。
//
// 重み: RtAoDenoise と同じ深度・法線・平面の 3 種 + 可視率の差を分散で正規化した輝度重み。
//   分散がほぼ 0 で履歴が十分長い画素（完全に照らされた/完全な影）は素通し。
#include "RtCommon.hlsli"

Texture2D<float2> gShTrace : register(t5);   // 今フレームのトレース
Texture2D<float4> gShTemp  : register(t7);   // 時間蓄積の出力

static const float2 kTap8[8] =
{
    float2( 0.7071,  0.7071), float2(-0.7071,  0.7071),
    float2(-0.7071, -0.7071), float2( 0.7071, -0.7071),
    float2( 1.0000,  0.0000), float2( 0.0000,  1.0000),
    float2(-1.0000,  0.0000), float2( 0.0000, -1.0000),
};

float RtSqr(float v) { return v * v; }

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    const int2  px = int2(i.pos.xy);
    const float d  = gDepth.Load(int3(px, 0));
    if (!RtInsideViewport(i.pos.xy)) return 1.0;
    if (d >= 1.0) return 1.0;

    const float4 hc   = gShTemp.Load(int3(px, 0));
    const float  mean = hc.x;
    const float  n    = hc.z;
    const float  var  = max(hc.y - mean * mean, 0.0);

    // 履歴が十分あり、分散がほぼ 0（完全に照らされた / 完全な影）なら素通し。
    if (n >= 8.0 && var < 2e-3) return saturate(mean);
    if (gGBufferValid < 0.5) return saturate(mean);

    const float3 Nc = RtOctDecode(gGBuffer.Load(int3(px, 0)).xy);
    const float3 Pc = RtWorldFromDepth(i.pos.xy, d);
    const float  zc = hc.w;

    // ---- 半影の幅を見積もる（遮蔽されたレイの距離の平均）----
    float tSum = 0.0, tCnt = 0.0;
    {
        const float t0 = gShTrace.Load(int3(px, 0)).y;
        if (t0 > 0.0) { tSum += t0; tCnt += 1.0; }
        [unroll]
        for (int k = 0; k < 8; ++k)
        {
            [unroll]
            for (int r = 0; r < 2; ++r)
            {
                const float2 o  = kTap8[k] * (r == 0 ? 4.0 : 12.0);
                const int2   tp = px + int2(round(o));
                if (!RtInsideViewport(float2(tp) + 0.5)) continue;
                const float tt = gShTrace.Load(int3(tp, 0)).y;
                if (tt > 0.0) { tSum += tt; tCnt += 1.0; }
            }
        }
    }
    const float tEst = (tCnt > 0.0) ? tSum / tCnt : 0.0;

    // 1 画素のワールド幅（その深度で）。透視は視角 × 距離、正射は定数。
    const float pxWorld   = (gShPixelAngle >= 0.0) ? gShPixelAngle * max(zc, 1e-3) : -gShPixelAngle;
    const float penumbraPx = 2.0 * tEst * gSunTan / max(pxWorld, 1e-6);
    float radiusPx = clamp(penumbraPx * 0.5, 1.0, 16.0);
    radiusPx = min(radiusPx * (1.0 + 3.0 / (n + 1.0)), 24.0);

    const float  ang = RT_2PI * frac(RtIgn(i.pos.xy) + gFrameIndex * 0.6180339887);
    const float2 rot = float2(cos(ang), sin(ang));

    // 履歴が短い間は分散が信用できない（常に 0 に近い）ので輝度重みを弱める。
    const float lumTrust = saturate((n - 1.0) / 6.0);
    const float sd = sqrt(var);

    float sum = mean, wsum = 1.0;   // 中心は必ず重み 1

    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        const float2 o = float2(kTap8[k].x * rot.x - kTap8[k].y * rot.y,
                                kTap8[k].x * rot.y + kTap8[k].y * rot.x) * radiusPx;
        const int2   tp  = px + int2(round(o));
        const float2 tpf = float2(tp) + 0.5;
        if (!RtInsideViewport(tpf)) continue;

        const float dt = gDepth.Load(int3(tp, 0));
        if (dt >= 1.0) continue;

        const float3 Nt = RtOctDecode(gGBuffer.Load(int3(tp, 0)).xy);
        const float3 Pt = RtWorldFromDepth(tpf, dt);
        const float  zt = RtViewZFromDepth(tpf, dt);
        const float4 ht = gShTemp.Load(int3(tp, 0));

        const float wz = exp(-abs(zt - zc) / max(0.05 * abs(zc), 1e-3));
        const float nc = RtSqr(RtSqr(max(0.0, dot(Nt, Nc))));
        const float3 dq = Pc - Pt;
        const float  d2 = dot(dq, dq);
        const float  pe = max(abs(dot(dq, Nt)), abs(dot(dq, Nc)));
        const float  wp = (d2 < 1e-4) ? 1.0 : RtSqr(max(0.0, 1.0 - 2.0 * pe / sqrt(d2)));
        const float  wg = exp(-RtSqr(length(o) / max(0.9 * radiusPx, 1e-3)));
        // SVGF の輝度重み: 平均の差を中心の標準偏差で割る（σl=4）。
        const float  wl = lerp(1.0, exp(-abs(ht.x - mean) / (4.0 * sd + 0.02)), lumTrust);

        const float w = wg * wz * nc * wp * wl;
        sum  += ht.x * w;
        wsum += w;
    }
    return saturate(sum / max(wsum, 1e-5));
}
