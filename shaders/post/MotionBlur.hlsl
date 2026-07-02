// カメラモーションブラー（velocity buffer 不要の深度再構成方式）。
// 各ピクセルのワールド位置を 深度 + 逆viewProj で復元し、前フレーム viewProj で再投影して
// スクリーン速度を求め、その方向に N タップ平均する。カメラの動きのみ（オブジェクト毎は将来）。

#include "FullscreenTri.hlsli"

cbuffer MBCB : register(b0)
{
    float4x4 invViewProj;    // 現フレームの逆 viewProj（転置済み）
    float4x4 prevViewProj;   // 前フレームの viewProj（転置済み）
    float4   rectP;          // xy=UVオフセット, zw=UVスケール
    float4   params;         // x=強度(シャッター係数), y=タップ数, z=最大ブラー(ローカルUV), w=未使用
};

Texture2D    gScene : register(t0);
Texture2D    gDepth : register(t1);
SamplerState gSamp  : register(s0);

float4 MotionBlurPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uvFull = i.uv * rectP.zw + rectP.xy;
    float  depth  = gDepth.Sample(gSamp, uvFull).r;

    // ビューポートローカル NDC → ワールド → 前フレーム UV
    float2 ndc   = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
    float4 world = mul(invViewProj, float4(ndc, depth, 1.0));
    world /= max(world.w, 1e-6);
    float4 prevClip = mul(prevViewProj, world);
    float2 prevNdc  = prevClip.xy / max(prevClip.w, 1e-6);
    float2 prevUv   = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);

    float2 vel = (prevUv - i.uv) * params.x;
    float  l   = length(vel);
    if (l > params.z) vel *= params.z / l;   // 破綻防止クランプ

    int    n   = clamp((int)params.y, 4, 16);
    float3 acc = 0.0;
    [loop] for (int k = 0; k < n; ++k)
    {
        float  t  = ((float)k / (float)(n - 1)) - 0.5;
        float2 su = saturate(i.uv + vel * t);
        acc += gScene.Sample(gSamp, su * rectP.zw + rectP.xy).rgb;
    }
    return float4(acc / (float)n, 1.0);
}
