// PBR.hlsli - Cook-Torrance BRDF functions for PBR rendering

#ifndef PBR_HLSLI
#define PBR_HLSLI

static const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz Normal Distribution Function
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0000001);
}

// Smith's Geometry function (Schlick-GGX)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

// Fresnel-Schlick approximation
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// roughness を考慮した Fresnel（IBL 用。グレージング角の過剰反射を抑える）
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 r = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0);
    return F0 + (r - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// ACES tone mapping (filmic)
float3 ACESFilm(float3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Normal map decoding: tangent space -> world space
//
// z は xy から再構成する（z = sqrt(1 - x^2 - y^2)）。
// 法線マップは BC5_UNORM（RG 2ch）で圧縮して読み込むため、サンプルの b は常に 0 になる。
// 単位長のタンジェント空間法線なら z は xy から一意に決まるので、
// 非圧縮(RGB)テクスチャでも同じ結果になり、経路を分ける必要がない。
float3 PerturbNormal(float3 worldNormal, float3 worldTangent, float tangentW,
                     float3 normalMapSample)
{
    float3 N = normalize(worldNormal);
    float3 T = normalize(worldTangent - dot(worldTangent, N) * N); // Gram-Schmidt orthogonalize
    float3 B = cross(N, T) * tangentW;
    float3x3 TBN = float3x3(T, B, N);

    float2 xy = normalMapSample.xy * 2.0 - 1.0;
    // ★z を 0 まで落とさないこと（2026-08-02）。
    //   xy の長さが 1 に達すると saturate で z=0 になり、法線が【面に完全に寝てしまう】。
    //   そうなるとライトがその面を舐める角度（懐中電灯で床を照らす等）で N·L が 0〜負に
    //   なり、面が黒い斑点で埋まって「向きによって光の広がり方が別物」に見える。
    //   xy が 1 を超えるのは異常ではなく、強めの法線マップ + 高いタイリング + フィルタリングで
    //   普通に起きる（実測: 床の uvTiling=67.6 で床全体が真っ黒になった）。
    //   接空間法線は面から離れすぎない、という物理的な前提を入れて打ち止める。
    float  z  = sqrt(saturate(1.0 - dot(xy, xy)));
    float3 tangentNormal = normalize(float3(xy, max(z, 0.35)));   // 幾何法線から最大約 69°
    return normalize(mul(tangentNormal, TBN));
}


// ---------------------------------------------------------------------------
// 法線マップフィルタリング（specular anti-aliasing / 分散→ラフネス）
//
// 1 画素の中に法線マップの山谷が何個も入る（高タイリング / 遠景 / グレージング角）と、
// 1 サンプルの法線はもはや画素を代表しない。本来評価したいのは「画素内の法線分布」なので、
//   ① 分布の広がり（分散）を GGX の α へ足し込む      → 鏡面のちらつきが消える
//   ② 分散が大きい画素は法線を幾何法線へ寄せる          → N·L の符号反転（黒斑点）が消える
// を行う。分散はスクリーン空間の法線微分から見積もる（Kaplanyan / Tokuyoshi の流儀）。
// これならタイリング倍率・距離・視野角を全部含んだ「実効的な」分散が 1 画素ごとに取れる。
//
// ★ミップに焼かない理由: 法線マップは BC5_UNORM(RG 2ch) で z を再構成しているので、
//   平均法線の長さ（＝分散）がテクスチャに残らない（常に単位長に戻る）。
//
// params: .x=強さ(0 で恒等) .y=α に足せる量の上限 .z=幾何法線へ寄せる強さ
void FilterShadingNormal(inout float3 N, inout float roughness, float3 Ng, float3 params)
{
    // ★ddx/ddy は分岐の外で取る（勾配命令を非一様な制御フローに入れない）。
    float3 dNx = ddx(N);
    float3 dNy = ddy(N);
    if (params.x <= 0.0) return;

    // 画素内の法線分散（スクリーン空間の 1 画素分の広がり）
    float variance = 0.25 * params.x * (dot(dNx, dNx) + dot(dNy, dNy));

    // ① 分散 → ラフネス。GGX の幅は α = roughness^2 なので α 側に足す。
    float alpha = roughness * roughness;
    alpha = saturate(alpha + min(2.0 * variance, max(params.y, 0.0)));
    roughness = sqrt(alpha);

    // ② 平均法線の復元。分散が 1 に近い＝画素内で法線が散り散り＝平均は幾何法線。
    //    normalize の前に lerp するので、t=1 でちょうど幾何法線になる。
    float t = saturate(variance * params.z);
    N = normalize(lerp(N, Ng, t));
}

#endif // PBR_HLSLI
