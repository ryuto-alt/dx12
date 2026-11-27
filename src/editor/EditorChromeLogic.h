#pragma once

// 計器のクロム（W1-A7 / GG-4）の純ロジック。ImGui にも DX12 にも依存しない（tests/editor_chrome_test.cpp が単体で検査する）。
//   ・主尺（タイトルバー下端の細 8px / 主 40px の目盛り）の位置
//   ・フォーカス・キャリパー（顎 2x8 + 2px の線）の矩形
//   ・ステータスバーの状態の和文 / 図番 UN-x.y.z / ミニゲージの刻み / GPU 名の短縮
// 寸法は呼び出し側が ThemeGraphite.gen.h（tokens.deco.*）から読んで引数で渡す。ここに数値を書かない。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::chrome
{

// ---- 主尺 ----
struct RulerTick
{
    float x;       // 物理 px（整数）
    bool  major;
};

// タイトルバーの左端 x0 から右端 x1（物理 px）までの目盛り。minorStep / majorStep は論理 px、scale は UI の倍率。
//   目盛りは「論理 px の整数倍の位置」を物理 px へ丸めるので、倍率 1.5 でも間隔が揃い、主目盛りは細目盛りの上に乗る。
//   x0 の位置（k = 0）は主目盛り。
inline void RulerTicks(float x0, float x1, float scale, int minorStep, int majorStep, std::vector<RulerTick>& out)
{
    out.clear();
    if (!(x1 > x0) || minorStep <= 0 || majorStep <= 0 || !(scale > 0.0f)) return;
    const float base = std::floor(x0 + 0.5f);
    for (int k = 0;; ++k)
    {
        const int   lx = k * minorStep;
        const float px = base + std::floor(static_cast<float>(lx) * scale + 0.5f);
        if (px >= x1) break;
        out.push_back({ px, (lx % majorStep) == 0 });
    }
}

// ---- キャリパー ----
struct PxRect
{
    float x0, y0, x1, y1;
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
};
struct CaliperGeom
{
    PxRect line;        // 2px の線（パネルの全幅）
    PxRect jawL, jawR;  // 顎（幅 jawW、高さ jawH。線の上端から下へ）
    bool   valid = false;
};

// 範囲 [x0, x1]（物理 px）と、タイトルバーの下端 yBottom から、キャリパーの矩形を整数 px で返す。
//   線は主尺の主目盛りの上（下端から majorH 上）に置く。顎は線の上端から jawH 下へ（= 主目盛りの高さの分だけ主尺に食い込む）。
//   幅が顎 2 本に満たないときは valid = false。
inline CaliperGeom CaliperGeometry(float x0, float x1, float yBottom, float majorH, float accentPx, float jawW, float jawH)
{
    CaliperGeom g;
    auto snap = [](float v) { return std::floor(v + 0.5f); };
    const float a = snap(x0), b = snap(x1);
    const float lineBottom = snap(yBottom - majorH);
    const float lineTop    = lineBottom - std::max(1.0f, snap(accentPx));
    const float jw = std::max(1.0f, snap(jawW));
    const float jh = std::max(1.0f, snap(jawH));
    if (!(b - a >= jw * 2.0f)) return g;
    g.line = { a, lineTop, b, lineBottom };
    g.jawL = { a, lineTop, a + jw, lineTop + jh };
    g.jawR = { b - jw, lineTop, b, lineTop + jh };
    g.valid = true;
    return g;
}

// ---- ステータスバー ----
enum class RunState : uint8_t { Edit, Play, Paused };

inline const char* StateLabel(RunState s)
{
    switch (s)
    {
    case RunState::Play:   return "再生中";
    case RunState::Paused: return "一時停止";
    default:               return "編集";
    }
}

// 図番。エンジンの版 "2.6.1" / "v2.6.1" → "UN-2.6.1"（決定 4。起動画面の UN-x.y.z と同じ表記）。
inline std::string DrawingNumber(std::string_view version)
{
    while (!version.empty() && (version.front() == 'v' || version.front() == 'V' || version.front() == ' ')) version.remove_prefix(1);
    std::string s = "UN-";
    s.append(version);
    return s;
}

// ミニゲージ（cells 刻み）の塗り数。値が NaN / 負 / 最大が 0 以下なら 0。1 以上の値は最低 1 刻み。
inline int GaugeCells(float value, float vmax, int cells)
{
    if (cells <= 0 || !std::isfinite(value) || !std::isfinite(vmax) || !(vmax > 0.0f) || !(value > 0.0f)) return 0;
    const float t = std::min(1.0f, value / vmax);
    return std::clamp(static_cast<int>(std::ceil(t * static_cast<float>(cells) - 1e-4f)), 1, cells);
}

// アダプタ名の短縮: "NVIDIA GeForce RTX 5060" → "RTX 5060"。ベンダー名が付いていなければそのまま。
inline std::string ShortGpuName(std::string_view name)
{
    static const char* const kPrefix[] = { "NVIDIA GeForce ", "NVIDIA ", "AMD Radeon(TM) ", "AMD Radeon ", "Intel(R) " };
    for (const char* p : kPrefix)
    {
        const std::string_view pv(p);
        if (name.size() > pv.size() && name.substr(0, pv.size()) == pv)
        {
            name.remove_prefix(pv.size());
            break;
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) name.remove_suffix(1);
    return std::string(name);
}

// VRAM の使用率（0..1）。取得できなければ負。
inline float VramRatio(double usedMB, double budgetMB)
{
    if (!(budgetMB > 0.0) || !(usedMB >= 0.0)) return -1.0f;
    return static_cast<float>(std::min(1.0, usedMB / budgetMB));
}

// 使用率の段階（0 = 通常 / 1 = 警告 / 2 = 危険）。
inline int VramLevel(float ratio)
{
    if (ratio >= 0.95f) return 2;
    if (ratio >= 0.85f) return 1;
    return 0;
}

} // namespace dx12e::chrome
