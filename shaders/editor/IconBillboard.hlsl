// エディタアイコンのビルボード描画。
// ワールド中心を投影し、クリップ空間でピクセルオフセットを加えることで
// 「常にカメラを向く・距離によらず一定スクリーンサイズ」の 2D アイコンになる。
// RootConstants b0: float4x4 viewProj(転置済み, 16) + float2 invScreen(2) + float2 pad

cbuffer IconCB : register(b0)
{
    float4x4 gViewProj;
    float2   gInvScreen;  // (1/screenW, 1/screenH)
    float2   gPad;
};

struct VSIn
{
    float3 center : POSITION;
    float2 offset : TEXCOORD0;  // ピクセル単位
    float3 color  : COLOR;
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float3 color : COLOR;
};

PSIn VSMain(VSIn v)
{
    PSIn o;
    float4 clip = mul(float4(v.center, 1.0f), gViewProj);
    // ピクセルオフセット → NDC: pixels * 2/screen、w を掛けて遠近に依らない一定サイズ
    clip.xy += v.offset * gInvScreen * 2.0f * clip.w;
    o.pos   = clip;
    o.color = v.color;
    return o;
}

float4 PSMain(PSIn p) : SV_TARGET
{
    return float4(p.color, 1.0f);
}
