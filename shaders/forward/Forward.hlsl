// Forward.hlsl - Forward rendering pass shader

// PerObject constants (b0) - MVP matrix as RootConstants
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
};

// PerFrame constants (b1) - View, Proj, time
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float    time;
    float3   padding;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
};

struct PSInput
{
    float4 positionSV : SV_POSITION;
    float4 color      : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV = mul(float4(input.position, 1.0f), mvp);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
