// ノードグラフの文字サイズの量子化（MOD-ED-14）。純関数（ImGui 非依存）。
// ズームのたびに別サイズのグリフを焼かないよう、フォントのサイズを段に量子化する。
// 段は zoom 以上の最小のものを選ぶ＝焼くサイズは常に目標以上で、描画は縮小のみ（r <= 1）。
#pragma once

#include <algorithm>
#include <cmath>

namespace dx12e::ng
{

struct QuantText
{
    float qpx; // 実際にフォントを焼くサイズ
    float r;   // px / qpx（頂点へ掛ける比。段ちょうどなら 1）
};

inline constexpr float kTextZoomSteps[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
inline constexpr int kTextZoomStepCount = static_cast<int>(sizeof(kTextZoomSteps) / sizeof(kTextZoomSteps[0]));

inline QuantText QuantizeText(float px, float zoom)
{
    const float z = std::max(0.01f, zoom);
    float step = kTextZoomSteps[kTextZoomStepCount - 1];
    for (float s : kTextZoomSteps)
        if (s >= z * 0.9995f) { step = s; break; }
    const float qpx = std::max(1.0f, std::floor(px * step / z + 0.5f));
    return {qpx, px / qpx};
}

} // namespace dx12e::ng
