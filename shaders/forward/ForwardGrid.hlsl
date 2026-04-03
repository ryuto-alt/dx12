// ForwardGrid.hlsl - Procedural grid floor shader + Shadow

// Texture and sampler (unused, but RootSignature requires binding)
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

// Shadow map
Texture2D              g_shadowMap      : register(t4);
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
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV  : SV_POSITION;
    float3 worldPos    : TEXCOORD0;
    float3 worldNormal : NORMAL;
    float4 shadowCoord : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV  = mul(float4(input.position, 1.0f), mvp);
    float4 worldPos4   = mul(float4(input.position, 1.0f), model);
    output.worldPos    = worldPos4.xyz;
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    output.shadowCoord = mul(worldPos4, lightViewProj);
    return output;
}

float CalcShadow(float4 shadowCoord)
{
    float3 projCoords = shadowCoord.xyz / shadowCoord.w;
    float2 shadowUV = projCoords.xy * 0.5f + 0.5f;
    shadowUV.y = 1.0f - shadowUV.y;

    if (shadowUV.x < 0 || shadowUV.x > 1 || shadowUV.y < 0 || shadowUV.y > 1)
        return 1.0f;

    float currentDepth = projCoords.z;
    float shadow = 0.0f;
    float texelSize = 1.0f / 2048.0f;

    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            shadow += g_shadowMap.SampleCmpLevelZero(g_shadowSampler, shadowUV + float2(x, y) * texelSize, currentDepth);
        }
    }
    return shadow / 9.0f;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 worldPos = input.worldPos;

    // グリッド線の太さ（ワールド単位）
    float lineWidth = 0.02f;
    float lineWidthMajor = 0.04f;

    // 1m間隔のマイナーグリッド
    float2 gridPos = frac(abs(worldPos.xz));
    gridPos = min(gridPos, 1.0f - gridPos);

    // 5m間隔のメジャーグリッド
    float2 majorGridPos = frac(abs(worldPos.xz) / 5.0f);
    majorGridPos = min(majorGridPos, 1.0f - majorGridPos) * 5.0f;

    // アンチエイリアス
    float2 dGrid = fwidth(worldPos.xz);

    float2 minorLine = smoothstep(lineWidth - dGrid, lineWidth + dGrid, gridPos);
    float minor = 1.0f - min(minorLine.x, minorLine.y);

    float2 majorLine = smoothstep(lineWidthMajor - dGrid, lineWidthMajor + dGrid, majorGridPos);
    float major = 1.0f - min(majorLine.x, majorLine.y);

    float axisX = 1.0f - smoothstep(lineWidthMajor - dGrid.y, lineWidthMajor + dGrid.y, abs(worldPos.z));
    float axisZ = 1.0f - smoothstep(lineWidthMajor - dGrid.x, lineWidthMajor + dGrid.x, abs(worldPos.x));

    float3 groundColor = float3(0.15f, 0.15f, 0.15f);
    float3 minorColor  = float3(0.3f, 0.3f, 0.3f);
    float3 majorColor  = float3(0.45f, 0.45f, 0.45f);
    float3 axisXColor  = float3(0.8f, 0.2f, 0.2f);
    float3 axisZColor  = float3(0.2f, 0.2f, 0.8f);

    float3 color = groundColor;
    color = lerp(color, minorColor, minor * 0.6f);
    color = lerp(color, majorColor, major * 0.8f);
    color = lerp(color, axisXColor, axisX);
    color = lerp(color, axisZColor, axisZ);

    float dist = length(worldPos.xz);
    float fade = 1.0f - saturate((dist - 20.0f) / 30.0f);

    float gridMask = max(max(minor, major), max(axisX, axisZ));
    float alpha = lerp(0.05f, 0.7f, gridMask) * fade;

    // シャドウ
    float shadow = CalcShadow(input.shadowCoord);

    // ライティング
    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0f);
    float3 lighting = ambientStrength * lightColor + NdotL * lightColor * 0.3f * shadow;

    return float4(color * lighting, alpha);
}
