// ForwardGrid.hlsl - Procedural grid floor shader
// ワールド座標ベースでグリッド線を描画する

// Texture and sampler (unused, but RootSignature requires binding)
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
    float3   lightDir;
    float    time;
    float3   lightColor;
    float    ambientStrength;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV : SV_POSITION;
    float3 worldPos   : TEXCOORD0;
    float3 worldNormal : NORMAL;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV  = mul(float4(input.position, 1.0f), mvp);
    float4 worldPos4   = mul(float4(input.position, 1.0f), model);
    output.worldPos    = worldPos4.xyz;
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 worldPos = input.worldPos;

    // グリッド線の太さ（ワールド単位）
    float lineWidth = 0.02f;
    float lineWidthMajor = 0.04f;

    // 1m間隔のマイナーグリッド
    float2 gridPos = frac(abs(worldPos.xz));
    gridPos = min(gridPos, 1.0f - gridPos);  // 0～0.5 → 線までの距離

    // 5m間隔のメジャーグリッド
    float2 majorGridPos = frac(abs(worldPos.xz) / 5.0f);
    majorGridPos = min(majorGridPos, 1.0f - majorGridPos) * 5.0f;

    // アンチエイリアス（画面空間の微分値でスムーズに）
    float2 dGrid = fwidth(worldPos.xz);

    // マイナーグリッド線
    float2 minorLine = smoothstep(lineWidth - dGrid, lineWidth + dGrid, gridPos);
    float minor = 1.0f - min(minorLine.x, minorLine.y);

    // メジャーグリッド線
    float2 majorLine = smoothstep(lineWidthMajor - dGrid, lineWidthMajor + dGrid, majorGridPos);
    float major = 1.0f - min(majorLine.x, majorLine.y);

    // 原点の軸線（X軸=赤、Z軸=青）
    float axisX = 1.0f - smoothstep(lineWidthMajor - dGrid.y, lineWidthMajor + dGrid.y, abs(worldPos.z));
    float axisZ = 1.0f - smoothstep(lineWidthMajor - dGrid.x, lineWidthMajor + dGrid.x, abs(worldPos.x));

    // 基本色
    float3 groundColor = float3(0.25f, 0.25f, 0.25f);  // ダークグレー
    float3 minorColor  = float3(0.4f, 0.4f, 0.4f);     // グレー
    float3 majorColor  = float3(0.55f, 0.55f, 0.55f);   // ライトグレー
    float3 axisXColor  = float3(0.8f, 0.2f, 0.2f);      // 赤（X軸）
    float3 axisZColor  = float3(0.2f, 0.2f, 0.8f);      // 青（Z軸）

    // 合成
    float3 color = groundColor;
    color = lerp(color, minorColor, minor * 0.6f);
    color = lerp(color, majorColor, major * 0.8f);
    color = lerp(color, axisXColor, axisX);
    color = lerp(color, axisZColor, axisZ);

    // 距離フェードアウト（遠くのグリッドを薄くする）
    float dist = length(worldPos.xz);
    float fade = 1.0f - saturate((dist - 20.0f) / 30.0f);

    // グリッドが無い部分は完全に暗い色
    float gridMask = max(max(minor, major), max(axisX, axisZ));
    float alpha = lerp(0.3f, 1.0f, gridMask) * fade;

    // 簡易ライティング
    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0f);
    float3 lighting = ambientStrength * lightColor + NdotL * lightColor * 0.3f;

    return float4(color * lighting, alpha);
}
