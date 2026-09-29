// エディタのテーマ・バリアント（アイデンティティ候補 A/B/C。editor/ThemeVariants.h + EditorTheme.h）の単体テスト。
//   ・Default が第 1 波の値（＝現行）のまま動かないこと（既定の見た目が 1px も変わらない根拠の 1 つ）
//   ・--theme-variant の値の解釈
//   ・全案で文字コントラストが AA(4.5:1)、選択面の上でも読めること / アクセント塗りの上の文字も読めること
//   ・全案でレイアウトに効く値（余白・行高）が同じ＝案でレイアウトが動かない
//   ・SetVariant の往復でトークンが元に戻ること / ScaleStyleFrom が全案で行高を整数 px にすること
// ImGui のコンテキストは要らない（ImGuiStyle は単なる構造体）。EditorTheme.h が imgui.h を include するので Gui ターゲットで有効。
#include "editor/EditorTheme.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

namespace th = dx12e::theme;

bool SameVec4(const ImVec4& a, const ImVec4& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// c を bg の上に alpha 合成（不透明な背景前提）
ImVec4 Over(const ImVec4& c, const ImVec4& bg)
{
    return ImVec4(bg.x + (c.x - bg.x) * c.w, bg.y + (c.y - bg.y) * c.w, bg.z + (c.z - bg.z) * c.w, 1.0f);
}

float Saturation(const ImVec4& c)
{
    const float mx = std::fmax(c.x, std::fmax(c.y, c.z)), mn = std::fmin(c.x, std::fmin(c.y, c.z));
    return mx <= 0.0f ? 0.0f : (mx - mn) / mx;
}

const th::Variant kAll[] = { th::Variant::Default, th::Variant::A, th::Variant::B, th::Variant::C };
const th::Variant kNew[] = { th::Variant::A, th::Variant::B, th::Variant::C };

void TestDefaultIsLegacy()
{
    // 第 1 波の EditorTheme.h の値そのもの（これが変わると既定の見た目が変わる）
    const th::Palette& p = th::SpecOf(th::Variant::Default).pal;
    CHECK(SameVec4(p.bg0, th::Hex(0x0E0E10)));
    CHECK(SameVec4(p.bg1, th::Hex(0x171719)));
    CHECK(SameVec4(p.bg2, th::Hex(0x1F1F23)));
    CHECK(SameVec4(p.bg3, th::Hex(0x2A2A2F)));
    CHECK(SameVec4(p.bg4, th::Hex(0x34343B)));
    CHECK(SameVec4(p.inputBg, th::Hex(0x0C0C0E)));
    CHECK(SameVec4(p.inputBorder, th::Hex(0x2E2E34)));
    CHECK(SameVec4(p.inputBorderHover, th::Hex(0x46464E)));
    CHECK(SameVec4(p.borderStrong, th::Hex(0x3A3A42)));
    CHECK(SameVec4(p.accent, th::Hex(0x2F8CFF)));
    CHECK(SameVec4(p.accentHover, th::Hex(0x57A3FF)));
    CHECK(SameVec4(p.accentPressed, th::Hex(0x1F6FD6)));
    CHECK(SameVec4(p.onAccent, th::Hex(0xFFFFFF)));
    CHECK(SameVec4(p.selection, th::Hex(0x2F8CFF, 0.30f)));
    CHECK(SameVec4(p.text, th::Hex(0xD6D6DB)));
    CHECK(SameVec4(p.textDim, th::Hex(0x9C9CA6)));
    CHECK(SameVec4(p.textFaint, th::Hex(0x90909A)));
    CHECK(SameVec4(p.good, th::Hex(0x4CBF7A)));
    CHECK(SameVec4(p.warn, th::Hex(0xE5A03C)));
    CHECK(SameVec4(p.bad, th::Hex(0xEE6A62)));
    CHECK(SameVec4(p.windowBg, p.bg1));   // 既定は不透明
    CHECK(SameVec4(p.popupBg, p.bg2));

    const th::Metrics& m = th::SpecOf(th::Variant::Default).m;
    CHECK(m.windowRounding == 0.0f && m.childRounding == 2.0f && m.frameRounding == 2.0f && m.grabRounding == 2.0f);
    CHECK(m.popupRounding == 4.0f && m.tabRounding == 3.0f && m.scrollbarRounding == 6.0f);
    CHECK(m.tabBarBorder == 1.0f && m.tabOverline == 2.0f && m.dockSeparator == 3.0f);

    const th::Deco& d = th::SpecOf(th::Variant::Default).deco;
    CHECK(d.glow == 0.0f && d.easeSec == 0.0f && !d.panelFocusEdge && !d.panelTopLine && !d.panelSheen && !d.layeredShadow);
    CHECK(d.rowStyle == th::RowStyle::Plain && d.headerStyle == th::HeaderStyle::Plain && !d.monoIcons);

    // 起動時のトークン（SetVariant 前）も Default = 現行
    CHECK(th::CurrentVariant() == th::Variant::Default);
    CHECK(SameVec4(th::Bg1, th::Hex(0x171719)));
    CHECK(SameVec4(th::Accent, th::Hex(0x2F8CFF)));
}

void TestParse()
{
    th::Variant v = th::Variant::B;
    CHECK(th::ParseVariant("a", v) && v == th::Variant::A);
    CHECK(th::ParseVariant("A", v) && v == th::Variant::A);
    CHECK(th::ParseVariant("b", v) && v == th::Variant::B);
    CHECK(th::ParseVariant("C", v) && v == th::Variant::C);
    CHECK(th::ParseVariant("default", v) && v == th::Variant::Default);
    CHECK(th::ParseVariant("0", v) && v == th::Variant::Default);
    v = th::Variant::B;
    CHECK(!th::ParseVariant("", v) && v == th::Variant::B);      // 解釈できなければ out を変えない
    CHECK(!th::ParseVariant("x", v) && v == th::Variant::B);
    CHECK(!th::ParseVariant("abc", v) && v == th::Variant::B);
}

void TestContrast()
{
    for (th::Variant var : kAll)
    {
        const th::Palette& p = th::SpecOf(var).pal;
        const ImVec4* surfaces[] = { &p.bg0, &p.bg1, &p.bg2, &p.bg3, &p.inputBg };
        for (const ImVec4* s : surfaces)
        {
            CHECKF(th::ContrastRatio(p.text, *s) >= 4.5f, "variant %d: text on surface = %.2f", static_cast<int>(var), th::ContrastRatio(p.text, *s));
            CHECKF(th::ContrastRatio(p.textMid, *s) >= 4.5f, "variant %d: textMid = %.2f", static_cast<int>(var), th::ContrastRatio(p.textMid, *s));
            CHECKF(th::ContrastRatio(p.textDim, *s) >= 4.5f, "variant %d: textDim = %.2f", static_cast<int>(var), th::ContrastRatio(p.textDim, *s));
            CHECKF(th::ContrastRatio(p.textFaint, *s) >= 4.5f, "variant %d: textFaint = %.2f", static_cast<int>(var), th::ContrastRatio(p.textFaint, *s));
            CHECKF(th::ContrastRatio(p.warn, *s) >= 4.5f, "variant %d: warn = %.2f", static_cast<int>(var), th::ContrastRatio(p.warn, *s));
            CHECKF(th::ContrastRatio(p.bad, *s) >= 4.5f || s == &p.bg3, "variant %d: bad = %.2f", static_cast<int>(var), th::ContrastRatio(p.bad, *s));
            CHECKF(th::ContrastRatio(p.good, *s) >= 4.5f, "variant %d: good = %.2f", static_cast<int>(var), th::ContrastRatio(p.good, *s));
            CHECKF(th::ContrastRatio(p.accentHover, *s) >= 4.5f, "variant %d: accentHover = %.2f", static_cast<int>(var), th::ContrastRatio(p.accentHover, *s));
        }
        // アクセント（UI 部品の 3:1）
        CHECKF(th::ContrastRatio(p.accent, p.bg1) >= 3.0f, "variant %d: accent on bg1 = %.2f", static_cast<int>(var), th::ContrastRatio(p.accent, p.bg1));
        // 選択面（bg1 の上に selection を重ねた色）の上でも本文が読める
        const ImVec4 sel = Over(p.selection, p.bg1);
        const ImVec4 selAct = Over(p.selectionActive, p.bg1);
        CHECKF(th::ContrastRatio(p.text, sel) >= 4.5f, "variant %d: text on selection = %.2f", static_cast<int>(var), th::ContrastRatio(p.text, sel));
        CHECKF(th::ContrastRatio(p.text, selAct) >= 4.5f, "variant %d: text on selectionActive = %.2f", static_cast<int>(var), th::ContrastRatio(p.text, selAct));
        // アクセント塗りの上の文字 / ✓（既定は白 3.4:1 の現行値。新案は 4.5:1 以上）
        const float need = (var == th::Variant::Default) ? 3.0f : 4.5f;
        CHECKF(th::ContrastRatio(p.onAccent, p.accent) >= need, "variant %d: onAccent on accent = %.2f", static_cast<int>(var), th::ContrastRatio(p.onAccent, p.accent));
        if (var != th::Variant::Default)   // 現行の白 on ホバー青(2.6:1)は第 1 波からの値。新案だけ満たす
            CHECKF(th::ContrastRatio(p.onAccent, p.accentHover) >= need, "variant %d: onAccent on accentHover = %.2f", static_cast<int>(var), th::ContrastRatio(p.onAccent, p.accentHover));
    }
}

void TestIdentityDiffers()
{
    // 案の見分け: アクセントの色相が互いに違い、C の種別色は無彩色
    const ImVec4& a = th::SpecOf(th::Variant::A).pal.accent;
    const ImVec4& b = th::SpecOf(th::Variant::B).pal.accent;
    const ImVec4& c = th::SpecOf(th::Variant::C).pal.accent;
    const ImVec4& d = th::SpecOf(th::Variant::Default).pal.accent;
    CHECK(!SameVec4(a, b) && !SameVec4(a, c) && !SameVec4(b, c) && !SameVec4(a, d));
    const th::Palette& pc = th::SpecOf(th::Variant::C).pal;
    const ImVec4* types[] = { &pc.typeMesh, &pc.typeLight, &pc.typeCamera, &pc.typeAudio, &pc.typeScript, &pc.typePhysics,
                              &pc.typeUi, &pc.typePrefab, &pc.typeEmpty, &pc.typeFolder, &pc.typeScene };
    for (const ImVec4* t : types) CHECKF(Saturation(*t) < 0.05f, "C の種別色は無彩色 (s=%.3f)", Saturation(*t));
    CHECK(th::SpecOf(th::Variant::C).deco.monoIcons);
    // B だけが半透明の窓面 / 大きめの角丸 / 太い分割
    const th::Spec& sb = th::SpecOf(th::Variant::B);
    CHECK(sb.pal.windowBg.w < 1.0f && sb.pal.popupBg.w < 1.0f);
    CHECK(sb.m.popupRounding > th::SpecOf(th::Variant::A).m.popupRounding);
    CHECK(sb.m.frameRounding > th::SpecOf(th::Variant::C).m.frameRounding);
    CHECK(th::SpecOf(th::Variant::A).deco.glow > th::SpecOf(th::Variant::B).deco.glow);
    CHECK(th::SpecOf(th::Variant::C).deco.glow == 0.0f);
}

void TestLayoutInvariant()
{
    // 案でレイアウトが動かない: 余白・行高に効く値は全案で同じ
    ImGuiStyle base;
    th::ApplyStyle(base);
    for (th::Variant var : kNew)
    {
        th::SetVariant(var);
        ImGuiStyle s;
        th::ApplyStyle(s);
        CHECK(s.WindowPadding.x == base.WindowPadding.x && s.WindowPadding.y == base.WindowPadding.y);
        CHECK(s.FramePadding.x == base.FramePadding.x && s.FramePadding.y == base.FramePadding.y);
        CHECK(s.ItemSpacing.x == base.ItemSpacing.x && s.ItemSpacing.y == base.ItemSpacing.y);
        CHECK(s.CellPadding.x == base.CellPadding.x && s.CellPadding.y == base.CellPadding.y);
        CHECK(s.IndentSpacing == base.IndentSpacing && s.ScrollbarSize == base.ScrollbarSize && s.GrabMinSize == base.GrabMinSize);
        const th::Metrics& m = th::SpecOf(var).m;
        CHECK(s.FrameRounding == m.frameRounding && s.PopupRounding == m.popupRounding && s.WindowRounding == m.windowRounding);
        CHECK(SameVec4(s.Colors[ImGuiCol_WindowBg], th::SpecOf(var).pal.windowBg));
        CHECK(SameVec4(s.Colors[ImGuiCol_Header], th::SpecOf(var).pal.selection));
        // 行高（FrameHeight = FontSize + FramePadding.y*2）は kRowH
        s.FontSizeBase = th::size::kFontBase;
        CHECK(s.FontSizeBase + s.FramePadding.y * 2.0f == th::size::kRowH);
        // 倍率 1.5 / 2.0 でも行高は整数 px（分数 px の行境界でにじまない）
        for (float scale : { 1.25f, 1.5f, 2.0f })
        {
            const ImGuiStyle sc = th::ScaleStyleFrom(s, scale);
            const float font = std::floor(s.FontSizeBase * scale + 0.5f);
            const float row = font + sc.FramePadding.y * 2.0f;
            CHECKF(std::fabs(row - std::floor(row + 0.5f)) < 1e-4f, "variant %d scale %.2f: 行高 %.3f が整数でない", static_cast<int>(var), scale, row);
        }
    }
    th::SetVariant(th::Variant::Default);
}

void TestSetVariantRoundTrip()
{
    const ImVec4 bg0 = th::Bg0, acc = th::Accent, sel = th::Selection, txt = th::Text, chrome = th::Chrome, dim = th::AccentDim2;
    for (th::Variant var : kNew)
    {
        th::SetVariant(var);
        CHECK(th::CurrentVariant() == var);
        const th::Palette& p = th::SpecOf(var).pal;
        CHECK(SameVec4(th::Bg0, p.bg0) && SameVec4(th::Accent, p.accent) && SameVec4(th::Selection, p.selection));
        CHECK(SameVec4(th::Text, p.text) && SameVec4(th::TextHi, p.text) && SameVec4(th::OnAccent, p.onAccent));
        CHECK(SameVec4(th::Chrome, p.bg1) && SameVec4(th::AccentDim2, p.selection) && SameVec4(th::FrameBg, p.inputBg));
        CHECK(SameVec4(th::TypeLight, p.typeLight) && SameVec4(th::Bad, p.bad));
        CHECK(!th::IsDefaultVariant());
    }
    th::SetVariant(th::Variant::Default);
    CHECK(th::IsDefaultVariant());
    CHECK(SameVec4(th::Bg0, bg0) && SameVec4(th::Accent, acc) && SameVec4(th::Selection, sel));
    CHECK(SameVec4(th::Text, txt) && SameVec4(th::Chrome, chrome) && SameVec4(th::AccentDim2, dim));

    // 切替要求（コマンドパレット等）は保留され、SetVariant を呼ぶまで反映されない
    th::RequestVariant(th::Variant::B);
    CHECK(th::g_pendingVariant == static_cast<int>(th::Variant::B) && th::CurrentVariant() == th::Variant::Default);
    th::g_pendingVariant = -1;
}

void TestNames()
{
    for (th::Variant var : kAll)
        CHECK(std::strlen(th::VariantName(var)) > 0);
    CHECK(std::string(th::VariantName(th::Variant::A)).find("ネオン") != std::string::npos);
    CHECK(std::string(th::VariantName(th::Variant::B)).find("グラス") != std::string::npos);
    CHECK(std::string(th::VariantName(th::Variant::C)).find("シグナル") != std::string::npos);
}
} // namespace

int main()
{
    TestDefaultIsLegacy();
    TestParse();
    TestContrast();
    TestIdentityDiffers();
    TestLayoutInvariant();
    TestSetVariantRoundTrip();
    TestNames();
    if (g_fail == 0) std::printf("ThemeVariantTests: all passed\n");
    else std::printf("ThemeVariantTests: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
