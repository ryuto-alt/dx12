// RtAoDenoise.hlsl — RT-AO の空間デノイザ（joint bilateral）+ 最終合成
//
// RtAo.hlsl が出す「生の可視率」を、深度・法線・接平面でエッジを守りながら平滑化し、
// 最後に intensity / pow / SSAO の min を掛けて t8（SSAO 枠）へ出す。
//
// 構成は Unity HDRP の RTAO デノイザと同じ「trace → denoise → compose」。
// 重み関数は HDRP の BilateralFilter.hlsl（depth / normal / plane の 3 種）の移植。
//
// ★なぜ非線形化をここでやるのか
//   intensity の掛け算・pow・SSAO との min はどれも非線形なので、ノイズが乗った値に
//   先に掛けてから平均すると、収束先が真の AO からずれる（Jensen の不等式）。
//   RtAo.hlsl は生の可視率だけを返し、ここで初めて非線形化する。
//
// ★平面重み（planeWeight）が効きどころ
//   深度も法線も似ているのに実は別の面（平行に並んだ 2 枚の壁、階段の踏面）を
//   唯一捕まえられるのがこれ。これが無いと段の境界で AO が必ず滲む。
//
// ponytail: 8 タップ 1 パスのみ。分離可能フィルタにはしない（joint bilateral は
//   厳密には分離不可で、横→縦に分けるとシルエットの角で十字に滲む）。
//   à-trous の多段化は「時間蓄積を入れても足りない」と実測できてから。

#include "RtCommon.hlsli"

// 単位円上の低食い違い 8 点（ポアソン風）。定数添字で引けるよう [unroll] 前提。
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
    if (d >= 1.0) return 1.0;                    // 空は遮蔽されない

    float ao = gAoRaw.Load(int3(px, 0));

    // 半径 0 = フィルタ無効（設定で切ったとき、生の値をそのまま非線形化して返す）
    const float radiusPx = gAoDenoiseRadius;
    if (radiusPx > 0.0 && gGBufferValid > 0.5)
    {
        const float3 Nc = RtOctDecode(gGBuffer.Load(int3(px, 0)).xy);
        const float3 Pc = RtWorldFromDepth(i.pos.xy, d);
        const float  zc = RtViewZFromDepth(i.pos.xy, d);

        // タップ配置をピクセル + フレームで回す。固定すると 8 点の羽根模様が
        // バンディングとして焼き付く（時間ディザと同じ理由）。
        const float  ang = RT_2PI * frac(RtIgn(i.pos.xy) + gFrameIndex * 0.6180339887);
        const float2 rot = float2(cos(ang), sin(ang));

        float sum = ao, wsum = 1.0;   // 中心は必ず重み 1（SVGF の作法）

        [unroll]
        for (int k = 0; k < 8; ++k)
        {
            const float2 o = float2(kTap8[k].x * rot.x - kTap8[k].y * rot.y,
                                    kTap8[k].x * rot.y + kTap8[k].y * rot.x) * radiusPx;
            const int2   tp = px + int2(o);
            const float2 tpf = i.pos.xy + o;
            if (!RtInsideViewport(tpf)) continue;

            const float dt = gDepth.Load(int3(tp, 0));
            if (dt >= 1.0) continue;

            const float3 Nt = RtOctDecode(gGBuffer.Load(int3(tp, 0)).xy);
            const float3 Pt = RtWorldFromDepth(tpf, dt);
            const float  zt = RtViewZFromDepth(tpf, dt);

            // 深度: ビュー Z の相対差（SSGI.hlsl と同じ式。far が大きいシーンでも壊れない）
            const float wz = exp(-abs(zt - zc) / max(0.05 * abs(zc), 1e-3));
            // 法線: dot の 4 乗（AO は法線に鈍感。GI 用の 128 乗だと曲面で全く効かない）
            const float nc = RtSqr(RtSqr(max(0.0, dot(Nt, Nc))));
            // 平面: 接平面からのズレを距離で正規化（HDRP の PLANE_WEIGHT）
            const float3 dq = Pc - Pt;
            const float  d2 = dot(dq, dq);
            const float  pe = max(abs(dot(dq, Nt)), abs(dot(dq, Nc)));
            const float  wp = (d2 < 1e-4) ? 1.0
                            : RtSqr(max(0.0, 1.0 - 2.0 * pe / sqrt(d2)));
            // 距離ガウシアン（σ = 0.9 × 半径。HDRP と同じ）
            const float  wg = exp(-RtSqr(length(o) / max(0.9 * radiusPx, 1e-3)));

            const float w = wg * wz * nc * wp;
            sum  += gAoRaw.Load(int3(tp, 0)) * w;
            wsum += w;
        }
        ao = sum / max(wsum, 1e-5);
    }

    // ---- ここで初めて非線形化する（旧 RtAo.hlsl の末尾から移設）----
    ao = 1.0 - (1.0 - saturate(ao)) * saturate(gIntensity);
    ao = saturate(pow(saturate(ao), max(gAoPower, 0.01)));

    // 「大きな遮蔽 = RT / 1px 単位の細部 = SSAO」の合成。SSAO 無効時は白 1x1 が張られるので
    // min しても素通し（＝分岐を増やさずに済む）。
    if (gCombineSsao > 0.5)
        ao = min(ao, gSsao.Load(int3(px, 0)));

    return ao;
}
