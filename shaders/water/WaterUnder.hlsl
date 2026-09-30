// WaterUnder.hlsl — 水中表現（最小）: カメラが水面より下にあるとき、不透明シーンの色へ全画面で Beer-Lambert 減衰 + 内部散乱を掛ける。
//   カメラ → シーン（またはカメラ → 水面）の経路が水中を通る長さ d で T = exp(-σ d)。水面を下から見る部分（スネルの窓・全反射）は
//   WaterSurface.hlsl の PS が描く。RT のみ（深度なし・ブレンドなし）で不透明シーンのコピーから作り直す。
#include "WaterCommon.hlsli"

struct VSFull
{
    float4 pos : SV_POSITION;
};

VSFull VSFullscreen(uint id : SV_VertexID)
{
    VSFull o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSUnder(VSFull i) : SV_TARGET
{
    const int body = (int)round(g_waterData[10].w);
    const float2 px = i.pos.xy;
    float3 src = g_sceneColor.Load(int3(px, 0)).rgb;
    if (body < 0) return float4(src, 1.0);
    gBase = WD_FRAME + (uint)body * WD_STRIDE;

    const float3 ext = g_waterData[gBase + WD_EXT].xyz;
    const float4 bScat = g_waterData[gBase + WD_SCATTER];
    const float level = g_waterData[gBase + WD_LEVEL].x;
    const float3 cam = cameraPos;

    const float dS = g_sceneDepth.Load(int3(px, 0));
    const float3 farP = ReconWorld(px, 1.0);
    const float3 dir = normalize(farP - cam);
    float dist = 1.0e4;
    if (dS < 0.99999) dist = length(ReconWorld(px, dS) - cam);
    if (dir.y > 1.0e-4) dist = min(dist, (level - cam.y) / dir.y);   // 水面を抜けたところまでが水中

    const float3 T = exp(-ext * dist);
    // 内部散乱の光: 太陽 + 空。カメラの深さぶん減衰した光が水柱に当たっていると近似する
    const float camDepth = max(level - cam.y, 0.0);
    const float shadow = 1.0;
    const float3 Ein = WaterIncidentEnergy(shadow, bScat.w) * exp(-ext * camDepth);
    float3 col = src * T + bScat.rgb * Ein * (1.0 - T);
#ifdef UNO_PHYSICAL_LIGHTS
    col = min(col, 6.0e4);
#endif
    return float4(col, 1.0);
}
