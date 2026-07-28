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
    // ★法線は G-Buffer の oct 法線を使う。ddx/ddy 版は面法線しか取れず quad 境界で壊れるので
    //   デノイザの bilateral 重みと食い違う。G-Buffer が無いフレームだけ従来の微分にフォールバック。
    //   （分岐より前に評価すること＝ddx/ddy は quad 内で分岐すると未定義）
    const float3 Nddx = RtNormalFromWorldPos(P);
    const float3 N    = (gGBufferValid > 0.5)
                      ? RtOctDecode(gGBuffer.Load(int3(int2(px), 0)).xy) : Nddx;

    if (!RtInsideViewport(px)) return 1.0;
    if (d >= 1.0) return 1.0;                    // 空は遮蔽されない

    const int rays = clamp((int)gAoRayCount, 1, 8);
    const float radius = max(gAoRadius, 1e-3);

    // 自己交差回避。半径に対する相対量で決めると、巨大スケールのシーンでも破綻しない。
    const float3 origin = P + N * max(gNormalBias, radius * 0.01);

    // ★時間方向にも回す。ここを固定すると毎フレーム同じレイ方向になり、デノイザが
    //   いくら平均しても分散が 1 ミリも下がらない（SSGI.hlsl:70-73 が同じ罠の記録）。
    //   黄金比オフセットは低食い違い列なので少ないフレーム数でもよく散る。
    const float ign = frac(RtIgn(px) + gFrameIndex * 0.6180339887);
    float occluded = 0.0;

    [loop]
    for (int k = 0; k < rays; ++k)
    {
        // ストラティファイした 1 次元 + R2 低食い違い列の 2 次元でコサイン重み半球を張る。
        // （旧: RtHash の sin ベース擬似乱数。白ノイズなうえ GPU 間で sin の精度が違う）
        float2 xi;
        xi.x = frac(((float)k + ign) / (float)rays);
        xi.y = frac(ign * 1.6180339887 + (float)k * 0.7548776662);
        float3 dir = RtCosineHemisphere(N, xi);
        // コサイン重みサンプルなので、遮蔽率はヒット数の単純平均でよい
        // （1/π の正規化と cos の重みが打ち消し合う）。
        occluded += 1.0 - RtTraceOcclusion(origin, dir, 0.0, radius);
    }

    // ★ここでは「生の可視率」だけを返す。intensity / pow / SSAO の min は
    //   デノイズ後（RtAoDenoise.hlsl）に掛ける。どれも非線形なので、ノイズが乗った値に
    //   先に掛けてから平均すると収束先が真の AO からずれる（Jensen の不等式）。
    //   Unity HDRP も Trace → Denoise → Compose の順で同じ分け方をしている。
    return saturate(1.0 - occluded / (float)rays);
}
