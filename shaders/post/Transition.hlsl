// シーントランジションのフルスクリーンオーバーレイ（実機の遷移）。
//
// 形の実装は shaders/post/TransitionCurtain.hlsli ただ 1 箇所。ここはそれを
// 画面へ被せるだけの薄い皮。エディタのプレビュー窓/サムネイル（TransitionPreview.hlsl）も
// 同じ関数を呼ぶので、「エディタで見た絵」と「実機で出る絵」は原理的にズレない。

#include "FullscreenTri.hlsli"
#include "TransitionCurtain.hlsli"

cbuffer TransCB : register(b0)
{
    float progress;  // 0..1 の被覆率（閉じ 0→1 / 開き 1→0）
    int   type;      // TransitionType。対応表は TransitionCurtain.hlsli の先頭
    float aspect;    // 画面アスペクト w/h
    float total;     // 遷移全体の進行 0..1（閉じ→開きを跨ぐ）
};

float4 TransPS(FSQuadVSOut i) : SV_TARGET
{
    return TransitionCurtain(i.uv, progress, total, type, aspect);
}
