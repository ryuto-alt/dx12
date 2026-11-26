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

// 静的初期化でも登録する（main.cpp の --theme-variant の解釈より前＝main の前に済む。g_graphiteSpecFn は定数初期化の nullptr なので順序の問題は無い）。
const bool g_autoRegistered = (RegisterGraphite(), true);
} // namespace

const Spec& GraphiteSpec() { return GraphiteSpecImpl(); }

void RegisterGraphite() { g_graphiteSpecFn = &GraphiteSpecImpl; }

} // namespace dx12e::theme
