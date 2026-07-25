// VelocityDebug.hlsl - 速度バッファ(モーションベクター)の可視化。目視検証用。
//
// R = +X（右）方向の速度, G = +Y（画面下）方向の速度。0.5 が「速度ゼロ」。
// つまり **静止していれば画面全体が均一な (0.5, 0.5, 0.5) のグレー** になるのが正解。
// ここが縞々に揺れていたらジッタの除去（VelocityCommon.hlsli の curClip.xy -= jitter*w）が
// 効いていない。カメラを右へパンすると全面が赤寄り、左へパンすると緑寄り＋暗くなる。
//
// 空(スカイボックス)は速度パスで描かれないので 0 のまま＝グレー。これは正常
// （TAA 解決シェーダが深度から再構成する）。
#include "../post/FullscreenTri.hlsli"

cbuffer DbgCB : register(b0)
{
    float4 rectP;   // xy=UVオフセット, zw=UVスケール（ビューポート矩形）
    float4 scale;   // x=表示倍率（速度をどれだけ強調するか）
};

Texture2D<float2> gVelocity : register(t0);
SamplerState      gPoint    : register(s0);

float4 VelocityDebugPS(FSQuadVSOut i) : SV_TARGET
{
    float2 v = gVelocity.SampleLevel(gPoint, i.uv * rectP.zw + rectP.xy, 0) * scale.x;
    return float4(saturate(0.5 + v.x), saturate(0.5 + v.y), 0.5, 1.0);
}
