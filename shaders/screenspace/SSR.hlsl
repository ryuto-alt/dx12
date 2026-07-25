// SSR.hlsl - スクリーン空間反射。
//
// TracePS   : ハーフ解像度。McGuire & Mara のスクリーン空間 DDA で深度バッファを辿り、
//             当たった点の「前フレームカラー」を反射放射輝度として返す。
//             出力 rgb=放射輝度 / a=confidence（0=完全に IBL へフォールバック）。
// UpsamplePS: フル解像度。ハーフの 3x3 を深度・法線考慮の joint bilateral で持ち上げる。
//
// 出典: Efficient GPU Screen-Space Ray Tracing (McGuire & Mara, JCGT 3(4), 2014)
//       https://jcgt.org/published/0003/04/04/paper.pdf
//       同次スクリーン空間で 2D 直線をラスタライズし 1/w を線形補間する＝透視補正が入るので、
//       ビュー空間の等間隔マーチのようなオーバーステップ/アンダーステップが原理的に起きない。
//
// ★ビューポート規約: シーンはフル RT のサブ矩形に描かれている（エディタのシーンビュー）。
//   ゲームモードは (0,0,w,h) なので、ここを間違えてもゲームでは再現しない。必ずエディタで検証すること。
#include "../post/FullscreenTri.hlsli"
#include "ScreenSpaceParams.hlsli"

// ---------------------------------------------------------------------------
// スクリーン空間 DDA。ヒットしたら true。
//   originVS/dirVS はビュー空間（LH: 前方 +Z）。stride はフル解像度ピクセル単位。
// ---------------------------------------------------------------------------
bool SS_TraceDDA(float3 originVS, float3 dirVS, float maxDistance, float thickness,
                 float stride, int maxSteps, float jitter,
                 out float2 hitUV, out float hitViewZ)
{
    hitUV = 0.0; hitViewZ = 0.0;

    const float zNear = gMisc.x;

    // レイの終点を near 平面の手前でクリップ
    float rayLen = maxDistance;
    if (dirVS.z < -1e-6 && originVS.z + dirVS.z * rayLen < zNear)
        rayLen = (zNear - originVS.z) / dirVS.z;
    if (rayLen <= 1e-4) return false;

    float3 p0 = originVS;
    float3 p1 = originVS + dirVS * rayLen;

    float4 h0 = mul(float4(p0, 1.0), gProj);
    float4 h1 = mul(float4(p1, 1.0), gProj);
    if (h0.w <= 1e-6 || h1.w <= 1e-6) return false;

    float k0 = 1.0 / h0.w, k1 = 1.0 / h1.w;

    // サブ矩形基準の NDC → フル RT のピクセル座標
    float2 s0 = SS_NDCToUV(h0.xy * k0, SS_INV_RT, gViewport) / max(SS_INV_RT, 1e-6);
    float2 s1 = SS_NDCToUV(h1.xy * k1, SS_INV_RT, gViewport) / max(SS_INV_RT, 1e-6);

    // 退化（始点と終点が同一ピクセル）を避ける
    if (dot(s1 - s0, s1 - s0) < 0.0001) s1 += float2(0.01, 0.01);
    float2 delta = s1 - s0;

    // ★McGuire の肝: |dx| < |dy| なら x/y を入れ替えて「必ず x が主軸」にする
    bool permute = false;
    if (abs(delta.x) < abs(delta.y)) { permute = true; delta = delta.yx; s0 = s0.yx; s1 = s1.yx; }

    float  stepDir = (delta.x >= 0.0) ? 1.0 : -1.0;
    float  invdx   = stepDir / delta.x;
    float2 dP      = float2(stepDir, delta.y * invdx);
    // ビュー空間位置 * k と k(=1/w) は x に対して線形補間できる（透視補正の本体）
    float3 Q0 = p0 * k0, Q1 = p1 * k1;
    float3 dQ = (Q1 - Q0) * invdx;
    float  dk = (k1 - k0) * invdx;

    dP *= stride; dQ *= stride; dk *= stride;

    float2 P = s0 + dP * jitter;
    float3 Q = Q0 + dQ * jitter;
    float  k = k0 + dk * jitter;

    float prevZ = p0.z;
    int   steps = clamp(maxSteps, 8, 128);

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        P += dP; Q += dQ; k += dk;
        if (k <= 1e-6) return false;

        float2 pxy = permute ? P.yx : P;
        if (!SS_InViewport(pxy, gViewport)) return false;

        // このステップ区間の view z レンジ [zMin, zMax]（LH: 大きいほど奥）
        float zMin = prevZ;
        float zMax = Q.z / k;
        prevZ = zMax;
        if (zMin > zMax) { float t = zMin; zMin = zMax; zMax = t; }

        float2 suv = pxy * SS_INV_RT;
        float  sd  = g_depth.SampleLevel(g_pointClamp, suv, 0);
        if (sd >= 1.0) continue;                   // 背景（クリア値）は遮蔽物ではない
        float sceneZ = SS_ViewPosFromDepth(suv, sd, gInvProj, SS_INV_RT, gViewport).z;

        // 区間が実表面をまたいだか。thickness で「物体の裏を突き抜けただけ」を除外する。
        if (zMax >= sceneZ && zMin <= sceneZ + thickness)
        {
            hitUV    = suv;
            hitViewZ = sceneZ;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// トレース（ハーフ解像度 RT へ描く。UV は解像度非依存なのでフル RT と共有できる）
// ---------------------------------------------------------------------------
float4 TracePS(FSQuadVSOut i) : SV_TARGET
{
    float2 fullPx = i.uv / max(SS_INV_RT, 1e-6);
    if (!SS_InViewport(fullPx, gViewport)) return 0.0;

    float d = g_depth.SampleLevel(g_pointClamp, i.uv, 0);
    if (d >= 1.0) return 0.0;                       // 背景は反射しない

    SSSurface surf = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, i.uv, 0));
    const float roughnessCutoff = gSsr1.y;
    if (surf.roughness > roughnessCutoff) return 0.0;   // 粗い面は IBL に任せる

    float3 P = SS_ViewPosFromDepth(i.uv, d, gInvProj, SS_INV_RT, gViewport);
    float3 V = normalize(-P);                       // ビュー空間ではカメラが原点
    float3 N = SS_ToViewNormal(surf.N);
    float  NoV = dot(N, V);
    if (NoV <= 0.0) return 0.0;                     // 裏を向いている面

    float3 R = reflect(-V, N);

    float jitter = SS_IGN(fullPx);                  // 時間的に不動（TAA の有無に依存しない）
    float3 origin = P + N * gSsr0.w;                // bias で自己交差を避ける

    float2 hitUV; float hitZ;
    if (!SS_TraceDDA(origin, R, gSsr0.x, gSsr0.y, gSsr0.z, (int)gSsr1.x, jitter, hitUV, hitZ))
        return 0.0;

    // ---- ヒット点の妥当性 ----
    // 後ろ向き面に当たった＝面の裏。反射としては信用しない。
    SSSurface hitSurf = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, hitUV, 0));
    float3 hitN = SS_ToViewNormal(hitSurf.N);
    if (dot(hitN, R) > 0.0) return 0.0;

    // ---- confidence（連続値でフェード。境界を目立たせない）----
    float2 hitNdc   = SS_UVToNDC(hitUV, SS_INV_RT, gViewport);
    float2 e        = min(hitNdc + 1.0, 1.0 - hitNdc);      // 0=画面端 1=中央寄り
    float  edgeFade = smoothstep(0.0, max(gSsr1.z, 1e-4), min(e.x, e.y));
    float  grazeFade= smoothstep(0.0, 0.25, NoV);
    float  roughFade= 1.0 - smoothstep(roughnessCutoff * 0.6, roughnessCutoff, surf.roughness);
    float  conf     = edgeFade * grazeFade * roughFade * saturate(gSsr1.w);
    if (conf <= 0.001) return 0.0;

    // ---- ヒット点の色を「前フレーム」から取る（hitpoint reprojection）----
    // 速度は「現UV - 前UV」なので引く。再投影先が矩形外なら再投影しない（軽微なズレ < 破綻）。
    float2 vel    = g_velocity.SampleLevel(g_pointClamp, hitUV, 0);
    float2 prevUV = hitUV - vel;
    float2 prevPx = prevUV / max(SS_INV_RT, 1e-6);
    float2 srcUV  = SS_InViewport(prevPx, gViewport) ? prevUV : hitUV;

    // 粗い面ほどハーフ解像度側（＝ぼけた前フレームカラー）へ寄せる。
    // ★SS_Sanitize 必須。前フレームカラーに Inf（HDR スカイボックスの太陽など）が
    //   混ざっていると ambient が NaN になって画面が真っ黒になる。
    float3 sharp = SS_Sanitize(g_colorA.SampleLevel(g_linearClamp, srcUV, 0).rgb);
    float3 blur  = SS_Sanitize(g_colorB.SampleLevel(g_linearClamp, srcUV, 0).rgb);
    float3 hitColor = lerp(sharp, blur, saturate(surf.roughness / max(roughnessCutoff, 1e-3)));

    return float4(hitColor, conf);
}

// ---------------------------------------------------------------------------
// アップサンプル（フル解像度）。g_colorA = ハーフのトレース結果。
// 深度・法線考慮の joint bilateral 3x3。confidence も一緒に持ち上げる。
// ---------------------------------------------------------------------------
float4 UpsamplePS(FSQuadVSOut i) : SV_TARGET
{
    float2 fullPx = i.uv / max(SS_INV_RT, 1e-6);
    if (!SS_InViewport(fullPx, gViewport)) return 0.0;

    float dFull = g_depth.SampleLevel(g_pointClamp, i.uv, 0);
    if (dFull >= 1.0) return 0.0;

    float  zFull = SS_ViewPosFromDepth(i.uv, dFull, gInvProj, SS_INV_RT, gViewport).z;
    float3 nFull = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, i.uv, 0)).N;

    float4 sum = 0.0;
    float  wsum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        float2 uv = i.uv + float2(x, y) * SS_INV_HALF;
        float  dz = g_depth.SampleLevel(g_pointClamp, uv, 0);
        if (dz >= 1.0) continue;
        float  z  = SS_ViewPosFromDepth(uv, dz, gInvProj, SS_INV_RT, gViewport).z;
        float3 n  = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, uv, 0)).N;

        float wz = exp(-abs(z - zFull) / max(0.05 * abs(zFull), 1e-3));
        float wn = pow(saturate(dot(n, nFull)), 8.0);
        float w  = wz * wn + 1e-4;

        sum  += g_colorA.SampleLevel(g_linearClamp, uv, 0) * w;
        wsum += w;
    }
    if (wsum <= 0.0) return 0.0;
    return sum / wsum;
}
