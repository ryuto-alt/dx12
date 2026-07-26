// ContactShadow.hlsl - 深度バッファをスクリーン空間でレイマーチする近接遮蔽（コンタクトシャドウ）。
//
// CSM(4カスケード)は解像度の都合で「物と地面が接している所」の細かい影が抜ける。
// そこだけを深度バッファのレイマーチで補う。太陽（平行光）専用。
//
// VS は post/FullscreenTri.hlsli の FSTriVS を流用（頂点バッファ不要のフルスクリーン三角形）。
// PS はカメラ深度(R32_FLOAT)を読み、ピクセルのビュー空間位置から太陽方向へ短いレイを飛ばして、
// 深度バッファに写っている物体に当たったら遮蔽とみなす。
// 出力は R8_UNORM（1=遮蔽なし, 0=完全遮蔽）。Forward の PS が太陽の寄与へ乗算する。
//
// 実装の要点（リサーチで確認した実用値・定石）:
//  - レイ長は「接触」スケールの短距離(既定 0.25m)。長くするほどサンプル間隔が粗くなり
//    ノイズが増える（UE のドキュメントも「長さを伸ばすほどノイズが増える」と明記）。
//  - ステップ数は 16 前後が品質/性能の相場。
//  - サンプル位置は interleaved gradient noise でディザして、少ないステップ数でも
//    縞（バンディング）が出ないようにする。TAA が無いので時間方向のジッタは入れない
//    （毎フレーム乱数を変えるとチラつくだけになる）。
//  - 厚み(thickness)判定は必須。深度バッファは「表面」しか持っていないので、
//    これが無いと遠景の物体が画面手前の全ピクセルを無限に遮蔽してしまう。
//    逆に厚みを超えた差分は「その物体の裏側を通り抜けた」とみなしてマーチを続ける
//    （march-behind-surfaces。コンタクトシャドウでは必須の挙動）。
//  - 自己遮蔽(self-shadow acne)対策は 3 段構え:
//      ① 深度から法線を再構成し、太陽に背を向けた面(N・L<=0)は即 1.0 で返す
//         （どのみち Forward 側の NdotL で真っ暗なので、ここで影を足す意味が無い）
//      ② レイ始点を法線方向へ押し出す
//      ③ 深度差が bias 以下のヒットは「自分自身」とみなして無視する
#include "../post/FullscreenTri.hlsli"   // FSTriVS / FSQuadVSOut(uv)

Texture2D<float> g_depth      : register(t0);  // カメラ深度 (R32_FLOAT, m_depthSrvIndex)
SamplerState     g_pointClamp : register(s0);  // POINT / CLAMP

// ContactShadowPass.cpp の ContactShadowParamsCB とバイト単位で一致させること。
cbuffer ContactShadowParams : register(b0)
{
    float4x4 proj;         // 64B  ビュー空間 → クリップ（転置済み: mul(row, proj)）
    float4x4 invProj;      // 64B  クリップ → ビュー（転置済み）
    float2   invRTSize;    // 1/width, 1/height
    float    rayLength;    // レイ長(ビュー空間 = メートル)
    float    thickness;    // 遮蔽とみなす深度差の上限(m)
    float    bias;         // 自己遮蔽バイアス(m)
    float    intensity;    // 遮蔽の強さ(0..1 相当)
    int      steps;        // レイマーチのステップ数
    float    maxDistance;  // この距離(m)を超えたらフェードアウト
    float    fadeDistance; // フェードにかける距離(m)
    float3   _pad0;
    float4   viewport;     // xy=ビューポート原点(px), zw=ビューポートサイズ(px)
    float4   lightDirVS;   // xyz = ビュー空間で「ライトへ向かう」単位ベクトル
};

// フル RT の UV(0..1) → サブビューポート基準の NDC(-1..1, y上向き)。SSAO.hlsl と同じ規約。
float2 UVToNDC(float2 uv)
{
    float2 px = uv / max(invRTSize, 1e-6);
    float2 vpUV = (px - viewport.xy) / viewport.zw;
    return float2(vpUV.x * 2.0 - 1.0, 1.0 - vpUV.y * 2.0);
}

// サブビューポート基準の NDC → 深度テクスチャをサンプルするフル RT UV(0..1)。
float2 NDCToUV(float2 ndc)
{
    float2 vpUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float2 px = viewport.xy + vpUV * viewport.zw;
    return px * invRTSize;
}

float3 ViewPosFromDepth(float2 uv, float depth)
{
    float2 ndc = UVToNDC(uv);
    float4 clip = float4(ndc, depth, 1.0);
    float4 view = mul(clip, invProj);
    return view.xyz / view.w;
}

// Interleaved gradient noise（Jorge Jimenez）。ピクセル座標だけで決まる空間ディザ。
float InterleavedGradientNoise(float2 px)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(px, magic.xy)));
}

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    float2 px = i.uv / max(invRTSize, 1e-6);
    float  d  = g_depth.Sample(g_pointClamp, i.uv);
    float3 P  = ViewPosFromDepth(i.uv, d);

    // 深度から法線を再構成（SSAO.hlsl と同じ勾配の外積。LH なので視線へ向くよう符号を整える）。
    // ddx/ddy は quad 内で分岐すると値が未定義になるので、早期リターンより前に必ず評価する。
    float3 N = normalize(cross(ddx(P), ddy(P)));
    if (dot(N, -P) < 0.0) N = -N;

    // ビューポート矩形の外（エディタのパネル下に潜る領域）は素通し。
    if (px.x < viewport.x || px.x > viewport.x + viewport.z ||
        px.y < viewport.y || px.y > viewport.y + viewport.w)
        return 1.0;

    if (d >= 1.0)
        return 1.0;   // 背景（クリア値）に影は落ちない

    // カメラから遠いピクセルはフェードアウト（スクリーン空間の精度が落ちる領域を切る）。
    float distFade = 1.0 - saturate((P.z - maxDistance) / max(fadeDistance, 1e-4));
    if (distFade <= 0.0)
        return 1.0;

    float3 L = normalize(lightDirVS.xyz);

    // ① 太陽に背を向けた面は Forward 側の NdotL で既に真っ暗。ここで影を重ねると
    //    輪郭に自己遮蔽のシミが出るだけなので早期に抜ける。
    float NdotL = dot(N, L);
    if (NdotL <= 0.0)
        return 1.0;

    // ② レイ始点を法線方向へ押し出す。グレージング角ほど大きく（1/NdotL ではなく
    //    (1-NdotL) の線形項で頭打ちにして、真横から当たる面で爆発しないようにする）。
    float startOffset = bias * (1.0 + 2.0 * (1.0 - NdotL));
    float3 origin = P + N * startOffset;

    int   stepCount = clamp(steps, 4, 32);
    float invSteps  = 1.0 / (float)stepCount;
    float jitter    = InterleavedGradientNoise(px);   // ③ 空間ディザで縞を散らす

    float occluded = 0.0;
    float hitFade  = 1.0;

    [loop]
    for (int k = 1; k <= stepCount; ++k)
    {
        // 最初のサンプルでも必ず 1 ステップ以上進む（k=1..N、ジッタは 0..1 ステップ分）。
        float  t = rayLength * ((float)k + jitter) * invSteps;
        float3 samplePos = origin + L * t;

        float4 clip = mul(float4(samplePos, 1.0), proj);
        if (clip.w <= 0.0) break;                     // カメラ後方へ抜けた
        float2 sndc = clip.xy / clip.w;
        if (any(abs(sndc) > 1.0)) break;              // 画面外へ抜けた＝これ以上は判定できない
        float2 suv = NDCToUV(sndc);

        float sd = g_depth.Sample(g_pointClamp, suv);
        if (sd >= 1.0) continue;                      // 背景は遮蔽物ではない

        float sceneZ = ViewPosFromDepth(suv, sd).z;
        float dz = samplePos.z - sceneZ;              // >0 = レイが表面より奥にいる

        if (dz > bias)
        {
            if (dz < thickness)
            {
                // 遮蔽ヒット。画面端に近いヒットは信頼できないので、その分だけ弱める。
                float2 edge = min(sndc.xy + 1.0, 1.0 - sndc.xy);   // 0=端, 1=中央寄り
                hitFade  = smoothstep(0.0, 0.2, min(edge.x, edge.y));
                occluded = 1.0;
                break;
            }
            // 厚みを超えた深度差 = その物体の「裏側」を通っただけ。
            // ここで打ち切ると近接遮蔽が拾えなくなるのでマーチを続ける
            // （march-behind-surfaces）。
        }
    }

    float shadow = 1.0 - occluded * saturate(intensity) * hitFade * distFade;
    return saturate(shadow);
}
