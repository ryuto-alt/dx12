// WaterSurface.hlsl — 水面 W1: 頂点（クリップマップのリング + ゲルストナー変位）とピクセル（屈折 + 吸収 + 反射 + 泡 + 岸）。
//   VS/PS はメインのルートシグネチャで動く。リソースの前提は WaterCommon.hlsli。
//   -D UNO_PHYSICAL_LIGHTS=1: Q2 の物理ライティング単位（クラスタ光の減衰が逆二乗）。
#include "WaterCommon.hlsli"

struct VSIn
{
    float2 grid : POSITION;   // 整数格子座標 [-m, m]
};

struct VSOut
{
    float4 pos    : SV_POSITION;
    float3 wpos   : TEXCOORD0;   // 変位後のワールド座標
    float2 x0     : TEXCOORD1;   // 変位前（Lagrange）の XZ
    float  vdepth : TEXCOORD2;   // view 空間の z
};

float3 WaterDisplaced(float2 xz, float cell)
{
    const float4 lv = g_waterData[gBase + WD_LEVEL];
    const float4 dt = g_waterData[gBase + WD_DETAIL];
    const WaterGerstner g = WaterEvalGerstner((uint)(lv.y + 0.5), xz, lv.z, dt.zw, cell, 0.0);
    return float3(xz.x + g.disp.x, lv.x + g.disp.y, xz.y + g.disp.z);
}

VSOut VSMain(VSIn i)
{
    gBase = WD_FRAME + (uint)(gLvl1.w + 0.5) * WD_STRIDE;
    const float cell = gLvl0.x;
    const int m = (int)(gLvl0.y + 0.5);
    const int2 g = int2(round(i.grid));
    const float2 xz = gLvl0.zw + float2(g) * cell;

    float3 p;
    const bool edgeX = (abs(g.x) == m);
    const bool edgeZ = (abs(g.y) == m);
    if (edgeX && ((g.y & 1) != 0))
        p = 0.5 * (WaterDisplaced(xz - float2(0.0, cell), cell) + WaterDisplaced(xz + float2(0.0, cell), cell));   // 外周の奇数頂点は粗いレベルの辺の中点へ（亀裂を無くす）
    else if (edgeZ && ((g.x & 1) != 0))
        p = 0.5 * (WaterDisplaced(xz - float2(cell, 0.0), cell) + WaterDisplaced(xz + float2(cell, 0.0), cell));
    else
        p = WaterDisplaced(xz, cell);

    VSOut o;
    o.pos = mul(float4(p, 1.0), gViewProj);
    o.wpos = p;
    o.x0 = xz;
    o.vdepth = o.pos.w;
    return o;
}

// 手続きディテール法線（2 スケール）。戻り値は高さ勾配（法線へ (-gx, 0, -gz) を足す）。w = 泡ノイズ（大スケール）
float3 WaterDetailSlope(float2 xz, float t, float2 flow, float tile, float drift)
{
    const float2 adv = flow * t + float2(drift, drift * 0.6) * t;
    const float2 uv1 = (xz - adv) / tile;
    // 小スケール: 回転 + 別の流速（繰り返しを隠す）
    const float2 r = float2(xz.x * 0.8 - xz.y * 0.6, xz.x * 0.6 + xz.y * 0.8);
    const float2 uv2 = (r - adv * float2(-0.7, 1.2)) / (tile * 0.19) + float2(0.37, 0.61);
    const float4 s1 = g_waterTex.Sample(g_sampler, uv1);
    const float4 s2 = g_waterTex.Sample(g_sampler, uv2);
    const float2 gr = (s1.rg - 0.5) * 2.0 * 1.0 + (s2.rg - 0.5) * 2.0 * 0.55;
    return float3(gr, s1.b);
}

float4 PSMain(VSOut i) : SV_TARGET
{
    gBase = WD_FRAME + (uint)(gLvl1.w + 0.5) * WD_STRIDE;
    if (!WaterInside(i.x0)) discard;

    const float4 bLevel  = g_waterData[gBase + WD_LEVEL];
    const float4 bFoam   = g_waterData[gBase + WD_FOAM];
    const float3 ext     = g_waterData[gBase + WD_EXT].xyz;
    const float4 bScat   = g_waterData[gBase + WD_SCATTER];
    const float4 bOpt    = g_waterData[gBase + WD_OPTICS];
    const float4 bDetail = g_waterData[gBase + WD_DETAIL];
    const float4 bMisc   = g_waterData[gBase + WD_MISC];
    const float  ior = bScat.w;
    const float  t = bLevel.z;
    const float2 flow = bDetail.zw;

    const float3 Pw = i.wpos;
    const float3 V = normalize(cameraPos - Pw);
    const float2 px = i.pos.xy;

    // ---- 法線（解析ゲルストナー + ディテール）----
    const float foot = max(length(ddx(i.x0)), length(ddy(i.x0)));
    const WaterGerstner gw = WaterEvalGerstner((uint)(bLevel.y + 0.5), i.x0, t, flow, 0.0, foot);
    const float3 det = WaterDetailSlope(i.x0, t, flow, bDetail.x, bDetail.y);
    const float detW = bOpt.z;
    float3 N = normalize(gw.normal + float3(-det.x, 0.0, -det.y) * (detW * 0.35));
    // 足跡より細かいディテールは傾きの分散としてラフネスへ回す
    const float detVar = 0.5 * (detW * 0.35) * (detW * 0.35) * saturate(foot * 10.0 / bDetail.x);
    const float alphaEff2 = bOpt.x * bOpt.x * bOpt.x * bOpt.x + gw.slopeVar + detVar;   // GGX の α² = 基本 α² + 傾きの分散
    const float roughEff = clamp(sqrt(sqrt(alphaEff2)), 0.01, 1.0);                       // 「roughness」= √α

    const bool under = (cameraPos.y < Pw.y);
    float3 Nv = under ? -N : N;    // 視線側を向く法線
    float NoV = dot(Nv, V);
    if (NoV < 0.03) { Nv = normalize(Nv + V * (0.03 - NoV)); NoV = saturate(dot(Nv, V)); }
    NoV = max(NoV, 0.02);

    const float3 L = normalize(-lightDir);
    const float shadow = WaterShadow(Pw, i.vdepth, i.pos.xy);
    const float3 Ein = WaterIncidentEnergy(shadow, ior);

    float3 color;
    float alpha = 1.0;

    if (!under)
    {
        // ================= 水面の上から =================
        const float F = WaterFresnel(NoV, ior);

        // ---- 屈折 ----
        const float dS = g_sceneDepth.Load(int3(px, 0));
        const bool noBottom = (dS >= 0.99999);
        const float3 wB = ReconWorld(px, dS);
        float hVert = noBottom ? 400.0 : max(Pw.y - wB.y, 0.0);

        bool ok;
        float3 R = WaterRefract(-V, Nv, 1.0 / ior, ok);
        if (R.y > -0.06) R = normalize(float3(R.x, -0.06, R.z));
        float2 colorPx = px;
        float3 wHit = wB;
        float  dHit = dS;
        if (!noBottom)
        {
            // 屈折した視線が水底に当たる点を推定（水平な底なら厳密）→ 画面へ射影 → その深度で検証
            const float tR = hVert / max(-R.y, 0.06);
            const float3 Ph = Pw + R * tR;
            float wProj;
            float2 pxR = ProjectPx(Ph, wProj);
            pxR = lerp(px, pxR, bOpt.y);   // refractionStrength
            const float4 sc = g_waterData[8];
            if (wProj > 0.1 && pxR.x >= 1.0 && pxR.y >= 1.0 && pxR.x < sc.x - 1.0 && pxR.y < sc.y - 1.0)
            {
                const float dR = g_sceneDepth.Load(int3(pxR, 0));
                const float3 wR = ReconWorld(pxR, dR);
                // リーク対策: 屈折先が水面より手前（= 水の手前にある物体）/ 水面より上（岸の物体）なら採用しない
                const bool inFront = (dR < 0.99999) && (LinearDepth(dR) < i.vdepth - 0.02);
                const bool above = (wR.y > Pw.y + 0.02);
                if (dR < 0.99999 && !inFront && !above)
                {
                    colorPx = pxR;
                    wHit = wR;
                    dHit = dR;
                    hVert = max(Pw.y - wR.y, 0.0);
                }
            }
        }
        const float tPath = min(hVert / max(-R.y, 0.06), 600.0);
        const float3 T = exp(-ext * tPath);
        float3 behind = g_sceneColor.SampleLevel(g_iblSampler, colorPx * g_waterData[8].zw, 0.0).rgb;

        // ---- コースティクス（水底に当たる太陽光の集光。水面直下の浅い所ほど強い）----
        if (bOpt.w > 0.0 && !noBottom)
        {
            const float2 cxz = wHit.xz;
            const float ca1 = g_waterTex.SampleLevel(g_sampler, (cxz - flow * t) / 2.6 + float2(t * 0.011, t * 0.007), 0.0).a;
            const float ca2 = g_waterTex.SampleLevel(g_sampler, (cxz.yx - flow.yx * t * 0.8) / 1.9 + float2(-t * 0.009, t * 0.013), 0.0).a;
            const float c = min(ca1, ca2) * 1.9;
            const float depthFade = exp(-0.28 * hVert);
            behind *= 1.0 + bOpt.w * shadow * saturate(L.y) * depthFade * c * c * 2.2;
        }
        const float3 Lscat = bScat.rgb * Ein * (1.0 - T);
        float3 refracted = (behind * T + Lscat) * (1.0 - F);

        // ---- 反射 ----
        float3 Rr = reflect(-V, Nv);
        Rr = normalize(float3(Rr.x, max(Rr.y, 0.02), Rr.z));
        float3 env;
        if (hasIBL != 0u)
        {
            const float mip = roughEff * maxPrefilterMip;
            env = g_prefilteredMap.SampleLevel(g_iblSampler, Rr, mip).rgb * iblIntensity;
        }
        else
            env = ambientStrength.xxx * float3(0.85, 0.95, 1.1);
        float4 ssr = 0.0;
        if (bMisc.x > 0.5 && roughEff < 0.4)
        {
            const float jit = frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715))));
            ssr = WaterTraceSSR(Pw, Rr, px, jit);
            ssr.a *= saturate(1.0 - roughEff * 2.5);
        }
        const float3 refl = lerp(env, ssr.rgb, ssr.a);

        // ---- 太陽 / 点光源の鏡面（フレネルは正確な誘電体の式）----
        const float3 H = normalize(V + L);
        const float NoL = saturate(dot(Nv, L));
        const float Fh = WaterFresnel(saturate(dot(H, V)), ior);
        const float aG = roughEff;
        const float Dg = DistributionGGX(Nv, H, aG);
        const float Gg = GeometrySmith(Nv, V, L, aG);
        const float3 sunSpec = lightColor * shadow * (Dg * Gg * Fh / (4.0 * NoV * NoL + 1.0e-4) * NoL);
        const float3 punctual = AccumulatePunctualLights(Nv, V, Pw, 0.0.xxx, float3(0.02, 0.02, 0.02), 0.0, aG, i.pos.xy);

        color = refracted + refl * F + sunSpec + punctual;

        // ---- 泡（波頭のヤコビアン + 岸の深度差）----
        {
            const float fn = WaterFoamNoise(i.x0, t, flow, bDetail.x);
            const float crestMask = saturate((0.78 - gw.jac) * 3.2) * bFoam.x;
            const float shoreMask = noBottom ? 0.0 : (1.0 - saturate(hVert / bFoam.z)) * bFoam.y;
            const float mask = max(crestMask, shoreMask);
            const float foam = saturate((mask * 1.6 - (1.0 - fn) * 0.9) * 3.5) * saturate(mask * 2.0);
            const float3 foamAlbedo = float3(0.86, 0.9, 0.92);
            const float3 foamLit = foamAlbedo * Ein * 1.0 + AccumulatePunctualLights(Nv, V, Pw, foamAlbedo, float3(0.04, 0.04, 0.04), 0.0, 0.7, i.pos.xy);
            color = lerp(color, foamLit, foam * 0.92);
            // 岸のフェード（水深が浅い所ほど透明）。泡は透明にしない
            const float shore = (bFoam.w > 1.0e-4 && !noBottom) ? saturate(hVert / bFoam.w) : 1.0;
            alpha = max(shore, foam * 0.92);
        }
    }
    else
    {
        // ================= 水面の下から（スネルの窓 + 全反射）=================
        const float Ff = WaterFresnel(NoV, 1.0 / ior);   // 全反射で 1
        const float camDist = length(cameraPos - Pw);
        const float3 Tcam = exp(-ext * camDist);
        // 窓の向こう（大気側）: 屈折した視線で不透明シーンを引く（遠方の点へ射影）
        bool ok;
        float3 Rt = WaterRefract(-V, Nv, ior, ok);
        float2 pxU = px;
        if (ok)
        {
            float wProj;
            const float2 pxR = ProjectPx(Pw + Rt * 60.0, wProj);
            const float4 sc = g_waterData[8];
            if (wProj > 0.1 && pxR.x >= 1.0 && pxR.y >= 1.0 && pxR.x < sc.x - 1.0 && pxR.y < sc.y - 1.0) pxU = pxR;
        }
        const float3 above = g_sceneColor.SampleLevel(g_iblSampler, pxU * g_waterData[8].zw, 0.0).rgb;
        const float camDepthBelow = max(Pw.y - cameraPos.y, 0.0);
        const float3 EinCam = Ein * exp(-ext * camDepthBelow);
        const float3 seen = above * Tcam + bScat.rgb * EinCam * (1.0 - Tcam);
        // 全反射: 水中の景色（深い所の水の色）
        const float3 body = bScat.rgb * EinCam;
        color = seen * (1.0 - Ff) + body * Ff;
    }

#ifdef UNO_PHYSICAL_LIGHTS
    color = min(color, 6.0e4);
#endif
    if (alpha < 0.01) discard;
    return float4(color, alpha);
}
