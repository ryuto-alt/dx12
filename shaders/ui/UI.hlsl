// UI.hlsl - 2D UI rendering for RmlUi
// RootConstants b0: float4x4 ortho (16 DWORD) + float2 translation + float2 pad (4 DWORD)
// SRV t0: UI texture
// Sampler s0: linear clamp

cbuffer UIConstants : register(b0)
{
    float4x4 gOrtho;
    float2   gTranslation;
    float2   _pad;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSIn
{
    float2 pos   : POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

PSIn VSMain(VSIn v)
{
    PSIn o;
    float2 translated = v.pos + gTranslation;
    o.pos   = mul(float4(translated, 0.0f, 1.0f), gOrtho);
    o.color = v.color;
    o.uv    = v.uv;
    return o;
}

float4 PSMain(PSIn p) : SV_TARGET
{
    float4 texColor = gTexture.Sample(gSampler, p.uv);
    return texColor * p.color;
}
