// SSGI.hlsl - スクリーン空間 GI（間接拡散）。
//
// TracePS   : ハーフ解像度。cosine-weighted 半球レイを短距離マーチし、当たった点の
//             「前フレームカラー（ハーフ＝ぼけている＝分散が小さい）」を積分する。
//             外れたレイは IBL の irradiance を積む（★これが無いとカメラを回すたびに
//             GI の明るさが変動する。SSGI で一番目立つアーティファクト）。
//             出力 rgb=間接放射照度（albedo を掛ける前） / a=ビュー空間 Z（disocclusion 判定用）。
// TemporalPS: ハーフ。速度で再投影 → 深度不一致で履歴破棄 → 指数移動平均。
//             TAA に任せてはいけない（TAA の近傍クリップはノイズ自体を範囲に含めるので収束しない）。
// UpsamplePS: フル解像度。SSR と同じ joint bilateral。
#include "../post/FullscreenTri.hlsli"
#include "ScreenSpaceParams.hlsli"

// 法線まわりの正規直交基底（Duff et al. branchless ONB）
void BuildONB(float3 n, out float3 t, out float3 b)
{
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// 短距離のビュー空間等間隔マーチ（ContactShadow.hlsl と同形）。
// レイ長が radius(既定 6m) に限定されるので DDA の複雑さは不要。
bool SS_TraceLinear(float3 originVS, float3 dirVS, float rayLength, float thickness,
                    int steps, float jitter, out float2 hitUV)
{
    hitUV = 0.0;
    float invSteps = 1.0 / (float)steps;
    [loop]
    for (int k = 1; k <= steps; ++k)
    {
        float  t  = rayLength * ((float)k + jitter) * invSteps;
        float3 sp = originVS + dirVS * t;

        float4 clip = mul(float4(sp, 1.0), gProj);
        if (clip.w <= 1e-6) return false;
        float2 sndc = clip.xy / clip.w;
        if (any(abs(sndc) > 1.0)) return false;
        float2 suv = SS_NDCToUV(sndc, SS_INV_RT, gViewport);

        float sd = g_depth.SampleLevel(g_pointClamp, suv, 0);
        if (sd >= 1.0) continue;                    // 背景は遮蔽物ではない
        float sceneZ = SS_ViewPosFromDepth(suv, sd, gInvProj, SS_INV_RT, gViewport).z;

        float dz = sp.z - sceneZ;                   // >0 = レイが表面より奥
        if (dz > 0.0)
        {
            if (dz < thickness) { hitUV = suv; return true; }
            // 厚みを超えた = 物体の裏を通り抜けただけ。マーチ続行（march-behind-surfaces）
        }
    }
    return false;
}

float4 TracePS(FSQuadVSOut i) : SV_TARGET
{
    float2 fullPx = i.uv / max(SS_INV_RT, 1e-6);
    if (!SS_InViewport(fullPx, gViewport)) return 0.0;

    float d = g_depth.SampleLevel(g_pointClamp, i.uv, 0);
    if (d >= 1.0) return 0.0;

    float3 P = SS_ViewPosFromDepth(i.uv, d, gInvProj, SS_INV_RT, gViewport);
    SSSurface surf = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, i.uv, 0));
    float3 N = SS_ToViewNormal(surf.N);

    float3 T, B; BuildONB(N, T, B);
    // ★時間方向にも回す。ここを固定すると毎フレーム同じノイズになり、
    //   TemporalPS がいくら平均してもノイズが一切収束しない（実際に踏んだ）。
    //   黄金比オフセットは低食い違い列になるので少ないフレーム数でもよく散る。
    float  ign = frac(SS_IGN(fullPx) + gMisc2.x * 0.6180339887);

    const int   n          = clamp((int)gSsgi0.z, 1, 4);
    const int   stepCount  = clamp((int)gSsgi0.w, 4, 24);
    const float radius     = gSsgi0.x;
    const float thickness  = gSsgi0.y;
    const float clampValue = gSsgi1.y;
    const bool  iblFall    = (gSsgi1.w > 0.5) && (gMisc.w > 0.5);

    float3 origin = P + N * 0.02;
    float3 sum    = 0.0;

    [loop]
    for (int k = 0; k < n; ++k)
    {
        // 層化 + IGN オフセット。時間ジッタは入れない（TemporalPS が時間方向を担当する）。
        float u1 = frac((float)k / (float)n + ign);
        float u2 = frac(ign * 1.6180339887 + (float)k * 0.7548776662);

        // cosine-weighted 半球サンプル（Malley）
        float  r    = sqrt(u1);
        float  phi  = 6.2831853 * u2;
        float3 dirT = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
        float3 dir  = normalize(dirT.x * T + dirT.y * B + dirT.z * N);

        float2 hitUV;
        if (SS_TraceLinear(origin, dir, radius, thickness, stepCount, ign, hitUV))
        {
            // ヒット面がこちらを向いていないなら光は来ない（面の裏を見ている）
            float3 hitN = SS_ToViewNormal(SS_UnpackGBuffer(
                              g_gbuffer.SampleLevel(g_pointClamp, hitUV, 0)).N);
            if (dot(hitN, -dir) > 0.0)
            {
                // ヒット点の色を前フレームから。速度で再投影 + ハーフ解像度で分散を落とす。
                float2 vel    = g_velocity.SampleLevel(g_pointClamp, hitUV, 0);
                float2 prevUV = hitUV - vel;
                float2 prevPx = prevUV / max(SS_INV_RT, 1e-6);
                float2 srcUV  = SS_InViewport(prevPx, gViewport) ? prevUV : hitUV;
                // SS_Sanitize 必須（前フレームカラーの Inf が ambient を NaN にする）
                float3 c      = SS_Sanitize(g_colorB.SampleLevel(g_linearClamp, srcUV, 0).rgb);
                sum += min(c, clampValue.xxx);
            }
        }
        else if (iblFall)
        {
            // ★画面外へ抜けたレイは IBL の irradiance を積む。
            //   これをやらないとカメラを回すたびに GI の明るさが変動する。
            //   dir はビュー空間なのでワールドへ戻してからキューブを引く。
            float3 dirW = normalize(mul(float4(dir, 0.0), gInvView).xyz);
            // ★環境キューブは Inf を持ちうる（fp16 上限を超える太陽）。ヒット時と同じく
            //   sanitize + clamp を通さないと ambient が NaN になり全ジオメトリが黒くなる。
            sum += min(SS_Sanitize(g_irradiance.SampleLevel(g_linearClamp, dirW, 0).rgb),
                       clampValue.xxx);
        }
    }

    // cosine-weighted の pdf = cos/PI なので estimator は単純平均で正しい
    // （L_o = ∫ L_i cosθ/π dω → (1/N)ΣL_i）。albedo はフォワード側で掛ける。
    float3 gi = SS_Sanitize(sum / (float)n * gSsgi1.x);
    return float4(gi, P.z);
}

// ---------------------------------------------------------------------------
// 時間蓄積（ハーフ）。g_colorA = 今フレームのトレース結果 / g_colorB = 前フレームの履歴。
// ---------------------------------------------------------------------------
float4 TemporalPS(FSQuadVSOut i) : SV_TARGET
{
    float4 cur = g_colorA.SampleLevel(g_pointClamp, i.uv, 0);
    if (gMisc.z < 0.5) return cur;                  // 履歴無効（初回 / リサイズ直後）

    float2 vel    = g_velocity.SampleLevel(g_pointClamp, i.uv, 0);
    float2 histUV = i.uv - vel;
    float2 histPx = histUV / max(SS_INV_RT, 1e-6);
    if (!SS_InViewport(histPx, gViewport)) return cur;

    float4 hist = g_colorB.SampleLevel(g_linearClamp, histUV, 0);

    // disocclusion: 履歴の .a に入れておいたビュー空間 Z と現フレームを比べる（相対 10%）
    if (abs(cur.w - hist.w) > 0.1 * max(abs(cur.w), 1e-3)) return cur;

    // 速度が大きいほど履歴を薄める（lag 対策）
    float speed = length(vel) / max(length(SS_INV_RT), 1e-6);   // px/frame
    float fb    = lerp(gSsgi1.z, gSsgi1.z * 0.5, saturate(speed / 16.0));
    return float4(lerp(cur.rgb, hist.rgb, fb), cur.w);
}

// ---------------------------------------------------------------------------
// アップサンプル（フル解像度）。g_colorA = ハーフの履歴（時間蓄積後）。
// ---------------------------------------------------------------------------
float4 UpsamplePS(FSQuadVSOut i) : SV_TARGET
{
    float2 fullPx = i.uv / max(SS_INV_RT, 1e-6);
    if (!SS_InViewport(fullPx, gViewport)) return 0.0;

    float dFull = g_depth.SampleLevel(g_pointClamp, i.uv, 0);
    if (dFull >= 1.0) return 0.0;

    float  zFull = SS_ViewPosFromDepth(i.uv, dFull, gInvProj, SS_INV_RT, gViewport).z;
    float3 nFull = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, i.uv, 0)).N;

    float3 sum = 0.0;
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

        sum  += g_colorA.SampleLevel(g_linearClamp, uv, 0).rgb * w;
        wsum += w;
    }
    if (wsum <= 0.0) return 0.0;
    return float4(sum / wsum, 1.0);
}

// ---------------------------------------------------------------------------
// 前フレームカラーのハーフ解像度版を作る（4 タップ box）。g_colorA = フル解像度のコピー。
// ヒット点の色をここから取ると、それだけで SSGI の分散が激減する（実質的なプリフィルタ）。
// ---------------------------------------------------------------------------
float4 DownsamplePS(FSQuadVSOut i) : SV_TARGET
{
    // ★このパスだけは cbuffer を読まない（前フレームカラーの退避はパラメータ確定より前に
    //   走るため）。テクセルサイズはリソースから直接引く。
    uint w, h;
    g_colorA.GetDimensions(w, h);
    float2 o = 0.5 / float2(max(w, 1u), max(h, 1u));
    float3 c = g_colorA.SampleLevel(g_linearClamp, i.uv + float2(-o.x, -o.y), 0).rgb
             + g_colorA.SampleLevel(g_linearClamp, i.uv + float2( o.x, -o.y), 0).rgb
             + g_colorA.SampleLevel(g_linearClamp, i.uv + float2(-o.x,  o.y), 0).rgb
             + g_colorA.SampleLevel(g_linearClamp, i.uv + float2( o.x,  o.y), 0).rgb;
    return float4(c * 0.25, 1.0);
}
