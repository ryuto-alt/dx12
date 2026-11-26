// Graphite Gauge の生成物（src/editor/ThemeGraphite.gen.h / UiMotionTokens.gen.h）と、その入力（assets/editor/theme/gg/tokens.json）の
// 突き合わせ（W1-A2 / GG-1）。GPU 不要。
//   ・色トークン: `cpp` 欄が既存の Palette.xxx のものは、同名のフィールドへ同じ値が入っている（仕様の改名で黙って値が落ちない）
//   ・色トークンの総数（別名を除く）= 生成したフィールドの総数（落ちたトークン・余ったフィールドが無い）
//   ・黄の予算（GgYellowBudget）の各値が tokens.yellowBudget の値と一致
//   ・黄ゾーン（OKLCH C>=0.07 かつ h 75..105）にある色トークンは accent 系の 4 つの色だけ
//   ・モーション: 曲線・時間・待ち・Reduce Motion が tokens.motion と一致（34 の用途）
//   ・装飾の寸法（deco.*）
// 生成物の最新確認と SHA256 / integrate_check の検査は ctest の GgTokensGenCheck（tools/theme/gen_gg_tokens.py --check）が行う。
#include "editor/EditorTheme.h"
#include "editor/ThemeGraphite.gen.h"
#include "editor/UiMotionTokens.gen.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#ifndef GG_TEST_ROOT
#error GG_TEST_ROOT must be defined
#endif

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

namespace th = dx12e::theme;
namespace gg = dx12e::theme::gg;
namespace mt = dx12e::ui::motion_tokens;
using json = nlohmann::json;

bool ParseHex(const std::string& v, unsigned& rgb, unsigned& a)
{
    static const std::regex re("^#([0-9A-Fa-f]{6})([0-9A-Fa-f]{2})?$");
    std::smatch m;
    if (!std::regex_match(v, m, re)) return false;
    rgb = static_cast<unsigned>(std::stoul(m[1].str(), nullptr, 16));
    a = m[2].matched ? static_cast<unsigned>(std::stoul(m[2].str(), nullptr, 16)) : 255u;
    return true;
}
bool SameColor(const ImVec4& c, const std::string& hex)
{
    unsigned rgb = 0, a = 0;
    if (!ParseHex(hex, rgb, a)) return false;
    const ImVec4 e = gg::detail::C(rgb, a);
    return c.x == e.x && c.y == e.y && c.z == e.z && c.w == e.w;
}

struct Lch { double C, h; };
Lch ToOklch(unsigned rgb)
{
    auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const double r = lin(((rgb >> 16) & 0xFF) / 255.0), g = lin(((rgb >> 8) & 0xFF) / 255.0), b = lin((rgb & 0xFF) / 255.0);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    double h = std::atan2(B, A) * 180.0 / 3.14159265358979323846;
    if (h < 0) h += 360.0;
    return { std::sqrt(A * A + B * B), h };
}

json LoadTokens()
{
    std::ifstream f(std::string(GG_TEST_ROOT) + "/assets/editor/theme/gg/tokens.json", std::ios::binary);
    if (!f) { std::printf("FAIL tokens.json が開けない\n"); ++g_fail; return json::object(); }
    std::stringstream ss; ss << f.rdbuf();
    return json::parse(ss.str());
}

// 既存の Palette のフィールド名 → メンバ（ThemeVariants.h の宣言と同じ名前。ここに無い `Palette.xxx` は仕様の不整合）
struct PEntry { const char* name; ImVec4 th::Palette::* mem; };
#define P(n) { #n, &th::Palette::n }
const PEntry kPalette[] = {
    P(bg0), P(bg1), P(bg2), P(bg3), P(bg4), P(popupBg), P(inputBg), P(inputBgHover), P(inputBgActive), P(inputBorder), P(inputBorderHover),
    P(border), P(borderStrong), P(separator), P(accent), P(accentHover), P(accentPressed), P(onAccent),
    P(selection), P(selectionHover), P(selectionActive), P(tabOverline), P(tabOverlineDim),
    P(text), P(textMid), P(textDim), P(textFaint), P(good), P(warn), P(bad),
    P(typeMesh), P(typeLight), P(typeCamera), P(typeAudio), P(typeScript), P(typePhysics), P(typeUi), P(typePrefab), P(typeEmpty), P(typeFolder), P(typeScene),
    P(btnActive), P(scrollGrab), P(scrollGrabHover), P(scrollGrabActive), P(tableBorderLight), P(tableRowAlt), P(treeLines),
    P(textSelectedBg), P(dockingPreview), P(separatorHover), P(resizeGripHover),
};
#undef P

void TestColors(const json& tok)
{
    const th::Palette& pal = gg::kGraphitePalette;
    int nonAlias = 0, matchedPalette = 0;
    std::set<std::string> palNamesInTokens;
    for (const char* grp : { "surface", "text", "accent", "selection", "status", "statusCvd", "axis", "kind", "viewport", "timeline", "play", "overlay", "gauge" })
    {
        for (auto it = tok["color"][grp].begin(); it != tok["color"][grp].end(); ++it)
        {
            const std::string cpp = it.value()["cpp"].get<std::string>();
            const std::string val = it.value()["value"].get<std::string>();
            unsigned rgb, a;
            CHECKF(ParseHex(val, rgb, a), "%s.%s の値が色でない: %s", grp, it.key().c_str(), val.c_str());
            if (!cpp.empty() && cpp[0] == '=') continue;   // 別名（フィールドを持たない）
            ++nonAlias;
            static const std::regex re("^Palette\\.(\\w+)");
            std::smatch m;
            if (!std::regex_search(cpp, m, re)) continue;   // NEW ...
            const std::string name = m[1].str();
            bool found = false;
            for (const PEntry& e : kPalette)
                if (name == e.name)
                {
                    found = true;
                    ++matchedPalette;
                    palNamesInTokens.insert(name);
                    CHECKF(SameColor(pal.*(e.mem), val), "Palette.%s が tokens の %s.%s (%s) と一致しない", name.c_str(), grp, it.key().c_str(), val.c_str());
                }
            CHECKF(found, "tokens の `Palette.%s` に対応する既存フィールドが無い（%s.%s）", name.c_str(), grp, it.key().c_str());
        }
    }
    // 既存の Palette の全フィールド（windowBg を除く）が tokens のどれかから来ている
    for (const PEntry& e : kPalette)
        CHECKF(palNamesInTokens.count(e.name) == 1, "Palette.%s に対応する tokens が無い", e.name);
    CHECK(SameColor(pal.windowBg, tok["color"]["surface"]["bg1"]["value"].get<std::string>()));   // windowBg = bg1
    CHECK(matchedPalette == static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0])));
    // 総数: 別名を除く全トークン = 既存 Palette（windowBg を除く）+ 生成した追加色の全フィールド
    const int generated = static_cast<int>(sizeof(th::Palette) / sizeof(ImVec4)) - 1
        + static_cast<int>((sizeof(gg::GgViewportPalette) + sizeof(gg::GgTimelinePalette) + sizeof(gg::GgStatus) + sizeof(gg::GgStatusCvd)
                            + sizeof(gg::GgAxis) + sizeof(gg::GgKind) + sizeof(gg::GgChrome)) / sizeof(ImVec4));
    CHECKF(nonAlias == generated, "tokens の別名でない色 %d 個 != 生成したフィールド %d 個", nonAlias, generated);
}

void TestYellow(const json& tok)
{
    // accent 系の 4 つの色（rgb）
    const char* accents[] = { "accent", "accentHover", "accentPressed", "accentDim" };
    std::set<unsigned> accentRgb;
    for (const char* k : accents)
    {
        unsigned rgb, a;
        CHECK(ParseHex(tok["color"]["accent"][k]["value"].get<std::string>(), rgb, a));
        accentRgb.insert(rgb);
    }
    const json& yz = tok["yellowBudget"]["yellowZone"];
    const double cmin = yz["oklchChromaMin"].get<double>(), hmin = yz["hueMin"].get<double>(), hmax = yz["hueMax"].get<double>();
    int inZone = 0;
    for (auto g = tok["color"].begin(); g != tok["color"].end(); ++g)
        for (auto it = g.value().begin(); it != g.value().end(); ++it)
        {
            unsigned rgb, a;
            if (!ParseHex(it.value()["value"].get<std::string>(), rgb, a)) continue;
            const Lch k = ToOklch(rgb);
            if (k.C >= cmin && k.h >= hmin && k.h <= hmax)
            {
                ++inZone;
                CHECKF(accentRgb.count(rgb) == 1, "color.%s.%s が黄ゾーンにあるのに accent 系の色ではない", g.key().c_str(), it.key().c_str());
            }
        }
    CHECK(inZone >= 10);   // accent 系は別名（トンボ・枠・タグ…）を含めて何度も出る
    CHECK(static_cast<float>(yz["oklchChromaMin"].get<double>()) == gg::GgYellowBudget{}.zoneChromaMin);
}

void TestBudget(const json& tok)
{
    const gg::GgYellowBudget b{};
    const json& yb = tok["yellowBudget"];
    CHECK(b.budgetLineCount == static_cast<int>(yb["lines"].size()));
    CHECK(b.fillCount == static_cast<int>(yb["fills"].size()));
    CHECK(b.zoneChromaMin == static_cast<float>(yb["yellowZone"]["oklchChromaMin"].get<double>()));
    CHECK(b.zoneHueMin == static_cast<float>(yb["yellowZone"]["hueMin"].get<double>()));
    CHECK(b.zoneHueMax == static_cast<float>(yb["yellowZone"]["hueMax"].get<double>()));
    std::string tests;
    for (const auto& s : yb["tests"]) tests += s.get<std::string>() + " ";
    std::smatch m;
    CHECK(std::regex_search(tests, m, std::regex("編集モード\\s*≤\\s*([0-9.]+)\\s*%")) && b.editPixelRatioMax == std::stof(m[1].str()));
    CHECK(std::regex_search(tests, m, std::regex("再生モード\\s*≤\\s*([0-9.]+)\\s*%")) && b.playPixelRatioMax == std::stof(m[1].str()));
    const std::string worst = yb["screenCap"]["worstCase"].get<std::string>();
    CHECK(std::regex_search(worst, m, std::regex("=\\s*線\\s*([0-9]+)\\s*箇所")) && b.editLinesMax == std::stoi(m[1].str()));
    CHECK(b.editFillsMax == 0);   // 「編集モードで黄の塗り矩形が 0 個」
    {
        const size_t pos = tests.find("黄の塗り矩形");   // std::regex は UTF-8 の文字クラスを扱えないので、先に位置で絞る
        const std::string rest = pos == std::string::npos ? std::string() : tests.substr(pos);
        CHECK(std::regex_search(rest, m, std::regex("が\\s*([0-9]+)\\s*個")) && b.editFillsMax == std::stoi(m[1].str()));
    }
    // 起動画面で黄を許す要素（4 つ。tokens の boot[0].where の先頭の「/」区切り）
    CHECK(b.bootAllowedCount == 4 && static_cast<int>(sizeof(gg::kBootYellowAllowed) / sizeof(gg::kBootYellowAllowed[0])) == b.bootAllowedCount);
    const std::string where = yb["boot"][0]["where"].get<std::string>();
    for (const char* s : gg::kBootYellowAllowed) CHECKF(where.find(s) != std::string::npos, "boot の許可要素が tokens に無い: %s", s);
    // 番号つきの予算（01〜09 と F1〜F2）
    CHECK(yb["lines"].size() == 9 && yb["fills"].size() == 2);
}

void TestDeco(const json& tok)
{
    const gg::GgDeco d{};
    const json& de = tok["deco"];
    CHECK(d.scaleMinorStep == de["scaleMinorStep"]["value"].get<int>() && d.scaleMajorStep == de["scaleMajorStep"]["value"].get<int>());
    CHECK(d.scaleMinorH == 3 && d.scaleMajorH == 6 && d.scaleMinorAlpha == 0.13f && d.scaleMajorAlpha == 0.28f);
    CHECK(d.caliperJawW == 2 && d.caliperJawH == 8 && d.tomboSelArm == 10 && d.tomboSelOffset == 8 && d.tomboTileArm == 9);
    CHECK(d.hatchAngle == 45 && d.hatchStep == 6 && d.hatchAlpha == 0.07f);
    CHECK(d.warnBarDashLen == 4 && d.warnBarDashGap == 3 && d.playDashLen == 10 && d.playDashGap == 6 && d.badBarW == 3);
    CHECK(d.progressStep == 12 && d.dockGripW == 2 && d.dockGripH == 24 && d.dockHitPad == 3 && d.playTagH == 23);
    CHECK(d.caliper == de["caliper"]["value"].get<bool>() && d.headerOverline == de["headerOverline"]["value"].get<bool>());
    CHECK(d.rowNeedleBar == (de["rowStyle"]["value"].get<std::string>() == "NeedleBar"));
    // 線幅の規則（theme::HairPx() 等が使う数値）
    CHECK(d.hairMinPx == 1 && d.accentMinPx == 2 && d.accentMulPx == 2 && d.playFrameMinPx == 3 && d.playFrameMulPx == 3 && d.tickAlphaScale == 0.75f);
    const gg::GgLayout l{};
    const json& ly = tok["metric"]["layout"];
    CHECK(l.rowH == ly["rowH"]["value"].get<int>() && l.headerH == 26 && l.titleBarH == 32 && l.statusBarH == 22 && l.instrumentRailH == 22);
    CHECK(l.playGutter == 5 && l.dockTabBarH == 30 && l.inspectorLabelW == 86);
    CHECK(l.rowH == static_cast<int>(th::size::kRowH));
    // Metrics（ThemeGraphite.gen.h の Graphite 用）
    CHECK(gg::kMetrics.fontBase == static_cast<float>(tok["typography"]["scale"]["body"]["imguiPx"].get<double>()));
    CHECK(gg::kMetrics.fontMono == static_cast<float>(tok["typography"]["scale"]["mono"]["imguiPx"].get<double>()));
}

#define DURATIONS(X) \
    X(press, Press) X(tooltipOut, TooltipOut) X(hover, Hover) X(vernierFade, VernierFade) X(sliderJump, SliderJump) \
    X(release, Release) X(selectionSlide, SelectionSlide) X(unfocusDim, UnfocusDim) X(toolUnderline, ToolUnderline) \
    X(tomboDraw, TomboDraw) X(tabOverlineSlide, TabOverlineSlide) X(caliperSlide, CaliperSlide) X(paletteOut, PaletteOut) \
    X(tooltipIn, TooltipIn) X(popupIn, PopupIn) X(paletteIn, PaletteIn) X(scrimIn, ScrimIn) X(toastOut, ToastOut) \
    X(toastReflow, ToastReflow) X(toastIn, ToastIn) X(dropTargetIn, DropTargetIn) X(dragGhostIn, DragGhostIn) \
    X(chevronRotate, ChevronRotate) X(treeGuideDraw, TreeGuideDraw) X(dropSettle, DropSettle) X(dragReturn, DragReturn) \
    X(progressStep, ProgressStep) X(playButton, PlayButton) X(pauseDash, PauseDash) X(hatchFade, HatchFade) \
    X(playStop, PlayStop) X(playStart, PlayStart) X(progressDoneFade, ProgressDoneFade) X(cameraFrame, CameraFrame)

mt::Curve CurveOf(const std::string& s)
{
    if (s == "standard") return mt::Curve::Standard;
    if (s == "exit") return mt::Curve::Exit;
    if (s == "linear") return mt::Curve::Linear;
    if (s == "expo") return mt::Curve::Expo;
    if (s == "spring") return mt::Curve::Spring;
    return mt::Curve::Stepped;
}

void TestMotion(const json& tok)
{
    const json& mo = tok["motion"];
    int n = 0;
#define X(key, Name) do { ++n; const json& j = mo["durationsMs"][#key]; \
        CHECKF(mt::k##Name.ms == j["value"].get<float>(), "durationsMs.%s の ms", #key); \
        CHECKF(mt::k##Name.curve == CurveOf(j["curve"].get<std::string>()), "durationsMs.%s の曲線", #key); } while (0);
    DURATIONS(X)
#undef X
    CHECKF(n == static_cast<int>(mo["durationsMs"].size()), "durationsMs の数 %d != tokens %d（用途が増えた。DURATIONS を更新する）", n, static_cast<int>(mo["durationsMs"].size()));
    CHECK(mt::kTooltipDelayMs == mo["waitsMs"]["tooltipDelay"]["value"].get<float>());
    CHECK(mt::kReadoutHoldMs == mo["waitsMs"]["readoutHold"]["value"].get<float>());
    CHECK(mt::kToastLifeMs == mo["waitsMs"]["toastLife"]["value"].get<float>() && mo["waitsMs"].size() == 3);
    CHECK(mt::kD60 == 60.0f && mt::kD90 == 90.0f && mt::kD120 == 120.0f && mt::kD160 == 160.0f && mt::kD200 == 200.0f && mt::kD240 == 240.0f);
    CHECK(mo["scaleMs"].size() == 6);
    // 曲線: standard = cubic-bezier(0.2, 0.8, 0.2, 1)
    CHECK(mt::kStandard.x1 == 0.2f && mt::kStandard.y1 == 0.8f && mt::kStandard.x2 == 0.2f && mt::kStandard.y2 == 1.0f);
    CHECK(mt::kExit.x1 == 0.4f && mt::kExit.y1 == 0.0f && mt::kExit.x2 == 1.0f && mt::kExit.y2 == 1.0f);
    CHECK(mt::kExpo.x1 == 0.16f && mt::kExpo.y1 == 1.0f && mt::kExpo.x2 == 0.3f && mt::kExpo.y2 == 1.0f);
    CHECK(mt::kSpringOmega == 14.0f && mt::kSteppedPx == 12.0f && mt::kSteppedSteps == 12 && mt::kSnapPx == 0.5f);
    CHECK(mt::kReduceMotionDurationMs == 0.0f);
}
} // namespace

int main()
{
    const json tok = LoadTokens();
    if (tok.empty()) return 1;
    CHECK(tok["meta"]["version"].get<std::string>() == "3.1.0");
    TestColors(tok);
    TestYellow(tok);
    TestBudget(tok);
    TestDeco(tok);
    TestMotion(tok);
    if (g_fail == 0) std::printf("GgTokensGenTests: all passed\n");
    else std::printf("GgTokensGenTests: %d failure(s)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
