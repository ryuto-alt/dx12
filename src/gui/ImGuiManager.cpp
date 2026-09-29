#include "gui/ImGuiManager.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/GameUiFont.h"
#include "core/DpiScale.h"
#include "gui/ImGuizmo.h"
#include <vector>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <cstring>
#include "core/vfs/Vfs.h"
#include "editor/EditorTheme.h"
#include "gui/VirtualInputImGui.h"
#include "gui/FloatingGuard.h"

#include <filesystem>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>   // g.Windows（別ビューポートの警告に窓名を出す）
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#pragma warning(pop)

namespace dx12e
{

namespace
{
// 仮想入力モード中は「メインウィンドウは最小化されていない」と ImGui に答える。
//   --background=minimized ではウィンドウが本当に最小化されているが、ImGui は最小化された
//   ビューポートの位置/サイズ更新や描画を省く。論理解像度で動かし続けたいので偽る。
bool (*g_origGetWindowMinimized)(ImGuiViewport*) = nullptr;
bool GetWindowMinimizedOverride(ImGuiViewport* vp)
{
    if (vinput::Enabled() && vp == ImGui::GetMainViewport()) return false;
    return g_origGetWindowMinimized ? g_origGetWindowMinimized(vp) : false;
}

// ---- multi-viewport（フローティング窓を別 OS 窓へ引き出す機能）の可否 ----
// ★仮想入力 / --background の間は必ず切る。imgui.ini の ViewportPos が残っていると、フローティング窓が
//   メインウィンドウの外（＝ユーザーの実画面）へ別 OS 窓として出てしまう事故があった
//   （マテリアルエディタが (60,60) に出た）。切っておけば全窓がメインビューポート内に留まる。
bool g_viewportsWanted = false;   // 通常起動で multi-viewport を有効にしたか（仮想入力を切ったら戻す）
bool g_viewportsForbidden = false; // --background: multi-viewport を二度と有効にしない
bool g_editorStyle     = false;   // エディタのテーマを適用したか（ゲームモードでは false）

// ---- DPI（表示倍率）----
//   ・g_mainScale   : メインビューポートの UI 倍率（OS の倍率 or --dpi-scale）。ini の保存にも使う
//   ・g_appliedScale: 今 ImGuiStyle / theme::Scale() に反映している倍率（ビューポートごとに切り替わり得る）
HWND  g_mainHwnd      = nullptr;
float g_mainScale     = 1.0f;
float g_appliedScale  = 0.0f;
float g_iniScale      = 0.0f;   // imgui.ini に書かれていた倍率（0 = 記録なし = 旧版の ini = 100% 基準）
float g_initialScale  = 1.0f;   // ini を読む時点のメイン倍率
float (*g_origGetWindowDpiScale)(ImGuiViewport*) = nullptr;

float ScaleForHwnd(HWND h)
{
    UINT dpiVal = 96;
    if (h)
    {
        using Fn = UINT(WINAPI*)(HWND);
        static Fn fn = [] { HMODULE u = GetModuleHandleW(L"user32.dll");
                            return u ? reinterpret_cast<Fn>(GetProcAddress(u, "GetDpiForWindow")) : nullptr; }();
        if (fn) { const UINT d = fn(h); if (d) dpiVal = d; }
    }
    return dx12e::dpi::DpiToScale(dpiVal);
}

// メインビューポートの倍率。ゲームは 1.0 固定（旧来の見た目・ゲーム内 UI の論理解像度を守る）。
float ComputeMainScale()
{
    if (!g_editorStyle) return 1.0f;
    return dx12e::dpi::EffectiveScale(ScaleForHwnd(g_mainHwnd));
}

// 倍率を ImGuiStyle と theme::Scale() へ反映する。基準スタイル（100%）から毎回作り直す（累積しない・往復で戻る）。
void ApplyUiScale(float scale)
{
    if (!g_editorStyle || !theme::BaseStyleValid()) return;
    if (scale <= 0.0f) scale = 1.0f;
    if (scale == g_appliedScale) return;
    g_appliedScale = scale;
    ImGui::GetStyle() = theme::ScaleStyleFrom(theme::BaseStyle(), scale);
    theme::SetScale(scale);
    ImGuizmo::SetScreenScale(scale);
    // マウスのドラッグ/ダブルクリックの許容距離（物理 px）も倍率に比例させる（200% で 6px だと手ぶれでドラッグ扱いになる）
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDragThreshold = 6.0f * scale;
    io.MouseDoubleClickMaxDist = 6.0f * scale;
}

// ビューポートごとの DPI（multi-viewport で別モニターへ引き出した窓）。--dpi-scale があれば全ビューポートがそれ。
float GetWindowDpiScaleHook(ImGuiViewport* vp)
{
    if (!g_editorStyle) return 1.0f;
    if (dx12e::dpi::HasOverride()) return dx12e::dpi::Override();
    HWND h = vp ? static_cast<HWND>(vp->PlatformHandleRaw ? vp->PlatformHandleRaw : vp->PlatformHandle) : nullptr;
    if (!h) h = g_mainHwnd;
    return dx12e::dpi::ClampScale(ScaleForHwnd(h));
}

// Begin() が窓のビューポートを切り替えるたびに呼ばれる（imgui 1.92 の Platform_OnChangedViewport）。
// そのビューポートの倍率でスタイルを切り替える＝各窓が自分のモニターの倍率で描かれる。
// 全ビューポートが同じ倍率なら ApplyUiScale は何もしない（コストゼロ・PushStyleVar の巻き戻しも起きない）。
void OnChangedViewportHook(ImGuiViewport* vp)
{
    if (!g_editorStyle || !vp) return;
    const float s = (vp == ImGui::GetMainViewport()) ? g_mainScale
                  : (vp->DpiScale > 0.0f ? vp->DpiScale : g_mainScale);
    ApplyUiScale(s);
}

// ドックのノード（分割の大きさ）を f 倍する。ImGui は窓の Pos/Size は倍率変更で拡縮する（ConfigDpiScaleViewports）が、
// ドックの分割は「ピクセル固定」のまま残す＝倍率が 1.5 倍になっても左右のパネル幅は物理 px のまま（論理では 2/3 に縮む）。
// 論理サイズを保つため、倍率が変わった時 / 倍率違いの ini を読んだ時に SizeRef と Size を同じ比で拡縮する。
int ScaleDockNodes(ImGuiContext* ctx, float f)
{
    int n = 0;
    ImGuiStorage& nodes = ctx->DockContext.Nodes;
    for (int i = 0; i < nodes.Data.Size; ++i)
        if (ImGuiDockNode* node = static_cast<ImGuiDockNode*>(nodes.Data[i].val_p))
        {
            // 丸めない（float のまま）: 倍率を往復（1.0 -> 1.5 -> 2.0 -> 1.0）しても 1px もずれない。ImGui が配置時に整数へ丸める。
            node->Size.x *= f;    node->Size.y *= f;
            node->SizeRef.x *= f; node->SizeRef.y *= f;
            ++n;
        }
    return n;
}

// ---- imgui.ini の倍率マーカー ----
// ini の窓位置/サイズ/ドックのサイズは【物理 px】で保存される。倍率が変わっても崩れないよう、
// 保存時の倍率を [DpiScale][Main] に記録し、読み込み時に (今の倍率 / 保存時の倍率) で拡縮する。
// 旧版の ini（マーカー無し）は DPI 非対応＝論理 px（100% 基準）とみなす。
// ★ドックのノードは imgui 内蔵ハンドラの ApplyAll が設定から構築するので、その【後】（ハンドラは末尾に登録）に、
//   出来上がったノードの Size / SizeRef を拡縮する。窓の Pos/Size は最初の Begin で設定から適用されるので、設定側を拡縮する。
void* DpiIniReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
{
    return std::strcmp(name, "Main") == 0 ? reinterpret_cast<void*>(1) : nullptr;
}
void DpiIniReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
    if (std::strncmp(line, "Scale=", 6) == 0)
    {
        const float v = static_cast<float>(std::atof(line + 6));
        if (v > 0.0f) g_iniScale = v;
    }
}
void DpiIniClearAll(ImGuiContext*, ImGuiSettingsHandler*) { g_iniScale = 0.0f; }
void DpiIniApplyAll(ImGuiContext* ctx, ImGuiSettingsHandler*)
{
    const float from = g_iniScale > 0.0f ? g_iniScale : 1.0f;
    const float f = g_mainScale / from;   // 起動時だけでなく実行中の ini 読み込み（テスト）でも今の倍率へ換算する
    if (std::fabs(f - 1.0f) < 0.001f) return;
    auto sc = [f](ImVec2ih& v) {
        v.x = static_cast<short>(std::lround(static_cast<float>(v.x) * f));
        v.y = static_cast<short>(std::lround(static_cast<float>(v.y) * f));
    };
    int nWin = 0, nNode = 0;
    for (ImGuiWindowSettings* ws = ctx->SettingsWindows.begin(); ws != nullptr; ws = ctx->SettingsWindows.next_chunk(ws))
    {
        sc(ws->Pos); sc(ws->Size); ++nWin;
    }
    nNode = ScaleDockNodes(ctx, f);
    Logger::Info("imgui.ini の倍率を換算: {:.2f} -> {:.2f} (窓 {} / ドックノード {})", from, g_mainScale, nWin, nNode);
}
void DpiIniWriteAll(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf)
{
    buf->appendf("[%s][Main]\nScale=%.4f\n\n", h->TypeName, static_cast<double>(g_mainScale));
}

// 起動時に別ビューポート(OS 窓)が 1 枚でも生成されたらログに警告する（画面に窓が出ている合図）。
void WarnSecondaryViewports()
{
    ImGuiContext* gp = ImGui::GetCurrentContext();
    if (!gp) return;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Viewports.Size <= 1) return;
    static std::set<ImGuiID> warned;
    for (int i = 1; i < pio.Viewports.Size; ++i)
    {
        ImGuiViewport* vp = pio.Viewports[i];
        if (!warned.insert(vp->ID).second) continue;
        const char* name = "(不明)";
        for (ImGuiWindow* w : gp->Windows)
            if (w->Viewport == vp && !(w->Flags & ImGuiWindowFlags_ChildWindow)) { name = w->Name; break; }
        if (vinput::Enabled())
            Logger::Warn("★警告: 仮想入力モード中に別ビューポート(別 OS 窓)が生成されました。実画面に窓が出ています: "
                         "window='{}' pos=({:.0f},{:.0f}) size=({:.0f},{:.0f})",
                         name, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
        else
            Logger::Warn("別ビューポート(別 OS 窓)が生成されました: window='{}' pos=({:.0f},{:.0f}) size=({:.0f},{:.0f})",
                         name, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
    }
}

// ---- フォント ----
struct FontSrc
{
    std::string path;
    ImU32       fontNo    = 0;
    float       sizeScale = 1.0f;          // ExtraSizeScale（字の大きさの微調整）
    ImVec2      offset    = ImVec2(0, 0);  // GlyphOffset（縦位置の微調整）
};

std::string WinFontPath(const char* file)
{
    char win[MAX_PATH] = {};
    const UINT n = ::GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = n ? std::string(win) : std::string("C:\\Windows");
    return dir + "\\Fonts\\" + file;
}

// 最初に見つかった 1 本を返す（無ければ空）。
FontSrc FirstExisting(std::initializer_list<FontSrc> cands)
{
    for (const FontSrc& c : cands)
    {
        std::error_code ec;
        if (!c.path.empty() && std::filesystem::exists(c.path, ec)) return c;
    }
    return FontSrc{};
}

// 見つかった順に 1 つの ImFont へマージして返す（先頭が主フォント）。何も無ければ nullptr。
ImFont* BuildFont(ImGuiIO& io, float size, const std::vector<FontSrc>& srcs)
{
    ImFont* font = nullptr;
    for (const FontSrc& s : srcs)
    {
        if (s.path.empty()) continue;
        ImFontConfig cfg;
        cfg.FontNo         = s.fontNo;
        cfg.MergeMode      = (font != nullptr);
        cfg.ExtraSizeScale = s.sizeScale;
        cfg.GlyphOffset    = s.offset;
        // Windows 標準フォントは ttc の中に複数入っている。名前はデバッグ用。
        std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", std::filesystem::path(s.path).filename().string().c_str());
        ImFont* f = io.Fonts->AddFontFromFileTTF(s.path.c_str(), size, &cfg);
        if (!font) font = f;
    }
    return font;
}

// エディタのフォント一式（本文 / 太字 / 等幅 + Lucide アイコン）を読み込んで theme::g_fonts へ渡す。
// 全部 Windows 標準フォント（C:\Windows\Fonts）を存在確認つきで使い、無ければ次の候補へフォールバック。
//   本文 = Segoe UI（欧文。バックスラッシュも ¥ にならない）
//        + Yu Gothic UI（日本語。Segoe UI と縦メトリクスが同じなので行がずれない）
//        + Segoe UI Symbol（▾ ▼ ▲ ▽ → などの記号。無いと '?' になる）
//        + Lucide（アイコン）
bool LoadEditorFonts(ImGuiIO& io)
{
    using namespace dx12e::theme;
    const float base = size::kFontBase;

    const FontSrc latin     = FirstExisting({ {WinFontPath("segoeui.ttf")} });
    const FontSrc latinBold = FirstExisting({ {WinFontPath("segoeuib.ttf")}, {WinFontPath("segoeui.ttf")} });
    // 日本語（UI 用の字形。ttc の番号は fontTools で確認: YuGothM#1 = Yu Gothic UI Regular / YuGothB#1 = Bold）
    const FontSrc jp     = FirstExisting({ {WinFontPath("YuGothM.ttc"), 1}, {WinFontPath("YuGothR.ttc"), 1},
                                           {WinFontPath("meiryo.ttc"), 2}, {WinFontPath("meiryo.ttc"), 0},
                                           {WinFontPath("msgothic.ttc"), 0} });
    const FontSrc jpBold = FirstExisting({ {WinFontPath("YuGothB.ttc"), 1}, {WinFontPath("meiryob.ttc"), 2},
                                           {WinFontPath("meiryob.ttc"), 0}, {WinFontPath("YuGothM.ttc"), 1} });
    const FontSrc symbol = FirstExisting({ {WinFontPath("seguisym.ttf")} });
    // 等幅: 字の大きさを本文と揃えるため 0.875 倍（フォントサイズ = 行高は本文と同じにして frame 高を変えない）
    FontSrc mono = FirstExisting({ {WinFontPath("CascadiaMono.ttf")}, {WinFontPath("consola.ttf")} });
    mono.sizeScale = 0.875f;

    // Lucide（ISC ライセンス。assets/editor/fonts/ に同梱）。開発時は BaseDir、配布時は exe 隣の assets。
    FontSrc lucide = FirstExisting({
        {PathResolver::BaseDir() + "assets/editor/fonts/lucide.ttf"},
        {PathResolver::AssetsDir() + "editor/fonts/lucide.ttf"},
    });
    lucide.sizeScale = 0.90f;                     // 1em = 本文サイズ × 0.9（字の em より一回り大きく見える）
    lucide.offset    = ImVec2(0.0f, 2.6f);        // 本文のベースラインへ合わせる（アイコンは 1em がベースライン上に乗るため）

    auto stack = [&](const FontSrc& l, const FontSrc& j, float scaleLatin) {
        std::vector<FontSrc> v;
        FontSrc ll = l; ll.sizeScale = scaleLatin;
        // ★Lucide を日本語/記号フォントより【先に】マージする。マージは先勝ちで、Yu Gothic UI / Segoe UI Symbol は
        //   私用領域(U+E000〜)にも別のグリフを持っているため、後ろに置くと ICON_* が別の絵になる。
        if (!ll.path.empty()) v.push_back(ll);
        if (!lucide.path.empty()) v.push_back(lucide);
        if (!j.path.empty())  v.push_back(j);
        if (!symbol.path.empty()) v.push_back(symbol);
        return v;
    };

    ImFont* body = BuildFont(io, base, stack(latin, jp, 1.0f));
    if (!body) return false;
    ImFont* bold = BuildFont(io, base, stack(latinBold.path.empty() ? latin : latinBold,
                                             jpBold.path.empty() ? jp : jpBold, 1.0f));
    ImFont* mon  = BuildFont(io, base, stack(mono.path.empty() ? latin : mono, jp, mono.path.empty() ? 1.0f : mono.sizeScale));

    // ゲーム UI 用の既定フォント = 旧来の Yu Gothic Medium 17px（出荷したゲームと文字の幅・行高を揃える。core/GameUiFont.h）。
    {
        const FontSrc legacy = FirstExisting({ {WinFontPath("YuGothM.ttc"), 0}, {WinFontPath("meiryo.ttc"), 0} });
        if (!legacy.path.empty())
            GameUiDefaultFont() = BuildFont(io, 17.0f, { legacy });
    }

    theme::g_fonts.body  = body;
    theme::g_fonts.bold  = bold ? bold : body;
    theme::g_fonts.mono  = mon  ? mon  : body;
    theme::g_fonts.icons = !lucide.path.empty();
    io.FontDefault = body;
    Logger::Info("UI fonts: latin={} jp={} symbol={} mono={} icons={}",
                 latin.path.empty() ? "-" : "segoeui", jp.path.empty() ? "-" : "ok",
                 symbol.path.empty() ? "-" : "ok", mono.path.empty() ? "-" : "ok",
                 lucide.path.empty() ? "MISSING" : "lucide");
    if (lucide.path.empty())
        Logger::Error("assets/editor/fonts/lucide.ttf が見つかりません。ツールバー/ヒエラルキーのアイコンが '?' になります");
    return true;
}

// 配布ゲーム(GameRuntime)向けの旧スタイル。エディタ専用テーマ（EditorTheme.h）の値を変えても
// ゲームの見た目が動かないよう、旧パレットをここへリテラルで固定している。
void ApplyLegacyGameStyle(ImGuiStyle& style)
{
    using theme::Hex;
    const ImVec4 AppBg = Hex(0x0e0f12), PanelBg = Hex(0x16171b), Chrome = Hex(0x1a1b20), GroupBg = Hex(0x202127);
    const ImVec4 FrameBg = Hex(0x1d1e23), FrameBgHi = Hex(0x242530), FrameBgActive = Hex(0x2b2c38);
    const ImVec4 Border = Hex(0x25262c);
    const ImVec4 Accent = Hex(0x4c8dff), AccentLight = Hex(0x6ba2ff);
    const ImVec4 AccentDim = Hex(0x4c8dff, 0.18f), AccentDim2 = Hex(0x4c8dff, 0.32f);
    const ImVec4 TextHi = Hex(0xe6e7ea), TextFaint = Hex(0x74767f);

    ImGui::StyleColorsDark();
    style.WindowRounding          = 7.0f;
    style.ChildRounding           = 6.0f;
    style.FrameRounding           = 5.0f;
    style.PopupRounding           = 7.0f;
    style.GrabRounding            = 5.0f;
    style.TabRounding             = 6.0f;
    style.ScrollbarRounding       = 7.0f;
    style.WindowPadding           = ImVec2(9, 8);
    style.FramePadding            = ImVec2(8, 5);
    style.CellPadding             = ImVec2(6, 4);
    style.ItemSpacing             = ImVec2(8, 7);
    style.ItemInnerSpacing        = ImVec2(6, 5);
    style.IndentSpacing           = 16.0f;
    style.ScrollbarSize           = 11.0f;
    style.GrabMinSize             = 10.0f;
    style.WindowBorderSize        = 1.0f;
    style.ChildBorderSize         = 0.0f;
    style.FrameBorderSize         = 0.0f;
    style.TabBarBorderSize        = 1.0f;
    style.TabBarOverlineSize      = 2.0f;
    style.DockingSeparatorSize    = 1.0f;
    style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding    = ImVec2(18, 6);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = TextHi;
    c[ImGuiCol_TextDisabled]         = TextFaint;
    c[ImGuiCol_WindowBg]             = PanelBg;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = Hex(0x1b1c21, 0.98f);
    c[ImGuiCol_Border]               = Border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TitleBg]              = Chrome;
    c[ImGuiCol_TitleBgActive]        = Chrome;
    c[ImGuiCol_TitleBgCollapsed]     = Hex(0x16171b, 0.75f);
    c[ImGuiCol_MenuBarBg]            = Chrome;
    c[ImGuiCol_FrameBg]              = FrameBg;
    c[ImGuiCol_FrameBgHovered]       = FrameBgHi;
    c[ImGuiCol_FrameBgActive]        = FrameBgActive;
    c[ImGuiCol_Button]               = GroupBg;
    c[ImGuiCol_ButtonHovered]        = Hex(0x2a2c33);
    c[ImGuiCol_ButtonActive]         = Hex(0x32343c);
    c[ImGuiCol_Header]               = AccentDim;
    c[ImGuiCol_HeaderHovered]        = Hex(0x4c8dff, 0.12f);
    c[ImGuiCol_HeaderActive]         = AccentDim2;
    c[ImGuiCol_Tab]                       = Chrome;
    c[ImGuiCol_TabHovered]                = Hex(0x2a2c33);
    c[ImGuiCol_TabSelected]               = PanelBg;
    c[ImGuiCol_TabSelectedOverline]       = Accent;
    c[ImGuiCol_TabDimmed]                 = Chrome;
    c[ImGuiCol_TabDimmedSelected]         = PanelBg;
    c[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x4c8dff, 0.45f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = Hex(0x33343c);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x44454f);
    c[ImGuiCol_ScrollbarGrabActive]  = Hex(0x55565f);
    c[ImGuiCol_SliderGrab]           = Accent;
    c[ImGuiCol_SliderGrabActive]     = AccentLight;
    c[ImGuiCol_CheckMark]            = AccentLight;
    c[ImGuiCol_Separator]            = Border;
    c[ImGuiCol_SeparatorHovered]     = AccentDim2;
    c[ImGuiCol_SeparatorActive]      = Accent;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = AccentDim2;
    c[ImGuiCol_ResizeGripActive]     = Accent;
    c[ImGuiCol_DockingPreview]       = AccentDim2;
    c[ImGuiCol_DockingEmptyBg]       = AppBg;
    c[ImGuiCol_TableHeaderBg]        = Chrome;
    c[ImGuiCol_TableBorderStrong]    = Border;
    c[ImGuiCol_TableBorderLight]     = Hex(0x202127);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = Hex(0xffffff, 0.02f);
    c[ImGuiCol_TextSelectedBg]       = AccentDim2;
    c[ImGuiCol_NavCursor]            = Accent;
}
} // namespace

// ---------------------------------------------------------------------------
// フローティング窓の収容（gui/FloatingGuard.h）
// ---------------------------------------------------------------------------
namespace floatguard
{
namespace
{
ImVec2 g_areaMin(0, 0);
ImVec2 g_areaMax(0, 0);
bool   g_areaSet = false;
} // namespace

void SetArea(ImVec2 areaMin, ImVec2 areaMax)
{
    g_areaMin = areaMin;
    g_areaMax = areaMax;
    g_areaSet = true;
}

void Apply(bool always)
{
    ImGuiContext* gp = ImGui::GetCurrentContext();
    if (!gp) return;
    ImGuiContext& g = *gp;
    static std::unordered_map<ImGuiID, int> s_settle;   // 出現直後の残りフレーム数
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    const float margin = 8.0f;

    const ImU32 edge = ImGui::GetColorU32(theme::BorderStrong);
    for (ImGuiWindow* w : g.Windows)
    {
        if (!w->Active || w->Hidden) continue;

        // 外枠: メニュー/ポップアップとフローティング窓に 1px の強い枠（ビューポートの暗い絵の上で同化しないように）。
        // 窓自身の枠は控えめな色で 1 本引かれているので、その上へ重ねる（ドロップシャドウの代わり）。
        {
            const bool popup    = (w->Flags & ImGuiWindowFlags_Popup) && !(w->Flags & ImGuiWindowFlags_Tooltip);
            const bool floating = !(w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip |
                                                ImGuiWindowFlags_NoTitleBar)) && !w->DockIsActive && !w->DockNode;
            if ((popup || floating) && w->DrawList)
            {
                w->DrawList->PushClipRectFullScreen();
                w->DrawList->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), edge,
                                     w->WindowRounding, 0, 1.0f);
                w->DrawList->PopClipRect();
            }
        }

        // NoInputs は画面全体を覆う透明オーバーレイ（ImGuizmo の "gizmo" 窓など）。位置は毎フレーム呼び出し側が
        // SetNextWindowPos で決めているので触らない（触ると UI 自動テストが「窓を動かせない」と誤検知する）。
        if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup |
                        ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs))
            continue;
        if (w->DockIsActive || w->DockNode) continue;                       // ドック窓は dock 側が面倒を見る
        if (w->ViewportOwned && w->Viewport != mainVp) continue;            // 人が意図して別 OS 窓へ出した
        if (w->Viewport != mainVp && !always) continue;

        if (!g_areaSet) continue;
        int& settle = s_settle[w->ID];
        if (w->Appearing) settle = 8;
        if (!always && settle <= 0) continue;
        if (settle > 0) --settle;

        const ImVec2 sz = w->Size;
        const float minX = g_areaMin.x + margin, minY = g_areaMin.y + margin;
        const float maxX = (std::max)(minX, g_areaMax.x - sz.x - margin);
        const float maxY = (std::max)(minY, g_areaMax.y - sz.y - margin);
        const ImVec2 np((std::min)((std::max)(w->Pos.x, minX), maxX),
                        (std::min)((std::max)(w->Pos.y, minY), maxY));
        if (np.x != w->Pos.x || np.y != w->Pos.y)
            ImGui::SetWindowPos(w, np, ImGuiCond_Always);
    }
}
} // namespace floatguard


void ImGuiManager::Initialize(
    HWND hwnd,
    GraphicsDevice& device,
    ID3D12CommandQueue* commandQueue,
    DescriptorHeap& srvHeap,
    DXGI_FORMAT rtvFormat,
    u32 frameCount)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_hwnd = hwnd;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // multi-viewport: フローティング窓(マテリアルエディタ等)をメインウィンドウの外へ
    // ドラッグすると独立したOSウィンドウになる(Unreal/Unityと同じ)。ドック中のコアパネルは
    // NoUndocking なので出て行かない。有効時、ImGui座標系は「スクリーン座標」になる点に注意
    // (絶対座標(0,0)前提の窓は GetMainViewport()->Pos 基準に直してある)。
    // ★仮想入力モード(--background / --virtual-input)では有効にしない。imgui.ini に残った ViewportPos で
    //   フローティング窓が「ユーザーの実画面」へ別 OS 窓として出る事故を根絶するため（全窓がメインビューポート内に留まる）。
    g_viewportsWanted = true;
    if (!vinput::Enabled())
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    else
        Logger::Info("multi-viewport: 無効（仮想入力モード。全ウィンドウはメインウィンドウ内に留まる）");
    io.ConfigViewportsNoTaskBarIcon = true;   // 引き出した窓はタスクバーに出さない(UE/Unityと同じ)
    // ID 衝突警告のビジュアルオーバーレイを抑制（誤検出で popup が塞がれることがある）
    io.ConfigDebugHighlightIdConflicts = false;

    // ===== フォント =====
    // エディタ: 本文/太字/等幅 + アイコン（LoadEditorFonts）。配布ゲーム: 従来どおり 17px 1 本。
    bool editorFonts = false;
    if (!vfs::InGameMode())
        editorFonts = LoadEditorFonts(io);

    if (!editorFonts)
    {
        // 日本語フォント読み込み。Yu Gothic Medium（Win標準・レンダリングがくっきり）優先、
        // 無ければ Meiryo にフォールバック。サイズは 17px（可読性優先＝Unreal 寄りの密度）。
        const char* candidates[] = {
            "C:\\Windows\\Fonts\\YuGothM.ttc",   // Yu Gothic Medium
            "C:\\Windows\\Fonts\\meiryo.ttc",
        };
        bool loaded = false;

        // ★配布ゲームでは pak 内のフォントを最優先にする。
        //   以前は OS の Yu Gothic / Meiryo を直読みするだけで、その 2 つが入っていない環境
        //   （日本語 SKU 以外の素の Windows。両方とも同じオプション機能に入っている）では
        //   ImGui が ProggyClean（ASCII のみ）へフォールバックし、**日本語 UI が全部消える**。
        //   開発機では絶対に再現しないので気づけない類の壊れ方だった。
        //   BuildGame が assets/fonts/ から 1 本選んで manifest の uiFont に書いている。
        //   フォントのバイト列は ImGui が所有する（FontDataOwnedByAtlas 既定 true）ので
        //   ここで確保したメモリを渡し切りにしてよい。
        if (vfs::InGameMode())
        {
            vfs::BootConfig boot;
            if (vfs::ReadBootConfig(boot) && !boot.uiFont.empty())
            {
                std::vector<uint8_t> bytes = vfs::ReadAsset(boot.uiFont);
                if (!bytes.empty())
                {
                    void* owned = IM_ALLOC(bytes.size());
                    std::memcpy(owned, bytes.data(), bytes.size());
                    io.Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(bytes.size()), 17.0f,
                        nullptr, io.Fonts->GetGlyphRangesJapanese());
                    Logger::Info("UI font loaded from pak: {}", boot.uiFont);
                    loaded = true;
                }
                else
                {
                    Logger::Error("manifest の uiFont を pak から読めません: {}", boot.uiFont);
                }
            }
        }

        for (const char* fontPath : candidates)
        {
            if (loaded) break;
            if (!std::filesystem::exists(fontPath)) continue;
            io.Fonts->AddFontFromFileTTF(fontPath, 17.0f, nullptr,
                io.Fonts->GetGlyphRangesJapanese());
            Logger::Info("Japanese font loaded: {}", fontPath);
            loaded = true;
            break;
        }
        if (!loaded)
        {
            // ★ここは警告ではなく**エラー**。この状態のまま出荷すると日本語が 1 文字も出ない。
            Logger::Error("日本語フォントが見つかりません。ASCII のみの内蔵フォントで続行するため、"
                          "日本語の UI テキストは表示されません "
                          "(dx12_install_font で assets/fonts/ に日本語対応フォントを入れ、"
                          "ゲームを再ビルドしてください)");
        }
    }

    // ===== テーマ =====
    ImGuiStyle& style = ImGui::GetStyle();
    if (editorFonts)
    {
        // エディタ: Unreal Editor 5 風（面の階調 / 角ばり / 凹み入力欄 / アクセントは選択とフォーカスのみ）。
        // 値の実体は editor/EditorTheme.h（トークン）。
        ImGui::StyleColorsDark();
        theme::ApplyStyle(style);
        style.FontSizeBase = theme::size::kFontBase;
        g_editorStyle = true;
        // 基準スタイル（100% 相当）を保持。倍率が変わるたびにここから作り直す（累積しない）。
        theme::BaseStyle() = style;
        theme::BaseStyleValid() = true;
        g_mainHwnd     = hwnd;
        g_mainScale    = ComputeMainScale();
        g_initialScale = g_mainScale;
        g_appliedScale = 0.0f;
        ApplyUiScale(g_mainScale);
        theme::g_restoreMainScaleFn = [] { ApplyUiScale(g_mainScale); };   // フォントはダイナミック（FontScaleDpi）なので作り直し不要
        // imgui.ini の窓サイズ/ドックのサイズを保存時の倍率から換算する
        {
            ImGuiSettingsHandler h;
            h.TypeName = "DpiScale";
            h.TypeHash = ImHashStr("DpiScale");
            h.ClearAllFn = DpiIniClearAll;
            h.ReadOpenFn = DpiIniReadOpen;
            h.ReadLineFn = DpiIniReadLine;
            h.ApplyAllFn = DpiIniApplyAll;
            h.WriteAllFn = DpiIniWriteAll;
            ImGui::AddSettingsHandler(&h);   // 末尾＝Docking ハンドラの ApplyAll の後に走る
        }
        Logger::Info("DPI: UI scale = {:.2f} ({})", g_mainScale, dx12e::dpi::HasOverride() ? "--dpi-scale override" : "OS setting");
    }
    else
    {
        ApplyLegacyGameStyle(style);
    }

    // Win32 backend
    ImGui_ImplWin32_Init(hwnd);

    // DPI: ビューポートごとの倍率（--dpi-scale の上書きを含む）と、窓ごとのスタイル切替。
    //   ConfigDpiScaleViewports: 別倍率のモニターへ移った時に imgui が窓の位置/サイズを比で拡縮し、
    //   引き出した窓（別 OS 窓）の WM_DPICHANGED では推奨矩形へ動かす。メイン窓の WM_DPICHANGED は Window::WndProc が処理する。
    if (g_editorStyle)
    {
        ImGuiPlatformIO& dpio = ImGui::GetPlatformIO();
        g_origGetWindowDpiScale = dpio.Platform_GetWindowDpiScale;
        dpio.Platform_GetWindowDpiScale = GetWindowDpiScaleHook;
        dpio.Platform_OnChangedViewport = OnChangedViewportHook;
        ImGui::GetIO().ConfigDpiScaleViewports = true;
    }

    // 仮想入力モード用: 最小化判定の差し替え（バックエンドの実装を包む。OFF の間は素通し）。
    {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        if (pio.Platform_GetWindowMinimized)
        {
            g_origGetWindowMinimized = pio.Platform_GetWindowMinimized;
            pio.Platform_GetWindowMinimized = GetWindowMinimizedOverride;
        }
    }

    // DX12 backend
    m_srvIndex = srvHeap.AllocateIndex();

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device            = device.GetDevice();
    initInfo.CommandQueue      = commandQueue;
    initInfo.NumFramesInFlight = frameCount;
    initInfo.RTVFormat         = rtvFormat;
    initInfo.DSVFormat         = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = srvHeap.GetHeap();
    initInfo.SrvDescriptorAllocFn  = nullptr;
    initInfo.SrvDescriptorFreeFn   = nullptr;
    initInfo.LegacySingleSrvCpuDescriptor = srvHeap.GetCpuHandle(m_srvIndex);
    initInfo.LegacySingleSrvGpuDescriptor = srvHeap.GetGpuHandle(m_srvIndex);

    ImGui_ImplDX12_Init(&initInfo);

    Logger::Info("ImGui initialized (SRV index={})", m_srvIndex);
}

void ImGuiManager::SetIniSavingDisabled(bool on)
{
    m_iniSavingDisabled = on;
    if (on && ImGui::GetCurrentContext())
    {
        g_viewportsForbidden = true;
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    }
}

void ImGuiManager::SetVirtualInput(bool on)
{
    vinput_gui::SetImGuiVirtualMode(on);
    // 仮想入力の間は multi-viewport を切る（別 OS 窓が実画面に出ないように）。切ると ImGui は
    // 引き出し済みの窓もメインビューポートへ戻す。OFF に戻したら（通常起動で有効だったなら）復帰。
    if (ImGui::GetCurrentContext() && g_editorStyle)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (on)
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
        else if (g_viewportsWanted && !g_viewportsForbidden)
            io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    if (!on)
    {
        // 押しっぱなしで切ると ImGui 側にボタン/キー押下が残る。離したことにして次フレームへ流す。
        vinput::Queue& q = vinput::Global();
        const vinput::Frame release = q.MakeReleaseAllFrame();
        if (!release.empty() && ImGui::GetCurrentContext())
        {
            vinput_gui::ApplyContext ctx;
            ctx.viewportPos = ImGui::GetMainViewport()->Pos;
            ctx.displaySize = ImGui::GetIO().DisplaySize;
            vinput_gui::ApplyFrame(ImGui::GetIO(), release, ctx);
        }
        q.Reset();
        vinput_gui::ResetVirtualCursorState();
    }
}

float ImGuiManager::GetUiScale() const { return g_editorStyle ? g_mainScale : 1.0f; }

void ImGuiManager::SetUiScaleOverride(float scale)
{
    dx12e::dpi::SetOverride(scale);
}

void ImGuiManager::BeginFrame()
{
    // DPI: メインビューポートの倍率が変わっていたら（別モニターへ移動 / OS 設定変更 / オーバーライド）ここで切り替える。
    //   NewFrame の前に済ませる＝そのフレームは最初から新しい倍率のスタイルとフォントで描かれる。
    if (g_editorStyle)
    {
        const float want = ComputeMainScale();
        if (want != g_mainScale)
        {
            Logger::Info("DPI: UI scale {:.2f} -> {:.2f}", g_mainScale, want);
            const float ratio = want / g_mainScale;
            g_mainScale = want;
            // ドックの分割も同じ比で拡縮（窓の Pos/Size は ImGui が ConfigDpiScaleViewports で拡縮するが、ドックは物理 px 固定のまま残る）
            if (ImGui::GetCurrentContext()) ScaleDockNodes(ImGui::GetCurrentContext(), ratio);
        }
        ApplyUiScale(g_mainScale);
    }

    if (vinput::Enabled())
    {
        ImGuiIO& io = ImGui::GetIO();
        // ★バックエンドが SetCursorPos を呼ぶ唯一の条件を潰す（ナビ由来のマウス移動要求）。
        io.WantSetMousePos = false;

        // Win32 バックエンドの NewFrame は「実マウス位置の取り込み」「修飾キーの整合」等を
        // イベントキューへ積む。仮想入力が実入力に上書きされないよう、積まれた分を捨てる。
        // （実カーソルの位置は GetCursorPos で読まれるだけで、OS には何も書き込まない）
        const int keep = vinput_gui::InputQueueSize();
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        vinput_gui::TruncateInputQueue(keep);

        // 最小化されたままの窓でもレイアウトを潰さない。
        if ((io.DisplaySize.x < 1.0f || io.DisplaySize.y < 1.0f) && m_logicalW > 0 && m_logicalH > 0)
            io.DisplaySize = ImVec2(static_cast<float>(m_logicalW), static_cast<float>(m_logicalH));

        // 仮想入力キューから 1 フレームぶんを流し込む（ImGui::NewFrame の直前）。
        vinput::Queue& q = vinput::Global();
        q.ClearAnchors();
        const vinput::Frame frame = q.Pump();
        vinput_gui::ApplyContext ctx;
        ctx.viewportPos    = ImGui::GetMainViewport()->Pos;
        ctx.displaySize    = io.DisplaySize;
        ctx.mainViewportId = ImGui::GetMainViewport()->ID;
        ctx.keySink        = m_virtualKeySink;
        vinput_gui::ApplyFrame(io, frame, ctx);
    }
    else
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
    }
    ImGui::NewFrame();

    // レイアウト(imgui.ini)は最初の NewFrame で読まれる。以後は保存しない。
    if (m_iniSavingDisabled && !m_iniSavingDisabledApplied)
    {
        ImGui::GetIO().IniFilename = nullptr;
        m_iniSavingDisabledApplied = true;
    }
}

void ImGuiManager::EndFrame(ID3D12GraphicsCommandList* cmdList)
{
    if (vinput::Enabled())
        vinput_gui::DrawVirtualCursor();   // スクショに「AI が今どこを操作しているか」が映る
    if (g_editorStyle)
        floatguard::Apply(vinput::Enabled() || g_viewportsForbidden);   // 全パネルの描画後: フローティング窓をメイン窓内へ収める
    WarnSecondaryViewports();              // 別 OS 窓が 1 枚でも生えたらログに警告（実画面に窓が出ている合図）
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

void ImGuiManager::RenderPlatformWindows()
{
    // multi-viewport のセカンダリウィンドウ(引き出したフローティング窓)を描画・Present する。
    // DX12バックエンドが専用のコマンドリスト/スワップチェインを内部管理してキューへ直接投げるため、
    // メインのコマンドリストを ExecuteCommandList した後・Present の前に呼ぶこと
    // (メインリスト内で遷移させたテクスチャ(サムネイル等)をセカンダリ側が参照しても順序が正しくなる)。
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault(nullptr, nullptr);
    }
}

void ImGuiManager::Shutdown()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    Logger::Info("ImGui shut down");
}

LRESULT ImGuiManager::WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
}

} // namespace dx12e
