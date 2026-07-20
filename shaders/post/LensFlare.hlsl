// 疑似レンズフレア（John Chapman "Pseudo Lens Flare"）。
// ブルームチェーンの縮小ミップ（ビューポートローカル 0..1）を入力に、
// UV反転ゴースト＋ハロー＋RGB分離（色収差）を生成する。強度は焼き込み、uber が加算合成。

#include "FullscreenTri.hlsli"

cbuffer LFCB : register(b0)
{
    float4 p0;  // x=ゴースト数, y=分散(dispersal), z=ハロー半径, w=強度
    float4 p1;  // x=色収差量, y=中心減衰pow, zw=未使用
};

Texture2D    gSrc  : register(t0);   // ブルーム縮小ミップ（ローカル 0..1）
SamplerState gSamp : register(s0);

static float3 SampleCA(float2 uv, float2 dir, float ca)
{
    return float3(gSrc.Sample(gSamp, uv - dir * ca).r,
                  gSrc.Sample(gSamp, uv).g,
                  gSrc.Sample(gSamp, uv + dir * ca).b);
}

float4 LensFlarePS(FSQuadVSOut i) : SV_TARGET
{
    float2 uv       = 1.0 - i.uv;                 // 画面中心対称に反転
    float2 ghostVec = (0.5 - uv) * p0.y;
    float2 caDir    = normalize(ghostVec + 1e-5);

    float3 acc = 0.0;

    // ゴースト（中心を挟んで並ぶ光斑）
    int n = clamp((int)p0.x, 1, 8);
    [loop] for (int g = 0; g < n; ++g)
    {
        float2 suv = frac(uv + ghostVec * (float)g);
        float  wgt = length(float2(0.5, 0.5) - suv) / length(float2(0.5, 0.5));
        wgt = pow(saturate(1.0 - wgt), p1.y);     // 画面中心に近い光源ほど強い
        acc += SampleCA(suv, caDir, p1.x) * wgt;
    }

    // ハロー（リング）
    float2 haloVec = normalize(ghostVec + 1e-5) * p0.z;
    float2 huv = frac(uv + haloVec);
    float  wh  = length(float2(0.5, 0.5) - huv) / length(float2(0.5, 0.5));
    wh = pow(saturate(1.0 - wh), 6.0);
    acc += SampleCA(huv, caDir, p1.x) * wh;

    return float4(acc * p0.w, 1.0);
}
