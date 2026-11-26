#pragma once

// Graphite Gauge（theme::Variant::Graphite）の Spec の提供口（W1 A2）。
// 値は生成ヘッダ ThemeGraphite.gen.h（tools/theme/gen_gg_tokens.py）にあり、それを include するのは ThemeGraphite.cpp と、
// 値を直接読むテスト・描画の .cpp だけ。ここは宣言だけ（重ヘッダから include しても生成物の再コンパイルを招かない）。

#include "editor/ThemeVariants.h"

namespace dx12e::theme
{

// Graphite の Spec（pal / m / deco / ext と、gg = 生成仕様）。gg は常に非 null。
const Spec& GraphiteSpec();

// ThemeVariants.h の g_graphiteSpecFn へ GraphiteSpec を登録する。何度呼んでも同じ（冪等）。
// ThemeGraphite.cpp は静的初期化でも自分を登録するが、リンカが TU を落とさないよう ImGuiManager からも明示的に呼ぶ。
void RegisterGraphite();

} // namespace dx12e::theme
