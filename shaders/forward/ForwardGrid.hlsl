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
// PCSS / 3x3 PCF の共有実装（g_shadowMap と Lighting.hlsli の後で include すること）
#include "ShadowPcss.hlsli"

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

float SampleCascade(int cascade, float3 worldPos, float2 svPos)
{
    // ★実体は ShadowPcss.hlsli（PCSS / 3x3 PCF の切替込み。4 つの PS で共有）
    return SampleShadowCascadeCommon(g_shadowMap, cascade, worldPos, svPos, 0.0f);
}

float CalcShadow(float3 worldPos, float viewDepth, float2 svPos)
{
    int c = SelectCascade(viewDepth);
    return SampleCascade(c, worldPos, svPos);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 worldPos = input.worldPos;

    // 1 ワールド単位あたりの画面変化量。これで割ると「線までの距離(ピクセル)」になる。
    float2 dW = max(fwidth(worldPos.xz), 1e-5f);

    // 線幅はワールド単位ではなくピクセル固定にする。ワールド幅の smoothstep だと
    // 遠距離/低角度で線がサブピクセルになり薄れて消えていた（グリッドが見えない主因）。
    const float kFinePx   = 1.0f;
    const float kCoarsePx = 1.5f;
    const float kAxisPx   = 2.0f;

    // セル間隔を 10 倍刻みで自動切替する（Blender/Unity と同じ考え方）。
    // 1セルが画面上で kTargetPx を切らないよう桁を上げる ＝ 遠くでも高空でも
    // 格子が詰まってモアレにならず、逆にグリッドが消えることもない＝実質無限グリッド。
    const float kTargetPx = 14.0f;
    float pxPerMeter = 1.0f / max(dW.x, dW.y);
    float level      = max(0.0f, log10(kTargetPx / pxPerMeter));   // 0=1m, 1=10m, 2=100m...
    float fineCell   = pow(10.0f, floor(level));
    float coarseCell = fineCell * 10.0f;
    float blend      = frac(level);   // 1 に近づくほど細い方を消して桁上がりする

    // cell 間隔の格子線。frac(p/cell+0.5)-0.5 で最寄りの線までの距離(セル比) → m → px。
    float2 dFine   = abs(frac(worldPos.xz / fineCell   + 0.5f) - 0.5f) * fineCell   / dW;
    float2 dCoarse = abs(frac(worldPos.xz / coarseCell + 0.5f) - 0.5f) * coarseCell / dW;
    float  minor   = (1.0f - saturate(min(dFine.x,   dFine.y)   / kFinePx))   * (1.0f - blend);
    float  major   =  1.0f - saturate(min(dCoarse.x, dCoarse.y) / kCoarsePx);

    float axisX = 1.0f - saturate(abs(worldPos.z) / dW.y / kAxisPx);
    float axisZ = 1.0f - saturate(abs(worldPos.x) / dW.x / kAxisPx);

    // 表示色は「線そのものの色」。ライティング/シャドウは掛けない＝ライトの当たり方や
    // 影で線が沈まない（暗いシーン・逆光でも同じ濃さで見える）。
    // 濃さは「あることが分かる」程度に抑える(以前は白くて配置物より目立っていた)。
    float3 minorColor = float3(0.52f, 0.53f, 0.56f);
    float3 majorColor = float3(0.68f, 0.69f, 0.73f);
    float3 axisXColor = float3(0.76f, 0.30f, 0.32f);
    float3 axisZColor = float3(0.30f, 0.46f, 0.80f);

    float3 color = minorColor;
    float  alpha = minor * 0.20f;

    color = lerp(color, majorColor, major);
    alpha = max(alpha, major * 0.34f);

    color = lerp(color, axisXColor, axisX);
    color = lerp(color, axisZColor, axisZ);
    alpha = max(alpha, max(axisX, axisZ) * 0.55f);

    // 距離フェード: 原点からではなく「カメラの真下」から測る＝カメラと一緒にグリッドが
    // ついてくる。板は ±10km(kEditorGridSize/2)あるので、どこへ飛んでも足元に線がある。
    // フェード半径はカメラ高度に比例させる: 地面に近いほど手元だけ、上へ引くほど広く出す。
    float fadeEnd   = clamp(abs(cameraPos.y - worldPos.y) * 14.0f, 400.0f, 9000.0f);
    float fadeStart = fadeEnd * 0.45f;
    float dist = length(worldPos.xz - cameraPos.xz);
    alpha *= 1.0f - saturate((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3f));

    // グリッド色は表示基準(sRGB風)で調整されている。シーンRTはリニアHDRなので
    // (最終段でACES+ガンマが掛かる)リニアへ変換して見た目を揃える。
    return float4(pow(max(color, 0.0f), 2.2f), alpha);
}
