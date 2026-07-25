// RtDebug.hlsl — TLAS が正しく建っているかを目視で確かめるためのプライマリレイ（計画09 Step 1）。
//
// カメラから 1 ピクセル 1 本ずつレイを飛ばし、
//   .r = レイのヒット距離(m)。ミスなら -1
//   .g = 深度バッファから復元した「ラスタライズ結果の距離」(m)。空なら -1
//   .b = 0（予約）
//   .a = 1=ヒット / 0=ミス
// を RGBA16F へ書く。表示は dx12_render_debug の
//   mode="rt"     … .r のヒートマップ（シルエットがラスタの絵と一致すること）
//   mode="rtDiff" … |.r - .g| のヒートマップ（★これが黒ならレイと三角形が完全一致）
// が担当する。可視化のために独立した仕組みは作らない（既存の render_debug に mode を足すだけ）。
//
// rtDiff が「行列の転置ミス / ノード変換の付け忘れ / LOD 違い」を一発で炙り出す。
// ヒット距離だけだと「それらしく見えるがズレている」を見逃す。
//
// ★スキンドと半透明は TLAS に入っていないので、そこは必ずミス（黒 / 赤）になる。仕様。
#include "RtCommon.hlsli"

float4 PSMain(FSQuadVSOut i) : SV_TARGET
{
    const float2 px = i.pos.xy;
    if (!RtInsideViewport(px)) return float4(-1, -1, 0, 0);

    // ピクセルを通るワールド空間のレイ（far 平面上の点で方向を作る）。
    const float2 ndc = RtPixelToNdc(px);
    float4 far = mul(float4(ndc, 1.0, 1.0), gInvViewProj);
    const float3 dir = normalize(far.xyz / far.w - gCameraPos);

    const float t = RtTraceClosestT(gCameraPos, dir, max(gZNear, 1e-3), RT_TMAX);

    // ラスタ側の距離（比較対象）。空なら -1。
    const float d = gDepth.Load(int3(int2(px), 0));
    float rasterT = -1.0;
    if (d < 1.0)
        rasterT = length(RtWorldFromDepth(px, d) - gCameraPos);

    return float4(t, rasterT, 0.0, (t >= 0.0) ? 1.0 : 0.0);
}
