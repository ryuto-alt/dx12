// AtmosphereSky.hlsl — 物理大気の「空パス」と「エアリアルパースペクティブ合成」(graphics。VS/PS とも 6_0)
//   VSFull  : 全画面三角形(far 平面。空は深度 1.0 のまま残る=以降のフォグ合成が空画素を判別できる)
//   PSSky   : Sky-View LUT + 太陽円盤 + 月 + 星 + 夜空の下限 を sceneRT へ(不透明描画の前・深度テスト OFF)
//   PSAp    : 遠景の霞。FogComposite と同じ「合成パス」の位置(フォグ合成の直後)で、深度から froxel を引く。
//             デュアルソースのブレンド dst = src0 + dst * src1 で、RGB の透過率をそのまま掛ける。
//             (FogComposite の dst = src.rgb + dst * src.a のカラー版。Forward は 1 行も変えない)
#include "AtmosphereCommon.hlsli"
#include "AtmosphereSkyRadiance.hlsli"

Texture2D<float> gDepth : register(t5);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSSky(VSOut i) : SV_TARGET
{
    float3 dir = AtPixelDir(i.uv, gInvViewProjJ);
    float3 c = SkyRadiance(dir);
    c = clamp(c, 0.0, 6.0e4);   // fp16(65504)のオーバーフローでポスト・TAA へ inf が漏れないように
    return float4(c, 1.0);
}

// ---------------------------------------------------------------------------
struct ApOut
{
    float4 c0 : SV_Target0;   // 加算する in-scattering(シーン単位)
    float4 c1 : SV_Target1;   // 既存の色に掛ける透過率(RGB)
};

ApOut PSAp(VSOut i)
{
    ApOut o;
    o.c0 = float4(0, 0, 0, 0);
    o.c1 = float4(1, 1, 1, 1);

    float d = gDepth.Load(int3(int2(i.pos.xy), 0));
    if (d >= 0.9999999) return o;     // 空の画素(深度が 1.0 のまま)は空パスが大気を含んでいる。★far が大きい(20 km)ので 0.999999 では遠景の地面を空と誤認する

    float2 ndc = i.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 w = mul(float4(ndc, d, 1.0), gInvViewProjJ);
    float3 P = w.xyz / w.w;
    float distKm = length(P - gCamPos) * 0.001;
    if (distKm <= gApStartKm) return o;

    float3 L1, T1, L0, T0;
    AtSampleAp(i.uv, distKm, L1, T1);
    AtSampleAp(i.uv, gApStartKm, L0, T0);
    // 区間 [start, d] だけの寄与: L = (L(d) - L(start)) / T(start)、T = T(d) / T(start)
    float3 invT0 = 1.0 / max(T0, 1e-4);
    float3 Lp = max((L1 - L0) * invT0, 0.0);
    float3 Tp = saturate(T1 * invT0);
    o.c0 = float4(Lp * gLightE * gApStrength, 0.0);
    o.c1 = float4(lerp(float3(1, 1, 1), Tp, gApStrength), 1.0);
    return o;
}
