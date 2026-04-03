// DebugLine.hlsl - Wireframe debug line rendering
// RootConstants b0: float4x4 viewProj (16 DWORD)

cbuffer ViewProj : register(b0)
{
    float4x4 gViewProj;
};

struct VSIn
{
    float3 pos   : POSITION;
    float3 color : COLOR;
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
};

PSIn VSMain(VSIn v)
{
    PSIn o;
    o.pos   = mul(float4(v.pos, 1.0f), gViewProj);
    o.color = v.color;
    return o;
}

float4 PSMain(PSIn p) : SV_TARGET
{
    return float4(p.color, 1.0f);
}
