// SSAO.hlsl - 深度から法線を再構築する半球カーネル SSAO。
// VS は post/FullscreenTri.hlsli の FSTriVS を流用（頂点バッファ不要のフルスクリーン三角形）。
// PS はカメラ深度(R32_FLOAT)を読み、ビュー空間の位置・法線を復元して半球カーネルで遮蔽を測る。
// 出力は R8_UNORM の AO（1=遮蔽なし, 0=完全遮蔽）。
#include "../post/FullscreenTri.hlsli"   // FSTriVS / FSQuadVSOut(uv)

Texture2D<float> g_depth      : register(t0);  // カメラ深度 (R32_FLOAT, m_depthSrvIndex)
SamplerState     g_pointClamp : register(s0);  // POINT / CLAMP

// SSAO パラメータ（独立 CB / b0）。SSAOPass.cpp の SSAOParams とバイト一致させること。
cbuffer SSAOParams : register(b0)
{
    float4x4 proj;       // 64B  ビュー空間 → クリップ（転置済み: mul(row, proj)）
    float4x4 invProj;    // 64B  クリップ → ビュー（転置済み）
    float2   invRTSize;  // 1/width, 1/height
    float    radius;     // ワールド(ビュー)空間半径
    float    bias;       // 自己遮蔽バイアス
    float    intensity;  // AO 強度スケール
    float    power;      // pow() のべき指数
    float    nearZ;      // カメラ近接面
    float    farZ;       // カメラ遠方面
    int      sampleCount;// 有効サンプル数（<=16）
    float    _pad0;
    float4   viewport;   // xy=ビューポート原点(px), zw=ビューポートサイズ(px)
    float4   kernel[16]; // 半球カーネル(xyz)。w は未使用
};

// フル RT の UV(0..1) → サブビューポート基準の NDC(-1..1, y上向き)。
// ジオメトリは深度バッファの矩形 viewport.xy/.zw にしか描かれていないため、
// 全面 UV をそのまま NDC へ等価マップせず、サブ矩形基準で変換する（エディタ視点で歪み回避）。
float2 UVToNDC(float2 uv)
{
    float2 px = uv / max(invRTSize, 1e-6);              // フル RT のピクセル座標
    float2 vpUV = (px - viewport.xy) / viewport.zw;     // サブ矩形内 0..1
    return float2(vpUV.x * 2.0 - 1.0, 1.0 - vpUV.y * 2.0);
}

// サブビューポート基準の NDC(-1..1, y上向き) → 深度テクスチャをサンプルするフル RT UV(0..1)。
float2 NDCToUV(float2 ndc)
{
    float2 vpUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);  // サブ矩形内 0..1
    float2 px = viewport.xy + vpUV * viewport.zw;       // フル RT のピクセル座標
    return px * invRTSize;
}

// ビュー空間位置を深度とスクリーン UV から復元（透視/正射いずれも invProj で復元）。
float3 ViewPosFromDepth(float2 uv, float depth)
{
    float2 ndc = UVToNDC(uv);
    float4 clip = float4(ndc, depth, 1.0);
    float4 view = mul(clip, invProj);   // 転置済み invProj に行ベクトルを掛ける
    return view.xyz / view.w;
}

float PSMain(FSQuadVSOut i) : SV_TARGET
{
    // ビューポート矩形の外（エディタのパネル下に潜る領域）は AO なしで素通し。
    float2 px = i.uv / max(invRTSize, 1e-6);
    if (px.x < viewport.x || px.x > viewport.x + viewport.z ||
        px.y < viewport.y || px.y > viewport.y + viewport.w)
        return 1.0;

    float d = g_depth.Sample(g_pointClamp, i.uv);
    if (d >= 1.0)
        return 1.0;   // 背景（クリア値）は AO なし

    float3 P = ViewPosFromDepth(i.uv, d);

    // 深度から法線を再構築（隣接ピクセルの位置勾配の外積）。
    // LH 投影で z 前方なので符号を整え、視線へ向く法線にする。
    float3 N = normalize(cross(ddx(P), ddy(P)));
    if (dot(N, -P) < 0.0) N = -N;

    // 回転ノイズ（手続き生成）で TBN を回し、少ないサンプルでもバンディングを散らす。
    float  rnd = frac(sin(dot(px, float2(12.9898, 78.233))) * 43758.5453);  // px=ピクセル座標
    float  ang = rnd * 6.2831853;
    float3 randVec = float3(cos(ang), sin(ang), 0.0);

    float3 T = normalize(randVec - N * dot(randVec, N));
    float3 B = cross(N, T);
    float3x3 TBN = float3x3(T, B, N);

    int count = min(sampleCount, 16);
    float occ = 0.0;
    [loop]
    for (int k = 0; k < count; ++k)
    {
        // 半球カーネルをビュー空間へ展開
        float3 smp = P + mul(kernel[k].xyz, TBN) * radius;

        // サンプル位置をクリップ→サブビューポート基準の NDC→フル RT UV へ
        float4 clip = mul(float4(smp, 1.0), proj);
        if (clip.w <= 0.0) continue;
        float2 sndc = clip.xy / clip.w;
        if (sndc.x < -1.0 || sndc.x > 1.0 || sndc.y < -1.0 || sndc.y > 1.0) continue;
        float2 suv = NDCToUV(sndc);

        // その UV の実深度からビュー Z を復元して遮蔽判定
        float sd = g_depth.Sample(g_pointClamp, suv);
        float sceneViewZ = ViewPosFromDepth(suv, sd).z;

        // レンジチェック（離れすぎたヒットは無視）
        float range = smoothstep(0.0, 1.0, radius / max(abs(P.z - sceneViewZ), 1e-4));
        // LH: より手前(=小さい view z)に遮蔽物。bias で自己遮蔽を緩和。
        occ += ((sceneViewZ <= smp.z - bias) ? 1.0 : 0.0) * range;
    }

    float ao = 1.0 - (occ / (float)count) * intensity;
    ao = saturate(ao);
    return pow(ao, power);
}
