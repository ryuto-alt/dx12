// モーションブラー。速度方向に N タップ平均する。速度の求め方は2通り:
//
//   params.w > 0.5 … 速度バッファ(TaaPass の RG16F)を読む。TAA 有効時のみ利用できる。
//                    カメラだけでなく**オブジェクト毎**の動きが入るので、静止カメラでも
//                    回転している物体がちゃんとブレる。
//   params.w = 0   … 従来の深度再構成。各ピクセルのワールド位置を 深度 + 逆viewProj で
//                    復元し、前フレーム viewProj で再投影する。カメラの動きだけ。
//
// ★符号規約に注意: 速度バッファは velocity = 現UV - 前UV（TAA の規約）だが、
//   このシェーダが欲しいのは「前フレームへ向かうベクトル」= 前UV - 現UV なので反転する。
//   ここを間違えると動いた方向と逆にブレる（症状が微妙で気づきにくい）。

#include "FullscreenTri.hlsli"

cbuffer MBCB : register(b0)
{
    float4x4 invViewProj;    // 現フレームの逆 viewProj（転置済み・ジッタなし）
    float4x4 prevViewProj;   // 前フレームの viewProj（転置済み・ジッタなし）
    float4   rectP;          // xy=UVオフセット, zw=UVスケール
    float4   params;         // x=強度(シャッター係数), y=タップ数, z=最大ブラー(ローカルUV), w=速度バッファを使うか
};

Texture2D    gScene    : register(t0);
Texture2D    gDepth    : register(t1);
// 速度バッファ。Texture2D<float2> ではなく float4 で受けるのは、速度バッファが使えない時に
// 別のテクスチャ（深度）をダミーとして張れるようにするため（params.w=0 なら読まない）。
Texture2D    gVelocity : register(t2);
SamplerState gSamp     : register(s0);

float4 MotionBlurPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uvFull = i.uv * rectP.zw + rectP.xy;

    float2 vel;
    if (params.w > 0.5)
    {
        // 速度バッファ方式（オブジェクト毎のブラー）。TAA の符号規約から反転する。
        vel = -gVelocity.Sample(gSamp, uvFull).rg * params.x;
    }
    else
    {
        // 深度再構成方式（カメラの動きのみ）
        float  depth = gDepth.Sample(gSamp, uvFull).r;
        float2 ndc   = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
        float4 world = mul(invViewProj, float4(ndc, depth, 1.0));
        world /= max(world.w, 1e-6);
        float4 prevClip = mul(prevViewProj, world);
        float2 prevNdc  = prevClip.xy / max(prevClip.w, 1e-6);
        float2 prevUv   = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
        vel = (prevUv - i.uv) * params.x;
    }

    float l = length(vel);
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
