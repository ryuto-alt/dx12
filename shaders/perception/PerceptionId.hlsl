// PerceptionId.hlsl — 知覚層（dx12_perceive）の ID パス。
//
// ★要求があったフレームだけ走る独立したパス。メインのルートシグネチャ（62/64 DWORD で満杯）には
//   1 DWORD も足さず、src/renderer/PerceptionPass.cpp の専用ルートシグネチャで描く。
//   出力は 3 枚の MRT + 自前の深度:
//     SV_Target0 (R32_UINT)           … エンティティ ID（0 = 何も描かれていない）
//     SV_Target1 (R32G32B32A32_FLOAT) … ワールド座標 xyz + カメラからの距離 w
//     SV_Target2 (R8G8B8A8_SNORM)     … シェーディング法線（ワールド, xyz）
//   被覆マスク用（遮蔽なしで対象だけを描く）の PSO は同じシェーダで RT0 だけを持つ。
//
// ★頂点変換はフォワード（Forward.hlsl / ForwardSkinned.hlsl）と同じ式に揃えてある:
//     静的   : wp = mul(pos, model)、N = normalize(mul(normal, (float3x3)model))
//     スキンド: skin = Σ w·B → mul(pos, skin) → model（DepthPrepassSkinned と同じ演算順）
//   ★法線はカメラ側へ裏返さない。フォワードは両面描画（CULL_NONE）で、裏面も表の法線のまま
//     ライティングする（＝裏面が見えていると灯りが手前でも真っ黒になる）。知覚層が見たいのは
//     「画面に出ている明るさの理由」なので、フォワードと同じ法線を返す。
// ★MASK（アルファクリップ）はフォワード / 深度パスと同じ UV・同じ cutoff で抜く（flags bit0）。

Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

StructuredBuffer<float4x4> g_bones : register(t3);

cbuffer PerDraw : register(b0)
{
    float4x4 model;         // transpose(world)（行ベクトル規約で mul(v, model)）
    uint     entityId;      // 書き込む ID（被覆マスクのパスでは 1）
    uint     flags;         // bit0 = MASK（アルファクリップ）
    float    alphaCutoff;
    float    pad0;
};

cbuffer PerPass : register(b1)
{
    float4x4 viewProj;      // transpose(viewProj)（ジッタなし）
    float3   cameraPos;
    float    pad1;
};

cbuffer MaskParams : register(b2)
{
    float4 uvScaleOffset;   // xy=スケール zw=オフセット（ComputeMeshUvScaleOffset と同じ値）
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float2 texCoord    : TEXCOORD0;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

struct PSInput
{
    float4 positionSV : SV_POSITION;
    float3 worldPos   : TEXCOORD1;
    float3 normal     : TEXCOORD2;
    float2 texCoord   : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    const float4 wp = mul(float4(input.position, 1.0f), model);
    o.positionSV = mul(wp, viewProj);
    o.worldPos   = wp.xyz;
    o.normal     = normalize(mul(input.normal, (float3x3)model));
    o.texCoord   = input.texCoord;
    return o;
}

PSInput VSMainSkinned(VSInput input)
{
    float4x4 skinMatrix =
        input.boneWeights.x * g_bones[input.boneIndices.x] +
        input.boneWeights.y * g_bones[input.boneIndices.y] +
        input.boneWeights.z * g_bones[input.boneIndices.z] +
        input.boneWeights.w * g_bones[input.boneIndices.w];
    const float4 skinnedPos    = mul(float4(input.position, 1.0f), skinMatrix);
    const float3 skinnedNormal = normalize(mul(input.normal, (float3x3)skinMatrix));

    PSInput o;
    const float4 wp = mul(skinnedPos, model);
    o.positionSV = mul(wp, viewProj);
    o.worldPos   = wp.xyz;
    o.normal     = normalize(mul(skinnedNormal, (float3x3)model));
    o.texCoord   = input.texCoord;
    return o;
}

struct PSOutput
{
    uint   id      : SV_Target0;
    float4 posDist : SV_Target1;
    float4 normal  : SV_Target2;
};

PSOutput PSMain(PSInput input)
{
    if (flags & 1u)
    {
        const float2 uv = input.texCoord * uvScaleOffset.xy + uvScaleOffset.zw;
        clip(g_albedo.Sample(g_sampler, uv).a - alphaCutoff);
    }
    PSOutput o;
    o.id      = entityId;
    o.posDist = float4(input.worldPos, length(input.worldPos - cameraPos));
    const float len = length(input.normal);
    o.normal  = float4(len > 1e-6 ? input.normal / len : float3(0, 0, 0), 0.0f);
    return o;
}
