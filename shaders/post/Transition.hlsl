// シーントランジションのフルスクリーンオーバーレイ。
// progress(0→1→0) を coverage として、type 別に黒オーバーレイのアルファを計算する。

#include "FullscreenTri.hlsli"

cbuffer TransCB : register(b0)
{
    float progress;  // 0..1 の被覆率
    int   type;      // 0=フェード, 1=横ワイプ, 2=円(アイリス), 3=縦ワイプ
    float aspect;    // 画面アスペクト w/h
    float pad;
};

float4 TransPS(FSQuadVSOut i) : SV_TARGET
{
    float a = 0.0;
    if (type == 0)
    {
        a = progress;
    }
    else if (type == 1)
    {
        a = (i.uv.x < progress) ? 1.0 : 0.0;
    }
    else if (type == 2)
    {
        float2 d = i.uv - 0.5;
        d.x *= aspect;
        float maxd = length(float2(0.5 * aspect, 0.5));
        float dist = length(d) / maxd;
        a = (dist > (1.0 - progress)) ? 1.0 : 0.0;
    }
    else
    {
        a = (i.uv.y < progress) ? 1.0 : 0.0;
    }
    return float4(0.0, 0.0, 0.0, a);
}
