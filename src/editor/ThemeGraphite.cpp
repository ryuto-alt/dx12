// Graphite Gauge の Spec（W1 A2）。生成ヘッダ ThemeGraphite.gen.h を include する唯一の TU。
// 仕様（tokens.json）を再生成しても、再コンパイルされるのはこの TU（と値を直接読む .cpp）だけ。
#include "editor/ThemeGraphite.h"

#include "editor/ThemeGraphite.gen.h"

namespace dx12e::theme
{

namespace
{
const Spec& GraphiteSpecImpl()
{
    static const Spec s = []() {
        Spec sp{};
        sp.pal  = gg::kGraphitePalette;
        sp.m    = gg::kMetrics;
        sp.deco = gg::kDeco;
        sp.ext  = gg::kExtra;
        sp.gg   = &gg::kGgSpec;
        return sp;
    }();
    return s;
}

// 登録の仕組み:
//   ・main.cpp が WinMain の先頭（--theme-variant の SetVariant より前）で theme::RegisterGraphite() を明示的に呼ぶ。
//   ・下の静的初期化（main の前）の登録は保険として残す（RegisterGraphite は冪等。ImGuiManager.cpp の参照でもリンクされる）。
//   ・g_graphiteSpecFn は定数初期化の nullptr なので、静的初期化の順序の問題は無い。
const bool g_autoRegistered = (RegisterGraphite(), true);
} // namespace

const Spec& GraphiteSpec() { return GraphiteSpecImpl(); }

void RegisterGraphite() { g_graphiteSpecFn = &GraphiteSpecImpl; }

} // namespace dx12e::theme
