// Skybox.hlsl - 全画面三角形で環境キューブを view ray でサンプルして背景を塗る（graphics）
// 深度テスト/書き込み OFF。メインパスの不透明描画前に描き、後続の不透明物が上書きする。
// forward と輝度を揃えるため skybox 側でも ACES + ガンマを適用して LDR で scene RT へ書く。

cbuffer SkyboxConstants : register(b0)
{
    float4x4 invViewProj;  // HLSL 行ベクトル mul 用に転置済み
    float    intensity;
    float3   _pad;
};

TextureCube  g_env  : register(t0);
SamplerState g_samp : register(s0);  // LINEAR CLAMP（mip有）

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;  // view ray（ワールド方向）
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    // (0,0) (2,0) (0,2) を覆う全画面三角形
    float2 uv = float2((id << 1) & 2, id & 2);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(ndc, 1.0, 1.0);  // far 平面(z=1)

    // far 平面の clip 点をワールドへ逆変換し、カメラ→far のワールド方向を得る
    float4 farW  = mul(float4(ndc, 1.0, 1.0), invViewProj);
    float4 nearW = mul(float4(ndc, 0.0, 1.0), invViewProj);
    farW  /= farW.w;
    nearW /= nearW.w;
    o.dir = farW.xyz - nearW.xyz;
    return o;
}

float3 ACESFilmSky(float3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 dir = normalize(i.dir);
    float3 col = g_env.SampleLevel(g_samp, dir, 0).rgb * intensity;

    // forward と統一: ACES + ガンマで LDR 化
    col = ACESFilmSky(col);
    col = pow(col, 1.0 / 2.2);
    return float4(col, 1.0);
}
