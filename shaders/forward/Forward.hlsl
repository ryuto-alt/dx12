// Forward.hlsl - Forward rendering pass shader with Lambert lighting

// Texture and sampler
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

// PerObject constants (b0) - MVP + Model matrix as RootConstants (32 DWORD)
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

// PerFrame constants (b1)
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float3   lightDir;    // directional light direction (world space)
    float    time;
    float3   lightColor;
    float    ambientStrength;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 texCoord : TEXCOORD0;
};

struct PSInput
{
    float4 positionSV  : SV_POSITION;
    float3 worldNormal : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV  = mul(float4(input.position, 1.0f), mvp);
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    output.color       = input.color;
    output.texCoord    = input.texCoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = g_albedo.Sample(g_sampler, input.texCoord);
    float4 baseColor = texColor * input.color;

    // Lambert diffuse
    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);  // light direction points toward light
    float  NdotL = max(dot(N, L), 0.0f);

    float3 ambient = ambientStrength * lightColor;
    float3 diffuse = NdotL * lightColor;
    float3 finalColor = baseColor.rgb * (ambient + diffuse);

    return float4(finalColor, baseColor.a);
}
