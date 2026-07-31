#pragma once

// 新規カスタムシェーダー(MeshRenderer::shaderPath 割当用)のテンプレート。
// エディタが assets/shaders/<name>.hlsl として書き出す雛形。
//
// 静的メッシュ用の共有 RootSignature(src/graphics/RootSignature.h)と Mesh::GetInputLayout() に
// 適合させてある: b0=PerObject(mvp+model)、b1=PerFrame(先頭6フィールドだけ部分宣言。
// shaders/forward/Lighting.hlsli の cbuffer PerFrameConstants と同一オフセットになるよう
// フィールド順を厳守すること)、t0+s0=アルベドテクスチャ。エントリポイントは VSMain/PSMain 固定
// (vs_6_0/ps_6_0)。出力はリニア HDR のまま(トーンマップは PostProcess の最終段で行う)。
namespace dx12e
{

inline const char* kNewShaderTemplate = R"HLSL(// カスタムシェーダー: 最小ランバート + アルベドテクスチャ
// エンジンの共有 RootSignature(b0=PerObject, b1=PerFrame の先頭部分, t0+s0=アルベド)に合わせてあります。
// 自由に書き換えてOK。保存すると自動でホットリロードされます。

Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

// PerObject constants (b0) - MVP + Model
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
    // ★エンジンは 40 DWORD 送っている。ここまで宣言すると
    //   MeshRenderer の「効果値 / シェーダーパラメータ」が使える
    //   （Inspector で編集でき、Lua からも書ける唯一の per-object の口）。
    //   使わないなら消してよいが、オフセットがずれるので順番は変えないこと。
    //   （カスタムシェーダーを割り当てた時点で自動インスタンシングの対象外になるので、
    //     b0 は常に 40 DWORD ぶん書かれる。ここを読んで安全）
    float  effectValue;     // MeshRenderer::effectValue（0..1 の汎用進捗値）
    float3 _reserved;       // 予約（エンジンが一律色ティントに使う）
    float4 shaderParams;    // MeshRenderer::shaderParams（汎用 float4）
};

// PerFrame constants (b1) - 先頭部分だけ宣言(shaders/forward/Lighting.hlsli と同一オフセット厳守)
cbuffer PerFrameConstants : register(b1)
{
    float4x4 view;
    float4x4 proj;
    float3   lightDir;   float time;
    float3   lightColor; float ambientStrength;
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
    float4 positionSV : SV_POSITION;
    float3 worldNormal : NORMAL;
    float4 color        : COLOR;
    float2 texCoord     : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV  = mul(float4(input.position, 1.0f), mvp);
    output.worldNormal = normalize(mul(input.normal, (float3x3)model));
    output.color        = input.color;
    output.texCoord      = input.texCoord;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 albedo = g_albedo.Sample(g_sampler, input.texCoord) * input.color;

    float3 N = normalize(input.worldNormal);
    float3 L = normalize(-lightDir);
    float  ndotl = max(dot(N, L), 0.0f);

    float3 color = albedo.rgb * (lightColor * ndotl + ambientStrength);
    return float4(color, albedo.a);
}
)HLSL";

} // namespace dx12e
