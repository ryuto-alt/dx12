// 表示倍率（DPI）の単体テスト。
//   ・core/DpiScale.h（純ロジック: 引数の解釈 / 範囲 / DPI↔倍率 / 論理→物理）
//   ・editor/EditorTheme.h（theme::Px / ScaleStyleFrom: 基準スタイルからの再計算が累積せず、往復で元に戻る）
// ImGui のコンテキストは要らない（ImGuiStyle は単なる構造体）。EditorTheme.h が imgui.h を include するので
// 統合ビルド（Gui ターゲットがある時）でだけ有効。
#include "core/DpiScale.h"
#include "editor/EditorTheme.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double aa_ = (a), bb_ = (b); if (!(std::fabs(aa_ - bb_) <= (eps))) { \
    std::printf("FAIL %s:%d  %s=%.4f vs %s=%.4f\n", __FILE__, __LINE__, #a, aa_, #b, bb_); ++g_fail; } } while (0)

namespace dpi = dx12e::dpi;
namespace th  = dx12e::theme;

ImGuiStyle MakeBase()
{
    ImGuiStyle s;
    th::ApplyStyle(s);
    s.FontSizeBase = th::size::kFontBase;
    return s;
}

bool SameSizes(const ImGuiStyle& a, const ImGuiStyle& b)
{
    return a.WindowPadding.x == b.WindowPadding.x && a.WindowPadding.y == b.WindowPadding.y
        && a.FramePadding.x == b.FramePadding.x && a.FramePadding.y == b.FramePadding.y
        && a.ItemSpacing.x == b.ItemSpacing.x && a.ItemSpacing.y == b.ItemSpacing.y
        && a.CellPadding.x == b.CellPadding.x && a.CellPadding.y == b.CellPadding.y
        && a.IndentSpacing == b.IndentSpacing && a.ScrollbarSize == b.ScrollbarSize
        && a.GrabMinSize == b.GrabMinSize && a.WindowRounding == b.WindowRounding
        && a.FrameRounding == b.FrameRounding && a.PopupRounding == b.PopupRounding
        && a.TabRounding == b.TabRounding && a.WindowBorderSize == b.WindowBorderSize
        && a.FrameBorderSize == b.FrameBorderSize && a.DockingSeparatorSize == b.DockingSeparatorSize
        && a.TabBarOverlineSize == b.TabBarOverlineSize && a.FontScaleDpi == b.FontScaleDpi
        && a.SeparatorTextPadding.x == b.SeparatorTextPadding.x && a.SeparatorTextPadding.y == b.SeparatorTextPadding.y;
}

void TestParse()
{
    float v = 0.0f;
    CHECK(dpi::ParseScale("1.5", v) && v == 1.5f);
    CHECK(dpi::ParseScale("1.25", v) && v == 1.25f);
    CHECK(dpi::ParseScale("150%", v) && v == 1.5f);
    CHECK(dpi::ParseScale("175", v) && std::fabs(v - 1.75f) < 1e-6f);   // % なしの 50 以上はパーセント指定
    CHECK(dpi::ParseScale(" 2.0 ", v) && v == 2.0f);
    CHECK(dpi::ParseScale("0.75", v) && v == 0.75f);
    CHECK(dpi::ParseScale("3.0", v) && v == 3.0f);
    CHECK(!dpi::ParseScale("", v));
    CHECK(!dpi::ParseScale("abc", v));
    CHECK(!dpi::ParseScale("1.5x", v));
    CHECK(!dpi::ParseScale("0.5", v));    // 範囲外
    CHECK(!dpi::ParseScale("3.5", v));
    CHECK(!dpi::ParseScale("-1", v));
    CHECK(!dpi::ParseScale("500%", v));
}

void TestScaleMath()
{
    CHECK(dpi::ClampScale(0.0f) == 1.0f);
    CHECK(dpi::ClampScale(-2.0f) == 1.0f);
    CHECK(dpi::ClampScale(0.1f) == dpi::kMinScale);
    CHECK(dpi::ClampScale(9.0f) == dpi::kMaxScale);
    CHECK(dpi::ClampScale(1.25f) == 1.25f);
    CHECK(dpi::DpiToScale(96) == 1.0f);
    CHECK(dpi::DpiToScale(120) == 1.25f);
    CHECK(dpi::DpiToScale(144) == 1.5f);
    CHECK(dpi::DpiToScale(168) == 1.75f);
    CHECK(dpi::DpiToScale(192) == 2.0f);
    CHECK(dpi::DpiToScale(0) == 1.0f);
    CHECK(dpi::ScaleToDpi(1.5f) == 144u);
    CHECK(dpi::LogicalToPhysical(1920, 1.0f) == 1920);
    CHECK(dpi::LogicalToPhysical(1920, 1.5f) == 2880);
    CHECK(dpi::LogicalToPhysical(1080, 1.25f) == 1350);
    CHECK(dpi::LogicalToPhysical(1080, 1.75f) == 1890);
    // オーバーライド
    dpi::SetOverride(0.0f);
    CHECK(!dpi::HasOverride());
    CHECK(dpi::EffectiveScale(1.5f) == 1.5f);
    dpi::SetOverride(2.0f);
    CHECK(dpi::HasOverride() && dpi::EffectiveScale(1.25f) == 2.0f);
    dpi::SetOverride(9.0f);   // 範囲外は丸める
    CHECK(dpi::Override() == dpi::kMaxScale);
    dpi::SetOverride(0.0f);
    CHECK(!dpi::HasOverride());
}

void TestPx()
{
    th::SetScale(1.0f);
    CHECK(th::Px(28.0f) == 28.0f);
    CHECK(th::Px(3.5f) == 3.5f);           // 100% は小数もそのまま（見た目を 1px も変えない）
    CHECK(th::Px(1.5f) == 1.5f);
    th::SetScale(1.25f);
    CHECK(th::Px(28.0f) == 35.0f);
    CHECK(th::Px(23.0f) == 29.0f);         // 28.75 → 29
    CHECK(th::Px(1.0f) == 1.0f);           // ヘアラインは 1px のまま
    CHECK_NEAR(th::PxF(1.5f), 1.875, 1e-5);
    CHECK_NEAR(th::ToLogical(th::Px(28.0f)), 28.0, 1e-4);
    th::SetScale(1.5f);
    CHECK(th::Px(28.0f) == 42.0f);
    CHECK(th::Px(1.0f) == 2.0f);           // 1.5 → 2（round half up）
    CHECK(th::Px(0.0f) == 0.0f);
    CHECK(th::Px(-60.0f) == -90.0f);       // 「右端から 60px」の負値
    const ImVec2 v = th::Px(10.0f, 4.0f);
    CHECK(v.x == 15.0f && v.y == 6.0f);
    th::SetScale(2.0f);
    CHECK(th::Px(28.0f) == 56.0f);
    th::SetScale(0.0f);                    // 不正値は 1.0 へ
    CHECK(th::Scale() == 1.0f);
    th::SetScale(1.0f);
}

void TestStyleScale()
{
    const ImGuiStyle base = MakeBase();
    const ImGuiStyle baseCopy = base;

    // 100% は基準そのもの（ImTrunc で FramePadding.y 3.5 が 3 になる等の劣化が無い）
    const ImGuiStyle s1 = th::ScaleStyleFrom(base, 1.0f);
    CHECK(SameSizes(s1, base));
    CHECK(s1.FramePadding.y == 3.5f);

    // 125 / 150 / 175 / 200 / 250 / 300%
    static const float kScales[] = { 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };
    for (float sc : kScales)
    {
        const ImGuiStyle s = th::ScaleStyleFrom(base, sc);
        CHECK_NEAR(s.FontScaleDpi, sc, 1e-6);
        CHECK(s.ItemSpacing.x == std::floor(base.ItemSpacing.x * sc + 0.5f));
        CHECK(s.WindowPadding.y == std::floor(base.WindowPadding.y * sc + 0.5f));
        CHECK(s.ScrollbarSize == std::floor(base.ScrollbarSize * sc + 0.5f));
        CHECK(s.DockingSeparatorSize == std::floor(base.DockingSeparatorSize * sc + 0.5f));
        CHECK(s.FrameBorderSize >= 1.0f);                 // 枠線は消えない
        // 行高: フォント高（16×倍率を整数へ）+ FramePadding.y×2 が Px(23) に一致する（分数 px の行境界を作らない）
        th::SetScale(sc);
        const float font = std::floor(th::size::kFontBase * sc + 0.5f);
        CHECK_NEAR(font + s.FramePadding.y * 2.0f, th::Px(th::size::kRowH), 1e-4);
        th::SetScale(1.0f);
        // 100% 比の論理サイズ: ±1px（丸めの範囲）
        CHECK(std::fabs(s.ItemSpacing.y / sc - base.ItemSpacing.y) <= 0.5f / sc + 1e-4f);
        CHECK(std::fabs(s.IndentSpacing / sc - base.IndentSpacing) <= 0.5f / sc + 1e-4f);
    }

    // 累積しない: 同じ倍率を何度計算しても同じ / 基準は書き換わらない
    const ImGuiStyle a = th::ScaleStyleFrom(base, 1.5f);
    const ImGuiStyle b = th::ScaleStyleFrom(base, 1.5f);
    CHECK(SameSizes(a, b));
    CHECK(SameSizes(base, baseCopy));

    // 往復（1.0 → 1.5 → 2.0 → 1.0）で元に戻る。基準から毎回作るので順序に依存しない
    const ImGuiStyle r15 = th::ScaleStyleFrom(base, 1.5f);
    const ImGuiStyle r20 = th::ScaleStyleFrom(base, 2.0f);
    const ImGuiStyle r10 = th::ScaleStyleFrom(base, 1.0f);
    CHECK(SameSizes(r10, base));
    CHECK(!SameSizes(r15, r20));
    CHECK(r20.ItemSpacing.x > r15.ItemSpacing.x);

    // 色は倍率で変わらない
    CHECK(std::memcmp(&a.Colors[ImGuiCol_Text], &base.Colors[ImGuiCol_Text], sizeof(ImVec4)) == 0);
}
} // namespace

int main()
{
    TestParse();
    TestScaleMath();
    TestPx();
    TestStyleScale();
    if (g_fail == 0) std::printf("dpi_scale_test: all passed\n");
    return g_fail == 0 ? 0 : 1;
}
