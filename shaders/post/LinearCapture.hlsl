// LinearCapture.hlsl — 線形 HDR スクリーンショット（screenshot_final {format:"pfm"|"exr"}）用の読み出し。
// ポスト(uber)へ入る直前のシーン色（TAA / DoF / モーションブラーまで済み・露出もトーンマップも掛かる前）を
// RGBA32F のバッファへそのまま写す。CPU 側が PFM / EXR にして書く。既定では走らない（要求のあるフレームだけ）。
// 出力は上の行から順（row 0 = 画面の上端）。

cbuffer LcCB : register(b0)
{
    uint2 size;    // 出力の幅・高さ [px]
    uint2 _pad;
};

Texture2D<float4>          gScene : register(t0);
RWStructuredBuffer<float4> gOut   : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    float4 c = gScene.Load(int3(id.xy, 0));
    // NaN / Inf は 0 に（比較ツールの平均や FLIP を汚さない）。個数は数えない（パストレーサーの nanSamples と違い 1 サンプルではないので）。
    if (isnan(c.r) || isinf(c.r)) c.r = 0.0;
    if (isnan(c.g) || isinf(c.g)) c.g = 0.0;
    if (isnan(c.b) || isinf(c.b)) c.b = 0.0;
    gOut[id.y * size.x + id.x] = float4(c.rgb, 1.0);
}
