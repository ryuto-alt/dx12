// RtAo.hlsl — DXR 1.1 inline raytracing による環境遮蔽（計画09 Step 3）。
//
// 出力は R8_UNORM（1=遮蔽なし）。**SSAO とまったく同じ形式・同じ t8 枠**へ書くので、
// フォワード PS は 1 行も変わらない。ルートシグネチャ増分ゼロ。
//
// SSAO との違い（これが RT-AO を入れる理由）:
//   SSAO は深度バッファ＝「画面に映っている表面」しか遮蔽物にできない。
//   画面外の壁・カメラに背を向けた面・手前の物の裏側は原理的に遮蔽に数えられず、
//   カメラを回すと AO がぬるっと変わる。RT-AO はシーン全体の BVH を撃つのでこれが無い。
//   逆に 1 ピクセル単位の細かい皺の遮蔽は苦手（レイ本数の都合）なので、
//   RtSettings::aoCombineWithSsao で SSAO と min() 合成できるようにしてある。
#include "RtCommon.hlsli"

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    const float2 px = i.pos.xy;
    const float  d  = gDepth.Load(int3(int2(px), 0));
    const float3 P  = RtWorldFromDepth(px, d);
    const float3 N  = RtNormalFromWorldPos(P);   // 分岐より前（ddx/ddy）

    if (!RtInsideViewport(px)) return 1.0;
    if (d >= 1.0) return 1.0;                    // 空は遮蔽されない

    const int rays = clamp((int)gAoRayCount, 1, 8);
    const float radius = max(gAoRadius, 1e-3);

    // 自己交差回避。半径に対する相対量で決めると、巨大スケールのシーンでも破綻しない。
    const float3 origin = P + N * max(gNormalBias, radius * 0.01);

    const float ign = RtIgn(px);
    float occluded = 0.0;

    [loop]
    for (int k = 0; k < rays; ++k)
    {
        // ストラティファイした 1 次元 + ハッシュの 2 次元でコサイン重み半球を張る。
        float2 xi;
        xi.x = frac(((float)k + ign) / (float)rays);
        xi.y = RtHash(px, 7.31 * (float)(k + 1) + gFrameJitter);
        float3 dir = RtCosineHemisphere(N, xi);
        // コサイン重みサンプルなので、遮蔽率はヒット数の単純平均でよい
        // （1/π の正規化と cos の重みが打ち消し合う）。
        occluded += 1.0 - RtTraceOcclusion(origin, dir, 0.0, radius);
    }

    float ao = 1.0 - saturate(occluded / (float)rays) * saturate(gIntensity);
    ao = saturate(pow(saturate(ao), max(gAoPower, 0.01)));

    // 「大きな遮蔽 = RT / 1px 単位の細部 = SSAO」の合成。SSAO 無効時は白 1x1 が張られるので
    // min しても素通し（＝分岐を増やさずに済む）。
    if (gCombineSsao > 0.5)
        ao = min(ao, gSsao.Load(int3(int2(px), 0)));

    return ao;
}
