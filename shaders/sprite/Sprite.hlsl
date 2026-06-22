// 2D スプライト。b0 の変換行列で頂点をクリップ空間へ。
//  - HUD(スクリーン空間): gTransform = 正射影(ピクセル→NDC)、頂点 z=0。
//  - ワールド空間: gTransform = カメラ ViewProj、頂点は CPU でワールド変換済みの 3D 座標。
// RootConstants b0: float4x4（転置済み）

cbuffer SpriteCB : register(b0)
{
    float4x4 gTransform;
};

struct VSIn
{
    float3 pos : POSITION;   // HUD は z=0、ワールドは 3D ワールド座標
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);

PSIn VSMain(VSIn v)
{
    PSIn o;
    o.pos = mul(float4(v.pos, 1.0f), gTransform);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

float4 PSMain(PSIn p) : SV_TARGET
{
    return gTex.Sample(gSamp, p.uv) * p.col;
}
