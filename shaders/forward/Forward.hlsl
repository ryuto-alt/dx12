// Forward.hlsl - Forward rendering pass shader with Lambert lighting + Shadow

// Texture and sampler
Texture2D    g_albedo    : register(t0);
SamplerState g_sampler   : register(s0);

// Shadow map
Texture2D              g_shadowMap      : register(t2);
SamplerComparisonState g_shadowSampler  : register(s1);

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
    float3   lightDir;
    float    time;
    float3   lightColor;
    float    ambientStrength;
    float4x4 lightViewProj;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    uint4  boneIndices : BLENDINDICES;  // unused (static mesh)
    float4 boneWeights : BLENDWEIGHT;   // unused (static mesh)
};

struct PSInput
{
    float4 positionSV   : SV_POSITION;
    float3 worldNormal  : NORMAL;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
    float4 shadowCoord  : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV  = mul(float4(input.position, 1.0f), mvp);
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    output.color       = input.color;
    output.texCoord    = input.texCoord;

    // ライト空間座標
    float4 worldPos = mul(float4(input.position, 1.0f), model);
    output.shadowCoord = mul(worldPos, lightViewProj);

    return output;
}

float CalcShadow(float4 shadowCoord)
{
    // パースペクティブ除算
    float3 projCoords = shadowCoord.xyz / shadowCoord.w;

    // NDC [-1,1] → UV [0,1]
    float2 shadowUV = projCoords.xy * 0.5f + 0.5f;
    shadowUV.y = 1.0f - shadowUV.y;  // DX12 UV座標系

    // 範囲外はシャドウなし
    if (shadowUV.x < 0 || shadowUV.x > 1 || shadowUV.y < 0 || shadowUV.y > 1)
        return 1.0f;

    float currentDepth = projCoords.z;

    // 3x3 PCF (Percentage Closer Filtering)
    float shadow = 0.0f;
    float texelSize = 1.0f / 2048.0f;  // シャドウマップ解像度

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float2 offset = float2(x, y) * texelSize;
            shadow += g_shadowMap.SampleCmpLevelZero(g_shadowSampler, shadowUV + offset, currentDepth);
        }
    }
    shadow /= 9.0f;

    return shadow;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = g_albedo.Sample(g_sampler, input.texCoord);
    float4 baseColor = texColor * input.color;

    // Lambert diffuse
    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float  NdotL = max(dot(N, L), 0.0f);

    float shadow = CalcShadow(input.shadowCoord);

    float3 ambient = ambientStrength * lightColor;
    float3 diffuse = NdotL * lightColor * shadow;
    float3 finalColor = baseColor.rgb * (ambient + diffuse);

    return float4(finalColor, baseColor.a);
}
