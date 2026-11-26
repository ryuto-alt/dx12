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

// 登録の仕組み（ThemeVariants.h の申し送り「静的ライブラリの自己登録で書かない。起動時に明示呼び出しで登録する」との差に注意）:
//   ・main.cpp の --theme-variant の解釈（SetVariant）は ImGuiManager::Initialize より前なので、そこでは明示呼び出しが間に合わない。
//     そのため、下の静的初期化（main の前）で登録する。
//   ・この TU は ImGuiManager.cpp が theme::RegisterGraphite() を参照するのでリンカに落とされない（Gui は静的ライブラリ）。
//     ImGuiManager.cpp の参照を外すと --theme-variant graphite が無効になる（main.cpp の先頭で theme::RegisterGraphite() を明示的に呼べば
//     この前提が要らなくなる。main.cpp は A2 の持ち物ではないので、メインの判断に任せる）。
//   ・g_graphiteSpecFn は定数初期化の nullptr なので、静的初期化の順序の問題は無い。
const bool g_autoRegistered = (RegisterGraphite(), true);
} // namespace

const Spec& GraphiteSpec() { return GraphiteSpecImpl(); }

void RegisterGraphite() { g_graphiteSpecFn = &GraphiteSpecImpl; }

} // namespace dx12e::theme
