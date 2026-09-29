#pragma once

// ===== エディタのアイデンティティ候補（テーマ・バリアント）=====
// Uno Engine のエディタに「UE5 のコピーではない独自の見た目」を与えるための候補 3 案を、
// トークン表として並べたもの。実際の適用は EditorTheme.h（theme::SetVariant → ApplyStyle）と
// UiWidgets.cpp の deco::*（グロー / ヘアライン / シグナルバー等の共通装飾ヘルパ）が行う。
//
//   Default … 現行（第 1〜3 波のまま。見た目は 1px も変わらない）
//   A       … ネオン・エッジ    : 藍寄りの暗い面 + 細いネオンのエッジライト（フォーカス / 選択 / アクティブ）
//   B       … グラス・レイヤー  : 階調を強めた半透明ガラス風の面 + 淡い光の縁 + 大きめの角丸 + 浮遊するポップアップ
//   C       … インク・アンド・シグナル : ほぼモノトーン + シグナルカラー 1 色（ライム）だけを状態表示に使う編集ツール風
//
// ★ここは純データ（imgui.h の ImVec4 だけに依存）。描画ロジックは持たない。
//   案を 1 つに決めたら、その案の表を Default へ移して他を消すだけで済む（呼び出し側は Variant を意識しない）。
// ★色の値を増やす時は Palette に足し、4 つの表（Default / A / B / C）と theme::SetVariant に配線する。
//   単体テスト（tests/theme_variant_test.cpp）が「Default が旧値と一致」「各案の文字コントラストが AA」を検査する。

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <cstring>
#include <string>

namespace dx12e::theme
{

// 0xRRGGBB → ImVec4（sRGB のまま。ImGui の頂点カラーに合わせてガンマ変換はしない）
inline ImVec4 Hex(unsigned int rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8)  & 0xFF) / 255.0f,
                  ( rgb        & 0xFF) / 255.0f,
                  a);
}

enum class Variant : int
{
    Default = 0,   // 現行
    A       = 1,   // ネオン・エッジ
    B       = 2,   // グラス・レイヤー
    C       = 3,   // インク・アンド・シグナル
};
inline constexpr int kVariantCount = 4;

inline const char* VariantName(Variant v)
{
    switch (v)
    {
    case Variant::A: return "A: ネオン・エッジ";
    case Variant::B: return "B: グラス・レイヤー";
    case Variant::C: return "C: インク・アンド・シグナル";
    default:         return "現行";
    }
}

// "a" / "A" / "b" / "c" / "default" / "0".."3"。解釈できなければ false（out は変えない）。
inline bool ParseVariant(const std::string& s, Variant& out)
{
    if (s.size() == 1)
    {
        switch (s[0])
        {
        case 'a': case 'A': case '1': out = Variant::A;       return true;
        case 'b': case 'B': case '2': out = Variant::B;       return true;
        case 'c': case 'C': case '3': out = Variant::C;       return true;
        case '0':                     out = Variant::Default; return true;
        default: return false;
        }
    }
    if (s == "default" || s == "Default" || s == "classic") { out = Variant::Default; return true; }
    return false;
}

// ---- 色トークン一式（案ごとに 1 つ）----
struct Palette
{
    // 面（深 → 浅）
    ImVec4 bg0, bg1, bg2, bg3, bg4;
    ImVec4 windowBg, popupBg;                       // 既定は bg1 / bg2 と同値（B は半透明）
    ImVec4 inputBg, inputBgHover, inputBgActive;
    ImVec4 inputBorder, inputBorderHover;
    ImVec4 border, borderStrong;
    ImVec4 separator;                               // ドック分割バー / セパレータの色（既定は border と同値。B は隙間に見せるため背景色）
    // アクセント（案ごとに色相が違う。選択・フォーカス・アクティブだけに使う）
    ImVec4 accent, accentHover, accentPressed;
    ImVec4 onAccent;                                // アクセント塗りの上の文字色
    ImVec4 selection, selectionHover, selectionActive;
    ImVec4 tabOverline, tabOverlineDim;
    // 文字
    ImVec4 text, textMid, textDim, textFaint;
    // 状態色
    ImVec4 good, warn, bad;
    // 種別色（アイコングリフの tint）
    ImVec4 typeMesh, typeLight, typeCamera, typeAudio, typeScript, typePhysics, typeUi,
           typePrefab, typeEmpty, typeFolder, typeScene;
    // ImGuiStyle へ流す細かい色
    ImVec4 btnActive, scrollGrab, scrollGrabHover, scrollGrabActive;
    ImVec4 tableBorderLight, tableRowAlt, treeLines;
    ImVec4 textSelectedBg, dockingPreview, separatorHover, resizeGripHover;
};

// ---- 形状・線幅（案ごとに 1 つ。行高・余白は全案で共通＝レイアウトは案で動かない）----
struct Metrics
{
    float windowRounding, childRounding, frameRounding, grabRounding, popupRounding, tabRounding, scrollbarRounding;
    float windowBorder, childBorder, popupBorder, frameBorder;
    float tabBarBorder, tabOverline, dockSeparator;
};

// ---- 装飾の方針（描画ヘルパ deco::* が読む。案の「シグネチャ要素」の有無と強さ）----
enum class RowStyle    : int { Plain = 0, NeonBar = 1, GlassPill = 2, SignalBar = 3 };
enum class HeaderStyle : int { Plain = 0, GlowTick = 1, Glass = 2, SignalTick = 3 };

struct Deco
{
    float glow;            // 0..1: エッジのハロー（フォーカス/アクティブ）の強さ。0 = 無し
    float hoverTint;       // ホバー中の面に足すアクセントの alpha（0 = 従来のグレーのまま）
    float easeSec;         // hover/active の遷移時間（秒）。0 = 即時（現行）
    bool  panelFocusEdge;  // フォーカス中のパネルにエッジライトを引く（A）
    bool  panelTopLine;    // 全パネルの上端に光のヘアライン（B）
    bool  panelSheen;      // パネル面にごく淡いグラデ（B）
    bool  layeredShadow;   // ポップアップ/メニュー/パレットを多重半透明の影で浮かせる（A は軽く / B は厚く）
    float shadowStrength;  // 0..1
    float rimAlpha;        // 浮遊物の淡い光の縁（白の alpha。0 = 無し）
    RowStyle    rowStyle;
    HeaderStyle headerStyle;
    bool  monoIcons;       // アイコンを無彩色にする（C）。アクティブだけシグナル色
    bool  statusTopLine;   // ステータスバー上端の細い線
    bool  toolbarLine;     // ツールバー下端の光の線（A）
    ImVec4 backdropTop;    // ドック窓の背後に敷く光のグラデ（B。窓面が半透明なので透けて見える）。alpha 0 = 無し
    ImVec4 backdropBottom;
};

struct Spec
{
    Palette pal;
    Metrics m;
    Deco    deco;
};

namespace detail
{

inline Spec MakeDefault()
{
    // ★現行値そのもの（第 1 波の EditorTheme.h のトークン表 / ApplyStyle の直書き値）。ここを変えると既定の見た目が変わる。
    Spec s{};
    Palette& p = s.pal;
    p.bg0 = Hex(0x0E0E10);  p.bg1 = Hex(0x171719);  p.bg2 = Hex(0x1F1F23);  p.bg3 = Hex(0x2A2A2F);  p.bg4 = Hex(0x34343B);
    p.windowBg = p.bg1;     p.popupBg = p.bg2;
    p.inputBg = Hex(0x0C0C0E); p.inputBgHover = Hex(0x111114); p.inputBgActive = Hex(0x141418);
    p.inputBorder = Hex(0x2E2E34); p.inputBorderHover = Hex(0x46464E);
    p.border = Hex(0x2A2A2F);  p.borderStrong = Hex(0x3A3A42);  p.separator = p.border;
    p.accent = Hex(0x2F8CFF);  p.accentHover = Hex(0x57A3FF);  p.accentPressed = Hex(0x1F6FD6);
    p.onAccent = Hex(0xFFFFFF);
    p.selection = Hex(0x2F8CFF, 0.30f);  p.selectionHover = Hex(0x2F8CFF, 0.16f);  p.selectionActive = Hex(0x2F8CFF, 0.42f);
    p.tabOverline = p.accent;  p.tabOverlineDim = Hex(0x2F8CFF, 0.40f);
    p.text = Hex(0xD6D6DB);  p.textMid = Hex(0xBEBEC7);  p.textDim = Hex(0x9C9CA6);  p.textFaint = Hex(0x90909A);
    p.good = Hex(0x4CBF7A);  p.warn = Hex(0xE5A03C);  p.bad = Hex(0xEE6A62);
    p.typeMesh = Hex(0xA9AFBC);   p.typeLight = Hex(0xE7B55A);  p.typeCamera = Hex(0x5FA8FF);
    p.typeAudio = Hex(0x55C4C4);  p.typeScript = Hex(0xA994F0); p.typePhysics = Hex(0x6DBE7A);
    p.typeUi = Hex(0xD97BBB);     p.typePrefab = Hex(0x6FA8FF); p.typeEmpty = Hex(0x8E919C);
    p.typeFolder = Hex(0xB4B6C0); p.typeScene = Hex(0xE58A55);
    p.btnActive = Hex(0x3E3E46);
    p.scrollGrab = Hex(0x36363D);  p.scrollGrabHover = Hex(0x4A4A53);  p.scrollGrabActive = Hex(0x5C5C67);
    p.tableBorderLight = Hex(0x222226);  p.tableRowAlt = Hex(0xFFFFFF, 0.02f);  p.treeLines = Hex(0xFFFFFF, 0.14f);
    p.textSelectedBg = Hex(0x2F8CFF, 0.35f);  p.dockingPreview = Hex(0x2F8CFF, 0.30f);
    p.separatorHover = Hex(0x2F8CFF, 0.60f);  p.resizeGripHover = Hex(0x2F8CFF, 0.40f);

    Metrics& m = s.m;
    m.windowRounding = 0.0f; m.childRounding = 2.0f; m.frameRounding = 2.0f; m.grabRounding = 2.0f;
    m.popupRounding = 4.0f;  m.tabRounding = 3.0f;   m.scrollbarRounding = 6.0f;
    m.windowBorder = 1.0f; m.childBorder = 1.0f; m.popupBorder = 1.0f; m.frameBorder = 1.0f;
    m.tabBarBorder = 1.0f; m.tabOverline = 2.0f; m.dockSeparator = 3.0f;

    Deco& d = s.deco;
    d.glow = 0.0f; d.hoverTint = 0.0f; d.easeSec = 0.0f;
    d.panelFocusEdge = false; d.panelTopLine = false; d.panelSheen = false;
    d.layeredShadow = false;  d.shadowStrength = 0.0f; d.rimAlpha = 0.0f;
    d.rowStyle = RowStyle::Plain; d.headerStyle = HeaderStyle::Plain;
    d.monoIcons = false; d.statusTopLine = false; d.toolbarLine = false;
    d.backdropTop = ImVec4(0, 0, 0, 0); d.backdropBottom = ImVec4(0, 0, 0, 0);
    return s;
}

// ------------------------------------------------------------------
// 案 A「ネオン・エッジ」: 現行の延長。藍寄りの暗い面（青みのあるニュートラル）に、
// フォーカス / 選択 / アクティブだけ細いネオンのエッジライトを走らせる。ホバーは微発光。
// ------------------------------------------------------------------
inline Spec MakeA()
{
    Spec s = MakeDefault();
    Palette& p = s.pal;
    p.bg0 = Hex(0x090A10);  p.bg1 = Hex(0x11131B);  p.bg2 = Hex(0x181B26);  p.bg3 = Hex(0x232736);  p.bg4 = Hex(0x2D3247);
    p.windowBg = p.bg1;     p.popupBg = Hex(0x141721);
    p.inputBg = Hex(0x0A0B12); p.inputBgHover = Hex(0x0E1019); p.inputBgActive = Hex(0x0F121C);
    p.inputBorder = Hex(0x272C3F); p.inputBorderHover = Hex(0x414A69);
    p.border = Hex(0x1F2333);  p.borderStrong = Hex(0x39415E);  p.separator = p.border;
    p.accent = Hex(0x2F96FF);  p.accentHover = Hex(0x62B4FF);  p.accentPressed = Hex(0x1C7BE6);
    p.onAccent = Hex(0x06101F);   // ネオンの塗りの上は暗い紺（白だと 3:1。新案の塗り上の文字は 4.5:1 以上に揃える）
    p.selection = Hex(0x2F96FF, 0.22f);  p.selectionHover = Hex(0x2F96FF, 0.11f);  p.selectionActive = Hex(0x2F96FF, 0.34f);
    p.tabOverline = p.accentHover;  p.tabOverlineDim = Hex(0x2F96FF, 0.45f);
    p.text = Hex(0xDCDFEC);  p.textMid = Hex(0xC1C6D8);  p.textDim = Hex(0xA0A6BC);  p.textFaint = Hex(0x9299B2);
    p.good = Hex(0x4FD08A);  p.warn = Hex(0xF0B04A);  p.bad = Hex(0xFF7570);
    p.typeMesh = Hex(0xAEB6CC);   p.typeLight = Hex(0xFFC15E);  p.typeCamera = Hex(0x5CB0FF);
    p.typeAudio = Hex(0x50D8D2);  p.typeScript = Hex(0xB39CFF); p.typePhysics = Hex(0x70D48D);
    p.typeUi = Hex(0xF283CB);     p.typePrefab = Hex(0x70B3FF); p.typeEmpty = Hex(0x8F96AE);
    p.typeFolder = Hex(0xB9BFD4); p.typeScene = Hex(0xFF9C5E);
    p.btnActive = Hex(0x383E56);
    p.scrollGrab = Hex(0x2E3449);  p.scrollGrabHover = Hex(0x444C68);  p.scrollGrabActive = Hex(0x596382);
    p.tableBorderLight = Hex(0x1B1F2C);  p.tableRowAlt = Hex(0xB4C8FF, 0.022f);  p.treeLines = Hex(0xB4C8FF, 0.15f);
    p.textSelectedBg = Hex(0x2F96FF, 0.35f);  p.dockingPreview = Hex(0x2F96FF, 0.28f);
    p.separatorHover = Hex(0x62B4FF, 0.65f);  p.resizeGripHover = Hex(0x2F96FF, 0.45f);

    Metrics& m = s.m;
    m.childRounding = 3.0f; m.frameRounding = 3.0f; m.grabRounding = 3.0f;
    m.popupRounding = 6.0f; m.tabRounding = 4.0f;

    Deco& d = s.deco;
    d.glow = 1.0f;  d.hoverTint = 0.07f;  d.easeSec = 0.11f;
    d.panelFocusEdge = true;
    d.layeredShadow = true;  d.shadowStrength = 0.55f;  d.rimAlpha = 0.0f;
    d.rowStyle = RowStyle::NeonBar;  d.headerStyle = HeaderStyle::GlowTick;
    d.statusTopLine = true;  d.toolbarLine = true;
    return s;
}

// ------------------------------------------------------------------
// 案 B「グラス・レイヤー」: 面の階調を強め、半透明のガラス風パネル（上端に光のヘアライン・面にごく淡いグラデ）、
// 境界は淡い光の縁、角丸は大きめ。ポップアップ/メニュー/パレットは多重半透明の影で浮かせる（疑似ブラー）。
// ------------------------------------------------------------------
inline Spec MakeB()
{
    Spec s = MakeDefault();
    Palette& p = s.pal;
    p.bg0 = Hex(0x06070B);  p.bg1 = Hex(0x14161E);  p.bg2 = Hex(0x1D202B);  p.bg3 = Hex(0x292D3D);  p.bg4 = Hex(0x363B50);
    p.windowBg = Hex(0x14161E, 0.93f);  p.popupBg = Hex(0x1A1D29, 0.95f);
    p.inputBg = Hex(0x0C0E15); p.inputBgHover = Hex(0x10121B); p.inputBgActive = Hex(0x141721);
    p.inputBorder = Hex(0x2D3347); p.inputBorderHover = Hex(0x4D5878);
    p.border = Hex(0x262B3C);  p.borderStrong = Hex(0x424A69);  p.separator = p.bg0;   // パネル間は「隙間」（背景が見える）
    p.accent = Hex(0x7A96FF);  p.accentHover = Hex(0xA0B5FF);  p.accentPressed = Hex(0x5A78EE);
    p.onAccent = Hex(0x0A0F24);
    p.selection = Hex(0x7A96FF, 0.24f);  p.selectionHover = Hex(0x7A96FF, 0.12f);  p.selectionActive = Hex(0x7A96FF, 0.36f);
    p.tabOverline = Hex(0xFFFFFF, 0.30f);  p.tabOverlineDim = Hex(0xFFFFFF, 0.16f);
    p.text = Hex(0xDEE1EE);  p.textMid = Hex(0xC3C8DA);  p.textDim = Hex(0xA2A8BF);  p.textFaint = Hex(0x949BB4);
    p.good = Hex(0x54CB92);  p.warn = Hex(0xEDB55A);  p.bad = Hex(0xF57A7A);
    p.typeMesh = Hex(0xB2B9D0);   p.typeLight = Hex(0xF2C169);  p.typeCamera = Hex(0x7BADFF);
    p.typeAudio = Hex(0x62CFCF);  p.typeScript = Hex(0xB8A2FA); p.typePhysics = Hex(0x78C98F);
    p.typeUi = Hex(0xE58AC8);     p.typePrefab = Hex(0x86ADFF); p.typeEmpty = Hex(0x9299B2);
    p.typeFolder = Hex(0xBEC4D9); p.typeScene = Hex(0xF0A070);
    p.btnActive = Hex(0x424862);
    p.scrollGrab = Hex(0x333950);  p.scrollGrabHover = Hex(0x4A5270);  p.scrollGrabActive = Hex(0x606A8C);
    p.tableBorderLight = Hex(0x1E2230);  p.tableRowAlt = Hex(0xC8D2FF, 0.025f);  p.treeLines = Hex(0xC8D2FF, 0.14f);
    p.textSelectedBg = Hex(0x7A96FF, 0.38f);  p.dockingPreview = Hex(0x7A96FF, 0.30f);
    p.separatorHover = Hex(0xA0B5FF, 0.60f);  p.resizeGripHover = Hex(0x7A96FF, 0.45f);

    Metrics& m = s.m;
    m.windowRounding = 8.0f; m.childRounding = 6.0f; m.frameRounding = 5.0f; m.grabRounding = 5.0f;
    m.popupRounding = 10.0f; m.tabRounding = 6.0f;   m.scrollbarRounding = 8.0f;
    m.tabBarBorder = 0.0f;   m.tabOverline = 1.0f;   m.dockSeparator = 8.0f;

    Deco& d = s.deco;
    d.glow = 0.35f;  d.hoverTint = 0.05f;  d.easeSec = 0.16f;
    d.panelTopLine = true;   d.panelSheen = true;
    d.layeredShadow = true;  d.shadowStrength = 1.0f;  d.rimAlpha = 0.11f;
    d.rowStyle = RowStyle::GlassPill;  d.headerStyle = HeaderStyle::Glass;
    d.backdropTop = Hex(0xB0C4FF);  d.backdropBottom = Hex(0x6A3CB0);
    return s;
}

// ------------------------------------------------------------------
// 案 C「インク・アンド・シグナル」: ほぼ無彩色の落ち着いた階調に、シグナル 1 色（ライム）だけを状態表示に使う。
// 面・角・影は控えめ。アイコンも無彩色（アクティブだけシグナル）。装飾はシグナルバーのみ。
// ------------------------------------------------------------------
inline Spec MakeC()
{
    Spec s = MakeDefault();
    Palette& p = s.pal;
    p.bg0 = Hex(0x0A0A0A);  p.bg1 = Hex(0x131313);  p.bg2 = Hex(0x1B1B1B);  p.bg3 = Hex(0x252525);  p.bg4 = Hex(0x303030);
    p.windowBg = p.bg1;     p.popupBg = Hex(0x191919);
    p.inputBg = Hex(0x0D0D0D); p.inputBgHover = Hex(0x101010); p.inputBgActive = Hex(0x121212);
    p.inputBorder = Hex(0x2B2B2B); p.inputBorderHover = Hex(0x484848);
    p.border = Hex(0x242424);  p.borderStrong = Hex(0x3B3B3B);  p.separator = p.border;
    p.accent = Hex(0xC6F82C);  p.accentHover = Hex(0xDBFF66);  p.accentPressed = Hex(0xA3D416);
    p.onAccent = Hex(0x0A0A0A);
    p.selection = Hex(0xFFFFFF, 0.085f);  p.selectionHover = Hex(0xFFFFFF, 0.05f);  p.selectionActive = Hex(0xFFFFFF, 0.13f);
    p.tabOverline = p.accent;  p.tabOverlineDim = Hex(0xC6F82C, 0.40f);
    p.text = Hex(0xE6E6E6);  p.textMid = Hex(0xC6C6C6);  p.textDim = Hex(0xA0A0A0);  p.textFaint = Hex(0x8F8F8F);
    p.good = Hex(0xC6F82C);  p.warn = Hex(0xE7B04A);  p.bad = Hex(0xF2675F);
    // 種別色は無彩色（アイコンで種別を見分けるのは形。色は状態のためにとっておく）
    p.typeMesh = Hex(0xB4B4B4);   p.typeLight = Hex(0xC8C8C8);  p.typeCamera = Hex(0xB4B4B4);
    p.typeAudio = Hex(0xB4B4B4);  p.typeScript = Hex(0xB4B4B4); p.typePhysics = Hex(0xB4B4B4);
    p.typeUi = Hex(0xB4B4B4);     p.typePrefab = Hex(0xC8C8C8); p.typeEmpty = Hex(0x909090);
    p.typeFolder = Hex(0xB4B4B4); p.typeScene = Hex(0xC8C8C8);
    p.btnActive = Hex(0x3A3A3A);
    p.scrollGrab = Hex(0x333333);  p.scrollGrabHover = Hex(0x4A4A4A);  p.scrollGrabActive = Hex(0x666666);
    p.tableBorderLight = Hex(0x1F1F1F);  p.tableRowAlt = Hex(0xFFFFFF, 0.02f);  p.treeLines = Hex(0xFFFFFF, 0.13f);
    p.textSelectedBg = Hex(0xC6F82C, 0.30f);  p.dockingPreview = Hex(0xC6F82C, 0.22f);
    p.separatorHover = Hex(0xC6F82C, 0.55f);  p.resizeGripHover = Hex(0xC6F82C, 0.40f);

    Metrics& m = s.m;
    m.childRounding = 0.0f; m.frameRounding = 2.0f; m.grabRounding = 1.0f;
    m.popupRounding = 3.0f; m.tabRounding = 2.0f;   m.scrollbarRounding = 2.0f;

    Deco& d = s.deco;
    d.glow = 0.0f;  d.hoverTint = 0.0f;  d.easeSec = 0.06f;
    d.layeredShadow = true;  d.shadowStrength = 0.30f;  d.rimAlpha = 0.0f;
    d.rowStyle = RowStyle::SignalBar;  d.headerStyle = HeaderStyle::SignalTick;
    d.monoIcons = true;
    return s;
}

} // namespace detail

inline const Spec& SpecOf(Variant v)
{
    static const Spec kDefault = detail::MakeDefault();
    static const Spec kA = detail::MakeA();
    static const Spec kB = detail::MakeB();
    static const Spec kC = detail::MakeC();
    switch (v)
    {
    case Variant::A: return kA;
    case Variant::B: return kB;
    case Variant::C: return kC;
    default:         return kDefault;
    }
}

} // namespace dx12e::theme
