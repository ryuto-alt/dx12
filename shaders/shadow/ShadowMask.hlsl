// ShadowMask.hlsl - アルファクリップ(MASK)マテリアル専用の深度パス。
//
// 通常の ShadowPass.hlsl は PS を持たない＝葉テクスチャの抜けを知らないので、
// 「葉の影が板の影になる」。ここは UV を渡して baseColor.a < cutoff を discard する。
// CSM / スポット影 / ポイント影 / カメラの深度プリパス（SSAO・LESS_EQUAL の土台）で共用する。
//
// ★このファイル 1 本から 4 本の CSO を作る（静的 / インスタンシング / スキンド の VS と 共有 PS）。
//   b0 の意味が VS ごとに違うので注意:
//     VSMain          … mvp(=world*viewProj), model      （呼び出し側は 32 DWORD 書く）
//     VSMainInstanced … transpose(viewProj)              （16 DWORD。world は slot1 から）
//     VSMainSkinned   … mvp, model + t3 のボーン行列     （32 DWORD）
// ★cutoff / UV 変換は b2（PBRMaterial ルート定数）から読む。フォワードとまったく同じ値を
//   同じ規則で読むこと。ズレると「本体は抜けているのに影だけ残る」になる。

Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

StructuredBuffer<float4x4> g_bones : register(t3);

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

cbuffer PBRMaterial : register(b2)
{
    float defaultMetallic;
    float defaultRoughness;
    uint  pbrFlags;        // bit8..15 = alphaCutoff(8bit 量子化)
    uint  packedTint;
    float4 uvScaleOffset;  // xy=スケール zw=オフセット（無効時は (1,1,0,0)）
};

struct PSInput
{
    float4 positionSV : SV_POSITION;
    float2 texCoord   : TEXCOORD0;
};

// ---- 静的メッシュ ----
struct VSInput
{
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    o.positionSV = mul(float4(input.position, 1.0f), mvp);
    o.texCoord   = input.texCoord;
    return o;
}

// ---- インスタンシング（slot1 の per-instance world、b0 は transpose(viewProj)）----
struct VSInputInstanced
{
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
    float4 ir0      : TEXCOORD8;
    float4 ir1      : TEXCOORD9;
    float4 ir2      : TEXCOORD10;
};

PSInput VSMainInstanced(VSInputInstanced input)
{
    float4x4 modelT = float4x4(input.ir0, input.ir1, input.ir2, float4(0, 0, 0, 1));
    float4 wp = mul(modelT, float4(input.position, 1.0f));
    PSInput o;
    o.positionSV = mul(wp, mvp);   // ここでは b0 の先頭 16 DWORD = transpose(viewProj)
    o.texCoord   = input.texCoord;
    return o;
}

// ---- スキンド（ShadowPassSkinned.hlsl と同じ演算順にすること）----
struct VSInputSkinned
{
    float3 position    : POSITION;
    float2 texCoord    : TEXCOORD0;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

PSInput VSMainSkinned(VSInputSkinned input)
{
    float4 pos = float4(input.position, 1.0f);
    float4 skinnedPos = (float4)0;

    float totalWeight = input.boneWeights.x + input.boneWeights.y
                      + input.boneWeights.z + input.boneWeights.w;

    if (totalWeight > 0.0f)
    {
        skinnedPos += input.boneWeights.x * mul(pos, g_bones[input.boneIndices.x]);
        skinnedPos += input.boneWeights.y * mul(pos, g_bones[input.boneIndices.y]);
        skinnedPos += input.boneWeights.z * mul(pos, g_bones[input.boneIndices.z]);
        skinnedPos += input.boneWeights.w * mul(pos, g_bones[input.boneIndices.w]);
    }
    else
    {
        skinnedPos = pos;
    }

    PSInput o;
    o.positionSV = mul(skinnedPos, mvp);
    o.texCoord   = input.texCoord;
    return o;
}

// ---- 共有 PS（深度しか書かないので色は返さない）----
void PSMain(PSInput input)
{
    float2 uv = input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;
    const float cutoff = float((pbrFlags >> 8) & 0xFF) / 255.0;
    clip(g_albedo.Sample(g_sampler, uv).a - cutoff);
}
