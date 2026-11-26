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
    d.headerStyle = HeaderStyle::Plain;
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
