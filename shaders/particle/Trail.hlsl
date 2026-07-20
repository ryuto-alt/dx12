// Trail.hlsl - 軌跡リボン（剣の残像/弾道/魔法の尾）。
// CPU 側でカメラフェーシングの帯ジオメトリを構築し、頂点ストリームで受ける。
// PS はプロシージャルなストリーク断面（明るい芯＋ソフトな縁）。HDR へ加算 or 前乗算α。
// パーティクルと同じ RootSignature（b0 32定数 + t0 深度SRV + s0）を共有する。
#include "ParticleCommon.hlsli"

cbuffer CamCB : register(b0)
{
    float4x4 viewProj;   // 転置済み（mul(rowvec, M)）
    float4   camRight;   // 未使用（レイアウト共有のため）
    float4   camUp;      // 未使用
    float4   params;     // x=globalIntensity, y=未使用, z=time, w=softFadeDist
    float4   params2;    // x=projA, y=projB, z=1/RTw, w=1/RTh（z<=0 で soft 無効）
};

Texture2D    gSceneDepth : register(t0);
SamplerState sDepth      : register(s0);

struct VSIn
{
    float3 pos : POSITION;
    float4 col : COLOR0;      // rgb=HDR色(強度込み), a=寿命フェード
    float2 uv  : TEXCOORD0;   // x=尾0..先頭1, y=横 -1..1
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 col   : COLOR0;
    float2 uv    : TEXCOORD0;
    float  viewZ : TEXCOORD1;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos   = mul(float4(i.pos, 1.0), viewProj);
    o.col   = i.col;
    o.uv    = i.uv;
    o.viewZ = o.pos.w;
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    // soft particles: シーン深度で手動オクルージョン
    float soft = 1.0;
    if (params2.z > 0.0)
    {
        float2 suv = i.pos.xy * float2(params2.z, params2.w);
        float sceneD = gSceneDepth.SampleLevel(sDepth, suv, 0).r;
        float sceneZ = params2.y / (sceneD - params2.x);
        soft = saturate((sceneZ - i.viewZ) / max(params.w, 1e-3));
    }

    // 断面: 明るい芯 + ソフトな縁。先頭(u=1)ほど鋭く、尾は緩む
    float y = i.uv.y;
    float core = exp(-abs(y) * (5.0 + 6.0 * i.uv.x)) * (0.6 + 0.9 * i.uv.x);
    float halo = exp(-abs(y) * 2.2) * 0.45;
    float shape = core + halo;

    float cov = saturate(shape) * i.col.a * soft;
    clip(cov - 1e-3);
    float3 rgb = i.col.rgb * cov * params.x;   // 前乗算（加算/前乗算α 両PSO共用）
    return float4(rgb, cov);
}
