// 2D スプライト（スクリーン空間）。正射影でピクセル座標 → クリップ空間へ。
// RootConstants b0: float4x4 ortho（転置済み）

cbuffer SpriteCB : register(b0)
{
    float4x4 gOrtho;
};

struct VSIn
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);

PSIn VSMain(VSIn v)
{
    PSIn o;
    o.pos = mul(float4(v.pos, 0.0f, 1.0f), gOrtho);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

float4 PSMain(PSIn p) : SV_TARGET
{
    return gTex.Sample(gSamp, p.uv) * p.col;
}
