// スクリーンスペース ゴッドレイ（GPU Gems 3 ch.13 "Volumetric Light Scattering as a Post-Process"）。
// パス1(マスク): 深度から空(遠平面)を検出し、太陽スクリーン位置の周囲だけ明るいマスクを半解像度へ。
// パス2(放射ブラー): 各ピクセルから太陽位置へ 64 タップ行進（decay 減衰）して光条を生成。
// 出力はシーンと同じ正規化 UV レイアウト（サブ矩形対応）。uber パスが加算合成する。

#include "FullscreenTri.hlsli"

cbuffer GRCB : register(b0)
{
    float4 rectP;   // xy=UVオフセット, zw=UVスケール（フルRT正規化UV空間）
    float4 sunP;    // xy=太陽UV(フルRT空間), z=フェード(0..1), w=強度
    float4 sunCol;  // rgb=太陽色(ライト強度込み), w=density(行進距離 0..1)
    float4 marchP;  // x=decay(タップ毎減衰), y=アスペクト比(W/H), zw=未使用
};

Texture2D    gSrc  : register(t0);   // パス1: 深度(R32_FLOAT) / パス2: マスクRT
SamplerState gSamp : register(s0);

// ---- パス1: 空マスク ----
float4 GodRaysMaskPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uv = i.uv * rectP.zw + rectP.xy;
    float depth = gSrc.Sample(gSamp, uv).r;
    float sky = (depth >= 0.9999) ? 1.0 : 0.0;   // ジオメトリは遮蔽

    // 太陽からの距離で減衰するグロー（アスペクト補正付き）
    float2 d = uv - sunP.xy;
    d.x *= marchP.y;
    float glow = saturate(1.0 - length(d) * 2.0);
    glow *= glow;

    return float4(sunCol.rgb * (sky * glow), 1.0);
}

// ---- パス2: 太陽位置への放射ブラー ----
float4 GodRaysBlurPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uvFull = i.uv * rectP.zw + rectP.xy;
    float2 delta  = (uvFull - sunP.xy) * (sunCol.w / 64.0);

    float2 uv  = uvFull;
    float3 acc = 0.0;
    float  w   = 1.0;
    [loop] for (int k = 0; k < 64; ++k)
    {
        uv -= delta;
        // サブ矩形の外（クリア領域）を拾わない
        float2 cl = clamp(uv, rectP.xy, rectP.xy + rectP.zw);
        acc += gSrc.Sample(gSamp, cl).rgb * w;
        w   *= marchP.x;
    }

    // 強度×画面外フェードを焼き込む（uber は加算するだけ）
    return float4(acc * (sunP.w * sunP.z / 64.0), 1.0);
}
