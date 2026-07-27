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

// ---------------------------------------------------------------------------
// アンチエイリアス済みの格子線の被覆率(0..1)。widthPx = 画面上の線の太さ(ピクセル)。
// 手順は Ben Golus "The Best Darn Grid Shader (Yet)" に合わせてある:
//   ・drawWidth を 1px 未満にしない。細るぶんは輝度で表す(phone-wire AA)＝
//     遠距離/低角度でも線が「消える」のではなく「薄くなる」だけになる
//   ・1 セルがサブピクセルまで詰まったら一様な塗りへ落としてモアレを止める
// ponytail: 線幅が 0.5 セルを超える極太ケースの反転処理は使わないので省略。
// ---------------------------------------------------------------------------
float GridCoverage(float2 pos, float2 deriv, float cell, float widthPx)
{
    float2 uv      = pos / cell;
    float2 uvDeriv = deriv / cell;
    float2 target  = widthPx * uvDeriv;                    // 線幅(セル比)
    float2 drawW   = clamp(target, uvDeriv, 0.5f);
    float2 lineAA  = uvDeriv * 1.5f;
    float2 gridUV  = 1.0f - abs(frac(uv) * 2.0f - 1.0f);   // 0=線上, 1=セル中央
    float2 g       = smoothstep(drawW + lineAA, drawW - lineAA, gridUV);
    g             *= saturate(target / drawW);             // 1px 未満ぶんを輝度で補償
    g              = lerp(g, target, saturate(uvDeriv * 2.0f - 1.0f));  // モアレ抑制
    return lerp(g.x, 1.0f, g.y);
}

// 軸線(1 本だけ)の被覆率。格子と違い繰り返さないのでモアレ対策は要らない。
float AxisCoverage(float coord, float deriv, float widthPx)
{
    float px = abs(coord) / deriv;   // 軸までの距離(ピクセル)
    return 1.0f - smoothstep(widthPx - 0.5f, widthPx + 0.5f, px);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 worldPos = input.worldPos;

    // ワールド 1 単位あたりの画面変化量。ddx/ddy をベクトルとして長さを取る。
    // fwidth(=|ddx|+|ddy|) より斜め視点で正確＝カメラを回しても線幅がぶれない。
    float4 dd    = float4(ddx(worldPos.xz), ddy(worldPos.xz));
    float2 deriv = max(float2(length(dd.xz), length(dd.yw)), 1e-6f);

    // セル間隔を 10 倍刻みで自動切替する（Blender/Unity と同じ考え方）。
    // 1セルが画面上で kTargetPx を切らないよう桁を上げる ＝ 遠くでも高空でも
    // 格子が詰まってモアレにならず、逆にグリッドが消えることもない＝実質無限グリッド。
    const float kTargetPx = 16.0f;
    float level  = max(0.0f, log10(kTargetPx * max(deriv.x, deriv.y)));  // 0=1m, 1=10m...
    float fine   = pow(10.0f, floor(level));
    float coarse = fine * 10.0f;
    float fadeIn = 1.0f - frac(level);   // 桁上がりで細い方を消す

    // 線は「明るい芯」＋「その外側の暗い縁取り」の 2 層で描く。空のような明るい背景でも
    // 夜/暗い床でも必ずどちらかがコントラストを持つ＝背景色に依存せず読める。
    const float kMinorPx = 1.1f;
    const float kMajorPx = 1.8f;
    const float kAxisPx  = 2.4f;
    const float kHaloPx  = 1.5f;   // 縁取りは芯より何 px 太いか

    float minorC = GridCoverage(worldPos.xz, deriv, fine,   kMinorPx)           * fadeIn;
    float minorH = GridCoverage(worldPos.xz, deriv, fine,   kMinorPx + kHaloPx) * fadeIn;
    float majorC = GridCoverage(worldPos.xz, deriv, coarse, kMajorPx);
    float majorH = GridCoverage(worldPos.xz, deriv, coarse, kMajorPx + kHaloPx);
    float axisX  = AxisCoverage(worldPos.z, deriv.y, kAxisPx);   // +X 軸 (z=0)
    float axisZ  = AxisCoverage(worldPos.x, deriv.x, kAxisPx);   // +Z 軸 (x=0)
    float axisH  = max(AxisCoverage(worldPos.z, deriv.y, kAxisPx + kHaloPx),
                       AxisCoverage(worldPos.x, deriv.x, kAxisPx + kHaloPx));

    // 芯。ライティング/シャドウは掛けない＝ライトの当たり方や影で線が沈まない。
    const float3 kMinorColor = float3(0.86f, 0.87f, 0.90f);
    const float3 kMajorColor = float3(1.00f, 1.00f, 1.00f);
    const float3 kAxisXColor = float3(1.00f, 0.34f, 0.36f);
    const float3 kAxisZColor = float3(0.36f, 0.62f, 1.00f);
    const float3 kHaloColor  = float3(0.02f, 0.02f, 0.03f);

    float3 coreColor = kMinorColor;
    float  coreAlpha = minorC * 0.45f;
    coreColor = lerp(coreColor, kMajorColor, majorC);
    coreAlpha = max(coreAlpha, majorC * 0.80f);
    coreColor = lerp(coreColor, kAxisXColor, axisX);
    coreColor = lerp(coreColor, kAxisZColor, axisZ);
    coreAlpha = max(coreAlpha, max(axisX, axisZ) * 1.0f);

    float haloAlpha = max(max(minorH * 0.30f, majorH * 0.50f), axisH * 0.60f);

    // 芯を縁取りの上に合成して 1 枚のストレートアルファに畳む。
    // グリッド色は表示基準(sRGB風)で調整してあるので、合成はリニアへ直してから行う
    // （シーン RT はリニア HDR。最終段で ACES+ガンマが掛かる）。
    float  outAlpha = coreAlpha + haloAlpha * (1.0f - coreAlpha);
    float3 outColor = (pow(coreColor, 2.2f) * coreAlpha
                     + pow(kHaloColor, 2.2f) * haloAlpha * (1.0f - coreAlpha))
                    / max(outAlpha, 1e-4f);

    // 距離フェード: 原点からではなく「カメラの真下」から測る＝カメラと一緒にグリッドが
    // ついてくる。板は ±10km(kEditorGridSize/2)あるので、どこへ飛んでも足元に線がある。
    // フェード半径はカメラ高度に比例させる: 地面に近いほど手元だけ、上へ引くほど広く出す。
    // ★下限は 400m ではなく 20m。400m だとカメラ高 1.7m の一人称でも fadeStart=220m に
    //   なり、30m 程度の屋内シーンではフェードが一切効かず、床全面にグリッドの膜が
    //   乗ったままになる（グリッド板は y=0 で床の上面と同一平面、かつ DepthBias が
    //   負なので必ず床に勝つ）。20m なら fadeEnd≈24m / fadeStart≈13m で足元だけに出る。
    float fadeEnd   = clamp(abs(cameraPos.y - worldPos.y) * 14.0f, 20.0f, 9000.0f);
    float fadeStart = fadeEnd * 0.55f;
    float dist = length(worldPos.xz - cameraPos.xz);
    outAlpha *= 1.0f - saturate((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3f));

    return float4(outColor, outAlpha);
}
