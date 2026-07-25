// モーションブラー。速度方向に N タップ平均する。速度の求め方は2通り:
//
//   params.w > 0.5 … 速度バッファ(TaaPass の RG16F)を読む。TAA 有効時のみ利用できる。
//                    カメラだけでなく**オブジェクト毎**の動きが入るので、静止カメラでも
//                    回転している物体がちゃんとブレる。
//   params.w = 0   … 従来の深度再構成。各ピクセルのワールド位置を 深度 + 逆viewProj で
//                    復元し、前フレーム viewProj で再投影する。カメラの動きだけ。
//
// ★空(深度=1.0)は速度バッファ方式でも深度再構成にフォールバックする。
//   スカイボックスは深度テスト OFF で速度パスにも参加しないので、速度バッファ上は
//   ずっと 0 のまま＝そのまま読むと「TAA を ON にした途端に空だけブラーが消える」。
//   TAA.hlsl も同じ理由で同じフォールバックを持っている。
//
// ★符号について: 速度バッファは velocity = 現UV - 前UV（TAA の規約）だが、
//   このシェーダが欲しいのは「前フレームへ向かうベクトル」= 前UV - 現UV なので反転してある。
//   現在のタップ列 t = k/(n-1) - 0.5 は 0 対称なので実は符号を間違えても結果は同じだが、
//   片側カーネル（t ∈ [-1, 0] 等）に変えた瞬間に効いてくる。ここを触るときは思い出すこと。

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
SamplerState gSamp     : register(s0);   // LINEAR CLAMP（シーンのタップ用）
// ★速度は必ず POINT で読むこと。バイリニアだと物体のシルエットで前景と背景の速度が
//   混ざり、動く物の周囲 1px に逆方向のブラーの縁ができる（TAA も同じ理由で POINT）。
SamplerState gPoint    : register(s1);   // POINT CLAMP（速度/深度用）

float4 MotionBlurPS(FSQuadVSOut i) : SV_TARGET
{
    float2 uvFull = i.uv * rectP.zw + rectP.xy;
    float  depth  = gDepth.SampleLevel(gPoint, uvFull, 0).r;
    const bool isSky = (depth >= 1.0 - 1e-6);

    float2 vel;
    if (params.w > 0.5 && !isSky)
    {
        // 速度バッファ方式（オブジェクト毎のブラー）。TAA の符号規約から反転する。
        vel = -gVelocity.SampleLevel(gPoint, uvFull, 0).rg * params.x;
    }
    else
    {
        // 深度再構成方式（カメラの動きのみ）。空もここを通る。
        float2 ndc   = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
        float4 world = mul(invViewProj, float4(ndc, depth, 1.0));
        // w は負にもなり得る（錐台外の再構成）。abs で守りつつ符号は保つこと。
        world /= (abs(world.w) > 1e-6 ? world.w : 1e-6);
        float4 prevClip = mul(prevViewProj, world);
        float2 prevNdc  = prevClip.xy / (abs(prevClip.w) > 1e-6 ? prevClip.w : 1e-6);
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
