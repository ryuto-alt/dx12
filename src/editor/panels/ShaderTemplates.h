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
    // ★ここから下の float 8 個が「名前付きパラメーター」の枠です。
    //   好きな名前・型で宣言すると、その名前のまま Inspector の Shader 欄に
    //   ウィジェットが生えます（保存 → ホットリロード → 項目が増える）。
    //   行末の注釈で見た目を指定できます:
    //       // @range(min,max) … スライダーになる
    //       // @color          … カラーピッカーになる（名前に Color/Tint を含めば自動）
    //   使えるのは float / float2 / float3 / float4 の計 8 個ぶんまで。
    //   （float3・float4 は HLSL の規則で 16 バイト境界から始まる点に注意）
    float  effectValue;     // @range(0,1)  汎用進捗値。Lua: scene:setMeshEffect(e, v)
    float  _glow;           // @range(0,4)  ★この行を真似すれば項目が増えます（下の PSMain で使用）
    float2 _reserved;       // 未使用（名前が pad/reserved 等の変数は Inspector に出しません）
    float4 shaderParams;    // 汎用 float4。Lua: scene:setMeshParams(e, x,y,z,w)
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
    color += albedo.rgb * _glow;   // ← Inspector の「_glow」スライダーがそのままここに来る
    return float4(color, albedo.a);
}
)HLSL";

// ===== スクリーンシェーダー（CameraComponent::screenShaderPath）の雛形 =====
//
// ポストプロセスが終わった【完成した画面】をテクスチャとして受け取り、好きに書き換える 1 パス。
// ルートシグネチャは src/renderer/ScreenShaderPass.cpp が固定で持っている:
//   t0 = 画面カラー(LDR/ガンマ空間) / t1 = 深度 / b0 = ScreenShaderCB(20 DWORD)
//   s0 = linear clamp / s1 = point clamp
// ★これ以外のレジスタ（b1, t2, ...）を宣言すると PSO 生成に失敗して素通しになる。
//   その場合は dx12_engine.log に理由が出る。
//
// 画面のサンプルは必ず SampleScreen(uv) を通すこと。中間 RT はウィンドウ全面で、絵は
// その中の「シーンビューの矩形」にしか入っていない。写像は uvOffsetScale で渡してある。
inline const char* kNewScreenShaderTemplate = R"HLSL(// スクリーンシェーダー: 画面全体に掛かる 1 パス
// カメラ（CameraComponent）の「画面シェーダー」に割り当てて使います。
// assets/shaders/ に置いて保存すると自動でホットリロードされます。
//
// 使えるもの（この 4 つ以外のレジスタは宣言しないこと）:
//   t0 = 画面カラー / t1 = 深度 / b0 = 下の ScreenShaderCB / s0 linear, s1 point

Texture2D    gScreen : register(t0);
Texture2D    gDepth  : register(t1);
SamplerState gLinear : register(s0);
SamplerState gPoint  : register(s1);

cbuffer ScreenShaderCB : register(b0)
{
    float4 resolution;    // xy = 画面の px, zw = 1/px
    float4 timeParams;    // x = 経過秒, y = デルタ秒, z = アスペクト(W/H), w = フレーム番号
    float4 params;        // Inspector の「パラメーター」float4（好きに使ってよい）
    float4 cameraParams;  // x = near, y = far, z = 垂直FOV(度), w = 正射なら 1
    float4 uvOffsetScale; // ★内部用。SampleScreen / SampleDepth が使う
};

// uv は 0..1 で「画面の左上→右下」。必ずこれ経由で読むこと。
float3 SampleScreen(float2 uv)
{
    return gScreen.Sample(gLinear, uv * uvOffsetScale.zw + uvOffsetScale.xy).rgb;
}
float SampleDepth(float2 uv)
{
    return gDepth.Sample(gPoint, uv * uvOffsetScale.zw + uvOffsetScale.xy).r;
}
// 非線形の深度をカメラからの距離(m)へ直す（透視カメラのみ）。
float LinearDepth(float d)
{
    float n = cameraParams.x, f = cameraParams.y;
    return (n * f) / max(f - d * (f - n), 1e-6);
}

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

// 頂点バッファ無しのフルスクリーン三角形（3 頂点で画面を覆う定石）。
VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 col = SampleScreen(i.uv);

    // ── ここから下を書き換える ──
    // 例: 走る走査線 + 周辺を少し暗く（params.x で強さを調整できる）
    float strength = saturate(params.x > 0.0 ? params.x : 0.35);
    float band = sin((i.uv.y + timeParams.x * 0.15) * resolution.y * 0.5) * 0.5 + 0.5;
    col *= lerp(1.0, band, strength * 0.35);

    float2 d = i.uv - 0.5;
    col *= 1.0 - saturate(dot(d, d) * 1.2) * strength;

    return float4(col, 1.0);
}
)HLSL";

} // namespace dx12e
