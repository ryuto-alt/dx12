#pragma once
// UI の時計（ui::clock）。エディタ UI のアニメ・点滅・トーストなど「時間で見た目が変わるもの」は
// ImGui::GetTime() を直接読まず、ここの Now() を使う（W1-A2 の ui::anim もこの時計を使う）。
//
//   通常:               Now() = ImGui::GetTime()（実時間）
//   --ui-deterministic: Now() = 「フレーム番号 × 1/60」秒。実時間に依存しないので、UI ゴールデン
//                       （tests/golden/ui/）の撮影が何度撮っても同じ絵になる。
//
// ★ImGuiManager::BeginFrame が毎フレーム Advance() を呼ぶ（決定論モードのときだけ io.DeltaTime も 1/60 に固定）。
#include <cstdint>
#include <imgui.h>

namespace dx12e::ui::clock
{
inline constexpr double kFixedDt = 1.0 / 60.0;

namespace detail
{
inline bool&     Det()   { static bool v = false;      return v; }
inline uint64_t& Frame() { static uint64_t v = 0;      return v; }
} // namespace detail

/// 決定論モードの ON/OFF（--ui-deterministic）。
inline void SetDeterministic(bool on) { detail::Det() = on; }
inline bool Deterministic()           { return detail::Det(); }

/// 1 フレーム進める（ImGuiManager::BeginFrame から。決定論モード以外では数えるだけ）。
inline void Advance() { ++detail::Frame(); }
inline uint64_t FrameIndex() { return detail::Frame(); }

/// UI の現在時刻（秒）。
inline double Now()
{
    return detail::Det() ? static_cast<double>(detail::Frame()) * kFixedDt : ImGui::GetTime();
}
} // namespace dx12e::ui::clock
