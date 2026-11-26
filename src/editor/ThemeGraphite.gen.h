// ★自動生成: tools/theme/gen_gg_tokens.py（入力 assets/editor/theme/gg/tokens.json、SHA256 = fa050cd1d8a7e6f9a6bf3848fa572463214995df7cca83fc7f87567ef024024d）。手で直さない。
// Graphite Gauge v3.1.0 のトークン。これを include してよいのは ThemeGraphite.cpp と、値を直接読むテスト・描画の .cpp だけ
// （EditorTheme.h / ThemeVariants.h などの重ヘッダから include しない。仕様の再生成で再コンパイルされる TU を最小に保つため）。
#pragma once

#include "editor/ThemeVariants.h"

namespace dx12e::theme::gg
{

namespace detail
{
// 0xRRGGBB と alpha（0..255）→ ImVec4（ThemeVariants.h の Hex() と同じ計算）
constexpr ImVec4 C(unsigned int rgb, unsigned int a = 255u)
{
    return ImVec4(((rgb >> 16) & 0xFFu) / 255.0f, ((rgb >> 8) & 0xFFu) / 255.0f, (rgb & 0xFFu) / 255.0f, a / 255.0f);
}
} // namespace detail

// ---- 色（tokens.color.* の `cpp` 欄の NEW 行。フィールド名は cpp 欄の名前から機械的に決める）----
struct GgViewportPalette
{
    ImVec4 bgKey;
    ImVec4 bgMid;
    ImVec4 bgLow;
    ImVec4 bgEdge;
    ImVec4 bgFill;
    ImVec4 floorNear;
    ImVec4 floorFar;
    ImVec4 gridMinor;
    ImVec4 gridMajor;
    ImVec4 gridOrigin;
    ImVec4 selOutline;
    ImVec4 selOutline2;
    ImVec4 selOccluded;
    ImVec4 dimLine;
    ImVec4 labelBg;
    ImVec4 handle;
    ImVec4 icon;
    ImVec4 iconHalo;
    ImVec4 iconDim;
    ImVec4 lightRange;
    ImVec4 frustum;
    ImVec4 audioRange;
    ImVec4 collider;
    ImVec4 hoverOutline;
};
struct GgTimelinePalette
{
    ImVec4 tick;
    ImVec4 tickMajor;
    ImVec4 range;
    ImVec4 outOfRange;
    ImVec4 key;
    ImVec4 keyEdge;
    ImVec4 keySel;
};
struct GgStatus
{
    ImVec4 info;
    ImVec4 badText;
    ImVec4 goodBg;
    ImVec4 warnBg;
    ImVec4 badBg;
    ImVec4 infoBg;
    ImVec4 infoEdge;
    ImVec4 goodEdge;
    ImVec4 warnEdge;
    ImVec4 badEdge;
};
struct GgStatusCvd
{
    ImVec4 good;
    ImVec4 warn;
    ImVec4 bad;
    ImVec4 info;
};
struct GgAxis
{
    ImVec4 x;
    ImVec4 y;
    ImVec4 z;
    ImVec4 hot;
    ImVec4 shadow;
    ImVec4 planeX;
    ImVec4 planeY;
    ImVec4 planeZ;
};
struct GgKind
{
    ImVec4 prefabText;
    ImVec4 material;
    ImVec4 texture;
};
// 面・文字・アクセント・再生・暗幕・目盛りのうち、既存の Palette に無いもの
struct GgChrome
{
    ImVec4 grooveHl;
    ImVec4 inLine;
    ImVec4 inLineHover;
    ImVec4 controlEdge;
    ImVec4 primaryBtn;
    ImVec4 primaryBtnHover;
    ImVec4 primaryBtnEdge;
    ImVec4 primaryBtnActive;
    ImVec4 textDisabled;
    ImVec4 link;
    ImVec4 accentDim;
    ImVec4 selBarSecondary;
    ImVec4 dockingPreviewEdge;
    ImVec4 dragDropTarget;
    ImVec4 playFrame;
    ImVec4 playKeyline;
    ImVec4 hatch;
    ImVec4 scrimPalette;
    ImVec4 scrimModal;
    ImVec4 scrimNav;
    ImVec4 navHighlight;
    ImVec4 popupInnerTop;
    ImVec4 shadowNear;
    ImVec4 shadowFar;
    ImVec4 tickMinor;
    ImVec4 tickMajor;
    ImVec4 vernierMinor;
    ImVec4 vernierMajor;
};

// ---- 装飾の寸法（tokens.deco.* / tokens.line.*。論理 px は ui::Px を通して使う）----
struct GgDeco
{
    int scaleMinorStep = 8;
    int scaleMinorH = 3;
    int scaleMajorStep = 40;
    int scaleMajorH = 6;
    float scaleMinorAlpha = 0.13f;
    float scaleMajorAlpha = 0.28f;
    int caliperJawW = 2;
    int caliperJawH = 8;
    int tomboSelArm = 10;
    int tomboSelOffset = 8;
    int tomboTileArm = 9;
    int tomboTileSecondaryArm = 6;
    int hatchAngle = 45;
    int hatchStep = 6;
    float hatchAlpha = 0.07f;
    int vernierFineStep = 8;
    int vernierMajorStep = 40;
    float vernierMinorAlpha = 0.3f;
    float vernierMajorAlpha = 0.55f;
    int progressStep = 12;
    int dockGripW = 2;
    int dockGripH = 24;
    int dockHitPad = 3;
    int badBarW = 3;
    int playTagH = 23;
    int playTagPadX = 8;
    float roundButton = 3.0f;
    float roundPill = 4.0f;
    float roundPlayTag = 0.0f;
    int warnBarDashLen = 4;
    int warnBarDashGap = 3;
    int playDashLen = 10;
    int playDashGap = 6;
    bool caliper = true;
    bool headerOverline = false;
    bool rowNeedleBar = true;
    int hairMinPx = 1;
    int accentMinPx = 2;
    int accentMulPx = 2;
    int playFrameMinPx = 3;
    int playFrameMulPx = 3;
    float tickAlphaScale = 0.75f;
};

// ---- レイアウト寸法（tokens.metric.layout / component。論理 px）----
struct GgLayout
{
    int rowH = 23;
    int headerH = 26;
    int toolbarBtn = 28;
    int titleBarH = 32;
    int toolRowH = 36;
    int dockTabBarH = 30;
    int instrumentRailH = 22;
    int statusBarH = 22;
    int checkBox = 16;
    int toggleW = 30;
    int toggleH = 16;
    int swatchW = 46;
    int axisTagW = 15;
    int inspectorLabelW = 86;
    int paletteRowH = 34;
    int windowPadX = 8;
    int windowPadY = 8;
    int itemSpacingX = 6;
    int itemSpacingY = 4;
    int indent = 16;
    int scrollbar = 10;
    int playGutter = 5;
    int vpNarrowW = 980;
    int progressH = 6;
    int progressThinH = 4;
    int progressNeedleH = 12;
    int scrollMarkW = 3;
    int scrollMarkH = 2;
    int scrollMarkMinGap = 2;
    int buttonPadX = 10;
    int splitArrowW = 21;
    int toolBtnW = 28;
    int iconToggle = 23;
    int tooltipMaxW = 360;
    int modalMinW = 420;
    int modalMaxW = 560;
    int modalTop = 40;
    int toastGap = 6;
    int toastBottom = 12;
};

// ---- 黄の予算（tokens.yellowBudget。テストが突き合わせる）----
struct GgYellowBudget
{
    int   editLinesMax = 9;          // 編集中に同時に見える黄の線の最大数（予算 01〜08 の最悪ケース）
    int   budgetLineCount = 9;       // 予算表の線の番号の数（deco::Needle* / Tombo* / Caliper の呼び出し箇所数に一致させる）
    int   fillCount = 2;             // 予算表の黄の面の番号の数（F1 再生ボタン、F2 再生タグ）
    float editPixelRatioMax = 1.5f;    // 編集モードの黄の画素比の上限（%。OKLCH で C>=0.07 かつ h in [75,105]）
    float playPixelRatioMax = 2.5f;    // 再生モードの黄の画素比の上限（%）
    int   editFillsMax = 0;          // 編集モードで許す黄の塗り矩形の数（面積 16px2 以上かつ短辺 4px 以上）
    float zoneChromaMin = 0.07f;
    float zoneHueMin = 75.0f;
    float zoneHueMax = 105.0f;
    int   bootAllowedCount = 4;      // 起動画面で黄を許す要素の数（下の kBootYellowAllowed）
};
inline constexpr const char* kBootYellowAllowed[] = {
    "四隅のトンボ",
    "文字盤の校正三角と指針・指針の残像線（1px、α 24%・10%）",
    "ハブ",
    "現在ステップの上線",
};

struct GgSpec
{
    GgViewportPalette viewport;
    GgTimelinePalette timeline;
    GgStatus status;
    GgStatusCvd statusCvd;
    GgAxis axis;
    GgKind kind;
    GgChrome chrome;
    GgDeco deco;
    GgLayout layout;
    GgYellowBudget budget;
};

using detail::C;

// ---- 値 ----
inline constexpr Palette kGraphitePalette = []() constexpr {
    Palette s{};
    s.bg0 = C(0x131312);
    s.bg1 = C(0x1B1A19);
    s.bg2 = C(0x222120);
    s.bg3 = C(0x2A2927);
    s.bg4 = C(0x34322F);
    s.windowBg = C(0x1B1A19);
    s.popupBg = C(0x242321);
    s.inputBg = C(0x151514);
    s.inputBgHover = C(0x181817);
    s.inputBgActive = C(0x111110);
    s.inputBorder = C(0x2C2B28);
    s.inputBorderHover = C(0x3A3935);
    s.border = C(0x2C2B28);
    s.borderStrong = C(0x46443F);
    s.separator = C(0x0A0A09);
    s.accent = C(0xF0C23B);
    s.accentHover = C(0xF6D265);
    s.accentPressed = C(0xD4A82A);
    s.onAccent = C(0x1C1705);
    s.selection = C(0x33322E);
    s.selectionHover = C(0x373632);
    s.selectionActive = C(0x3D3B36);
    s.tabOverline = C(0xF0C23B);
    s.tabOverlineDim = C(0x9A8240);
    s.text = C(0xE6E2DA);
    s.textMid = C(0xCFCAC1);
    s.textDim = C(0xB4AFA6);
    s.textFaint = C(0xA39E94);
    s.good = C(0x58BFB4);
    s.warn = C(0xF18B73);
    s.bad = C(0xEC5480);
    s.typeMesh = C(0xB9B3A8);
    s.typeLight = C(0xE8E0CC);
    s.typeCamera = C(0xA6BFD2);
    s.typeAudio = C(0x9DB9C4);
    s.typeScript = C(0xB6A1E0);
    s.typePhysics = C(0x9CC27A);
    s.typeUi = C(0xDE8FB5);
    s.typePrefab = C(0x7DA9EC);
    s.typeEmpty = C(0x9A958B);
    s.typeFolder = C(0xC2B59A);
    s.typeScene = C(0xD4A08C);
    s.btnActive = C(0x3D3B36);
    s.scrollGrab = C(0x3A3935);
    s.scrollGrabHover = C(0x5E5A53);
    s.scrollGrabActive = C(0x8A857C);
    s.tableBorderLight = C(0x232220);
    s.tableRowAlt = C(0xE6E2DA, 6);
    s.treeLines = C(0xE6E2DA, 36);
    s.textSelectedBg = C(0x8A857C, 102);
    s.dockingPreview = C(0xE6E2DA, 26);
    s.separatorHover = C(0xB4AFA6, 153);
    s.resizeGripHover = C(0xB4AFA6, 102);
    return s;
}();
inline constexpr GgViewportPalette kViewport = []() constexpr {
    GgViewportPalette s{};
    s.bgKey = C(0x8A857B);
    s.bgMid = C(0x5E5A53);
    s.bgLow = C(0x3B3935);
    s.bgEdge = C(0x262522);
    s.bgFill = C(0x9FB6CC, 56);
    s.floorNear = C(0x55524C);
    s.floorFar = C(0x1B1A18);
    s.gridMinor = C(0xE6E2DA, 14);
    s.gridMajor = C(0xE6E2DA, 33);
    s.gridOrigin = C(0xE6E2DA, 66);
    s.selOutline = C(0xF4F1EA, 217);
    s.selOutline2 = C(0xF4F1EA, 128);
    s.selOccluded = C(0xF4F1EA, 56);
    s.dimLine = C(0xE6E2DA, 191);
    s.labelBg = C(0x131312, 224);
    s.handle = C(0xE6E2DA);
    s.icon = C(0xE6E2DA, 235);
    s.iconHalo = C(0x0A0A09, 153);
    s.iconDim = C(0xE6E2DA, 102);
    s.lightRange = C(0xE8E0CC, 115);
    s.frustum = C(0xA6BFD2, 153);
    s.audioRange = C(0x9DB9C4, 89);
    s.collider = C(0x9CC27A, 178);
    s.hoverOutline = C(0xA6BFD2, 153);
    return s;
}();
inline constexpr GgTimelinePalette kTimeline = []() constexpr {
    GgTimelinePalette s{};
    s.tick = C(0xE6E2DA, 33);
    s.tickMajor = C(0xE6E2DA, 71);
    s.range = C(0xE6E2DA, 13);
    s.outOfRange = C(0xE6E2DA, 18);
    s.key = C(0x8A857C);
    s.keyEdge = C(0x0A0A09);
    s.keySel = C(0xF4F1EA);
    return s;
}();
inline constexpr GgStatus kStatus = []() constexpr {
    GgStatus s{};
    s.info = C(0x8FB0D6);
    s.badText = C(0xFB63B0);
    s.goodBg = C(0x58BFB4, 13);
    s.warnBg = C(0xF18B73, 13);
    s.badBg = C(0xEC5480, 15);
    s.infoBg = C(0x8FB0D6, 10);
    s.infoEdge = C(0x8FB0D6, 71);
    s.goodEdge = C(0x58BFB4, 71);
    s.warnEdge = C(0xF18B73, 71);
    s.badEdge = C(0xEC5480, 71);
    return s;
}();
inline constexpr GgStatusCvd kStatusCvd = []() constexpr {
    GgStatusCvd s{};
    s.good = C(0x7DC3DE);
    s.warn = C(0x937AF7);
    s.bad = C(0xE45268);
    s.info = C(0xB4AFA6);
    return s;
}();
inline constexpr GgAxis kAxis = []() constexpr {
    GgAxis s{};
    s.x = C(0xCF6872);
    s.y = C(0xA9DD83);
    s.z = C(0x6E98DA);
    s.hot = C(0xF4F1EA);
    s.shadow = C(0x000000, 115);
    s.planeX = C(0xCF6872, 46);
    s.planeY = C(0xA9DD83, 46);
    s.planeZ = C(0x6E98DA, 46);
    return s;
}();
inline constexpr GgKind kKind = []() constexpr {
    GgKind s{};
    s.prefabText = C(0xA9C4F0);
    s.material = C(0x8A857C);
    s.texture = C(0xB6A1E0);
    return s;
}();
inline constexpr GgChrome kChrome = []() constexpr {
    GgChrome s{};
    s.grooveHl = C(0x2F2E2B);
    s.inLine = C(0x716D65);
    s.inLineHover = C(0x8A857C);
    s.controlEdge = C(0x77736A);
    s.primaryBtn = C(0x3F3D38);
    s.primaryBtnHover = C(0x4A4842);
    s.primaryBtnEdge = C(0x8A857C);
    s.primaryBtnActive = C(0x34322F);
    s.textDisabled = C(0x6E6A62);
    s.link = C(0xA9C4F0);
    s.accentDim = C(0x9A8240);
    s.selBarSecondary = C(0x8A857C);
    s.dockingPreviewEdge = C(0xE6E2DA, 153);
    s.dragDropTarget = C(0xE6E2DA);
    s.playFrame = C(0xF0C23B);
    s.playKeyline = C(0x0A0A09, 217);
    s.hatch = C(0xE6E2DA, 18);
    s.scrimPalette = C(0x000000, 87);
    s.scrimModal = C(0x000000, 140);
    s.scrimNav = C(0x000000, 115);
    s.navHighlight = C(0xE6E2DA, 178);
    s.popupInnerTop = C(0xFFFFFF, 13);
    s.shadowNear = C(0x000000, 102);
    s.shadowFar = C(0x000000, 140);
    s.tickMinor = C(0xE6E2DA, 33);
    s.tickMajor = C(0xE6E2DA, 71);
    s.vernierMinor = C(0xE6E2DA, 76);
    s.vernierMajor = C(0xE6E2DA, 140);
    return s;
}();

inline constexpr GgSpec kGgSpec{ kViewport, kTimeline, kStatus, kStatusCvd, kAxis, kKind, kChrome, GgDeco{}, GgLayout{}, GgYellowBudget{} };

// ---- トークン → 生成フィールドの対応表（全色トークン。別名は参照先のフィールドを指す）----
struct GgFieldRef { const char* path; const char* cpp; const char* member; const ImVec4* ptr; bool alias; };
inline constexpr GgFieldRef kGgFieldMap[] = {
    { "color.surface.bg0", "Palette.bg0", "Palette::bg0", &kGraphitePalette.bg0, false },
    { "color.surface.bg1", "Palette.bg1 (= windowBg)", "Palette::bg1", &kGraphitePalette.bg1, false },
    { "color.surface.bg2", "Palette.bg2", "Palette::bg2", &kGraphitePalette.bg2, false },
    { "color.surface.bg3", "Palette.bg3", "Palette::bg3", &kGraphitePalette.bg3, false },
    { "color.surface.bg4", "Palette.bg4", "Palette::bg4", &kGraphitePalette.bg4, false },
    { "color.surface.popup", "Palette.popupBg", "Palette::popupBg", &kGraphitePalette.popupBg, false },
    { "color.surface.groove", "Palette.separator", "Palette::separator", &kGraphitePalette.separator, false },
    { "color.surface.grooveHl", "NEW Palette.grooveHl", "GgChrome::grooveHl", &kChrome.grooveHl, false },
    { "color.surface.input", "Palette.inputBg", "Palette::inputBg", &kGraphitePalette.inputBg, false },
    { "color.surface.inputHover", "Palette.inputBgHover", "Palette::inputBgHover", &kGraphitePalette.inputBgHover, false },
    { "color.surface.inputActive", "Palette.inputBgActive", "Palette::inputBgActive", &kGraphitePalette.inputBgActive, false },
    { "color.surface.inputBorder", "Palette.inputBorder", "Palette::inputBorder", &kGraphitePalette.inputBorder, false },
    { "color.surface.inputBorderHover", "Palette.inputBorderHover", "Palette::inputBorderHover", &kGraphitePalette.inputBorderHover, false },
    { "color.surface.inLine", "NEW Palette.inLine", "GgChrome::inLine", &kChrome.inLine, false },
    { "color.surface.inLineHover", "NEW Palette.inLineHover", "GgChrome::inLineHover", &kChrome.inLineHover, false },
    { "color.surface.controlEdge", "NEW Palette.controlEdge", "GgChrome::controlEdge", &kChrome.controlEdge, false },
    { "color.surface.border", "Palette.border", "Palette::border", &kGraphitePalette.border, false },
    { "color.surface.borderStrong", "Palette.borderStrong", "Palette::borderStrong", &kGraphitePalette.borderStrong, false },
    { "color.surface.btnActive", "Palette.btnActive", "Palette::btnActive", &kGraphitePalette.btnActive, false },
    { "color.surface.scrollGrab", "Palette.scrollGrab", "Palette::scrollGrab", &kGraphitePalette.scrollGrab, false },
    { "color.surface.scrollGrabHover", "Palette.scrollGrabHover", "Palette::scrollGrabHover", &kGraphitePalette.scrollGrabHover, false },
    { "color.surface.scrollGrabActive", "Palette.scrollGrabActive", "Palette::scrollGrabActive", &kGraphitePalette.scrollGrabActive, false },
    { "color.surface.tableBorderLight", "Palette.tableBorderLight", "Palette::tableBorderLight", &kGraphitePalette.tableBorderLight, false },
    { "color.surface.tableRowAlt", "Palette.tableRowAlt", "Palette::tableRowAlt", &kGraphitePalette.tableRowAlt, false },
    { "color.surface.treeLines", "Palette.treeLines", "Palette::treeLines", &kGraphitePalette.treeLines, false },
    { "color.surface.primaryBtn", "NEW Palette.primaryBtn", "GgChrome::primaryBtn", &kChrome.primaryBtn, false },
    { "color.surface.primaryBtnHover", "NEW Palette.primaryBtnHover", "GgChrome::primaryBtnHover", &kChrome.primaryBtnHover, false },
    { "color.surface.primaryBtnEdge", "NEW Palette.primaryBtnEdge", "GgChrome::primaryBtnEdge", &kChrome.primaryBtnEdge, false },
    { "color.surface.primaryBtnActive", "NEW Palette.primaryBtnActive", "GgChrome::primaryBtnActive", &kChrome.primaryBtnActive, false },
    { "color.text.t1", "Palette.text", "Palette::text", &kGraphitePalette.text, false },
    { "color.text.t2", "Palette.textMid", "Palette::textMid", &kGraphitePalette.textMid, false },
    { "color.text.t3", "Palette.textDim", "Palette::textDim", &kGraphitePalette.textDim, false },
    { "color.text.t4", "Palette.textFaint (= ImGuiCol_TextDisabled)", "Palette::textFaint", &kGraphitePalette.textFaint, false },
    { "color.text.textDisabled", "NEW Palette.textDisabled", "GgChrome::textDisabled", &kChrome.textDisabled, false },
    { "color.text.link", "NEW Palette.link (ImGuiCol_TextLink)", "GgChrome::link", &kChrome.link, false },
    { "color.text.textSelectedBg", "Palette.textSelectedBg", "Palette::textSelectedBg", &kGraphitePalette.textSelectedBg, false },
    { "color.accent.accent", "Palette.accent", "Palette::accent", &kGraphitePalette.accent, false },
    { "color.accent.accentHover", "Palette.accentHover", "Palette::accentHover", &kGraphitePalette.accentHover, false },
    { "color.accent.accentPressed", "Palette.accentPressed", "Palette::accentPressed", &kGraphitePalette.accentPressed, false },
    { "color.accent.accentDim", "NEW Palette.accentDim", "GgChrome::accentDim", &kChrome.accentDim, false },
    { "color.accent.onAccent", "Palette.onAccent", "Palette::onAccent", &kGraphitePalette.onAccent, false },
    { "color.accent.tabOverline", "Palette.tabOverline", "Palette::tabOverline", &kGraphitePalette.tabOverline, false },
    { "color.accent.tabOverlineDim", "Palette.tabOverlineDim", "Palette::tabOverlineDim", &kGraphitePalette.tabOverlineDim, false },
    { "color.selection.sel", "Palette.selection", "Palette::selection", &kGraphitePalette.selection, false },
    { "color.selection.selHover", "Palette.selectionHover", "Palette::selectionHover", &kGraphitePalette.selectionHover, false },
    { "color.selection.selActive", "Palette.selectionActive", "Palette::selectionActive", &kGraphitePalette.selectionActive, false },
    { "color.selection.barSecondary", "NEW Palette.selBarSecondary", "GgChrome::selBarSecondary", &kChrome.selBarSecondary, false },
    { "color.selection.dockingPreview", "Palette.dockingPreview", "Palette::dockingPreview", &kGraphitePalette.dockingPreview, false },
    { "color.selection.dockingPreviewEdge", "NEW Palette.dockingPreviewEdge", "GgChrome::dockingPreviewEdge", &kChrome.dockingPreviewEdge, false },
    { "color.selection.dragDropTarget", "NEW (ImGuiCol_DragDropTarget へ)", "GgChrome::dragDropTarget", &kChrome.dragDropTarget, false },
    { "color.selection.separatorHover", "Palette.separatorHover", "Palette::separatorHover", &kGraphitePalette.separatorHover, false },
    { "color.selection.resizeGripHover", "Palette.resizeGripHover", "Palette::resizeGripHover", &kGraphitePalette.resizeGripHover, false },
    { "color.status.good", "Palette.good", "Palette::good", &kGraphitePalette.good, false },
    { "color.status.warn", "Palette.warn", "Palette::warn", &kGraphitePalette.warn, false },
    { "color.status.bad", "Palette.bad", "Palette::bad", &kGraphitePalette.bad, false },
    { "color.status.info", "NEW Palette.info", "GgStatus::info", &kStatus.info, false },
    { "color.status.badText", "NEW Palette.badText", "GgStatus::badText", &kStatus.badText, false },
    { "color.status.goodBg", "NEW Palette.goodBg", "GgStatus::goodBg", &kStatus.goodBg, false },
    { "color.status.warnBg", "NEW Palette.warnBg", "GgStatus::warnBg", &kStatus.warnBg, false },
    { "color.status.badBg", "NEW Palette.badBg", "GgStatus::badBg", &kStatus.badBg, false },
    { "color.status.infoBg", "NEW Palette.infoBg", "GgStatus::infoBg", &kStatus.infoBg, false },
    { "color.status.infoEdge", "NEW Palette.infoEdge", "GgStatus::infoEdge", &kStatus.infoEdge, false },
    { "color.status.goodEdge", "NEW Palette.goodEdge", "GgStatus::goodEdge", &kStatus.goodEdge, false },
    { "color.status.warnEdge", "NEW Palette.warnEdge", "GgStatus::warnEdge", &kStatus.warnEdge, false },
    { "color.status.badEdge", "NEW Palette.badEdge", "GgStatus::badEdge", &kStatus.badEdge, false },
    { "color.statusCvd.good", "NEW Palette(cvd).good", "GgStatusCvd::good", &kStatusCvd.good, false },
    { "color.statusCvd.warn", "NEW Palette(cvd).warn", "GgStatusCvd::warn", &kStatusCvd.warn, false },
    { "color.statusCvd.bad", "NEW Palette(cvd).bad", "GgStatusCvd::bad", &kStatusCvd.bad, false },
    { "color.statusCvd.info", "NEW Palette(cvd).info", "GgStatusCvd::info", &kStatusCvd.info, false },
    { "color.axis.x", "NEW Palette.axisX", "GgAxis::x", &kAxis.x, false },
    { "color.axis.y", "NEW Palette.axisY", "GgAxis::y", &kAxis.y, false },
    { "color.axis.z", "NEW Palette.axisZ", "GgAxis::z", &kAxis.z, false },
    { "color.axis.hot", "NEW Palette.axisHot", "GgAxis::hot", &kAxis.hot, false },
    { "color.axis.shadow", "NEW Palette.axisShadow", "GgAxis::shadow", &kAxis.shadow, false },
    { "color.axis.planeX", "NEW (axisX × planeAlpha)", "GgAxis::planeX", &kAxis.planeX, false },
    { "color.axis.planeY", "NEW (axisY × planeAlpha)", "GgAxis::planeY", &kAxis.planeY, false },
    { "color.axis.planeZ", "NEW (axisZ × planeAlpha)", "GgAxis::planeZ", &kAxis.planeZ, false },
    { "color.kind.mesh", "Palette.typeMesh", "Palette::typeMesh", &kGraphitePalette.typeMesh, false },
    { "color.kind.light", "Palette.typeLight", "Palette::typeLight", &kGraphitePalette.typeLight, false },
    { "color.kind.camera", "Palette.typeCamera", "Palette::typeCamera", &kGraphitePalette.typeCamera, false },
    { "color.kind.audio", "Palette.typeAudio", "Palette::typeAudio", &kGraphitePalette.typeAudio, false },
    { "color.kind.script", "Palette.typeScript", "Palette::typeScript", &kGraphitePalette.typeScript, false },
    { "color.kind.physics", "Palette.typePhysics", "Palette::typePhysics", &kGraphitePalette.typePhysics, false },
    { "color.kind.ui", "Palette.typeUi", "Palette::typeUi", &kGraphitePalette.typeUi, false },
    { "color.kind.prefab", "Palette.typePrefab", "Palette::typePrefab", &kGraphitePalette.typePrefab, false },
    { "color.kind.prefabText", "NEW Palette.typePrefabText", "GgKind::prefabText", &kKind.prefabText, false },
    { "color.kind.empty", "Palette.typeEmpty", "Palette::typeEmpty", &kGraphitePalette.typeEmpty, false },
    { "color.kind.folder", "Palette.typeFolder", "Palette::typeFolder", &kGraphitePalette.typeFolder, false },
    { "color.kind.scene", "Palette.typeScene", "Palette::typeScene", &kGraphitePalette.typeScene, false },
    { "color.kind.material", "NEW Palette.typeMaterial", "GgKind::material", &kKind.material, false },
    { "color.kind.texture", "NEW Palette.typeTexture", "GgKind::texture", &kKind.texture, false },
    { "color.viewport.bgKey", "NEW Palette.vpBgKey", "GgViewportPalette::bgKey", &kViewport.bgKey, false },
    { "color.viewport.bgMid", "NEW Palette.vpBgMid", "GgViewportPalette::bgMid", &kViewport.bgMid, false },
    { "color.viewport.bgLow", "NEW Palette.vpBgLow", "GgViewportPalette::bgLow", &kViewport.bgLow, false },
    { "color.viewport.bgEdge", "NEW Palette.vpBgEdge", "GgViewportPalette::bgEdge", &kViewport.bgEdge, false },
    { "color.viewport.bgFill", "NEW Palette.vpBgFill", "GgViewportPalette::bgFill", &kViewport.bgFill, false },
    { "color.viewport.floorNear", "NEW Palette.vpFloorNear", "GgViewportPalette::floorNear", &kViewport.floorNear, false },
    { "color.viewport.floorFar", "NEW Palette.vpFloorFar", "GgViewportPalette::floorFar", &kViewport.floorFar, false },
    { "color.viewport.gridMinor", "NEW Palette.vpGridMinor", "GgViewportPalette::gridMinor", &kViewport.gridMinor, false },
    { "color.viewport.gridMajor", "NEW Palette.vpGridMajor", "GgViewportPalette::gridMajor", &kViewport.gridMajor, false },
    { "color.viewport.gridOrigin", "NEW Palette.vpGridOrigin", "GgViewportPalette::gridOrigin", &kViewport.gridOrigin, false },
    { "color.viewport.selOutline", "NEW Palette.vpSelOutline (SelectionOutline の selColor)", "GgViewportPalette::selOutline", &kViewport.selOutline, false },
    { "color.viewport.selOutline2", "NEW Palette.vpSelOutline2", "GgViewportPalette::selOutline2", &kViewport.selOutline2, false },
    { "color.viewport.selOccluded", "NEW Palette.vpSelOccluded", "GgViewportPalette::selOccluded", &kViewport.selOccluded, false },
    { "color.viewport.dimLine", "NEW Palette.vpDimLine", "GgViewportPalette::dimLine", &kViewport.dimLine, false },
    { "color.viewport.dimLabelBg", "NEW Palette.vpLabelBg", "GgViewportPalette::labelBg", &kViewport.labelBg, false },
    { "color.viewport.handle", "NEW Palette.vpHandle", "GgViewportPalette::handle", &kViewport.handle, false },
    { "color.viewport.iconFill", "NEW Palette.vpIcon", "GgViewportPalette::icon", &kViewport.icon, false },
    { "color.viewport.iconHalo", "NEW Palette.vpIconHalo", "GgViewportPalette::iconHalo", &kViewport.iconHalo, false },
    { "color.viewport.iconDim", "NEW Palette.vpIconDim", "GgViewportPalette::iconDim", &kViewport.iconDim, false },
    { "color.viewport.lightRange", "NEW Palette.vpLightRange", "GgViewportPalette::lightRange", &kViewport.lightRange, false },
    { "color.viewport.cameraFrustum", "NEW Palette.vpFrustum", "GgViewportPalette::frustum", &kViewport.frustum, false },
    { "color.viewport.audioRange", "NEW Palette.vpAudioRange", "GgViewportPalette::audioRange", &kViewport.audioRange, false },
    { "color.viewport.colliderWire", "NEW Palette.vpCollider", "GgViewportPalette::collider", &kViewport.collider, false },
    { "color.viewport.hoverOutline", "NEW Palette.vpHoverOutline（SelectionOutline の hoverColor を置き換え）", "GgViewportPalette::hoverOutline", &kViewport.hoverOutline, false },
    { "color.timeline.rulerTick", "NEW Palette.tlTick", "GgTimelinePalette::tick", &kTimeline.tick, false },
    { "color.timeline.rulerTickMajor", "NEW Palette.tlTickMajor", "GgTimelinePalette::tickMajor", &kTimeline.tickMajor, false },
    { "color.timeline.range", "NEW Palette.tlRange", "GgTimelinePalette::range", &kTimeline.range, false },
    { "color.timeline.outOfRange", "NEW Palette.tlOutOfRange", "GgTimelinePalette::outOfRange", &kTimeline.outOfRange, false },
    { "color.timeline.key", "NEW Palette.tlKey", "GgTimelinePalette::key", &kTimeline.key, false },
    { "color.timeline.keyEdge", "NEW Palette.tlKeyEdge", "GgTimelinePalette::keyEdge", &kTimeline.keyEdge, false },
    { "color.timeline.keySel", "NEW Palette.tlKeySel", "GgTimelinePalette::keySel", &kTimeline.keySel, false },
    { "color.play.frame", "NEW Palette.playFrame (= accent)", "GgChrome::playFrame", &kChrome.playFrame, false },
    { "color.play.frameKeyline", "NEW Palette.playKeyline", "GgChrome::playKeyline", &kChrome.playKeyline, false },
    { "color.play.editHatch", "NEW Palette.hatch", "GgChrome::hatch", &kChrome.hatch, false },
    { "color.overlay.scrimPalette", "NEW Palette.scrimPalette", "GgChrome::scrimPalette", &kChrome.scrimPalette, false },
    { "color.overlay.scrimModal", "NEW Palette.scrimModal", "GgChrome::scrimModal", &kChrome.scrimModal, false },
    { "color.overlay.navDim", "NEW Palette.scrimNav", "GgChrome::scrimNav", &kChrome.scrimNav, false },
    { "color.overlay.navHighlight", "NEW Palette.navHighlight", "GgChrome::navHighlight", &kChrome.navHighlight, false },
    { "color.overlay.popupInnerTop", "Deco.popupRim (alpha)", "GgChrome::popupInnerTop", &kChrome.popupInnerTop, false },
    { "color.overlay.shadowNear", "NEW Deco.shadowNear", "GgChrome::shadowNear", &kChrome.shadowNear, false },
    { "color.overlay.shadowFar", "NEW Deco.shadowFar", "GgChrome::shadowFar", &kChrome.shadowFar, false },
    { "color.gauge.tickMinor", "NEW Palette.tickMinor（= deco.scaleMinorAlpha）", "GgChrome::tickMinor", &kChrome.tickMinor, false },
    { "color.gauge.tickMajor", "NEW Palette.tickMajor（= deco.scaleMajorAlpha）", "GgChrome::tickMajor", &kChrome.tickMajor, false },
    { "color.gauge.vernierMinor", "NEW Palette.vernierMinor", "GgChrome::vernierMinor", &kChrome.vernierMinor, false },
    { "color.gauge.vernierMajor", "NEW Palette.vernierMajor", "GgChrome::vernierMajor", &kChrome.vernierMajor, false },
    { "color.selection.barFocused", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.selection.barUnfocused", "= NEW Palette.accentDim", "GgChrome::accentDim", &kChrome.accentDim, true },
    { "color.selection.hover", "= Palette.bg3", "Palette::bg3", &kGraphitePalette.bg3, true },
    { "color.status.goodText", "= Palette.good", "Palette::good", &kGraphitePalette.good, true },
    { "color.status.warnText", "= Palette.warn", "Palette::warn", &kGraphitePalette.warn, true },
    { "color.status.infoText", "= NEW Palette.info", "GgStatus::info", &kStatus.info, true },
    { "color.viewport.selTombo", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.viewport.selTomboDim", "= NEW Palette.accentDim", "GgChrome::accentDim", &kChrome.accentDim, true },
    { "color.viewport.dimLabelEdge", "= NEW Palette.playKeyline", "GgChrome::playKeyline", &kChrome.playKeyline, true },
    { "color.viewport.dimLabelText", "= Palette.text", "Palette::text", &kGraphitePalette.text, true },
    { "color.viewport.handleHot", "= NEW Palette.axisHot", "GgAxis::hot", &kAxis.hot, true },
    { "color.timeline.rulerBg", "= Palette.bg0", "Palette::bg0", &kGraphitePalette.bg0, true },
    { "color.timeline.rulerLabel", "= Palette.textDim", "Palette::textDim", &kGraphitePalette.textDim, true },
    { "color.timeline.trackBg", "= Palette.bg1", "Palette::bg1", &kGraphitePalette.bg1, true },
    { "color.timeline.trackAlt", "= Palette.tableRowAlt", "Palette::tableRowAlt", &kGraphitePalette.tableRowAlt, true },
    { "color.timeline.trackHeader", "= Palette.bg2", "Palette::bg2", &kGraphitePalette.bg2, true },
    { "color.timeline.trackSel", "= Palette.selection", "Palette::selection", &kGraphitePalette.selection, true },
    { "color.timeline.keyHover", "= Palette.textMid", "Palette::textMid", &kGraphitePalette.textMid, true },
    { "color.timeline.curveTangent", "= Palette.textDim", "Palette::textDim", &kGraphitePalette.textDim, true },
    { "color.timeline.event", "= Palette.typeScript", "Palette::typeScript", &kGraphitePalette.typeScript, true },
    { "color.timeline.playhead", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.timeline.playheadDim", "= NEW Palette.accentDim", "GgChrome::accentDim", &kChrome.accentDim, true },
    { "color.play.framePaused", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.play.tagBg", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.play.tagText", "= Palette.onAccent", "Palette::onAccent", &kGraphitePalette.onAccent, true },
    { "color.play.tagPausedBg", "= Palette.bg0", "Palette::bg0", &kGraphitePalette.bg0, true },
    { "color.play.tagPausedEdge", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.play.tagPausedText", "= Palette.text", "Palette::text", &kGraphitePalette.text, true },
    { "color.play.buttonFill", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.play.buttonIcon", "= Palette.onAccent", "Palette::onAccent", &kGraphitePalette.onAccent, true },
    { "color.play.statusBar", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
    { "color.overlay.toastLife", "= Palette.textDim", "Palette::textDim", &kGraphitePalette.textDim, true },
    { "color.overlay.focusRing", "= Palette.accent", "Palette::accent", &kGraphitePalette.accent, true },
};

// ---- コントラスト検査の表（final/contrast.md §1〜§3 と同じ組。文字 4.5:1 / UI 境界 3:1）----
// fgOver / bgOver が空でなければ「fg を fgOver の上に合成した色」。docRatio は contrast.md の比（2 桁）。forbidden は「禁止の組み合わせ」（比は基準未満で正しい）。
struct GgContrastPair { const ImVec4* fg; const ImVec4* fgOver; const ImVec4* bg; const ImVec4* bgOver; const char* fgName; const char* bgName;
                        float minRatio; float docRatio; bool text; bool forbidden; };
inline constexpr GgContrastPair kGgContrast[] = {
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.bg0, nullptr, "text.t1", "surface.bg0", 4.5f, 14.39f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.bg1, nullptr, "text.t1", "surface.bg1", 4.5f, 13.45f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.bg2, nullptr, "text.t1", "surface.bg2", 4.5f, 12.44f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.bg3, nullptr, "text.t1", "surface.bg3", 4.5f, 11.25f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.bg4, nullptr, "text.t1", "surface.bg4", 4.5f, 9.89f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.popupBg, nullptr, "text.t1", "surface.popup", 4.5f, 12.15f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.inputBg, nullptr, "text.t1", "surface.input", 4.5f, 14.14f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.inputBgHover, nullptr, "text.t1", "surface.inputHover", 4.5f, 13.75f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.inputBgActive, nullptr, "text.t1", "surface.inputActive", 4.5f, 14.62f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.selection, nullptr, "text.t1", "selection.sel", 4.5f, 9.93f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.selectionHover, nullptr, "text.t1", "selection.selHover", 4.5f, 9.36f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.selectionActive, nullptr, "text.t1", "selection.selActive", 4.5f, 8.66f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.bg0, nullptr, "text.t2", "surface.bg0", 4.5f, 11.39f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.bg1, nullptr, "text.t2", "surface.bg1", 4.5f, 10.65f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.bg2, nullptr, "text.t2", "surface.bg2", 4.5f, 9.85f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.bg3, nullptr, "text.t2", "surface.bg3", 4.5f, 8.91f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.bg4, nullptr, "text.t2", "surface.bg4", 4.5f, 7.83f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.popupBg, nullptr, "text.t2", "surface.popup", 4.5f, 9.62f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.inputBg, nullptr, "text.t2", "surface.input", 4.5f, 11.2f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.inputBgHover, nullptr, "text.t2", "surface.inputHover", 4.5f, 10.89f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.inputBgActive, nullptr, "text.t2", "surface.inputActive", 4.5f, 11.58f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.selection, nullptr, "text.t2", "selection.sel", 4.5f, 7.87f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.selectionHover, nullptr, "text.t2", "selection.selHover", 4.5f, 7.41f, true, false },
    { &kGraphitePalette.textMid, nullptr, &kGraphitePalette.selectionActive, nullptr, "text.t2", "selection.selActive", 4.5f, 6.86f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.bg0, nullptr, "text.t3", "surface.bg0", 4.5f, 8.52f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.bg1, nullptr, "text.t3", "surface.bg1", 4.5f, 7.96f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.bg2, nullptr, "text.t3", "surface.bg2", 4.5f, 7.37f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.bg3, nullptr, "text.t3", "surface.bg3", 4.5f, 6.66f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.bg4, nullptr, "text.t3", "surface.bg4", 4.5f, 5.86f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.popupBg, nullptr, "text.t3", "surface.popup", 4.5f, 7.2f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.inputBg, nullptr, "text.t3", "surface.input", 4.5f, 8.37f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.inputBgHover, nullptr, "text.t3", "surface.inputHover", 4.5f, 8.14f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.inputBgActive, nullptr, "text.t3", "surface.inputActive", 4.5f, 8.66f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.selection, nullptr, "text.t3", "selection.sel", 4.5f, 5.88f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.selectionHover, nullptr, "text.t3", "selection.selHover", 4.5f, 5.54f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.selectionActive, nullptr, "text.t3", "selection.selActive", 4.5f, 5.13f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.bg0, nullptr, "text.t4", "surface.bg0", 4.5f, 6.97f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.bg1, nullptr, "text.t4", "surface.bg1", 4.5f, 6.52f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.bg2, nullptr, "text.t4", "surface.bg2", 4.5f, 6.03f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.bg3, nullptr, "text.t4", "surface.bg3", 4.5f, 5.45f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.bg4, nullptr, "text.t4", "surface.bg4", 4.5f, 4.79f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.popupBg, nullptr, "text.t4", "surface.popup", 4.5f, 5.89f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.inputBg, nullptr, "text.t4", "surface.input", 4.5f, 6.85f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.inputBgHover, nullptr, "text.t4", "surface.inputHover", 4.5f, 6.66f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.inputBgActive, nullptr, "text.t4", "surface.inputActive", 4.5f, 7.09f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.selection, nullptr, "text.t4", "selection.sel", 4.5f, 4.81f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.selectionHover, nullptr, "text.t4", "selection.selHover", 4.5f, 4.54f, true, false },
    { &kGraphitePalette.textFaint, nullptr, &kGraphitePalette.selectionActive, nullptr, "text.t4", "selection.selActive", 4.5f, 4.2f, true, true },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg0, nullptr, "status.goodText", "surface.bg0", 4.5f, 8.43f, true, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg1, nullptr, "status.goodText", "surface.bg1", 4.5f, 7.88f, true, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg2, nullptr, "status.goodText", "surface.bg2", 4.5f, 7.29f, true, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg3, nullptr, "status.goodText", "surface.bg3", 4.5f, 6.59f, true, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg4, nullptr, "status.goodText", "surface.bg4", 4.5f, 5.8f, true, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.popupBg, nullptr, "status.goodText", "surface.popup", 4.5f, 7.12f, true, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.selection, nullptr, "status.goodText", "selection.sel", 4.5f, 5.82f, true, false },
    { &kGraphitePalette.good, nullptr, &kStatus.warnBg, &kGraphitePalette.bg1, "status.goodText", "status.warnBg ⊕ surface.bg1", 4.5f, 7.3f, true, false },
    { &kGraphitePalette.good, nullptr, &kStatus.badBg, &kGraphitePalette.bg1, "status.goodText", "status.badBg ⊕ surface.bg1", 4.5f, 7.4f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg0, nullptr, "status.warnText", "surface.bg0", 4.5f, 7.68f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg1, nullptr, "status.warnText", "surface.bg1", 4.5f, 7.18f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg2, nullptr, "status.warnText", "surface.bg2", 4.5f, 6.64f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg3, nullptr, "status.warnText", "surface.bg3", 4.5f, 6.01f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg4, nullptr, "status.warnText", "surface.bg4", 4.5f, 5.28f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.popupBg, nullptr, "status.warnText", "surface.popup", 4.5f, 6.49f, true, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.selection, nullptr, "status.warnText", "selection.sel", 4.5f, 5.3f, true, false },
    { &kGraphitePalette.warn, nullptr, &kStatus.warnBg, &kGraphitePalette.bg1, "status.warnText", "status.warnBg ⊕ surface.bg1", 4.5f, 6.65f, true, false },
    { &kGraphitePalette.warn, nullptr, &kStatus.badBg, &kGraphitePalette.bg1, "status.warnText", "status.badBg ⊕ surface.bg1", 4.5f, 6.74f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.bg0, nullptr, "status.badText", "surface.bg0", 4.5f, 6.65f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.bg1, nullptr, "status.badText", "surface.bg1", 4.5f, 6.22f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.bg2, nullptr, "status.badText", "surface.bg2", 4.5f, 5.75f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.bg3, nullptr, "status.badText", "surface.bg3", 4.5f, 5.2f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.bg4, nullptr, "status.badText", "surface.bg4", 4.5f, 4.57f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.popupBg, nullptr, "status.badText", "surface.popup", 4.5f, 5.62f, true, false },
    { &kStatus.badText, nullptr, &kGraphitePalette.selection, nullptr, "status.badText", "selection.sel", 4.5f, 4.59f, true, false },
    { &kStatus.badText, nullptr, &kStatus.warnBg, &kGraphitePalette.bg1, "status.badText", "status.warnBg ⊕ surface.bg1", 4.5f, 5.76f, true, false },
    { &kStatus.badText, nullptr, &kStatus.badBg, &kGraphitePalette.bg1, "status.badText", "status.badBg ⊕ surface.bg1", 4.5f, 5.84f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg0, nullptr, "status.infoText", "surface.bg0", 4.5f, 8.28f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg1, nullptr, "status.infoText", "surface.bg1", 4.5f, 7.74f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg2, nullptr, "status.infoText", "surface.bg2", 4.5f, 7.16f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg3, nullptr, "status.infoText", "surface.bg3", 4.5f, 6.47f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg4, nullptr, "status.infoText", "surface.bg4", 4.5f, 5.69f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.popupBg, nullptr, "status.infoText", "surface.popup", 4.5f, 6.99f, true, false },
    { &kStatus.info, nullptr, &kGraphitePalette.selection, nullptr, "status.infoText", "selection.sel", 4.5f, 5.71f, true, false },
    { &kStatus.info, nullptr, &kStatus.warnBg, &kGraphitePalette.bg1, "status.infoText", "status.warnBg ⊕ surface.bg1", 4.5f, 7.17f, true, false },
    { &kStatus.info, nullptr, &kStatus.badBg, &kGraphitePalette.bg1, "status.infoText", "status.badBg ⊕ surface.bg1", 4.5f, 7.26f, true, false },
    { &kChrome.link, nullptr, &kGraphitePalette.bg1, nullptr, "text.link", "surface.bg1", 4.5f, 9.8f, true, false },
    { &kChrome.link, nullptr, &kGraphitePalette.bg2, nullptr, "text.link", "surface.bg2", 4.5f, 9.06f, true, false },
    { &kChrome.link, nullptr, &kGraphitePalette.popupBg, nullptr, "text.link", "surface.popup", 4.5f, 8.85f, true, false },
    { &kKind.prefabText, nullptr, &kGraphitePalette.bg1, nullptr, "kind.prefabText", "surface.bg1", 4.5f, 9.8f, true, false },
    { &kKind.prefabText, nullptr, &kGraphitePalette.selection, nullptr, "kind.prefabText", "selection.sel", 4.5f, 7.24f, true, false },
    { &kKind.prefabText, nullptr, &kGraphitePalette.selectionHover, nullptr, "kind.prefabText", "selection.selHover", 4.5f, 6.82f, true, false },
    { &kKind.prefabText, nullptr, &kGraphitePalette.selectionActive, nullptr, "kind.prefabText", "selection.selActive", 4.5f, 6.31f, true, false },
    { &kAxis.x, nullptr, &kGraphitePalette.inputBg, nullptr, "axis.x", "surface.input", 4.5f, 5.11f, true, false },
    { &kAxis.x, nullptr, &kGraphitePalette.inputBgActive, nullptr, "axis.x", "surface.inputActive", 4.5f, 5.29f, true, false },
    { &kAxis.x, nullptr, &kGraphitePalette.bg1, nullptr, "axis.x", "surface.bg1", 4.5f, 4.86f, true, false },
    { &kAxis.y, nullptr, &kGraphitePalette.inputBg, nullptr, "axis.y", "surface.input", 4.5f, 11.62f, true, false },
    { &kAxis.y, nullptr, &kGraphitePalette.inputBgActive, nullptr, "axis.y", "surface.inputActive", 4.5f, 12.02f, true, false },
    { &kAxis.y, nullptr, &kGraphitePalette.bg1, nullptr, "axis.y", "surface.bg1", 4.5f, 11.05f, true, false },
    { &kAxis.z, nullptr, &kGraphitePalette.inputBg, nullptr, "axis.z", "surface.input", 4.5f, 6.24f, true, false },
    { &kAxis.z, nullptr, &kGraphitePalette.inputBgActive, nullptr, "axis.z", "surface.inputActive", 4.5f, 6.45f, true, false },
    { &kAxis.z, nullptr, &kGraphitePalette.bg1, nullptr, "axis.z", "surface.bg1", 4.5f, 5.93f, true, false },
    { &kGraphitePalette.onAccent, nullptr, &kGraphitePalette.accent, nullptr, "accent.onAccent", "accent.accent", 4.5f, 10.63f, true, false },
    { &kGraphitePalette.onAccent, nullptr, &kGraphitePalette.accentHover, nullptr, "accent.onAccent", "accent.accentHover", 4.5f, 12.2f, true, false },
    { &kGraphitePalette.onAccent, nullptr, &kGraphitePalette.accentPressed, nullptr, "accent.onAccent", "accent.accentPressed", 4.5f, 8.03f, true, false },
    { &kGraphitePalette.text, nullptr, &kGraphitePalette.bg0, nullptr, "play.tagPausedText", "play.tagPausedBg", 4.5f, 14.39f, true, false },
    { &kGraphitePalette.text, nullptr, &kViewport.labelBg, &kViewport.bgKey, "viewport.dimLabelText", "viewport.dimLabelBg ⊕ viewport.bgKey", 4.5f, 12.49f, true, false },
    { &kGraphitePalette.text, nullptr, &kViewport.labelBg, &kViewport.bgMid, "viewport.dimLabelText", "viewport.dimLabelBg ⊕ viewport.bgMid", 4.5f, 13.25f, true, false },
    { &kGraphitePalette.text, nullptr, &kViewport.labelBg, &kViewport.bgLow, "viewport.dimLabelText", "viewport.dimLabelBg ⊕ viewport.bgLow", 4.5f, 13.8f, true, false },
    { &kGraphitePalette.text, nullptr, &kViewport.labelBg, &kViewport.bgEdge, "viewport.dimLabelText", "viewport.dimLabelBg ⊕ viewport.bgEdge", 4.5f, 14.12f, true, false },
    { &kGraphitePalette.text, nullptr, &kViewport.labelBg, &kViewport.floorNear, "viewport.dimLabelText", "viewport.dimLabelBg ⊕ viewport.floorNear", 4.5f, 13.39f, true, false },
    { &kGraphitePalette.text, nullptr, &kViewport.labelBg, &kViewport.floorFar, "viewport.dimLabelText", "viewport.dimLabelBg ⊕ viewport.floorFar", 4.5f, 14.29f, true, false },
    { &kGraphitePalette.textDim, nullptr, &kGraphitePalette.bg0, nullptr, "timeline.rulerLabel", "timeline.rulerBg", 4.5f, 8.52f, true, false },
    { &kGraphitePalette.text, nullptr, &kChrome.scrimPalette, &kGraphitePalette.popupBg, "text.t1", "overlay.scrimPalette ⊕ surface.popup", 4.5f, 13.86f, true, false },
    { &kChrome.inLine, nullptr, &kGraphitePalette.inputBg, nullptr, "surface.inLine", "surface.input", 3.0f, 3.55f, false, false },
    { &kChrome.inLine, nullptr, &kGraphitePalette.bg1, nullptr, "surface.inLine", "surface.bg1", 3.0f, 3.37f, false, false },
    { &kChrome.inLine, nullptr, &kGraphitePalette.bg2, nullptr, "surface.inLine", "surface.bg2", 3.0f, 3.12f, false, false },
    { &kChrome.inLineHover, nullptr, &kGraphitePalette.inputBg, nullptr, "surface.inLineHover", "surface.input", 3.0f, 4.98f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.inputBg, nullptr, "accent.accent", "surface.input", 3.0f, 10.86f, false, false },
    { &kChrome.controlEdge, nullptr, &kGraphitePalette.bg1, nullptr, "surface.controlEdge", "surface.bg1", 3.0f, 3.68f, false, false },
    { &kChrome.controlEdge, nullptr, &kGraphitePalette.bg2, nullptr, "surface.controlEdge", "surface.bg2", 3.0f, 3.4f, false, false },
    { &kChrome.controlEdge, nullptr, &kGraphitePalette.bg3, nullptr, "surface.controlEdge", "surface.bg3", 3.0f, 3.08f, false, false },
    { &kChrome.controlEdge, nullptr, &kGraphitePalette.popupBg, nullptr, "surface.controlEdge", "surface.popup", 3.0f, 3.32f, false, false },
    { &kChrome.primaryBtnEdge, nullptr, &kGraphitePalette.bg1, nullptr, "surface.primaryBtnEdge", "surface.bg1", 3.0f, 4.74f, false, false },
    { &kChrome.primaryBtnEdge, nullptr, &kGraphitePalette.popupBg, nullptr, "surface.primaryBtnEdge", "surface.popup", 3.0f, 4.28f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.bg0, nullptr, "accent.accent", "surface.bg0", 3.0f, 11.05f, false, false },
    { &kChrome.accentDim, nullptr, &kGraphitePalette.bg0, nullptr, "accent.accentDim", "surface.bg0", 3.0f, 4.99f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.bg1, nullptr, "accent.accent", "surface.bg1", 3.0f, 10.33f, false, false },
    { &kChrome.accentDim, nullptr, &kGraphitePalette.bg1, nullptr, "accent.accentDim", "surface.bg1", 3.0f, 4.67f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.bg2, nullptr, "accent.accent", "surface.bg2", 3.0f, 9.56f, false, false },
    { &kChrome.accentDim, nullptr, &kGraphitePalette.bg2, nullptr, "accent.accentDim", "surface.bg2", 3.0f, 4.32f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.selection, nullptr, "accent.accent", "selection.sel", 3.0f, 7.63f, false, false },
    { &kChrome.accentDim, nullptr, &kGraphitePalette.selection, nullptr, "accent.accentDim", "selection.sel", 3.0f, 3.45f, false, false },
    { &kChrome.selBarSecondary, nullptr, &kGraphitePalette.selection, nullptr, "selection.barSecondary", "selection.sel", 3.0f, 3.5f, false, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg0, nullptr, "status.good", "surface.bg0", 3.0f, 8.43f, false, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg1, nullptr, "status.good", "surface.bg1", 3.0f, 7.88f, false, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg2, nullptr, "status.good", "surface.bg2", 3.0f, 7.29f, false, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg3, nullptr, "status.good", "surface.bg3", 3.0f, 6.59f, false, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.bg4, nullptr, "status.good", "surface.bg4", 3.0f, 5.8f, false, false },
    { &kGraphitePalette.good, nullptr, &kGraphitePalette.popupBg, nullptr, "status.good", "surface.popup", 3.0f, 7.12f, false, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg0, nullptr, "status.warn", "surface.bg0", 3.0f, 7.68f, false, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg1, nullptr, "status.warn", "surface.bg1", 3.0f, 7.18f, false, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg2, nullptr, "status.warn", "surface.bg2", 3.0f, 6.64f, false, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg3, nullptr, "status.warn", "surface.bg3", 3.0f, 6.01f, false, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.bg4, nullptr, "status.warn", "surface.bg4", 3.0f, 5.28f, false, false },
    { &kGraphitePalette.warn, nullptr, &kGraphitePalette.popupBg, nullptr, "status.warn", "surface.popup", 3.0f, 6.49f, false, false },
    { &kGraphitePalette.bad, nullptr, &kGraphitePalette.bg0, nullptr, "status.bad", "surface.bg0", 3.0f, 5.44f, false, false },
    { &kGraphitePalette.bad, nullptr, &kGraphitePalette.bg1, nullptr, "status.bad", "surface.bg1", 3.0f, 5.09f, false, false },
    { &kGraphitePalette.bad, nullptr, &kGraphitePalette.bg2, nullptr, "status.bad", "surface.bg2", 3.0f, 4.7f, false, false },
    { &kGraphitePalette.bad, nullptr, &kGraphitePalette.bg3, nullptr, "status.bad", "surface.bg3", 3.0f, 4.25f, false, false },
    { &kGraphitePalette.bad, nullptr, &kGraphitePalette.bg4, nullptr, "status.bad", "surface.bg4", 3.0f, 3.74f, false, false },
    { &kGraphitePalette.bad, nullptr, &kGraphitePalette.popupBg, nullptr, "status.bad", "surface.popup", 3.0f, 4.6f, false, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg0, nullptr, "status.info", "surface.bg0", 3.0f, 8.28f, false, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg1, nullptr, "status.info", "surface.bg1", 3.0f, 7.74f, false, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg2, nullptr, "status.info", "surface.bg2", 3.0f, 7.16f, false, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg3, nullptr, "status.info", "surface.bg3", 3.0f, 6.47f, false, false },
    { &kStatus.info, nullptr, &kGraphitePalette.bg4, nullptr, "status.info", "surface.bg4", 3.0f, 5.69f, false, false },
    { &kStatus.info, nullptr, &kGraphitePalette.popupBg, nullptr, "status.info", "surface.popup", 3.0f, 6.99f, false, false },
    { &kAxis.x, nullptr, &kGraphitePalette.inputBg, nullptr, "axis.x", "surface.input", 3.0f, 5.11f, false, false },
    { &kAxis.x, nullptr, &kGraphitePalette.bg1, nullptr, "axis.x", "surface.bg1", 3.0f, 4.86f, false, false },
    { &kAxis.y, nullptr, &kGraphitePalette.inputBg, nullptr, "axis.y", "surface.input", 3.0f, 11.62f, false, false },
    { &kAxis.y, nullptr, &kGraphitePalette.bg1, nullptr, "axis.y", "surface.bg1", 3.0f, 11.05f, false, false },
    { &kAxis.z, nullptr, &kGraphitePalette.inputBg, nullptr, "axis.z", "surface.input", 3.0f, 6.24f, false, false },
    { &kAxis.z, nullptr, &kGraphitePalette.bg1, nullptr, "axis.z", "surface.bg1", 3.0f, 5.93f, false, false },
    { &kGraphitePalette.typeMesh, nullptr, &kGraphitePalette.bg1, nullptr, "kind.mesh", "surface.bg1", 3.0f, 8.34f, false, false },
    { &kGraphitePalette.typeMesh, nullptr, &kGraphitePalette.selection, nullptr, "kind.mesh", "selection.sel", 3.0f, 6.16f, false, false },
    { &kGraphitePalette.typeLight, nullptr, &kGraphitePalette.bg1, nullptr, "kind.light", "surface.bg1", 3.0f, 13.21f, false, false },
    { &kGraphitePalette.typeLight, nullptr, &kGraphitePalette.selection, nullptr, "kind.light", "selection.sel", 3.0f, 9.76f, false, false },
    { &kGraphitePalette.typeCamera, nullptr, &kGraphitePalette.bg1, nullptr, "kind.camera", "surface.bg1", 3.0f, 9.11f, false, false },
    { &kGraphitePalette.typeCamera, nullptr, &kGraphitePalette.selection, nullptr, "kind.camera", "selection.sel", 3.0f, 6.72f, false, false },
    { &kGraphitePalette.typeAudio, nullptr, &kGraphitePalette.bg1, nullptr, "kind.audio", "surface.bg1", 3.0f, 8.42f, false, false },
    { &kGraphitePalette.typeAudio, nullptr, &kGraphitePalette.selection, nullptr, "kind.audio", "selection.sel", 3.0f, 6.21f, false, false },
    { &kGraphitePalette.typeScript, nullptr, &kGraphitePalette.bg1, nullptr, "kind.script", "surface.bg1", 3.0f, 7.58f, false, false },
    { &kGraphitePalette.typeScript, nullptr, &kGraphitePalette.selection, nullptr, "kind.script", "selection.sel", 3.0f, 5.6f, false, false },
    { &kGraphitePalette.typePhysics, nullptr, &kGraphitePalette.bg1, nullptr, "kind.physics", "surface.bg1", 3.0f, 8.62f, false, false },
    { &kGraphitePalette.typePhysics, nullptr, &kGraphitePalette.selection, nullptr, "kind.physics", "selection.sel", 3.0f, 6.36f, false, false },
    { &kGraphitePalette.typeUi, nullptr, &kGraphitePalette.bg1, nullptr, "kind.ui", "surface.bg1", 3.0f, 7.2f, false, false },
    { &kGraphitePalette.typeUi, nullptr, &kGraphitePalette.selection, nullptr, "kind.ui", "selection.sel", 3.0f, 5.32f, false, false },
    { &kGraphitePalette.typePrefab, nullptr, &kGraphitePalette.bg1, nullptr, "kind.prefab", "surface.bg1", 3.0f, 7.25f, false, false },
    { &kGraphitePalette.typePrefab, nullptr, &kGraphitePalette.selection, nullptr, "kind.prefab", "selection.sel", 3.0f, 5.35f, false, false },
    { &kGraphitePalette.typeEmpty, nullptr, &kGraphitePalette.bg1, nullptr, "kind.empty", "surface.bg1", 3.0f, 5.83f, false, false },
    { &kGraphitePalette.typeEmpty, nullptr, &kGraphitePalette.selection, nullptr, "kind.empty", "selection.sel", 3.0f, 4.31f, false, false },
    { &kGraphitePalette.typeFolder, nullptr, &kGraphitePalette.bg1, nullptr, "kind.folder", "surface.bg1", 3.0f, 8.58f, false, false },
    { &kGraphitePalette.typeFolder, nullptr, &kGraphitePalette.selection, nullptr, "kind.folder", "selection.sel", 3.0f, 6.34f, false, false },
    { &kGraphitePalette.typeScene, nullptr, &kGraphitePalette.bg1, nullptr, "kind.scene", "surface.bg1", 3.0f, 7.62f, false, false },
    { &kGraphitePalette.typeScene, nullptr, &kGraphitePalette.selection, nullptr, "kind.scene", "selection.sel", 3.0f, 5.63f, false, false },
    { &kKind.texture, nullptr, &kGraphitePalette.bg1, nullptr, "kind.texture", "surface.bg1", 3.0f, 7.58f, false, false },
    { &kKind.texture, nullptr, &kGraphitePalette.selection, nullptr, "kind.texture", "selection.sel", 3.0f, 5.6f, false, false },
    { &kTimeline.key, nullptr, &kGraphitePalette.bg1, nullptr, "timeline.key", "timeline.trackBg", 3.0f, 4.74f, false, false },
    { &kTimeline.keySel, nullptr, &kGraphitePalette.bg1, nullptr, "timeline.keySel", "timeline.trackBg", 3.0f, 15.41f, false, false },
    { &kTimeline.keySel, nullptr, &kTimeline.key, nullptr, "timeline.keySel", "timeline.key", 3.0f, 3.25f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.bg0, nullptr, "timeline.playhead", "timeline.rulerBg", 3.0f, 11.05f, false, false },
    { &kGraphitePalette.accent, nullptr, &kGraphitePalette.bg1, nullptr, "timeline.playhead", "timeline.trackBg", 3.0f, 10.33f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.playKeyline, &kViewport.bgKey, "play.frame", "play.frameKeyline ⊕ viewport.bgKey", 3.0f, 10.1f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.playKeyline, &kViewport.bgMid, "play.frame", "play.frameKeyline ⊕ viewport.bgMid", 3.0f, 10.77f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.playKeyline, &kViewport.bgLow, "play.frame", "play.frameKeyline ⊕ viewport.bgLow", 3.0f, 11.23f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.playKeyline, &kViewport.bgEdge, "play.frame", "play.frameKeyline ⊕ viewport.bgEdge", 3.0f, 11.48f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.playKeyline, &kViewport.floorNear, "play.frame", "play.frameKeyline ⊕ viewport.floorNear", 3.0f, 10.89f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.playKeyline, &kViewport.floorFar, "play.frame", "play.frameKeyline ⊕ viewport.floorFar", 3.0f, 11.6f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.scrimPalette, &kGraphitePalette.bg1, "play.frame", "overlay.scrimPalette ⊕ surface.bg1", 3.0f, 11.21f, false, false },
    { &kChrome.playFrame, nullptr, &kChrome.scrimPalette, &kGraphitePalette.popupBg, "play.frame", "overlay.scrimPalette ⊕ surface.popup", 3.0f, 10.65f, false, false },
    { &kViewport.selOutline, &kViewport.bgKey, &kViewport.iconHalo, &kViewport.bgKey, "viewport.selOutline ⊕ viewport.bgKey", "viewport.iconHalo ⊕ viewport.bgKey", 3.0f, 8.53f, false, false },
    { &kViewport.icon, &kViewport.bgKey, &kViewport.iconHalo, &kViewport.bgKey, "viewport.iconFill ⊕ viewport.bgKey", "viewport.iconHalo ⊕ viewport.bgKey", 3.0f, 8.05f, false, false },
    { &kViewport.selOutline, &kViewport.bgMid, &kViewport.iconHalo, &kViewport.bgMid, "viewport.selOutline ⊕ viewport.bgMid", "viewport.iconHalo ⊕ viewport.bgMid", 3.0f, 10.32f, false, false },
    { &kViewport.icon, &kViewport.bgMid, &kViewport.iconHalo, &kViewport.bgMid, "viewport.iconFill ⊕ viewport.bgMid", "viewport.iconHalo ⊕ viewport.bgMid", 3.0f, 10.02f, false, false },
    { &kViewport.selOutline, &kViewport.bgLow, &kViewport.iconHalo, &kViewport.bgLow, "viewport.selOutline ⊕ viewport.bgLow", "viewport.iconHalo ⊕ viewport.bgLow", 3.0f, 11.59f, false, false },
    { &kViewport.icon, &kViewport.bgLow, &kViewport.iconHalo, &kViewport.bgLow, "viewport.iconFill ⊕ viewport.bgLow", "viewport.iconHalo ⊕ viewport.bgLow", 3.0f, 11.51f, false, false },
    { &kViewport.selOutline, &kViewport.bgEdge, &kViewport.iconHalo, &kViewport.bgEdge, "viewport.selOutline ⊕ viewport.bgEdge", "viewport.iconHalo ⊕ viewport.bgEdge", 3.0f, 12.19f, false, false },
    { &kViewport.icon, &kViewport.bgEdge, &kViewport.iconHalo, &kViewport.bgEdge, "viewport.iconFill ⊕ viewport.bgEdge", "viewport.iconHalo ⊕ viewport.bgEdge", 3.0f, 12.28f, false, false },
    { &kViewport.selOutline, &kViewport.floorNear, &kViewport.iconHalo, &kViewport.floorNear, "viewport.selOutline ⊕ viewport.floorNear", "viewport.iconHalo ⊕ viewport.floorNear", 3.0f, 10.65f, false, false },
    { &kViewport.icon, &kViewport.floorNear, &kViewport.iconHalo, &kViewport.floorNear, "viewport.iconFill ⊕ viewport.floorNear", "viewport.iconHalo ⊕ viewport.floorNear", 3.0f, 10.4f, false, false },
    { &kViewport.selOutline, &kViewport.floorFar, &kViewport.iconHalo, &kViewport.floorFar, "viewport.selOutline ⊕ viewport.floorFar", "viewport.iconHalo ⊕ viewport.floorFar", 3.0f, 12.44f, false, false },
    { &kViewport.icon, &kViewport.floorFar, &kViewport.iconHalo, &kViewport.floorFar, "viewport.iconFill ⊕ viewport.floorFar", "viewport.iconHalo ⊕ viewport.floorFar", 3.0f, 12.62f, false, false },
};

// ---- Metrics / Deco / Extra（ThemeVariants.h の既存の型へ）----
inline constexpr Metrics kMetrics = []() constexpr {
    Metrics m{};
    m.windowRounding = 0.0f;
    m.childRounding = 0.0f;
    m.frameRounding = 3.0f;
    m.grabRounding = 0.0f;
    m.popupRounding = 5.0f;
    m.tabRounding = 0.0f;
    m.scrollbarRounding = 2.0f;
    m.windowBorder = 1.0f;
    m.childBorder = 1.0f;
    m.popupBorder = 1.0f;
    m.frameBorder = 1.0f;
    m.tabBarBorder = 1.0f;
    m.tabOverline = 0.0f;
    m.dockSeparator = 1.0f;
    m.fontBase = 17.0f;   // typography.scale.body.imguiPx
    m.fontMono = 15.0f;   // typography.scale.mono.imguiPx
    return m;
}();

// ★RowStyle::NeedleBar（= 4）は ThemeVariants.h の列挙子に無い（重ヘッダの形は A2a の 1 回だけ）。旧型の rowStyle は Plain のままにして、
//   NeedleBar かどうかは GgDeco::rowNeedleBar で表す。
inline constexpr Deco kDeco = []() constexpr {
    Deco d{};
    d.glow = 0.0f;
    d.hoverTint = 0.0f;
    d.easeSec = 0.09f;
    d.shadowStrength = 0.55f;
    d.rimAlpha = 0.0f;
    d.popupRim = 0.05f;
    d.panelFocusEdge = false;
    d.panelTopLine = false;
    d.panelSheen = false;
    d.layeredShadow = true;
    d.monoIcons = false;
    d.statusTopLine = false;
    d.toolbarLine = false;
    d.rowStyle = RowStyle::Plain;
    d.headerStyle = HeaderStyle::Plain;   // deco.headerStyle == "Plain"（上で検査済み）
    d.backdropTop = C(0x000000, 0);
    d.backdropBottom = C(0x000000, 0);
    return d;
}();
// deco.popupRim（tokens.deco）。color.overlay.popupInnerTop の alpha との一致は tests/gg_tokens_gen_test.cpp が見る。
static_assert(kDeco.popupRim > 0.0f);
inline constexpr Extra kExtra = []() constexpr {
    Extra x{};
    x.needleDim = kChrome.accentDim;
    x.inLine = kChrome.inLine;
    x.info = kStatus.info;
    x.warnText = kGraphitePalette.warn;   // status.warnText = Palette.warn（別名）
    x.badText = kStatus.badText;
    x.axisX = kAxis.x; x.axisY = kAxis.y; x.axisZ = kAxis.z;
    return x;
}();

// 組み立て済みの Spec の部品（ThemeGraphite.cpp が Spec と gg::GgSpec をまとめる）。gg::kGgSpec の deco / layout / budget は
// 既定値の初期化子（GgDeco{} など）が tokens の値を持つ。

} // namespace dx12e::theme::gg
