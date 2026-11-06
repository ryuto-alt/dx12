// SSAO.hlsl - GTAO（Jimenez 2016 "Practical Real-Time Strategies for Accurate Indirect Occlusion"）。
// VS は post/FullscreenTri.hlsli の FSTriVS を流用（頂点バッファ不要のフルスクリーン三角形）。
// PS はカメラ深度(R32_FLOAT)から位置・法線を復元し、画素ごとに 2 スライス × 各側 stepCount ステップで
// ホライズン角を探して、余弦重み付きの解析解（論文の式 10）で可視率を積分する。
// 出力は R8_UNORM の AO（1=遮蔽なし, 0=完全遮蔽）。
#include "../post/FullscreenTri.hlsli"   // FSTriVS / FSQuadVSOut(uv)

Texture2D<float> g_depth      : register(t0);  // カメラ深度 (R32_FLOAT, m_depthSrvIndex)
SamplerState     g_pointClamp : register(s0);  // POINT / CLAMP

// SSAO パラメータ（独立 CB / b0）。SSAOPass.cpp の SSAOParamsCB とバイト一致させること。
cbuffer SSAOParams : register(b0)
{
    float4x4 proj;       // 64B  ビュー空間 → クリップ（転置済み: mul(row, proj)）
    float4x4 invProj;    // 64B  クリップ → ビュー（転置済み）
    float2   invRTSize;  // 1/width, 1/height
    float    radius;     // ワールド(ビュー)空間半径
    float    bias;       // ホライズン下限(cos)のオフセット（自己遮蔽の回避）
    float    intensity;  // AO 強度スケール
    float    power;      // pow() のべき指数
    float    nearZ;      // カメラ近接面
    float    farZ;       // カメラ遠方面
    int      stepCount;  // 片側あたりのステップ数
    uint     frameSeq;   // ノイズ位相用フレーム連番（TAA 無効時は 0）
    float2   _pad;
    float4   viewport;   // xy=ビューポート原点(px), zw=ビューポートサイズ(px)
};

static const float kPi     = 3.14159265359;
static const float kHalfPi = 1.57079632679;

// サブビューポート基準の NDC（フル RT の UV → NDC、y 上向き）。
float2 UVToNDC(float2 uv)
{
    float2 px = uv / max(invRTSize, 1e-6);
    float2 vpUV = (px - viewport.xy) / viewport.zw;
    return float2(vpUV.x * 2.0 - 1.0, 1.0 - vpUV.y * 2.0);
}

// ビュー空間位置を深度とスクリーン UV から復元（透視/正射いずれも invProj で復元）。
float3 ViewPosFromDepth(float2 uv, float depth)
{
    float2 ndc = UVToNDC(uv);
    float4 clip = float4(ndc, depth, 1.0);
    float4 view = mul(clip, invProj);
    return view.xyz / view.w;
}

// 整数ピクセル座標の深度（ビューポート矩形内へクランプ）。
float LoadDepth(float2 pxi)
{
    pxi = clamp(pxi, viewport.xy, viewport.xy + viewport.zw - 1.0);
    return g_depth.SampleLevel(g_pointClamp, (pxi + 0.5) * invRTSize, 0);
}

float3 ViewPosAt(float2 pxi)
{
    pxi = clamp(pxi, viewport.xy, viewport.xy + viewport.zw - 1.0);
    float2 uv = (pxi + 0.5) * invRTSize;
    return ViewPosFromDepth(uv, g_depth.SampleLevel(g_pointClamp, uv, 0));
}

// Interleaved Gradient Noise（Jimenez 2014）。
float IGN(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float FastACos(float x)
{
    return acos(clamp(x, -1.0, 1.0));
}

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    // ビューポート矩形の外（エディタのパネル下に潜る領域）は AO なしで素通し。
    float2 pxf = i.uv / max(invRTSize, 1e-6);
    if (pxf.x < viewport.x || pxf.x > viewport.x + viewport.z ||
        pxf.y < viewport.y || pxf.y > viewport.y + viewport.w)
        return 1.0;

    float2 pxi = floor(pxf);
    float  d   = LoadDepth(pxi);
    if (d >= 1.0)
        return 1.0;   // 背景（クリア値）は AO なし

    const float3 P = ViewPosAt(pxi);

    // ---- 頑健な法線再構成: 左右・上下それぞれ「深度差の小さい側」を選んで外積 ----
    float3 pL = ViewPosAt(pxi + float2(-1, 0));
    float3 pR = ViewPosAt(pxi + float2( 1, 0));
    float3 pU = ViewPosAt(pxi + float2(0, -1));
    float3 pD = ViewPosAt(pxi + float2(0,  1));
    float3 dx = (abs(pR.z - P.z) < abs(P.z - pL.z)) ? (pR - P) : (P - pL);
    float3 dy = (abs(pD.z - P.z) < abs(P.z - pU.z)) ? (pD - P) : (P - pU);

    // 視線ベクトル（P → カメラ）。透視は -normalize(P)、正射は固定。
    const bool isPersp = abs(proj[2][3]) > 0.5;
    const float3 V = isPersp ? -normalize(P) : float3(0.0, 0.0, -1.0);

    float3 N = cross(dx, dy);
    float  nLen = length(N);
    N = (nLen > 1e-12) ? N / nLen : V;
    if (dot(N, V) < 0.0) N = -N;

    // ---- 半径のスクリーン投影（px）----
    float pixelRadius = isPersp ? (radius * proj[0][0] * 0.5 * viewport.z / max(P.z, 1e-4))
                                : (radius * proj[0][0] * 0.5 * viewport.z);
    pixelRadius = min(pixelRadius, viewport.z * 0.25);   // 画面幅の 1/4 でクランプ
    if (pixelRadius < 2.0)
        return 1.0;

    // ---- ノイズ（IGN + フレームごとに回す黄金比位相）----
    const float framePhase  = frac(float(frameSeq & 0xFFFFu) * 0.61803398875);
    const float framePhase2 = frac(float(frameSeq & 0xFFFFu) * 0.75487766625);
    const float noiseSlice  = frac(IGN(pxi) + framePhase);
    const float noiseStep   = frac(IGN(pxi + float2(5.588238, 5.588238)) + framePhase2);

    const int   steps = clamp(stepCount, 2, 8);
    const float falloffRange = radius * 0.3;            // 半径の外側 30% でフェード
    const float falloffFrom  = radius - falloffRange;
    const float falloffMul   = -1.0 / max(falloffRange, 1e-5);
    const float falloffAdd   = radius / max(falloffRange, 1e-5);

    const int kSlices = 2;
    float visibility = 0.0;

    [unroll]
    for (int slice = 0; slice < kSlices; ++slice)
    {
        float phi = (noiseSlice + (float)slice / (float)kSlices) * kPi;
        float2 dirPx = float2(cos(phi), sin(phi));            // ピクセル空間（y 下向き）
        float3 dirView = float3(dirPx.x, -dirPx.y, 0.0);      // ビュー空間（y 上向き）

        // スライス平面 = span(u, V)。u は V に直交する平面内の方向。
        float3 u = dirView - V * dot(dirView, V);
        u = normalize(u);

        // 法線をスライス平面へ射影し、V からの角度 n（+u 側が正）と長さを得る。
        float nx = dot(N, u);
        float nz = dot(N, V);
        float projLen = sqrt(nx * nx + nz * nz);
        float n = atan2(nx, nz);
        float cosN = saturate(nz / max(projLen, 1e-5));
        float sinN = sin(n);

        // ホライズン cos の初期値 = 接平面（n ± π/2）。bias で下限を持ち上げて自己遮蔽を避ける。
        float lowCosPos = clamp(-sinN + bias, -1.0, 1.0);   // +u 側: cos(n + π/2) = -sin(n)
        float lowCosNeg = clamp( sinN + bias, -1.0, 1.0);   // -u 側: cos(n - π/2) =  sin(n)
        float hCosPos = lowCosPos;
        float hCosNeg = lowCosNeg;

        [loop]
        for (int s = 0; s < steps; ++s)
        {
            float t = ((float)s + noiseStep) / (float)steps;
            t *= t;                                               // 近傍に寄せる
            float off = max(t * pixelRadius, (float)s + 1.0);     // 最低でも 1px ずつ進める

            [unroll]
            for (int side = 0; side < 2; ++side)
            {
                float sgn = (side == 0) ? 1.0 : -1.0;
                float2 spx = floor(pxf + dirPx * (off * sgn));
                if (spx.x < viewport.x || spx.x >= viewport.x + viewport.z ||
                    spx.y < viewport.y || spx.y >= viewport.y + viewport.w)
                    continue;
                float2 suv = (spx + 0.5) * invRTSize;
                float  sd  = g_depth.SampleLevel(g_pointClamp, suv, 0);
                if (sd >= 1.0) continue;                          // 空は遮蔽しない

                float3 S = ViewPosFromDepth(suv, sd);
                float3 delta = S - P;
                float  dist  = length(delta);
                if (dist < 1e-5) continue;
                float shc = dot(delta / dist, V);

                // 距離による減衰（半径の外側でホライズンを下限へ戻す）
                float w = saturate(dist * falloffMul + falloffAdd);
                if (dist <= falloffFrom) w = 1.0;

                if (side == 0)
                {
                    shc = lerp(lowCosPos, shc, w);
                    // 薄い物ヒューリスティック: ホライズンが急に下がるサンプルは前のホライズンへ寄せる
                    hCosPos = (shc > hCosPos) ? shc : lerp(hCosPos, shc, 0.15);
                }
                else
                {
                    shc = lerp(lowCosNeg, shc, w);
                    hCosNeg = (shc > hCosNeg) ? shc : lerp(hCosNeg, shc, 0.15);
                }
            }
        }

        // ホライズン角（-u 側は負）。法線の半球 n ± π/2 にクランプ。
        float h0 = -FastACos(hCosNeg);
        float h1 =  FastACos(hCosPos);
        h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
        h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);

        // 余弦重み付きの解析解（式 10）
        float arc0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;
        float arc1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;
        visibility += projLen * (arc0 + arc1);
    }

    float ao = saturate(visibility / (float)kSlices);
    ao = saturate(1.0 - (1.0 - ao) * intensity);
    return pow(ao, power);
}
