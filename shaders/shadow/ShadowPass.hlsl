// ShadowPass.hlsl - Depth-only pass for shadow map generation (static meshes)

cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;    // lightViewProj * model
    float4x4 model;  // unused in shadow pass
};

// ★position だけ宣言する。深度しか書かないので他は要らない。
//   宣言すると DXC は使っていなくても入力署名へ残し（dxc -dumpbin の Used 列が空になる）、
//   IA は署名に従ってフェッチする＝1 頂点 12 バイトで済むところを 96 バイト読むことになる。
//   入力レイアウト（Mesh::GetInputLayout）は 7 要素のままでよい。D3D12 は
//   「入力レイアウト ⊇ シェーダの入力署名」を許すので、超過分は無視される。
struct VSInput
{
    float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0f), mvp);
}
