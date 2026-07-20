// ForwardGrid.hlsl - Procedural grid floor shader + Shadow

// PerFrame 定数バッファ(b1) は Lighting.hlsli を include して共有定義を使う。
// これで C++ FrameConstants(現 1136B：末尾に IBL 制御 16B を追加) とのバイト一致が
// 1 箇所で担保され、二重宣言のドリフトが起きない。grid は shadowParams までしか
// 参照しないので、末尾に追加された cameraPos / 点光源 / IBL 系メンバの影響は受けない。
#include "Lighting.hlsli"

// Texture and sampler (unused, but RootSignature requires binding)
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

// Shadow map (CSM: Texture2DArray, 1スライス=1カスケード)。g_shadowSampler(s1)は Lighting.hlsli で共有宣言。
Texture2DArray         g_shadowMap      : register(t4);

// PerObject constants (b0) - MVP + Model matrix as RootConstants (32 DWORD)
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
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
    float  viewDepth   : TEXCOORD1;  // view 空間深度(正値) → カスケード選択
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV  = mul(float4(input.position, 1.0f), mvp);
    float4 worldPos4   = mul(float4(input.position, 1.0f), model);
    output.worldPos    = worldPos4.xyz;
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    float4 viewPos4    = mul(worldPos4, view);
    output.viewDepth   = viewPos4.z;
    return output;
}

int SelectCascade(float viewDepth)
{
    int c = NUM_CASCADES - 1;
    [unroll]
    for (int i = 0; i < NUM_CASCADES; ++i)
    {
        if (viewDepth <= cascadeSplitsView[i]) { c = i; break; }
    }
    return c;
}

float SampleCascade(int cascade, float3 worldPos)
{
    float4 lc = mul(float4(worldPos, 1.0f), cascadeViewProj[cascade]);
    float3 proj = lc.xyz / lc.w;
    float2 uv = proj.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return 1.0f;

    float current = proj.z;
    float texel = shadowParams.x;
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
        s += g_shadowMap.SampleCmpLevelZero(g_shadowSampler,
                 float3(uv + float2(x, y) * texel, (float)cascade), current);
    return s / 9.0f;
}

float CalcShadow(float3 worldPos, float viewDepth)
{
    int c = SelectCascade(viewDepth);
    return SampleCascade(c, worldPos);
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

    // アンチエイリアス。低角度/遠距離で fwidth が肥大化し線が完全に消える(描画抜け)のを防ぐため上限を設ける。
    float2 dGrid = min(fwidth(worldPos.xz), 0.5f);

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

    // 距離フェード: グリッド平面(±250m, kEditorGridSize/2)のフチに達する前(=230m)に滑らかに 0 へ落とす。
    // これで平面の矩形エッジが見えず、無限グリッド風に広く表示される(以前は 50m で途切れて抜けて見えた)。
    float dist = length(worldPos.xz);
    float fade = 1.0f - saturate((dist - 120.0f) / 110.0f);

    float gridMask = max(max(minor, major), max(axisX, axisZ));
    // 線以外を透明にする。床全体へ薄い色を重ねると Scene ビューだけ明るさが変わって見える。
    // 0.4 = グリッドを薄めにして配置オブジェクトを見やすくする(以前は 0.7 で濃かった)。
    float alpha = gridMask * 0.4f * fade;

    // シャドウ（CSM カスケード選択）
    float shadow = CalcShadow(input.worldPos, input.viewDepth);

    // ライティング
    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0f);
    float3 lighting = ambientStrength * lightColor + NdotL * lightColor * 0.3f * shadow;

    // グリッド色は表示基準(sRGB風)で調整されている。シーンRTはリニアHDRになった
    // (最終段でACES+ガンマが掛かる)ため、リニアへ変換して見た目を従来と揃える。
    return float4(pow(max(color * lighting, 0.0f), 2.2f), alpha);
}
