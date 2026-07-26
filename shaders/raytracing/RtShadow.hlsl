// RtShadow.hlsl — DXR 1.1 inline raytracing による太陽の影（計画09 Step 2）。
//
// 出力は R8_UNORM（1=遮蔽なし）。**コンタクトシャドウとまったく同じ形式・同じ t11 枠**へ書く。
// フォワード PS の
//     shadow = min(shadow, g_contactShadow.Load(int3(input.positionSV.xy, 0)));
// は 1 行も変わらない。ルートシグネチャは 1 DWORD も増えない。
//
// ★CSM とのハイブリッド（重要）
//   TLAS には**スキンドと半透明が入っていない**（変形後頂点が GPU に無い / any-hit が要る）。
//   一方 CSM は「TLAS に入っていないものだけ」を描くように Application 側で切り替える
//   （Application::RenderDepthOnlyScene の skipRtCovered）。
//   つまり CSM と RT は**担当が排他**で、フォワードの min() が過不足なく合成する。
//   これをやらずに CSM を全部描いたままにすると、min() は「暗い方」を採るので
//   CSM のアクネ（本来明るいのに縞状に暗い）とカスケード境界の段差が**残ってしまう**。
//   RT 影の価値の半分はここにある。
//
// VS は post/FullscreenTri.hlsli の FSTriVS（頂点バッファ不要のフルスクリーン三角形）。
#include "RtCommon.hlsli"

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    const float2 px = i.pos.xy;                       // 出力 RT のピクセル中心座標
    const float  d  = gDepth.Load(int3(int2(px), 0));
    const float3 P  = RtWorldFromDepth(px, d);

    // ddx/ddy は分岐の前に評価する（quad 内で分岐すると未定義）。
    const float3 N = RtNormalFromWorldPos(P);

    if (!RtInsideViewport(px)) return 1.0;   // エディタのパネル下に潜る領域は素通し
    if (d >= 1.0) return 1.0;                // 空（背景）に影は落ちない

    // 太陽へ向かうベクトル（lightDir は光の進行方向）。
    const float3 L = normalize(-gLightDir);

    // 太陽に背を向けた面はフォワードの NdotL で既に真っ暗。
    // ここで影を重ねると輪郭に自己遮蔽のシミが出るだけなので抜ける。
    const float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 1.0;

    // ★アクネ対策はワールド空間の実距離オフセットだけ（CSM の depthBias とは別物）。
    //   深度バイアスと違い「影が浮く（peter-panning）」が原理的に起きない。
    //   グレージング角ほど大きく押し出す（1/NdotL は真横で爆発するので線形項で頭打ち）。
    const float  offset = gNormalBias * (1.0 + 3.0 * (1.0 - NdotL));
    const float3 origin = P + N * offset;

    const float2 xi = float2(RtIgn(px), RtHash(px, 3.71 + gFrameJitter));
    const float3 dir = RtJitterSunDir(L, xi, gSunTan);

    const float tMax = (gMaxDistance > 0.0) ? gMaxDistance : RT_TMAX;
    const float vis  = RtTraceOcclusion(origin, dir, 0.0, tMax);

    return saturate(1.0 - (1.0 - vis) * saturate(gIntensity));
}
