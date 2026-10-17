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
// ★RT_SHADOW_RG=1 でビルドした版（RtShadowTrace_PS.cso）は影デノイザの入力用で、出力が
//   R16G16_FLOAT: x = 可視率（R8 版と同じ値）、y = 遮蔽物までの距離 t（遮蔽なし・空は 0）。
//   RT_SHADOW_RG を付けない版は従来のまま（R8。ビット一致）。
//
// VS は post/FullscreenTri.hlsli の FSTriVS（頂点バッファ不要のフルスクリーン三角形）。
#include "RtCommon.hlsli"

#ifdef RT_SHADOW_RG
#define SHADOW_OUT_T   float2
#define SHADOW_RET(v)  return float2((v), 0.0)
#else
#define SHADOW_OUT_T   float
#define SHADOW_RET(v)  return (v)
#endif

SHADOW_OUT_T PSMain(FSQuadVSOut i) : SV_TARGET
{
    const float2 px = i.pos.xy;                       // 出力 RT のピクセル中心座標
    const float  d  = gDepth.Load(int3(int2(px), 0));
    const float3 P  = RtWorldFromDepth(px, d);

    // 法線は深度の隣接 4 点から段差をまたがない側を選んで作る（ddx/ddy は縁ででたらめになり、
    // レイ始点のオフセットがジッタごとにずれて影の縁がちらつく。RtCommon.hlsli の同名関数を参照）。
    float3 N = RtNormalFromDepthRobust(px, P);

    if (!RtInsideViewport(px)) SHADOW_RET(1.0);   // エディタのパネル下に潜る領域は素通し
    if (d >= 1.0) SHADOW_RET(1.0);                // 空（背景）に影は落ちない

    // 太陽へ向かうベクトル（lightDir は光の進行方向）。
    const float3 L = normalize(-gLightDir);

    // 太陽に背を向けた面も**抜けずに撃つ**（法線を太陽側へ返す）。
    // ★以前は「フォワードの NdotL で既に真っ暗」として 1.0 を返していたが、それが正しいのは片面の面だけ。
    //   葉カードなどの両面描画はフォワードが法線を裏返して照らすので、裏向きの葉が「影なし・日向」になり、
    //   建物の陰にある木が点描状に光っていた（Bistro で正解のパストレーサーの 65 倍の明るさ、2026-10-14）。
    //   片面の不透明物の裏面は画面に出ないので、ここで撃っても絵は変わらない（レイが無駄になるだけ）。
    float NdotL = dot(N, L);
    if (NdotL < 0.0) { N = -N; NdotL = -NdotL; }

    // ★アクネ対策はワールド空間の実距離オフセットだけ（CSM の depthBias とは別物）。
    //   深度バイアスと違い「影が浮く（peter-panning）」が原理的に起きない。
    //   グレージング角ほど大きく押し出す（1/NdotL は真横で爆発するので線形項で頭打ち）。
    const float  offset = gNormalBias * (1.0 + 3.0 * (1.0 - NdotL));
    const float3 origin = P + N * offset;

#ifdef RT_SHADOW_RG
    // ★デノイザは「ノイズが時間で変わる」ことが前提（変わらないと分散が 0 に見えて素通しされ、時間蓄積も効かない）。
    //   gFrameJitter は TAA 有効時しか回らないので、TAA が無いときは常に回る gFrameIndex から位相を作る。
    const float phase = (gFrameJitter != 0.0) ? gFrameJitter : frac(gFrameIndex * 0.6180339887);
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    const float2 xi = float2(frac(magic.z * frac(dot(px + phase, magic.xy))),
                             RtHash(px, 3.71 + phase));
#else
    const float2 xi = float2(RtIgn(px), RtHash(px, 3.71 + gFrameJitter));
#endif
    const float3 dir = RtJitterSunDir(L, xi, gSunTan);

    const float tMax = (gMaxDistance > 0.0) ? gMaxDistance : RT_TMAX;
    const float vis  = RtTraceOcclusion(origin, dir, 0.0, tMax);

    return saturate(1.0 - (1.0 - vis) * saturate(gIntensity));
}
