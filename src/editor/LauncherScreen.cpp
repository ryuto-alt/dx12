// プロジェクトランチャーの画面（UE5 プロジェクトブラウザ風・ダーク + ネオングロー）。設計は LauncherScreen.h / docs/LAUNCHER.md。
//
// 描き方の方針:
//   ・1 枚の全画面ウィンドウの中を ImDrawList で自前描画し、当たり判定は InvisibleButton（見た目と判定が同じ矩形）。
//   ・寸法は「論理 px（100% 表示の px）」で書き、ui::Px() を通す。色は EditorTheme.h のトークン。
//   ・動きは指数補間（launcher::SmoothTo）と臨界減衰スプリング。linear は使わない。設定でアニメを切ると即座に目標値へ。
//   ・重い処理（git/gh の存在確認・GitHub ログイン確認・クローン）はワーカースレッド。画面は止めない。
#include "editor/LauncherScreen.h"
#include "editor/UiWidgets.h"
#include "editor/EditorTheme.h"
#include "editor/EditorIcons.h"
#include "project/GitIntegration.h"
#include "project/LauncherLogic.h"
#include "project/LauncherMotion.h"
#include "core/Version.h"
#include "core/Logger.h"
#include "core/VirtualGuard.h"
#include "core/CrashHandler.h"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace dx12e
{
namespace
{
namespace th = theme;
namespace L  = launcher;
namespace fs = std::filesystem;

// ===========================================================================
// 小道具
// ===========================================================================
inline float P(float v)  { return ui::Px(v); }
inline float PF(float v) { return ui::PxF(v); }
inline ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
inline ImVec2 Sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
inline float  Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float  Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 画面全体（タブ切替の入場）の不透明度。自前描画の色に掛ける。
float g_alpha = 1.0f;

inline ImU32 U(const ImVec4& c, float a = 1.0f)
{
    ImVec4 v = c;
    v.w = Clamp(v.w * a * g_alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(v);
}
inline ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), Lerp(a.w, b.w, t));
}
inline const ImVec4& White() { static const ImVec4 w(1, 1, 1, 1); return w; }
inline const ImVec4& Black() { static const ImVec4 b(0, 0, 0, 1); return b; }

ImFont* FBody() { return th::g_fonts.body ? th::g_fonts.body : ImGui::GetFont(); }
ImFont* FBold() { return th::g_fonts.bold ? th::g_fonts.bold : FBody(); }
ImFont* FMono() { return th::g_fonts.mono ? th::g_fonts.mono : FBody(); }

float TextW(ImFont* f, float pxLogical, const char* s)
{
    return f->CalcTextSizeA(P(pxLogical), FLT_MAX, 0.0f, s).x;
}

void DText(ImFont* f, float pxLogical, ImVec2 pos, ImU32 col, const char* s)
{
    ImGui::GetWindowDrawList()->AddText(f, P(pxLogical), pos, col, s);
}

// 幅に収まる長さまで末尾を「…」で省略。
std::string FitEnd(ImFont* f, float pxLogical, const std::string& s, float maxW)
{
    if (TextW(f, pxLogical, s.c_str()) <= maxW) return s;
    size_t lo = 0, hi = L::Utf8Length(s);
    while (lo + 1 < hi)
    {
        const size_t mid = (lo + hi) / 2;
        const std::string t = s.substr(0, L::Utf8PrefixBytes(s, mid)) + "…";
        if (TextW(f, pxLogical, t.c_str()) <= maxW) lo = mid; else hi = mid;
    }
    return s.substr(0, L::Utf8PrefixBytes(s, lo)) + "…";
}

// パスの中央を省略（先頭のドライブと末尾のフォルダ名を残す）。
std::string FitPath(ImFont* f, float pxLogical, const std::string& s, float maxW)
{
    if (TextW(f, pxLogical, s.c_str()) <= maxW) return s;
    size_t lo = 4, hi = L::Utf8Length(s);
    while (lo + 1 < hi)
    {
        const size_t mid = (lo + hi) / 2;
        const std::string t = L::MiddleEllipsis(s, mid);
        if (TextW(f, pxLogical, t.c_str()) <= maxW) lo = mid; else hi = mid;
    }
    return L::MiddleEllipsis(s, lo);
}

std::wstring Widen(const std::string& u8)
{
    if (u8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), static_cast<int>(u8.size()), w.data(), n);
    return w;
}

// 仮想入力モード中は OS を触らない（ガード済み）。
void OpenUrl(const std::string& url)
{
    if (guard::Blocked("URL をブラウザで開く")) return;
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
void ShowInExplorer(const std::string& path, bool isDir)
{
    if (guard::Blocked("エクスプローラーで表示")) return;
    const std::wstring w = Widen(path);
    if (isDir) ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else       ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + w + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

std::string DocumentsDir()
{
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p)
    {
        r = L::PathToUtf8(fs::path(p));
        CoTaskMemFree(p);
    }
    return r;
}

// Windows の「アニメーション効果」（設定 > アクセシビリティ > 視覚効果）が切られているか。
bool SystemAnimationsEnabled()
{
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return on != FALSE;
    return true;
}

template <size_t N> void SetBuf(std::array<char, N>& b, const std::string& s)
{
    std::snprintf(b.data(), N, "%s", s.c_str());
}
template <size_t N> std::string GetBuf(const std::array<char, N>& b) { return std::string(b.data()); }

// ===========================================================================
// 状態
// ===========================================================================
enum Tab { kRecent = 0, kNew = 1, kOpen = 2, kClone = 3, kNews = 4, kTabCount = 5 };

struct RecentView
{
    L::RecentRecord rec;
    std::string     key;            // PathKey
    bool            exists = false; // フォルダがあり .dx12proj を持つ
    std::string     thumbPath;      // <root>/.dx12/thumbnail.png（あれば）
    bool            hasThumb = false;
    LauncherImage   img;
    bool            imgTried = false;
    // 省略文字列のキャッシュ（幅が変わったときだけ計算し直す）
    float           fitW = -1.0f;
    std::string     fitName, fitPath;
};

struct ProjectMeta
{
    bool        loaded = false;
    std::string key;
    std::string engineVersion;
    std::string defaultScene;
    std::string projectFile;
};

// ---- 非同期の共有状態。スレッドは detach するので、破棄されない領域（leak）に置く。
//      アプリ終了時に走っているスレッドが、先に破棄された静的オブジェクトへ書いて落ちるのを原理的に避ける。
struct EnvProbe
{
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> git{false}, gh{false};
    std::mutex        mu;
    std::string       user;         // GitHub ユーザー名（空 = 未ログイン）
};
struct LoginJob
{
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> neverAbort{false};
    std::mutex        mu;
    std::string       user;
};
struct CloneJob
{
    std::atomic<int>  state{0};     // 0=待機 1=実行中 2=成功 3=失敗
    std::mutex        mu;
    std::string       output;
    std::string       repoDir;
};

struct State
{
    bool  inited = false;
    Tab   tab = kRecent;
    Tab   prevTab = kRecent;
    float tabT = 0.0f;              // タブ切替からの経過秒（入場アニメ）
    double time = 0.0;
    bool  anim = true;              // 背景アニメ・入場アニメ・補間（設定で切る）

    // ---- 動き ----
    L::ParticleField particles;
    L::SpringCD      navSpring;     // ナビの選択帯の Y
    bool             navSpringInit = false;
    ImVec2           parallax = ImVec2(0, 0);
    std::unordered_map<ImGuiID, float> anim01;
    float            panelSlide = 0.0f;

    // ---- 画像 ----
    std::unordered_map<std::string, LauncherImage> images;
    std::unordered_set<std::string> imagesFailed;
    int loadBudget = 0;

    // ---- 最近 ----
    std::vector<RecentView> recents;
    std::vector<L::RecentRecord> recs;        // recents と同順（FilterRecents 用）
    bool recentsDirty = true;
    std::array<char, 160> search{};
    std::string selKey;
    ProjectMeta meta;
    std::string confirmRemoveKey;             // 「一覧から削除」の確認中
    double flashUntil = 0.0;
    std::string flashText;

    // ---- 新規 ----
    int  tmpl = 0;
    std::array<char, 160> name{};
    std::array<char, 520> location{};
    int  shadowQ = 1;
    bool vsync = true;
    bool gitInit = false;
    bool formInited = false;
    L::Validation val;
    std::string   valKey;
    double        valStamp = 0.0;
    bool          valPending = true;      // 入力が変わってから検証が走るまでの間 true（作成ボタンを押させない）
    bool          valForce = false;       // 待たずに検証する

    // ---- 開く ----
    std::array<char, 520> openPath{};
    ProjectInfo openInfo;
    bool        openOk = false;
    std::string openErr;
    std::string openKey;

    // ---- クローン ----
    std::array<char, 520> cloneUrl{};
    std::array<char, 520> cloneParent{};
    L::Validation cloneVal;
    std::string   cloneKey;

    // ---- 環境（git / gh / ログイン）----
    EnvProbe*  env = nullptr;
    LoginJob*  login = nullptr;
    CloneJob*  clone = nullptr;
    bool       gitAvail = false, ghAvail = false;
    bool       envReady = false;          // git / gh の確認が 1 回終わった
    std::string ghUser;

    // ---- ニュース ----
    std::vector<L::NewsBlock> news;

    // ---- 検証用 ----
    int         renderedFrames = 0;
    int         lastRenderImGuiFrame = -100;
    std::string debugOpenPath;
};

State& St()
{
    static State* s = new State();   // 終了時に破棄しない（スレッドとの競合を避ける）
    return *s;
}

// ===========================================================================
// アニメーション部品
// ===========================================================================
// id ごとの 0..1 値を目標へ寄せる。アニメ OFF は即座。
float Ease(State& s, ImGuiID id, float target, float halfLife = 0.06f)
{
    auto it = s.anim01.find(id);
    if (it == s.anim01.end()) it = s.anim01.emplace(id, target).first;   // 初回は目標から始める（現れた瞬間に光らない）
    float& v = it->second;
    const float dt = ImGui::GetIO().DeltaTime;
    v = s.anim ? L::SmoothTo(v, target, (std::min)(dt, 0.1f), halfLife) : target;
    return v;
}

// ===========================================================================
// 描画の部品
// ===========================================================================
void GlowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, const ImVec4& c, float strength, float spreadPx)
{
    if (strength <= 0.003f) return;
    constexpr int kLayers = 7;
    for (int i = 0; i < kLayers; ++i)
    {
        const float t = static_cast<float>(i + 1) / kLayers;          // 0..1 外へ
        const float e = spreadPx * t;
        const float alpha = strength * (1.0f - t) * (1.0f - t) * 0.42f;
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), U(c, alpha), rounding + e,
                    0, (std::max)(1.0f, spreadPx / kLayers * 1.6f));
    }
}

// やわらかい放射グロー。頂点色で中心 → 外側へ滑らかに減衰させる（同心円の重ねだと輪が見える）。
// 減衰は (1-t)^2.2 を 7 本のリングで近似し、リング間は頂点色の補間に任せる。
void GlowCircle(ImDrawList* dl, ImVec2 center, float radius, const ImVec4& c, float strength)
{
    if (strength <= 0.003f || radius <= 1.0f) return;
    constexpr int kSeg = 40, kRings = 7;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(kSeg * 3 + (kRings - 1) * kSeg * 6, 1 + kRings * kSeg);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(center, uv, U(c, strength * 0.50f));
    for (int r = 1; r <= kRings; ++r)
    {
        const float t = static_cast<float>(r) / kRings;
        const float a = strength * 0.50f * std::pow(1.0f - t, 2.2f);
        const ImU32 col = U(c, a);
        for (int i = 0; i < kSeg; ++i)
        {
            const float ang = 6.2831853f * static_cast<float>(i) / kSeg;
            dl->PrimWriteVtx(ImVec2(center.x + std::cos(ang) * radius * t, center.y + std::sin(ang) * radius * t), uv, col);
        }
    }
    auto vid = [&](int ring, int i) { return static_cast<ImDrawIdx>(base + 1 + (ring - 1) * kSeg + (i % kSeg)); };
    for (int i = 0; i < kSeg; ++i)
    {
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(vid(1, i)); dl->PrimWriteIdx(vid(1, i + 1));
    }
    for (int r = 1; r < kRings; ++r)
        for (int i = 0; i < kSeg; ++i)
        {
            dl->PrimWriteIdx(vid(r, i));     dl->PrimWriteIdx(vid(r + 1, i));     dl->PrimWriteIdx(vid(r + 1, i + 1));
            dl->PrimWriteIdx(vid(r, i));     dl->PrimWriteIdx(vid(r + 1, i + 1)); dl->PrimWriteIdx(vid(r, i + 1));
        }
}

// 画像を矩形へ cover で貼る（はみ出しは切る）。zoom>1 でゆっくり寄る。
void ImageCover(ImDrawList* dl, const LauncherImage& im, ImVec2 a, ImVec2 b, float rounding, ImDrawFlags flags,
                float zoom, ImU32 tint, ImVec2 focus = ImVec2(0.5f, 0.5f))
{
    const float rw = b.x - a.x, rh = b.y - a.y;
    if (rw <= 0 || rh <= 0 || im.w <= 0 || im.h <= 0) return;
    const float ir = static_cast<float>(im.w) / static_cast<float>(im.h), rr = rw / rh;
    float uw = 1.0f, uh = 1.0f;
    if (ir > rr) uw = rr / ir; else uh = ir / rr;
    uw /= zoom; uh /= zoom;
    const float u0 = (1.0f - uw) * focus.x, v0 = (1.0f - uh) * focus.y;
    dl->AddImageRounded(static_cast<ImTextureID>(im.id), a, b, ImVec2(u0, v0), ImVec2(u0 + uw, v0 + uh), tint, rounding, flags);
}

// 縦グラデ（角丸つき）。上下の角丸帯 + 中央のグラデ矩形で作る。
void GradientRounded(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImDrawFlags flags, ImU32 top, ImU32 bottom)
{
    const bool rt = (flags & ImDrawFlags_RoundCornersTop) != 0 || flags == 0 || (flags & ImDrawFlags_RoundCornersAll) == ImDrawFlags_RoundCornersAll;
    const bool rb = (flags & ImDrawFlags_RoundCornersBottom) != 0 || flags == 0 || (flags & ImDrawFlags_RoundCornersAll) == ImDrawFlags_RoundCornersAll;
    const float rt_ = rt ? r : 0.0f, rb_ = rb ? r : 0.0f;
    if (rt_ > 0) dl->AddRectFilled(a, ImVec2(b.x, a.y + rt_ * 2.0f), top, r, ImDrawFlags_RoundCornersTop);
    if (rb_ > 0) dl->AddRectFilled(ImVec2(a.x, b.y - rb_ * 2.0f), b, bottom, r, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + rt_), ImVec2(b.x, b.y - rb_), top, top, bottom, bottom);
}

// 点線の矩形（新規作成タイル用）。
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap, float thick)
{
    auto seg = [&](ImVec2 p0, ImVec2 p1)
    {
        const float len = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
        if (len <= 0) return;
        const ImVec2 d((p1.x - p0.x) / len, (p1.y - p0.y) / len);
        for (float t = 0; t < len; t += dash + gap)
        {
            const float e = (std::min)(t + dash, len);
            dl->AddLine(ImVec2(p0.x + d.x * t, p0.y + d.y * t), ImVec2(p0.x + d.x * e, p0.y + d.y * e), col, thick);
        }
    };
    seg(a, ImVec2(b.x, a.y)); seg(ImVec2(b.x, a.y), b); seg(b, ImVec2(a.x, b.y)); seg(ImVec2(a.x, b.y), a);
}

void Spinner(ImDrawList* dl, ImVec2 c, float r, const ImVec4& col, double t)
{
    const float a0 = static_cast<float>(t) * 5.2f;
    dl->PathClear();
    dl->PathArcTo(c, r, a0, a0 + 4.3f, 24);
    dl->PathStroke(U(col), 0, P(2.2f));
}

void Icon(ImVec2 center, const char* glyph, const ImVec4& col, float pxLogical, float alpha = 1.0f)
{
    ui::DrawIconCentered(ImGui::GetWindowDrawList(), glyph, center, U(col, alpha), P(pxLogical));
}

// チップ（小さな丸角ラベル）。戻りは幅。
float Chip(ImVec2 pos, const char* text, const ImVec4& tint, bool draw = true)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = TextW(FBody(), 12.5f, text) + P(20.0f);
    const float h = P(24.0f);
    if (draw)
    {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), U(tint, 0.13f), h * 0.5f);
        dl->AddRect(ImVec2(pos.x + 0.5f, pos.y + 0.5f), ImVec2(pos.x + w - 0.5f, pos.y + h - 0.5f), U(tint, 0.34f), h * 0.5f, 0, 1.0f);
        dl->AddText(FBody(), P(12.5f), ImVec2(pos.x + P(10.0f), pos.y + (h - P(12.5f)) * 0.5f - P(0.5f)), U(Mix(tint, White(), 0.55f)), text);
    }
    return w;
}

// ---- ボタン ----
enum class BtnKind { Primary, Secondary, Ghost, Danger };

bool Btn(State& s, const char* id, const char* icon, const char* label, ImVec2 p, ImVec2 sz, BtnKind kind,
         bool enabled = true, const char* tip = nullptr, float fontPx = 14.0f)
{
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, sz) && enabled;
    const bool hov = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive() && enabled;
    if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Ease(s, ImGui::GetItemID(), (hov && enabled) ? 1.0f : 0.0f, 0.05f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = P(8.0f);
    const ImVec2 a = p, b(p.x + sz.x, p.y + sz.y);

    ImVec4 textCol = th::Text;
    if (!enabled)
    {
        dl->AddRectFilled(a, b, U(th::Bg3, 0.45f), r);
        dl->AddRect(a, b, U(White(), 0.05f), r);
        textCol = th::TextFaint;
    }
    else if (kind == BtnKind::Primary)
    {
        GlowRect(dl, a, b, r, th::Accent, 0.55f * h, P(14.0f));
        const ImVec4 base = held ? th::AccentPressed : Mix(th::Accent, th::AccentHover, h);
        dl->AddRectFilled(a, b, U(base), r);
        // 上端のうっすらとしたハイライト（面に厚みを出す）
        dl->AddRectFilledMultiColor(ImVec2(a.x + r, a.y + 1.0f), ImVec2(b.x - r, a.y + sz.y * 0.5f),
                                    U(White(), 0.16f), U(White(), 0.16f), U(White(), 0.0f), U(White(), 0.0f));
        textCol = White();
    }
    else if (kind == BtnKind::Danger)
    {
        dl->AddRectFilled(a, b, U(th::Bad, 0.06f + 0.10f * h + (held ? 0.08f : 0.0f)), r);
        dl->AddRect(a, b, U(th::Bad, 0.45f + 0.35f * h), r);
        textCol = th::Bad;
    }
    else if (kind == BtnKind::Secondary)
    {
        dl->AddRectFilled(a, b, U(Mix(th::Bg3, th::Bg4, h), 0.85f), r);
        dl->AddRect(a, b, U(Mix(White(), th::AccentHover, h), 0.10f + 0.5f * h), r);
        textCol = Mix(th::Text, White(), h);
    }
    else   // Ghost
    {
        if (h > 0.01f) dl->AddRectFilled(a, b, U(White(), 0.06f * h + (held ? 0.04f : 0.0f)), r);
        textCol = Mix(th::TextDim, th::Text, h);
    }

    // 中身（アイコン + ラベルを中央寄せ）
    const float iconW = icon ? P(fontPx + 4.0f) : 0.0f;
    const float gapW = (icon && label && *label) ? P(6.0f) : 0.0f;
    const float labW = (label && *label) ? TextW(FBold(), fontPx, label) : 0.0f;
    float x = p.x + (sz.x - (iconW + gapW + labW)) * 0.5f;
    const float cy = p.y + sz.y * 0.5f + (held ? P(0.5f) : 0.0f);
    if (icon) { Icon(ImVec2(x + iconW * 0.5f, cy), icon, textCol, fontPx + 1.0f); x += iconW + gapW; }
    if (label && *label)
        dl->AddText(FBold(), P(fontPx), ImVec2(x, cy - P(fontPx) * 0.5f - P(0.5f)), U(textCol), label);

    if (tip && *tip && hov && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 丸いアイコンボタン（ピン留め・コピー等）。
bool IconBtn(State& s, const char* id, const char* glyph, ImVec2 center, float dia, const ImVec4& tint, bool active,
             const char* tip = nullptr, float alpha = 1.0f)
{
    ImGui::SetCursorScreenPos(ImVec2(center.x - dia * 0.5f, center.y - dia * 0.5f));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(dia, dia));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Ease(s, ImGui::GetItemID(), hov ? 1.0f : 0.0f, 0.05f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(center, dia * 0.5f, U(active ? th::Accent : Black(), (active ? 0.9f : 0.55f + 0.2f * h) * alpha), 28);
    dl->AddCircle(center, dia * 0.5f, U(active ? th::AccentHover : White(), (0.14f + 0.35f * h) * alpha), 28, 1.0f);
    Icon(center, glyph, active ? White() : Mix(th::TextDim, tint, h), dia / (std::max)(ui::Scale(), 0.01f) * 0.46f, alpha);   // Icon() は論理 px を取る（dia は物理）
    if (tip && *tip && hov && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 入力欄（高さ 34px）。ui:: の見た目をそのまま使い、余白だけ広げる。
bool Input(const char* id, char* buf, size_t size, ImVec2 p, float w, const char* hint = nullptr, ImGuiInputTextFlags flags = 0)
{
    const float h = P(36.0f);
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemWidth(w);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(12.0f), (h - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, P(8.0f));
    const bool ch = hint ? ui::InputTextWithHint(id, hint, buf, size, flags) : ui::InputText(id, buf, size, flags);
    ImGui::PopStyleVar(2);
    return ch;
}

void Label(ImVec2 p, const char* text)
{
    DText(FBody(), 12.5f, p, U(th::TextDim), text);
}

// ---- 検証結果の表示。戻りは使った高さ。----
float DrawIssues(ImVec2 p, float w, const L::Validation& v, bool showEmptyOk = false, const char* okText = nullptr)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y;
    const auto sorted = v.Sorted();
    if (sorted.empty() && showEmptyOk && okText)
    {
        Icon(ImVec2(p.x + P(8.0f), y + P(10.0f)), ICON_OK, th::Good, 15.0f);
        DText(FBody(), 13.0f, ImVec2(p.x + P(24.0f), y + P(2.5f)), U(th::Good), okText);
        return P(22.0f);
    }
    for (const auto& is : sorted)
    {
        const ImVec4& c = is.sev == L::Severity::Error ? th::Bad : (is.sev == L::Severity::Warn ? th::Warn : th::Accent);
        const char* g = is.sev == L::Severity::Error ? ICON_ERROR : (is.sev == L::Severity::Warn ? ICON_WARN : ICON_INFO);
        const float tw = w - P(26.0f);
        const ImVec2 ts = FBody()->CalcTextSizeA(P(13.0f), FLT_MAX, tw, is.message.c_str());
        Icon(ImVec2(p.x + P(8.0f), y + P(10.0f)), g, c, 15.0f);
        dl->AddText(FBody(), P(13.0f), ImVec2(p.x + P(24.0f), y + P(2.5f)), U(Mix(c, White(), is.sev == L::Severity::Info ? 0.5f : 0.35f)),
                    is.message.c_str(), nullptr, tw);
        y += (std::max)(P(22.0f), ts.y + P(8.0f));
    }
    return y - p.y;
}

// ===========================================================================
// 画像
// ===========================================================================
const LauncherImage* GetImage(State& s, const LauncherHost& host, const std::string& abs)
{
    auto it = s.images.find(abs);
    if (it != s.images.end()) return it->second.id ? &it->second : nullptr;
    if (s.imagesFailed.count(abs)) return nullptr;
    if (!host.loadImage || s.loadBudget <= 0) return nullptr;      // 1 フレームに読む枚数を絞る（ヒッチ防止）
    --s.loadBudget;
    LauncherImage im = host.loadImage(abs);
    if (im.id == 0) { s.imagesFailed.insert(abs); return nullptr; }
    return &s.images.emplace(abs, im).first->second;
}

std::string LauncherAsset(const LauncherHost& host, const char* name)
{
    return host.assetsDir + "editor/launcher/" + name;
}

// ===========================================================================
// 最近のプロジェクト
// ===========================================================================
bool HasProjectFile(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".dx12proj") return true;
    return false;
}

void ReloadRecents(State& s)
{
    s.recentsDirty = false;
    s.recs = ProjectManager::LoadRecents();
    s.recents.clear();
    s.recents.reserve(s.recs.size());
    for (const auto& r : s.recs)
    {
        RecentView v;
        v.rec = r;
        v.key = L::PathKey(r.path);
        v.exists = HasProjectFile(L::PathFromUtf8(r.path));
        v.thumbPath = L::ThumbnailPath(r.path);
        std::error_code ec;
        v.hasThumb = v.exists && fs::exists(L::PathFromUtf8(v.thumbPath), ec);
        s.recents.push_back(std::move(v));
    }
    // 画像キャッシュのうちサムネイルは捨てる（更新されているかもしれない。ホスト側が更新日時でキーを分ける）
    for (auto it = s.images.begin(); it != s.images.end();)
        it = (it->first.find("/.dx12/thumbnail.png") != std::string::npos) ? s.images.erase(it) : std::next(it);
    for (auto it = s.imagesFailed.begin(); it != s.imagesFailed.end();)
        it = (it->find("/.dx12/thumbnail.png") != std::string::npos) ? s.imagesFailed.erase(it) : std::next(it);
    s.meta.loaded = false;
    Logger::Info("ランチャー: 最近のプロジェクト {} 件（保存先 {}）", s.recents.size(), ProjectManager::DataDir());
    // 選択が無い/消えたら先頭（最新）を選ぶ
    bool found = false;
    for (const auto& v : s.recents) if (v.key == s.selKey) found = true;
    if (!found) s.selKey = s.recents.empty() ? std::string() : s.recents.front().key;
}

RecentView* FindRecent(State& s, const std::string& key)
{
    for (auto& v : s.recents) if (v.key == key) return &v;
    return nullptr;
}

void LoadMeta(State& s, const RecentView& rv)
{
    s.meta = ProjectMeta{};
    s.meta.loaded = true;
    s.meta.key = rv.key;
    if (!rv.exists) return;
    std::error_code ec;
    for (auto it = fs::directory_iterator(L::PathFromUtf8(rv.rec.path), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        if (it->path().extension() != ".dx12proj") continue;
        s.meta.projectFile = L::PathToUtf8(it->path().filename());
        std::ifstream f(it->path(), std::ios::binary);
        const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
        if (j.is_object())
        {
            s.meta.engineVersion = j.value("version", std::string());
            s.meta.defaultScene  = j.value("defaultScene", j.value("startScene", std::string()));
        }
        break;
    }
}

bool OpenRecent(RecentView& rv, ProjectInfo& out)
{
    std::string err;
    if (!rv.exists || !ProjectManager::ResolveProjectPath(rv.rec.path, out, err))
    {
        Logger::Warn("ランチャー: プロジェクトを開けません: {} ({})", rv.rec.path, err);
        return false;
    }
    ProjectManager::AddToRecents(out);
    return true;
}

// ===========================================================================
// 非同期: 環境の確認 / GitHub ログイン / クローン
// ===========================================================================
void KickEnvProbe(State& s)
{
    if (!s.env) s.env = new EnvProbe();
    if (!s.login) s.login = new LoginJob();
    if (!s.clone) s.clone = new CloneJob();
    if (s.env->running.load()) return;
    s.env->running.store(true);
    s.env->done.store(false);
    std::thread([e = s.env]()
    {
        CrashHandler::PrepareThread();
        const bool git = GitIntegration::IsGitAvailable();
        const bool gh  = GitIntegration::IsGhAvailable();
        std::string user;
        if (gh) user = GitIntegration::GitHubUser();
        {
            std::lock_guard<std::mutex> lk(e->mu);
            e->user = user;
        }
        e->git.store(git);
        e->gh.store(gh);
        e->done.store(true);
        e->running.store(false);
    }).detach();
}

void PumpAsync(State& s)
{
    if (s.env && s.env->done.load())
    {
        s.env->done.store(false);
        s.gitAvail = s.env->git.load();
        s.ghAvail  = s.env->gh.load();
        s.envReady = true;
        std::lock_guard<std::mutex> lk(s.env->mu);
        s.ghUser = s.env->user;
    }
    if (s.login && s.login->done.load())
    {
        s.login->done.store(false);
        s.login->running.store(false);
        std::lock_guard<std::mutex> lk(s.login->mu);
        s.ghUser = s.login->user;
    }
}

void StartLogin(State& s)
{
    if (!s.login || s.login->running.load()) return;
    s.login->running.store(true);
    s.login->done.store(false);
    std::thread([l = s.login]()
    {
        CrashHandler::PrepareThread();
        auto r = GitIntegration::LoginAndWait(l->neverAbort);
        {
            std::lock_guard<std::mutex> lk(l->mu);
            l->user = r.output;
        }
        l->done.store(true);
    }).detach();
}

void StartClone(State& s, const std::string& url, const std::string& parent)
{
    if (!s.clone || s.clone->state.load() == 1) return;
    {
        std::lock_guard<std::mutex> lk(s.clone->mu);
        s.clone->output.clear();
        s.clone->repoDir.clear();
    }
    s.clone->state.store(1);
    std::thread([c = s.clone, url, parent]()
    {
        CrashHandler::PrepareThread();
        std::string repoDir;
        // 保存先の親フォルダが無いと git を起動できない（作業ディレクトリに指定するため）。先に作る。
        { std::error_code ec; fs::create_directories(L::PathFromUtf8(parent), ec); }
        auto r = GitIntegration::Clone(url, parent, repoDir);
        {
            std::lock_guard<std::mutex> lk(c->mu);
            c->output = r.output;
            c->repoDir = repoDir;
        }
        c->state.store(r.ok() ? 2 : 3);
    }).detach();
}

// ===========================================================================
// 初期化
// ===========================================================================
void EnsureInit(State& s, const LauncherHost& host)
{
    (void)host;
    if (s.inited) return;
    s.inited = true;
    s.anim = ProjectManager::GetEditorBool("launcherAnimations", SystemAnimationsEnabled());
    s.news = L::ParseNewsBody(kWhatsNewBody);
    const std::string lastTmpl = ProjectManager::GetEditorString("launcherLastTemplate", "fps");
    for (size_t i = 0; i < L::Templates().size(); ++i) if (lastTmpl == L::Templates()[i].id) s.tmpl = static_cast<int>(i);
    KickEnvProbe(s);
}

void InitForm(State& s)
{
    if (s.formInited) return;
    s.formInited = true;
    std::string loc = ProjectManager::GetEditorString("launcherLastLocation", "");
    if (loc.empty())
    {
        const std::string docs = DocumentsDir();
        loc = docs.empty() ? std::string() : L::JoinPath(docs, "UnoProjects");
    }
    SetBuf(s.location, loc);
    SetBuf(s.name, L::SuggestUniqueName(loc, "MyGame", L::RealFs()));
    SetBuf(s.cloneParent, loc);
    s.valKey.clear();
    s.valPending = true;
    s.valForce = true;    // 最初の検証は待たずに走らせる
}

// 入力のたびに走らせない（ディスクを叩くため）。入力が止まって少し経ってから検証する。
void UpdateFormValidation(State& s, double now)
{
    const std::string key = GetBuf(s.name) + "\n" + GetBuf(s.location);
    if (key != s.valKey) { s.valKey = key; s.valPending = true; s.valStamp = now; }
    if (s.valPending && (s.valForce || now - s.valStamp > 0.18))
    {
        s.valPending = false;
        s.valForce = false;
        s.val = L::ValidateNewProject(GetBuf(s.name), GetBuf(s.location), L::RealFs(), s.recs);
    }
}

void SwitchTab(State& s, Tab t)
{
    if (t == s.tab) return;
    s.prevTab = s.tab;
    s.tab = t;
    s.tabT = 0.0f;
    s.confirmRemoveKey.clear();
    if (t == kNew || t == kClone) InitForm(s);
    if (t == kRecent) s.recentsDirty = true;
}

// ===========================================================================
// 背景（ヒーロー画像・グラデ・ゆるやかな光・粒）
// ===========================================================================
void DrawBackground(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dt = (std::min)(ImGui::GetIO().DeltaTime, 0.1f);
    const ImVec2 E(O.x + Sz.x, O.y + Sz.y);

    // 土台: 深い青みのグラデ（Bg0 へ落ちる）
    const ImVec4 topL = Mix(th::Bg0, th::Hex(0x0A1630), 1.0f);
    const ImVec4 topR = Mix(th::Bg0, th::Hex(0x0B1A3A), 1.0f);
    // ヒーロー画像の高さまでで Bg0 へ落とす（画像の下端が同じ色になり、境目の線が出ない）
    const float baseH = (std::min)(Sz.y, P(560.0f));
    dl->AddRectFilled(O, E, U(th::Bg0));
    dl->AddRectFilledMultiColor(O, ImVec2(E.x, O.y + baseH), U(topL), U(topR), U(th::Bg0), U(th::Bg0));

    // ヒーロー画像（上部に敷き、下へ溶かす）。マウスで ±10px だけ視差。
    if (s.anim)
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float nx = Clamp((mp.x - O.x) / (std::max)(Sz.x, 1.0f) - 0.5f, -0.5f, 0.5f);
        const float ny = Clamp((mp.y - O.y) / (std::max)(Sz.y, 1.0f) - 0.5f, -0.5f, 0.5f);
        s.parallax.x = L::SmoothTo(s.parallax.x, nx, dt, 0.35f);
        s.parallax.y = L::SmoothTo(s.parallax.y, ny, dt, 0.35f);
    }
    else s.parallax = ImVec2(0, 0);

    const float heroH = (std::min)(Sz.y, P(560.0f));
    if (const LauncherImage* hero = GetImage(s, host, LauncherAsset(host, "hero.png")))
    {
        const float over = P(24.0f);
        const ImVec2 a(O.x - over + s.parallax.x * -P(20.0f), O.y - over + s.parallax.y * -P(12.0f));
        const ImVec2 b(E.x + over + s.parallax.x * -P(20.0f), O.y + heroH + over + s.parallax.y * -P(12.0f));
        dl->PushClipRect(O, ImVec2(E.x, O.y + heroH), true);
        ImageCover(dl, *hero, a, b, 0.0f, 0, 1.0f, U(White(), 0.46f), ImVec2(0.5f, 0.35f));
        dl->PopClipRect();
        // 下へ溶かす + 左（ナビ側）を暗く
        dl->AddRectFilledMultiColor(O, ImVec2(E.x, O.y + heroH),
                                    U(th::Bg0, 0.42f), U(th::Bg0, 0.42f), U(th::Bg0, 1.0f), U(th::Bg0, 1.0f));
        dl->AddRectFilledMultiColor(O, ImVec2(O.x + Sz.x * 0.45f, O.y + heroH),
                                    U(th::Bg0, 0.55f), U(th::Bg0, 0.0f), U(th::Bg0, 0.0f), U(th::Bg0, 0.55f));
    }

    // ゆっくり漂う光（3 つ）。アニメ OFF のときは静止位置。
    const float t = s.anim ? static_cast<float>(s.time) : 0.0f;
    struct Orb { float x, y, r, ph, sp, a; ImVec4 c; };
    const Orb orbs[3] = {
        { 0.18f, 0.16f, 560.0f, 0.0f, 0.070f, 0.50f, th::Accent },
        { 0.78f, 0.30f, 680.0f, 2.1f, 0.055f, 0.38f, Mix(th::Accent, th::Hex(0x6A5CFF), 0.55f) },
        { 0.55f, 0.98f, 760.0f, 4.0f, 0.045f, 0.26f, Mix(th::Accent, th::Hex(0x21D0C4), 0.45f) },
    };
    for (const Orb& o : orbs)
    {
        const ImVec2 c(O.x + Sz.x * o.x + std::cos(t * o.sp * 6.2831f + o.ph) * P(70.0f),
                       O.y + Sz.y * o.y + std::sin(t * o.sp * 6.2831f * 0.8f + o.ph) * P(46.0f));
        GlowCircle(dl, c, P(o.r), o.c, o.a);
    }

    // 粒（うっすら上へ漂う）
    if (s.anim)
    {
        const float lw = Sz.x / (std::max)(ui::Scale(), 0.01f), lh = Sz.y / (std::max)(ui::Scale(), 0.01f);
        if (s.particles.p.empty()) s.particles.Reset(20260930u, 46, lw, lh);
        s.particles.Resize(lw, lh);
        s.particles.Update(dt);
        for (const auto& q : s.particles.p)
        {
            const float tw = s.particles.Twinkle(q);
            const ImVec2 pos(O.x + (q.x + s.parallax.x * -q.depth * 28.0f) * ui::Scale(), O.y + (q.y + s.parallax.y * -q.depth * 18.0f) * ui::Scale());
            const float a = (0.10f + 0.42f * q.depth) * tw;
            dl->AddCircleFilled(pos, P(q.size), U(Mix(th::AccentHover, White(), 0.35f), a), 10);
        }
    }

    // 上端のキャプション帯（ウィンドウ操作の背景）。うっすら暗く。
    dl->AddRectFilledMultiColor(O, ImVec2(E.x, O.y + P(44.0f)), U(Black(), 0.32f), U(Black(), 0.32f), U(Black(), 0.0f), U(Black(), 0.0f));
}

// ===========================================================================
// 左ナビ
// ===========================================================================
struct NavItem { Tab tab; const char* icon; const char* label; };

void DrawNav(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz, float navW)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(O.x, O.y), b(O.x + navW, O.y + Sz.y);
    // ガラス面 + 右の細い境界
    dl->AddRectFilled(a, b, U(th::Bg0, 0.66f));
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + P(220.0f)), U(th::Accent, 0.06f), U(th::Accent, 0.02f), U(th::Accent, 0.0f), U(th::Accent, 0.0f));
    dl->AddLine(ImVec2(b.x - 0.5f, a.y), ImVec2(b.x - 0.5f, b.y), U(White(), 0.07f));

    // ブランド
    {
        const float logo = P(42.0f);
        const ImVec2 lp(O.x + P(22.0f), O.y + P(46.0f));
        GlowCircle(dl, ImVec2(lp.x + logo * 0.5f, lp.y + logo * 0.5f), P(52.0f), th::Accent, 0.55f);
        if (host.logo)
            dl->AddImageRounded(static_cast<ImTextureID>(host.logo), lp, ImVec2(lp.x + logo, lp.y + logo), ImVec2(0, 0), ImVec2(1, 1),
                                U(White()), logo * 0.22f);
        else
            dl->AddRectFilled(lp, ImVec2(lp.x + logo, lp.y + logo), U(th::Bg2), logo * 0.22f);
        DText(FBold(), 20.0f, ImVec2(lp.x + logo + P(12.0f), lp.y + P(1.0f)), U(White()), kEngineName);
        char ver[48];
        std::snprintf(ver, sizeof(ver), "v%s", kEngineVersion);
        DText(FMono(), 12.5f, ImVec2(lp.x + logo + P(12.0f), lp.y + P(24.0f)), U(th::TextDim), ver);
    }

    static const NavItem kItems[] = {
        { kRecent, ICON_CLOCK,       "最近のプロジェクト" },
        { kNew,    ICON_FOLDER_PLUS, "新規プロジェクト" },
        { kOpen,   ICON_FOLDER_OPEN, "プロジェクトを開く" },
        { kClone,  ICON_GIT_BRANCH,  "Git からクローン" },
        { kNews,   ICON_NEWSPAPER,   "ニュースと学習" },
    };
    const float itemH = P(46.0f), padX = P(14.0f);
    const float y0 = O.y + P(132.0f);
    const float gap = P(4.0f);

    // 選択帯（スプリングで滑る。行き過ぎない）
    const float targetY = y0 + static_cast<int>(s.tab) * (itemH + gap);
    if (!s.navSpringInit) { s.navSpring.Snap(targetY); s.navSpringInit = true; }
    if (s.anim) s.navSpring.Step(targetY, 22.0f, (std::min)(ImGui::GetIO().DeltaTime, 0.1f)); else s.navSpring.Snap(targetY);
    {
        const ImVec2 ba(O.x + padX, s.navSpring.x), bb(O.x + navW - padX, s.navSpring.x + itemH);
        GlowRect(dl, ba, bb, P(9.0f), th::Accent, 0.28f, P(12.0f));
        dl->AddRectFilledMultiColor(ba, bb, U(th::Accent, 0.24f), U(th::Accent, 0.10f), U(th::Accent, 0.10f), U(th::Accent, 0.24f));
        dl->AddRect(ba, bb, U(th::Accent, 0.38f), P(9.0f));
        dl->AddRectFilled(ImVec2(ba.x, ba.y + P(9.0f)), ImVec2(ba.x + P(3.0f), bb.y - P(9.0f)), U(th::AccentHover), P(2.0f));
    }

    for (int i = 0; i < 5; ++i)
    {
        const NavItem& it = kItems[i];
        const ImVec2 p(O.x + padX, y0 + i * (itemH + gap));
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##nav", ImVec2(navW - padX * 2.0f, itemH));
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float h = Ease(s, ImGui::GetItemID(), hov ? 1.0f : 0.0f, 0.06f);
        ImGui::PopID();
        const bool sel = (s.tab == it.tab);
        if (!sel && h > 0.01f)
            dl->AddRectFilled(p, ImVec2(p.x + navW - padX * 2.0f, p.y + itemH), U(White(), 0.06f * h), P(9.0f));
        const ImVec4 col = sel ? White() : Mix(th::TextMid, White(), h * 0.6f);
        Icon(ImVec2(p.x + P(26.0f), p.y + itemH * 0.5f), it.icon, sel ? th::AccentHover : Mix(th::TextDim, th::AccentHover, h * 0.7f), 18.0f);
        dl->AddText(sel ? FBold() : FBody(), P(15.5f), ImVec2(p.x + P(50.0f), p.y + itemH * 0.5f - P(9.0f)), U(col), it.label);
        if (clicked) SwitchTab(s, it.tab);
    }

    // ---- 下部: GitHub ログイン状態 / アニメ切替 ----
    {
        const float bx = O.x + padX, bw = navW - padX * 2.0f;
        float y = O.y + Sz.y - P(20.0f);

        // アニメーション切替
        {
            y -= P(34.0f);
            const ImVec2 p(bx, y);
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID("##anim");
            const bool clicked = ImGui::InvisibleButton("##t", ImVec2(bw, P(34.0f)));
            const bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const float h = Ease(s, ImGui::GetItemID(), hov ? 1.0f : 0.0f, 0.06f);
            const float on = Ease(s, ImGui::GetID("##on"), s.anim ? 1.0f : 0.0f, 0.07f);
            ImGui::PopID();
            if (h > 0.01f) dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + P(34.0f)), U(White(), 0.05f * h), P(8.0f));
            DText(FBody(), 13.0f, ImVec2(p.x + P(10.0f), p.y + P(9.0f)), U(Mix(th::TextDim, th::Text, h)), "背景アニメーション");
            // スイッチ
            const float sw = P(34.0f), sh = P(18.0f);
            const ImVec2 sp(p.x + bw - sw - P(8.0f), p.y + (P(34.0f) - sh) * 0.5f);
            dl->AddRectFilled(sp, ImVec2(sp.x + sw, sp.y + sh), U(Mix(th::Bg4, th::Accent, on)), sh * 0.5f);
            dl->AddCircleFilled(ImVec2(sp.x + sh * 0.5f + (sw - sh) * on, sp.y + sh * 0.5f), sh * 0.5f - P(2.5f), U(White()), 20);
            if (clicked)
            {
                s.anim = !s.anim;
                ProjectManager::SetEditorBool("launcherAnimations", s.anim);
            }
            if (hov && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("背景の光・粒・画面切替の動きをオン/オフします。\nOS の「アニメーション効果」がオフの環境では、初期値がオフになります。");
        }

        // GitHub 状態
        {
            y -= P(58.0f) + P(8.0f);
            const ImVec2 p(bx, y);
            const float h = P(58.0f);
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + h), U(White(), 0.04f), P(10.0f));
            dl->AddRect(p, ImVec2(p.x + bw, p.y + h), U(White(), 0.07f), P(10.0f));
            const ImVec2 ac(p.x + P(28.0f), p.y + h * 0.5f);
            const bool loggedIn = !s.ghUser.empty();
            const bool running = s.login && s.login->running.load();
            dl->AddCircleFilled(ac, P(15.0f), U(loggedIn ? th::Accent : th::Bg4, loggedIn ? 0.30f : 1.0f), 28);
            dl->AddCircle(ac, P(15.0f), U(loggedIn ? th::Accent : White(), loggedIn ? 0.8f : 0.12f), 28, 1.0f);
            if (loggedIn)
            {
                char ini[8] = { static_cast<char>(std::toupper(static_cast<unsigned char>(s.ghUser[0]))), 0 };
                const float iw = TextW(FBold(), 14.0f, ini);
                DText(FBold(), 14.0f, ImVec2(ac.x - iw * 0.5f, ac.y - P(8.0f)), U(White()), ini);
            }
            else Icon(ac, ICON_CIRCLE_USER, th::TextDim, 17.0f);

            const float tx = p.x + P(52.0f), tw = bw - P(52.0f) - P(8.0f);
            if (s.envReady && !s.ghAvail)
            {
                DText(FBody(), 13.0f, ImVec2(tx, p.y + P(10.0f)), U(th::Text), "GitHub");
                DText(FBody(), 11.5f, ImVec2(tx, p.y + P(30.0f)), U(th::TextFaint), FitEnd(FBody(), 11.5f, "gh が見つかりません", tw).c_str());
            }
            else if (running)
            {
                DText(FBody(), 13.0f, ImVec2(tx, p.y + P(10.0f)), U(th::Text), "ログイン待ち");
                DText(FBody(), 11.5f, ImVec2(tx, p.y + P(30.0f)), U(th::TextFaint), FitEnd(FBody(), 11.5f, "ブラウザで認証してください", tw).c_str());
                Spinner(ImGui::GetWindowDrawList(), ImVec2(p.x + bw - P(20.0f), p.y + h * 0.5f), P(7.0f), th::AccentHover, s.time);
            }
            else if (loggedIn)
            {
                const std::string who = FitEnd(FBold(), 13.0f, "@" + s.ghUser, tw);
                DText(FBold(), 13.0f, ImVec2(tx, p.y + P(10.0f)), U(th::Text), who.c_str());
                dl->AddCircleFilled(ImVec2(tx + P(4.0f), p.y + P(37.0f)), P(3.5f), U(th::Good), 12);
                DText(FBody(), 11.5f, ImVec2(tx + P(13.0f), p.y + P(30.0f)), U(th::TextDim), "ログイン中");
            }
            else
            {
                DText(FBody(), 13.0f, ImVec2(tx, p.y + P(10.0f)), U(th::Text), "GitHub");
                DText(FBody(), 11.5f, ImVec2(tx, p.y + P(30.0f)), U(th::TextFaint), "未ログイン");
                if (s.ghAvail && Btn(s, "##login", nullptr, "ログイン", ImVec2(p.x + bw - P(70.0f), p.y + (h - P(28.0f)) * 0.5f), ImVec2(P(62.0f), P(28.0f)),
                                     BtnKind::Secondary, true, "ブラウザで GitHub にログインします（gh auth login）", 12.5f))
                    StartLogin(s);
            }
        }
    }
}

// ===========================================================================
// 詳細パネルの枠（右）
// ===========================================================================
struct PanelRect { ImVec2 a, b; };

PanelRect DrawPanelFrame(State& s, ImVec2 O, ImVec2 Sz, float rightW, float topH)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float slide = s.anim ? (1.0f - L::EaseOutCubicf(s.tabT / 0.34f)) * P(26.0f) : 0.0f;
    const ImVec2 a(O.x + Sz.x - rightW + slide, O.y + topH), b(O.x + Sz.x + slide, O.y + Sz.y);
    dl->AddRectFilled(a, b, U(th::Bg0, 0.78f));
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + P(180.0f)), U(th::Accent, 0.05f), U(th::Accent, 0.05f), U(th::Accent, 0.0f), U(th::Accent, 0.0f));
    dl->AddLine(ImVec2(a.x + 0.5f, a.y), ImVec2(a.x + 0.5f, b.y), U(White(), 0.07f));
    return { a, b };
}

// ===========================================================================
// タブ: 最近
// ===========================================================================
void PageHeader(State& s, ImVec2 p, const char* title, const char* sub)
{
    (void)s;
    DText(FBold(), 30.0f, p, U(White()), title);
    if (sub && *sub) DText(FBody(), 14.0f, ImVec2(p.x + P(2.0f), p.y + P(42.0f)), U(th::TextDim), sub);
}

struct CardGeo { ImVec2 a; float w, thumbH, h; };

// 1 枚のカード。open / select / pin / remove の要求を返す。
struct CardResult { bool select = false, open = false, pin = false, remove = false, copyPath = false, explorer = false; };

CardResult DrawProjectCard(State& s, const LauncherHost& host, RecentView& rv, const CardGeo& g, int index, bool selected, float reveal)
{
    CardResult res;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(rv.key.c_str());

    const float r = P(11.0f);
    const float rev = reveal;
    const float lift = s.anim ? (1.0f - rev) * P(14.0f) : 0.0f;
    ImVec2 a(g.a.x, g.a.y + lift);
    ImVec2 b(a.x + g.w, a.y + g.h);

    // 当たり（カード全体）
    ImGui::SetCursorScreenPos(a);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(g.w, g.h));
    const bool hov = ImGui::IsItemHovered();
    const bool dbl = hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImGuiID cid = ImGui::GetItemID();
    const float h = Ease(s, cid, hov ? 1.0f : 0.0f, 0.07f);
    const float sel = Ease(s, ImGui::GetID("##sel"), selected ? 1.0f : 0.0f, 0.08f);
    if (clicked) res.select = true;
    if (dbl && rv.exists) res.open = true;

    // 右クリックメニュー
    if (ImGui::BeginPopupContextItem("##ctx"))
    {
        ui::PushMenuStyle();
        res.select = true;
        if (ui::MenuItem(ICON_PLAY, "開く", nullptr, false, rv.exists)) res.open = true;
        if (ui::MenuItem(rv.rec.pinned ? ICON_PIN_OFF : ICON_PIN, rv.rec.pinned ? "ピン留めを外す" : "ピン留めする")) res.pin = true;
        if (ui::MenuItem(ICON_FOLDER_OPEN, "エクスプローラーで表示", nullptr, false, rv.exists)) res.explorer = true;
        if (ui::MenuItem(ICON_COPY, "パスをコピー")) res.copyPath = true;
        ImGui::Separator();
        if (ui::MenuItem(ICON_TRASH, "一覧から削除")) res.remove = true;
        ui::PopMenuStyle();
        ImGui::EndPopup();
    }

    // ホバーで少し浮く
    const float up = P(3.0f) * h;
    a.y -= up; b.y -= up;

    // 影
    for (int i = 0; i < 4; ++i)
    {
        const float e = P(3.0f + i * 4.0f);
        dl->AddRectFilled(ImVec2(a.x - e * 0.4f, a.y + e * 0.9f), ImVec2(b.x + e * 0.4f, b.y + e * 1.3f), U(Black(), (0.16f - i * 0.035f) * rev), r + e * 0.5f);
    }
    // 選択 / ホバーのグロー
    GlowRect(dl, a, b, r, th::Accent, (0.10f + 0.55f * h + 0.40f * sel) * rev, P(16.0f));

    // 本体（ガラス）
    dl->AddRectFilled(a, b, U(th::Bg1, 0.86f * rev), r);

    // サムネイル
    const ImVec2 ta = a, tb(b.x, a.y + g.thumbH);
    bool drawn = false;
    if (rv.hasThumb)
    {
        if (!rv.imgTried || rv.img.id == 0)
        {
            if (const LauncherImage* im = GetImage(s, host, rv.thumbPath)) { rv.img = *im; rv.imgTried = true; }
        }
        if (rv.img.id)
        {
            ImageCover(dl, rv.img, ta, tb, r, ImDrawFlags_RoundCornersTop, 1.0f + 0.05f * h, U(White(), (rv.exists ? 1.0f : 0.35f) * rev));
            drawn = true;
        }
    }
    if (!drawn)
    {
        // サムネイルが無い: グラデ + プロジェクト名の頭文字（フォールバック）
        const uint32_t hsh = L::LHash32(static_cast<uint32_t>(std::hash<std::string>()(rv.key)));
        const float hue = static_cast<float>(hsh % 360u) / 360.0f;
        float rr, gg, bb2;
        ImGui::ColorConvertHSVtoRGB(hue, 0.42f, 0.26f, rr, gg, bb2);
        const ImVec4 c1(rr, gg, bb2, 1.0f);
        ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.10f, rr, gg, bb2);
        const ImVec4 c2(rr, gg, bb2, 1.0f);
        GradientRounded(dl, ta, tb, r, ImDrawFlags_RoundCornersTop, U(c1, rev), U(c2, rev));
        // 装飾の薄い格子
        dl->PushClipRect(ta, tb, true);
        const float step = P(28.0f);
        for (float x = ta.x + step; x < tb.x; x += step) dl->AddLine(ImVec2(x, ta.y), ImVec2(x, tb.y), U(White(), 0.03f * rev));
        for (float y = ta.y + step; y < tb.y; y += step) dl->AddLine(ImVec2(ta.x, y), ImVec2(tb.x, y), U(White(), 0.03f * rev));
        dl->PopClipRect();
        std::string ini = rv.rec.name.empty() ? "?" : rv.rec.name.substr(0, L::Utf8PrefixBytes(rv.rec.name, 1));
        for (char& ch : ini) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        const float iw = FBold()->CalcTextSizeA(P(46.0f), FLT_MAX, 0, ini.c_str()).x;
        dl->AddText(FBold(), P(46.0f), ImVec2((ta.x + tb.x) * 0.5f - iw * 0.5f, (ta.y + tb.y) * 0.5f - P(25.0f)), U(White(), 0.55f * rev), ini.c_str());
    }
    // サムネイルの下端を少し暗くして、境目を柔らかく
    dl->AddRectFilledMultiColor(ImVec2(ta.x, tb.y - P(34.0f)), tb, U(th::Bg1, 0.0f), U(th::Bg1, 0.0f), U(th::Bg1, 0.55f * rev), U(th::Bg1, 0.55f * rev));

    // ホバー: 中央に「開く」の丸
    if (rv.exists && h > 0.02f)
    {
        const ImVec2 c((ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f);
        const float d = P(46.0f) * (0.82f + 0.18f * h);
        dl->AddRectFilled(ta, tb, U(Black(), 0.30f * h), r, ImDrawFlags_RoundCornersTop);
        GlowCircle(dl, c, d * 1.7f, th::Accent, 0.7f * h);
        dl->AddCircleFilled(c, d * 0.5f, U(th::Accent, 0.92f * h), 32);
        Icon(ImVec2(c.x + P(1.5f), c.y), ICON_PLAY, White(), 19.0f * (0.85f + 0.15f * h), h);
    }

    // 枠線（通常はうっすら / ホバー・選択でアクセント）
    const float bo = Clamp(h * 0.7f + sel, 0.0f, 1.0f);
    dl->AddRect(a, b, U(Mix(White(), th::AccentHover, bo), (0.075f + 0.75f * bo) * rev), r, 0, sel > 0.5f ? P(1.6f) : 1.0f);

    // ---- テキスト ----
    const float pad = P(14.0f);
    const float tw = g.w - pad * 2.0f;
    if (rv.fitW != tw)
    {
        rv.fitW = tw;
        rv.fitName = FitEnd(FBold(), 16.0f, rv.rec.name, tw - (rv.rec.pinned ? P(22.0f) : 0.0f));
        rv.fitPath = FitPath(FMono(), 12.0f, rv.rec.path, tw);
    }
    const float ty = tb.y + P(11.0f);
    dl->AddText(FBold(), P(16.0f), ImVec2(a.x + pad, ty), U(rv.exists ? th::Text : th::TextDim, rev), rv.fitName.c_str());
    if (rv.rec.pinned) Icon(ImVec2(b.x - pad - P(6.0f), ty + P(9.0f)), ICON_PIN, th::AccentHover, 15.0f, rev);

    std::string meta;
    if (!rv.exists)
    {
        Icon(ImVec2(a.x + pad + P(7.0f), ty + P(32.0f)), ICON_WARN, th::Warn, 14.0f, rev);
        dl->AddText(FBody(), P(12.5f), ImVec2(a.x + pad + P(20.0f), ty + P(25.0f)), U(th::Warn, rev), "フォルダが見つかりません");
    }
    else
    {
        Icon(ImVec2(a.x + pad + P(6.0f), ty + P(32.0f)), ICON_CLOCK, th::TextFaint, 13.0f, rev);
        const std::string rel = rv.rec.lastOpened > 0 ? L::FormatRelativeTime(ProjectManager::NowEpoch(), rv.rec.lastOpened, 9 * 3600) : std::string("記録なし");
        dl->AddText(FBody(), P(12.5f), ImVec2(a.x + pad + P(18.0f), ty + P(25.0f)), U(th::TextDim, rev), rel.c_str());
    }
    dl->AddText(FMono(), P(12.0f), ImVec2(a.x + pad, ty + P(46.0f)), U(th::TextFaint, rev), rv.fitPath.c_str());

    // ピン（サムネイル右上。ホバー中 / ピン済みのとき表示）
    const float pinA = Clamp((rv.rec.pinned ? 1.0f : 0.0f) + h, 0.0f, 1.0f) * rev;
    if (pinA > 0.02f)
    {
        if (IconBtn(s, "##pin", rv.rec.pinned ? ICON_PIN_OFF : ICON_PIN, ImVec2(tb.x - P(22.0f), ta.y + P(22.0f)), P(28.0f), th::AccentHover,
                    rv.rec.pinned, rv.rec.pinned ? "ピン留めを外す" : "一覧の先頭にピン留め", pinA))
            res.pin = true;
    }
    ImGui::PopID();
    (void)index;
    return res;
}

// 「新規作成」タイル（点線）。
bool DrawNewTile(State& s, const CardGeo& g, float reveal)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID("##newtile");
    const float lift = s.anim ? (1.0f - reveal) * P(14.0f) : 0.0f;
    const ImVec2 a(g.a.x, g.a.y + lift), b(a.x + g.w, a.y + g.h);
    ImGui::SetCursorScreenPos(a);
    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(g.w, g.h));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Ease(s, ImGui::GetItemID(), hov ? 1.0f : 0.0f, 0.07f);
    ImGui::PopID();
    const float r = P(11.0f);
    GlowRect(dl, a, b, r, th::Accent, 0.5f * h * reveal, P(14.0f));
    dl->AddRectFilled(a, b, U(Mix(th::Bg1, th::Accent, 0.06f * h), 0.45f * reveal), r);
    DashedRect(dl, ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), U(Mix(White(), th::AccentHover, h), (0.16f + 0.6f * h) * reveal), P(7.0f), P(5.0f), 1.0f);
    const ImVec2 c((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * 0.42f);
    const float d = P(52.0f);
    dl->AddCircleFilled(c, d * 0.5f, U(th::Accent, (0.12f + 0.25f * h) * reveal), 36);
    dl->AddCircle(c, d * 0.5f, U(th::AccentHover, (0.35f + 0.5f * h) * reveal), 36, 1.0f);
    Icon(c, ICON_PLUS, Mix(th::AccentHover, White(), h), 24.0f, reveal);
    const char* t = "新規プロジェクト";
    const float tw = TextW(FBold(), 15.0f, t);
    dl->AddText(FBold(), P(15.0f), ImVec2(c.x - tw * 0.5f, c.y + d * 0.5f + P(14.0f)), U(Mix(th::TextMid, White(), h), reveal), t);
    const char* t2 = "テンプレートから作る";
    const float tw2 = TextW(FBody(), 12.5f, t2);
    dl->AddText(FBody(), P(12.5f), ImVec2(c.x - tw2 * 0.5f, c.y + d * 0.5f + P(36.0f)), U(th::TextFaint, reveal), t2);
    return clicked;
}

void DrawEmptyState(State& s, ImVec2 a, ImVec2 b, bool searching, const std::string& query, LauncherAction& action)
{
    (void)action;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * 0.36f);
    GlowCircle(dl, c, P(150.0f), th::Accent, 0.55f);
    dl->AddCircleFilled(c, P(44.0f), U(th::Bg2, 0.9f), 40);
    dl->AddCircle(c, P(44.0f), U(th::AccentHover, 0.45f), 40, 1.0f);
    Icon(c, searching ? ICON_SEARCH : ICON_FOLDER_PLUS, th::AccentHover, 34.0f);
    const std::string title = searching ? ("「" + query + "」に一致するプロジェクトはありません") : std::string("まだプロジェクトがありません");
    const float tw = TextW(FBold(), 20.0f, title.c_str());
    dl->AddText(FBold(), P(20.0f), ImVec2(c.x - tw * 0.5f, c.y + P(68.0f)), U(th::Text), title.c_str());
    const char* sub = searching ? "名前やパスの一部で検索できます。検索欄の ✕ で戻せます。" : "最初のプロジェクトを作るか、既存のプロジェクトを開きましょう。";
    const float sw = TextW(FBody(), 14.0f, sub);
    dl->AddText(FBody(), P(14.0f), ImVec2(c.x - sw * 0.5f, c.y + P(100.0f)), U(th::TextDim), sub);
    if (!searching)
    {
        const float bw = P(190.0f), bh = P(42.0f), gap = P(12.0f);
        const float x0 = c.x - bw - gap * 0.5f, y0 = c.y + P(140.0f);
        if (Btn(s, "##e_new", ICON_PLUS, "新規プロジェクト", ImVec2(x0, y0), ImVec2(bw, bh), BtnKind::Primary)) SwitchTab(s, kNew);
        if (Btn(s, "##e_open", ICON_FOLDER_OPEN, "プロジェクトを開く", ImVec2(x0 + bw + gap, y0), ImVec2(bw, bh), BtnKind::Secondary)) SwitchTab(s, kOpen);
    }
}

// 右の詳細（最近）
void DrawRecentDetail(State& s, const LauncherHost& host, const PanelRect& pr, ProjectInfo& out, LauncherAction& action)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    RecentView* rv = FindRecent(s, s.selKey);
    const float pad = P(24.0f);
    const float w = pr.b.x - pr.a.x - pad * 2.0f;
    ImVec2 p(pr.a.x + pad, pr.a.y + P(26.0f));

    if (!rv)
    {
        const ImVec2 c((pr.a.x + pr.b.x) * 0.5f, pr.a.y + (pr.b.y - pr.a.y) * 0.38f);
        Icon(c, ICON_FOLDER_OPEN, th::TextFaint, 40.0f);
        const char* t = "プロジェクトを選ぶと詳細が出ます";
        dl->AddText(FBody(), P(14.0f), ImVec2(c.x - TextW(FBody(), 14.0f, t) * 0.5f, c.y + P(40.0f)), U(th::TextDim), t);
        return;
    }
    if (!s.meta.loaded || s.meta.key != rv->key) LoadMeta(s, *rv);

    // サムネイル（16:9）
    const float th_ = w * 9.0f / 16.0f;
    const ImVec2 ta = p, tb(p.x + w, p.y + th_);
    const float r = P(12.0f);
    GlowRect(dl, ta, tb, r, th::Accent, 0.22f, P(18.0f));
    bool drawn = false;
    if (rv->img.id)
    {
        ImageCover(dl, rv->img, ta, tb, r, ImDrawFlags_RoundCornersAll, 1.0f, U(White(), rv->exists ? 1.0f : 0.35f));
        drawn = true;
    }
    if (!drawn)
    {
        const uint32_t hsh = L::LHash32(static_cast<uint32_t>(std::hash<std::string>()(rv->key)));
        const float hue = static_cast<float>(hsh % 360u) / 360.0f;
        float rr, gg, bb2;
        ImGui::ColorConvertHSVtoRGB(hue, 0.42f, 0.26f, rr, gg, bb2);
        const ImVec4 c1(rr, gg, bb2, 1.0f);
        ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.10f, rr, gg, bb2);
        GradientRounded(dl, ta, tb, r, ImDrawFlags_RoundCornersAll, U(c1), U(ImVec4(rr, gg, bb2, 1.0f)));
        std::string ini = rv->rec.name.empty() ? "?" : rv->rec.name.substr(0, L::Utf8PrefixBytes(rv->rec.name, 1));
        for (char& ch : ini) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        const float iw = FBold()->CalcTextSizeA(P(56.0f), FLT_MAX, 0, ini.c_str()).x;
        dl->AddText(FBold(), P(56.0f), ImVec2((ta.x + tb.x) * 0.5f - iw * 0.5f, (ta.y + tb.y) * 0.5f - P(30.0f)), U(White(), 0.55f), ini.c_str());
    }
    dl->AddRect(ta, tb, U(White(), 0.10f), r);
    p.y += th_ + P(20.0f);

    // 名前
    {
        const std::string nm = FitEnd(FBold(), 22.0f, rv->rec.name, w - (rv->rec.pinned ? P(28.0f) : 0.0f));
        dl->AddText(FBold(), P(22.0f), p, U(White()), nm.c_str());
        if (rv->rec.pinned) Icon(ImVec2(p.x + w - P(8.0f), p.y + P(13.0f)), ICON_PIN, th::AccentHover, 17.0f);
    }
    p.y += P(34.0f);

    // パス（折り返さず中央省略）+ 操作
    {
        const std::string path = FitPath(FMono(), 12.5f, rv->rec.path, w - P(66.0f));
        dl->AddText(FMono(), P(12.5f), ImVec2(p.x, p.y + P(5.0f)), U(th::TextDim), path.c_str());
        if (IconBtn(s, "##copy", ICON_COPY, ImVec2(p.x + w - P(48.0f), p.y + P(13.0f)), P(26.0f), th::AccentHover, false, "パスをコピー"))
        {
            ImGui::SetClipboardText(rv->rec.path.c_str());
            s.flashText = "パスをコピーしました"; s.flashUntil = s.time + 1.6;
        }
        if (IconBtn(s, "##explorer", ICON_EXTERNAL, ImVec2(p.x + w - P(16.0f), p.y + P(13.0f)), P(26.0f), th::AccentHover, false, "エクスプローラーで表示"))
            ShowInExplorer(rv->rec.path, true);
    }
    p.y += P(34.0f);
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), U(White(), 0.07f));
    p.y += P(14.0f);

    // 情報行
    auto row = [&](const char* k, const std::string& v, const ImVec4* col = nullptr)
    {
        dl->AddText(FBody(), P(13.0f), p, U(th::TextDim), k);
        const std::string t = FitEnd(FBody(), 13.0f, v, w - P(110.0f));
        dl->AddText(FBody(), P(13.0f), ImVec2(p.x + P(110.0f), p.y), U(col ? *col : th::Text), t.c_str());
        p.y += P(26.0f);
    };
    if (!rv->exists)
    {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + P(56.0f)), U(th::Warn, 0.10f), P(8.0f));
        dl->AddRect(p, ImVec2(p.x + w, p.y + P(56.0f)), U(th::Warn, 0.40f), P(8.0f));
        Icon(ImVec2(p.x + P(20.0f), p.y + P(28.0f)), ICON_WARN, th::Warn, 20.0f);
        dl->AddText(FBody(), P(13.0f), ImVec2(p.x + P(40.0f), p.y + P(9.0f)), U(th::Warn), "フォルダが見つかりません");
        dl->AddText(FBody(), P(12.0f), ImVec2(p.x + P(40.0f), p.y + P(30.0f)), U(th::TextDim), "移動・削除された可能性があります");
        p.y += P(68.0f);
    }
    const std::string opened = rv->rec.lastOpened > 0
        ? (L::FormatDate(rv->rec.lastOpened, 9 * 3600) + "  (" + L::FormatRelativeTime(ProjectManager::NowEpoch(), rv->rec.lastOpened, 9 * 3600) + ")")
        : std::string("記録なし");
    row("最終オープン", opened);
    if (rv->exists)
    {
        row("プロジェクト", s.meta.projectFile.empty() ? std::string("-") : s.meta.projectFile);
        row("エンジン", s.meta.engineVersion.empty() ? std::string("-") : ("v" + s.meta.engineVersion + " で作成"));
        row("開始シーン", s.meta.defaultScene.empty() ? std::string("-") : s.meta.defaultScene);
    }

    // ---- 操作のヒント（下部のボタンの上に薄く）----
    {
        const float hy = pr.b.y - pad - P(46.0f) - P(10.0f) - P(38.0f) - P(20.0f) - P(96.0f);
        if (hy > p.y + P(16.0f))
        {
            const char* hints[3][2] = { { ICON_PLAY, "ダブルクリック / Enter で開く" }, { ICON_SEARCH, "Ctrl+F で検索" }, { ICON_ELLIPSIS, "右クリックでメニュー" } };
            for (int i = 0; i < 3; ++i)
            {
                Icon(ImVec2(p.x + P(8.0f), hy + i * P(26.0f) + P(9.0f)), hints[i][0], th::TextFaint, 14.0f);
                dl->AddText(FBody(), P(12.5f), ImVec2(p.x + P(24.0f), hy + i * P(26.0f) + P(2.0f)), U(th::TextFaint), hints[i][1]);
            }
        }
    }

    // ---- 下部のアクション（固定）----
    const float bh = P(46.0f);
    float y = pr.b.y - pad - bh;
    if (Btn(s, "##open", ICON_PLAY, "プロジェクトを開く", ImVec2(pr.a.x + pad, y), ImVec2(w, bh), BtnKind::Primary, rv->exists,
            rv->exists ? nullptr : "フォルダが見つからないので開けません", 15.0f))
    {
        if (OpenRecent(*rv, out)) action = LauncherAction::OpenExisting;
    }
    y -= P(10.0f) + P(38.0f);
    const bool confirming = (s.confirmRemoveKey == rv->key);
    if (!confirming)
    {
        const float bw = (w - P(10.0f)) * 0.5f;
        if (Btn(s, "##pinbtn", rv->rec.pinned ? ICON_PIN_OFF : ICON_PIN, rv->rec.pinned ? "ピン留めを外す" : "ピン留め", ImVec2(pr.a.x + pad, y), ImVec2(bw, P(38.0f)), BtnKind::Secondary, true, nullptr, 13.5f))
        {
            ProjectManager::SetRecentPinned(rv->rec.path, !rv->rec.pinned);
            s.recentsDirty = true;
        }
        if (Btn(s, "##rm", ICON_TRASH, "一覧から削除", ImVec2(pr.a.x + pad + bw + P(10.0f), y), ImVec2(bw, P(38.0f)), BtnKind::Danger, true,
                "一覧から外します（フォルダやファイルは消えません）", 13.5f))
            s.confirmRemoveKey = rv->key;
    }
    else
    {
        // インライン確認（モーダルを出さない）
        const ImVec2 a(pr.a.x + pad, y - P(6.0f)), b(pr.a.x + pad + w, y + P(38.0f));
        dl->AddRectFilled(ImVec2(a.x, a.y - P(28.0f)), b, U(th::Bad, 0.07f), P(8.0f));
        dl->AddText(FBody(), P(12.5f), ImVec2(a.x + P(10.0f), a.y - P(20.0f)), U(th::TextMid), "一覧から外します。フォルダは消えません。");
        const float bw = (w - P(10.0f)) * 0.5f;
        if (Btn(s, "##rm_ok", ICON_TRASH, "外す", ImVec2(a.x, y), ImVec2(bw, P(38.0f)), BtnKind::Danger, true, nullptr, 13.5f))
        {
            ProjectManager::RemoveFromRecents(rv->rec.path);
            s.confirmRemoveKey.clear();
            s.selKey.clear();
            s.recentsDirty = true;
        }
        if (Btn(s, "##rm_cancel", nullptr, "やめる", ImVec2(a.x + bw + P(10.0f), y), ImVec2(bw, P(38.0f)), BtnKind::Secondary, true, nullptr, 13.5f))
            s.confirmRemoveKey.clear();
    }
    (void)host;
}

void DrawRecentTab(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz, float cx0, float cx1, float topH, ProjectInfo& out, LauncherAction& action)
{
    const float padX = P(40.0f);
    const float x0 = cx0 + padX, x1 = cx1 - padX;

    // 見出し + 検索
    PageHeader(s, ImVec2(x0, O.y + topH + P(24.0f)), "最近のプロジェクト",
               s.recents.empty() ? "作成したプロジェクトがここに並びます" : nullptr);
    {
        char sub[64];
        const std::vector<int> vis = L::FilterRecents(s.recs, GetBuf(s.search));
        if (!s.recents.empty())
        {
            std::snprintf(sub, sizeof(sub), "%d 件%s", static_cast<int>(vis.size()),
                          vis.size() != s.recs.size() ? " を表示（検索中）" : "");
            DText(FBody(), 14.0f, ImVec2(x0 + P(2.0f), O.y + topH + P(24.0f) + P(42.0f)), U(th::TextDim), sub);
        }
    }
    const float sw = (std::min)(P(340.0f), (x1 - x0) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(x1 - sw, O.y + topH + P(30.0f)));
    ImGui::SetNextItemWidth(sw);
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(28.0f), (P(38.0f) - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, P(19.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && ImGui::GetIO().KeyCtrl) ImGui::SetKeyboardFocusHere();
        ui::SearchField("##search", s.search.data(), s.search.size(), "名前・パスで検索  (Ctrl+F)");
        ImGui::PopStyleVar(3);
    }

    const std::vector<int> vis = L::FilterRecents(s.recs, GetBuf(s.search));
    const bool searching = GetBuf(s.search).find_first_not_of(" \t") != std::string::npos;

    // ---- グリッド ----
    const float gy0 = O.y + topH + P(96.0f);
    const ImVec2 gridA(x0 - P(18.0f), gy0), gridB(x1 + P(18.0f), O.y + Sz.y);
    if (s.recents.empty() || (vis.empty() && searching))
    {
        DrawEmptyState(s, ImVec2(x0, gy0), ImVec2(x1, O.y + Sz.y), searching, GetBuf(s.search), action);
        return;
    }

    const float gap = P(22.0f);
    const float availW = (x1 - x0);
    const float minW = P(272.0f);
    int cols = (std::max)(1, static_cast<int>(std::floor((availW + gap) / (minW + gap))));
    float cw = (availW - gap * (cols - 1)) / cols;
    if (cw > P(380.0f)) cw = P(380.0f);
    const float thumbH = cw * 9.0f / 16.0f;
    const float ch = thumbH + P(82.0f);

    ImGui::SetCursorScreenPos(gridA);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, P(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    ImGui::BeginChild("##grid", ImVec2(gridB.x - gridA.x, gridB.y - gridA.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavInputs);
    {
        const ImVec2 base = ImGui::GetCursorScreenPos();
        const float padTop = P(16.0f), padL = P(18.0f);
        const bool showNewTile = !searching;
        const int total = static_cast<int>(vis.size()) + (showNewTile ? 1 : 0);
        int rows = (total + cols - 1) / cols;
        for (int i = 0; i < total; ++i)
        {
            const int col = i % cols, row = i / cols;
            CardGeo g{ ImVec2(base.x + padL + col * (cw + gap), base.y + padTop + row * (ch + gap)), cw, thumbH, ch };
            const float rev = s.anim ? L::RevealAmount(s.tabT, i, 0.045f, 0.36f) : 1.0f;
            // 画面外はスキップ（描画だけ。当たり判定も不要）
            const ImVec2 wp = ImGui::GetWindowPos();
            const float wh = ImGui::GetWindowHeight();
            if (g.a.y + ch < wp.y - P(40.0f) || g.a.y > wp.y + wh + P(40.0f)) continue;

            if (showNewTile && i == 0)
            {
                if (DrawNewTile(s, g, rev)) SwitchTab(s, kNew);
                continue;
            }
            const int vi = i - (showNewTile ? 1 : 0);
            RecentView& rv = s.recents[static_cast<size_t>(vis[static_cast<size_t>(vi)])];
            const CardResult cr = DrawProjectCard(s, host, rv, g, vi, rv.key == s.selKey, rev);
            if (cr.select) s.selKey = rv.key;
            if (cr.open && rv.exists && OpenRecent(rv, out)) action = LauncherAction::OpenExisting;
            if (cr.pin) { ProjectManager::SetRecentPinned(rv.rec.path, !rv.rec.pinned); s.recentsDirty = true; }
            if (cr.copyPath) { ImGui::SetClipboardText(rv.rec.path.c_str()); s.flashText = "パスをコピーしました"; s.flashUntil = s.time + 1.6; }
            if (cr.explorer) ShowInExplorer(rv.rec.path, true);
            if (cr.remove) { s.selKey = rv.key; s.confirmRemoveKey = rv.key; }
        }
        ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + padTop + rows * (ch + gap) + P(24.0f)));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    // キーボード: Enter で選択中を開く / Esc で検索を消す
    if (!ImGui::GetIO().WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            if (RecentView* rv = FindRecent(s, s.selKey))
                if (rv->exists && OpenRecent(*rv, out)) action = LauncherAction::OpenExisting;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && searching && ImGui::GetIO().WantTextInput)
        s.search[0] = '\0';
}

// ===========================================================================
// タブ: 新規
// ===========================================================================
void DrawTemplateArt(State& s, const LauncherHost& host, const L::TemplateDef& d, ImVec2 a, ImVec2 b, float r, ImDrawFlags flags, float zoom, float alpha)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (d.cardImage && d.cardImage[0])
        if (const LauncherImage* im = GetImage(s, host, LauncherAsset(host, d.cardImage)))
        {
            ImageCover(dl, *im, a, b, r, flags, zoom, U(White(), alpha));
            return;
        }
    // フォールバック: グラデ + 大きなアイコン
    GradientRounded(dl, a, b, r, flags, U(th::Hex(d.gradA), alpha), U(th::Hex(d.gradB), alpha));
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    GlowCircle(dl, c, (b.y - a.y) * 0.8f, th::Hex(d.accent), 0.5f * alpha);
    ui::DrawIconCentered(dl, d.iconGlyph, c, U(Mix(th::Hex(d.accent), White(), 0.4f), 0.85f * alpha), (b.y - a.y) * 0.34f);
}

bool DrawTemplateCard(State& s, const LauncherHost& host, const L::TemplateDef& d, int idx, const CardGeo& g, bool selected, float reveal)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(d.id);
    const float lift = s.anim ? (1.0f - reveal) * P(14.0f) : 0.0f;
    ImVec2 a(g.a.x, g.a.y + lift), b(a.x + g.w, a.y + g.h);
    ImGui::SetCursorScreenPos(a);
    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(g.w, g.h));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = Ease(s, ImGui::GetItemID(), hov ? 1.0f : 0.0f, 0.07f);
    const float sel = Ease(s, ImGui::GetID("##sel"), selected ? 1.0f : 0.0f, 0.08f);
    ImGui::PopID();
    const float up = P(3.0f) * h;
    a.y -= up; b.y -= up;
    const float r = P(12.0f);
    const ImVec4 acc = th::Hex(d.accent);

    for (int i = 0; i < 4; ++i)
    {
        const float e = P(3.0f + i * 4.0f);
        dl->AddRectFilled(ImVec2(a.x - e * 0.4f, a.y + e * 0.9f), ImVec2(b.x + e * 0.4f, b.y + e * 1.3f), U(Black(), (0.16f - i * 0.035f) * reveal), r + e * 0.5f);
    }
    GlowRect(dl, a, b, r, acc, (0.08f + 0.5f * h + 0.55f * sel) * reveal, P(18.0f));
    dl->AddRectFilled(a, b, U(th::Bg1, 0.88f * reveal), r);
    const ImVec2 ta = a, tb(b.x, a.y + g.thumbH);
    DrawTemplateArt(s, host, d, ta, tb, r, ImDrawFlags_RoundCornersTop, 1.0f + 0.05f * h, reveal);
    dl->AddRectFilledMultiColor(ImVec2(ta.x, tb.y - P(30.0f)), tb, U(th::Bg1, 0.0f), U(th::Bg1, 0.0f), U(th::Bg1, 0.5f * reveal), U(th::Bg1, 0.5f * reveal));
    const float bo = Clamp(h * 0.6f + sel, 0.0f, 1.0f);
    dl->AddRect(a, b, U(Mix(White(), Mix(acc, th::AccentHover, 0.4f), bo), (0.075f + 0.85f * bo) * reveal), r, 0, sel > 0.5f ? P(1.8f) : 1.0f);

    // 選択チェック
    if (sel > 0.02f)
    {
        const ImVec2 c(tb.x - P(20.0f), ta.y + P(20.0f));
        dl->AddCircleFilled(c, P(12.0f) * (0.6f + 0.4f * sel), U(th::Accent, sel * reveal), 24);
        Icon(c, ICON_CHECK, White(), 14.0f, sel * reveal);
    }
    const float pad = P(14.0f);
    dl->AddText(FBold(), P(17.0f), ImVec2(a.x + pad, tb.y + P(11.0f)), U(White(), reveal), d.name);
    const float nw = TextW(FBold(), 17.0f, d.name);
    dl->AddText(FBody(), P(12.5f), ImVec2(a.x + pad + nw + P(10.0f), tb.y + P(14.0f)), U(Mix(acc, White(), 0.5f), reveal), d.subtitle);
    const std::string tag = FitEnd(FBody(), 12.5f, d.tagline, g.w - pad * 2.0f);
    dl->AddText(FBody(), P(12.5f), ImVec2(a.x + pad, tb.y + P(36.0f)), U(th::TextDim, reveal), tag.c_str());
    (void)idx;
    return clicked;
}

void DrawNewTab(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz, float cx0, float cx1, float topH, ProjectInfo& out, LauncherAction& action)
{
    InitForm(s);
    UpdateFormValidation(s, s.time);

    const float padX = P(40.0f);
    const float x0 = cx0 + padX, x1 = cx1 - padX;
    PageHeader(s, ImVec2(x0, O.y + topH + P(24.0f)), "新規プロジェクト", "テンプレートを選び、名前と保存場所を決めて作成します");

    const float cy0 = O.y + topH + P(96.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 - P(18.0f), cy0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, P(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    ImGui::BeginChild("##newscroll", ImVec2(x1 - x0 + P(36.0f), O.y + Sz.y - cy0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavInputs);
    {
        const ImVec2 base0 = ImGui::GetCursorScreenPos();
        const ImVec2 base(base0.x + P(18.0f), base0.y + P(8.0f));
        const float W = x1 - x0;
        ImDrawList* cdl = ImGui::GetWindowDrawList();

        // ---- ギャラリー ----
        const auto& T = L::Templates();
        const int n = static_cast<int>(T.size());
        const float gap = P(18.0f);
        const float minW = P(214.0f);
        int cols = (std::max)(1, (std::min)(n, static_cast<int>(std::floor((W + gap) / (minW + gap)))));
        float cw = (W - gap * (cols - 1)) / cols;
        if (cw > P(330.0f)) cw = P(330.0f);
        const float thumbH = cw * 10.0f / 16.0f;
        const float chh = thumbH + P(64.0f);
        for (int i = 0; i < n; ++i)
        {
            const int col = i % cols, row = i / cols;
            CardGeo g{ ImVec2(base.x + col * (cw + gap), base.y + row * (chh + gap)), cw, thumbH, chh };
            const float rev = s.anim ? L::RevealAmount(s.tabT, i, 0.06f, 0.4f) : 1.0f;
            if (DrawTemplateCard(s, host, T[static_cast<size_t>(i)], i, g, s.tmpl == i, rev))
            {
                s.tmpl = i;
                ProjectManager::SetEditorString("launcherLastTemplate", T[static_cast<size_t>(i)].id);
            }
        }
        const int rows = (n + cols - 1) / cols;
        float y = base.y + rows * (chh + gap) + P(10.0f);

        // ---- フォーム（ガラスのカード）----
        const float fx = base.x, fw = W;
        const float pad = P(22.0f);
        const float fieldsW = fw - pad * 2.0f;
        const float formTop = y;
        // 内容の高さは描きながら決まるので、背景は最後に描く（先に矩形を予約してチャンネル分割）
        ImDrawListSplitter split;
        split.Split(ImGui::GetWindowDrawList(), 2);
        split.SetCurrentChannel(ImGui::GetWindowDrawList(), 1);

        float fy = formTop + pad;
        DText(FBold(), 17.0f, ImVec2(fx + pad, fy), U(White()), "プロジェクトの設定");
        fy += P(34.0f);

        // 名前
        Label(ImVec2(fx + pad, fy), "プロジェクト名");
        fy += P(20.0f);
        const bool nameCh = Input("##pname", s.name.data(), s.name.size(), ImVec2(fx + pad, fy), fieldsW, "MyGame");
        (void)nameCh;
        fy += P(36.0f) + P(14.0f);

        // 保存場所 + 参照
        Label(ImVec2(fx + pad, fy), "保存場所");
        fy += P(20.0f);
        const float browseW = P(104.0f);
        Input("##ploc", s.location.data(), s.location.size(), ImVec2(fx + pad, fy), fieldsW - browseW - P(10.0f), "C:\\Users\\...\\UnoProjects");
        if (Btn(s, "##browse", ICON_FOLDER_OPEN, "参照…", ImVec2(fx + pad + fieldsW - browseW, fy), ImVec2(browseW, P(36.0f)), BtnKind::Secondary, true,
                "保存場所のフォルダを選びます", 13.5f))
        {
            std::string picked;
            if (ProjectManager::PickFolder(host.hwnd, picked, L"プロジェクトの保存場所を選択"))
            {
                SetBuf(s.location, picked);
            }
        }
        fy += P(36.0f) + P(12.0f);

        // 作成先
        {
            const std::string nm = L::Trim(GetBuf(s.name));
            const std::string target = L::JoinPath(L::Trim(GetBuf(s.location)), nm.empty() ? std::string("<プロジェクト名>") : nm);
            Label(ImVec2(fx + pad, fy), "作成先");
            const std::string shown = FitPath(FMono(), 13.0f, target, fieldsW - P(64.0f));
            DText(FMono(), 13.0f, ImVec2(fx + pad + P(64.0f), fy - P(1.0f)), U(Mix(th::Text, th::AccentHover, 0.25f)), shown.c_str());
            fy += P(26.0f);
        }

        // 検証
        const float ih = DrawIssues(ImVec2(fx + pad, fy), fieldsW, s.val, !s.valPending, "この場所に作成できます");
        fy += ih + P(10.0f);

        cdl->AddLine(ImVec2(fx + pad, fy), ImVec2(fx + fw - pad, fy), U(White(), 0.07f));
        fy += P(16.0f);

        // レンダー設定 / オプション
        DText(FBold(), 14.0f, ImVec2(fx + pad, fy), U(th::TextMid), "初期設定");
        fy += P(28.0f);
        {
            const float colW = (fieldsW - P(24.0f)) * 0.5f;
            Label(ImVec2(fx + pad, fy), "影の品質");
            ImGui::SetCursorScreenPos(ImVec2(fx + pad, fy + P(20.0f)));
            ImGui::SetNextItemWidth(colW);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(P(12.0f), (P(36.0f) - ImGui::GetFontSize()) * 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, P(8.0f));
            static const char* kShadow[] = { "軽量（1024）", "標準（2048）", "高品質（4096）", "最高（8192）" };
            ui::Combo("##shadowq", &s.shadowQ, kShadow, 4);
            ImGui::PopStyleVar(2);
            Label(ImVec2(fx + pad, fy + P(66.0f)), "影の解像度です。あとからエンジン設定でも変えられます。");

            const float cx = fx + pad + colW + P(24.0f);
            ImGui::SetCursorScreenPos(ImVec2(cx, fy + P(4.0f)));
            ui::Checkbox("垂直同期（VSync）を使う", &s.vsync);
            ImGui::SetCursorScreenPos(ImVec2(cx, fy + P(34.0f)));
            ImGui::BeginDisabled(!s.gitAvail);
            ui::Checkbox("Git リポジトリを初期化する（.gitignore つき）", &s.gitInit);
            ImGui::EndDisabled();
            if (!s.gitAvail) DText(FBody(), 12.0f, ImVec2(cx + P(24.0f), fy + P(58.0f)), U(th::TextFaint), "git が見つからないため使えません");
        }
        fy += P(92.0f);

        // 作成ボタン
        const bool canCreate = !s.val.HasError() && !s.valPending;
        const float bw = P(240.0f), bh = P(48.0f);
        {
            const auto& d = T[static_cast<size_t>(s.tmpl)];
            std::string lab = std::string("「") + d.name + "」で作成";
            if (Btn(s, "##create", ICON_ROCKET, lab.c_str(), ImVec2(fx + fw - pad - bw, fy), ImVec2(bw, bh), BtnKind::Primary, canCreate,
                    canCreate ? nullptr : "赤い項目を直すと作成できます", 15.0f))
            {
                const std::string nm = L::Trim(GetBuf(s.name));
                const std::string parent = L::Trim(GetBuf(s.location));
                const fs::path proj = L::PathFromUtf8(parent) / L::PathFromUtf8(nm);
                ProjectInfo info;
                info.name         = nm;
                info.rootDir      = L::PathToUtf8(proj);
                info.assetsDir    = L::PathToUtf8(proj / "assets") + "/";
                info.scriptsDir   = L::PathToUtf8(proj / "scripts") + "/";
                info.defaultScene = "scenes/main.json";
                info.templateId   = d.id;
                info.newShadowQuality = s.shadowQ;
                info.newVsync         = s.vsync ? 1 : 0;
                info.newGitInit       = s.gitInit && s.gitAvail;
                out = info;
                action = LauncherAction::CreateNew;
                ProjectManager::SetEditorString("launcherLastLocation", parent);
                ProjectManager::SetEditorString("launcherLastTemplate", d.id);
            }
            const float hintW = fieldsW - bw - P(16.0f);
            const char* why = s.val.HasError() ? "作成できない項目があります。上の赤い表示を直してください。"
                              : (s.val.HasWarn() ? "警告があります。内容を確認のうえ、そのまま作成もできます。" : "作成すると、そのままエディタで開きます。");
            const ImVec4 wc = s.val.HasError() ? th::Bad : (s.val.HasWarn() ? th::Warn : th::TextDim);
            cdl->AddText(FBody(), P(13.0f), ImVec2(fx + pad, fy + (bh - P(13.0f)) * 0.5f), U(wc), FitEnd(FBody(), 13.0f, why, hintW).c_str());
        }
        fy += bh + pad;

        // ---- 背景（ガラス）を後ろのチャンネルへ ----
        split.SetCurrentChannel(ImGui::GetWindowDrawList(), 0);
        {
            const ImVec2 fa(fx, formTop), fb(fx + fw, fy);
            ImDrawList* d2 = ImGui::GetWindowDrawList();
            d2->AddRectFilled(fa, fb, U(th::Bg1, 0.82f), P(14.0f));
            d2->AddLine(ImVec2(fa.x + P(14.0f), fa.y + 0.5f), ImVec2(fb.x - P(14.0f), fa.y + 0.5f), U(White(), 0.10f));
            d2->AddRect(fa, fb, U(White(), 0.08f), P(14.0f));
        }
        split.Merge(ImGui::GetWindowDrawList());
        ImGui::SetCursorScreenPos(ImVec2(base.x, fy + P(16.0f)));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

void DrawNewDetail(State& s, const LauncherHost& host, const PanelRect& pr)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& T = L::Templates();
    const auto& d = T[static_cast<size_t>(Clamp(static_cast<float>(s.tmpl), 0.0f, static_cast<float>(T.size() - 1)))];
    const ImVec4 acc = th::Hex(d.accent);
    const float pad = P(24.0f);
    const float w = pr.b.x - pr.a.x - pad * 2.0f;
    ImVec2 p(pr.a.x + pad, pr.a.y + P(26.0f));

    DText(FBody(), 12.5f, p, U(th::TextFaint), "選択中のテンプレート");
    p.y += P(24.0f);

    // 大きなプレビュー（テンプレ切替で入れ替わるとき、うっすらクロスフェード）
    const float artH = w * 10.0f / 16.0f;
    const float r = P(12.0f);
    GlowRect(dl, p, ImVec2(p.x + w, p.y + artH), r, acc, 0.4f, P(20.0f));
    DrawTemplateArt(s, host, d, p, ImVec2(p.x + w, p.y + artH), r, ImDrawFlags_RoundCornersAll, 1.0f, 1.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + artH), U(White(), 0.10f), r);
    // 画像が無い時のフォールバックに名前は不要。画像に載せる小さな名札
    {
        const float lw = TextW(FBold(), 13.0f, d.name) + P(22.0f);
        dl->AddRectFilled(ImVec2(p.x + P(12.0f), p.y + P(12.0f)), ImVec2(p.x + P(12.0f) + lw, p.y + P(36.0f)), U(Black(), 0.55f), P(12.0f));
        DText(FBold(), 13.0f, ImVec2(p.x + P(23.0f), p.y + P(17.0f)), U(White()), d.name);
    }
    p.y += artH + P(18.0f);

    DText(FBold(), 21.0f, p, U(White()), d.name);
    DText(FBody(), 13.5f, ImVec2(p.x + TextW(FBold(), 21.0f, d.name) + P(12.0f), p.y + P(5.0f)), U(Mix(acc, White(), 0.5f)), d.subtitle);
    p.y += P(34.0f);

    // 説明（折り返し）
    {
        const ImVec2 ts = FBody()->CalcTextSizeA(P(13.5f), FLT_MAX, w, d.description);
        dl->AddText(FBody(), P(13.5f), p, U(th::TextMid), d.description, nullptr, w);
        p.y += ts.y + P(18.0f);
    }

    // 含まれる機能（チップの折り返し）
    DText(FBold(), 13.0f, p, U(th::TextDim), "含まれる機能");
    p.y += P(24.0f);
    {
        float x = p.x, y = p.y;
        for (const char* f : d.features)
        {
            const float cw2 = Chip(ImVec2(0, 0), f, acc, false);
            if (x + cw2 > p.x + w) { x = p.x; y += P(32.0f); }
            Chip(ImVec2(x, y), f, acc, true);
            x += cw2 + P(8.0f);
        }
        p.y = y + P(24.0f) + P(20.0f);
    }

    // 同梱物
    DText(FBold(), 13.0f, p, U(th::TextDim), "同梱されるもの");
    p.y += P(24.0f);
    for (const char* inc : d.includes)
    {
        Icon(ImVec2(p.x + P(7.0f), p.y + P(9.0f)), ICON_CHECK, th::Good, 14.0f);
        const ImVec2 ts = FBody()->CalcTextSizeA(P(13.0f), FLT_MAX, w - P(24.0f), inc);
        dl->AddText(FBody(), P(13.0f), ImVec2(p.x + P(24.0f), p.y + P(2.0f)), U(th::TextMid), inc, nullptr, w - P(24.0f));
        p.y += (std::max)(P(24.0f), ts.y + P(8.0f));
    }
}

// 手順カード（開く / クローンの右側に置く）。番号つきの短い説明。
void DrawStepsCard(ImVec2 p, float w, const char* title, const char* const* steps, int n)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = P(20.0f);
    float y = p.y + pad;
    const float tw = w - pad * 2.0f - P(34.0f);
    // 先に高さを測る
    float h = pad + P(30.0f);
    for (int i = 0; i < n; ++i) h += (std::max)(P(30.0f), FBody()->CalcTextSizeA(P(13.0f), FLT_MAX, tw, steps[i]).y + P(14.0f));
    h += pad * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(th::Bg1, 0.62f), P(14.0f));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U(White(), 0.07f), P(14.0f));
    dl->AddText(FBold(), P(14.5f), ImVec2(p.x + pad, y), U(th::TextMid), title);
    y += P(32.0f);
    for (int i = 0; i < n; ++i)
    {
        const float rh = (std::max)(P(30.0f), FBody()->CalcTextSizeA(P(13.0f), FLT_MAX, tw, steps[i]).y + P(14.0f));
        const ImVec2 c(p.x + pad + P(11.0f), y + P(11.0f));
        dl->AddCircleFilled(c, P(11.0f), U(th::Accent, 0.16f), 24);
        dl->AddCircle(c, P(11.0f), U(th::AccentHover, 0.5f), 24, 1.0f);
        char num[8];
        std::snprintf(num, sizeof(num), "%d", i + 1);
        dl->AddText(FBold(), P(12.0f), ImVec2(c.x - TextW(FBold(), 12.0f, num) * 0.5f, c.y - P(7.0f)), U(th::AccentHover), num);
        dl->AddText(FBody(), P(13.0f), ImVec2(p.x + pad + P(34.0f), y + P(3.0f)), U(th::TextDim), steps[i], nullptr, tw);
        y += rh;
    }
}

// ===========================================================================
// タブ: 開く
// ===========================================================================
void DrawOpenTab(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz, float cx0, float cx1, float topH, ProjectInfo& out, LauncherAction& action)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = cx0 + P(40.0f), x1 = cx1 - P(40.0f);
    PageHeader(s, ImVec2(x0, O.y + topH + P(24.0f)), "プロジェクトを開く", "既存のプロジェクトのフォルダ、または .dx12proj を指定します");

    const float W = (std::min)(x1 - x0, P(860.0f));
    const float fx = x0, fw = W, pad = P(24.0f);
    float y = O.y + topH + P(112.0f);
    const float top = y;
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);

    y += pad;
    Label(ImVec2(fx + pad, y), "プロジェクトのパス");
    y += P(20.0f);
    const float browseW = P(104.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    const bool ch = Input("##openpath", s.openPath.data(), s.openPath.size(), ImVec2(fx + pad, y), fw - pad * 2.0f - browseW - P(10.0f),
                          "C:\\Users\\...\\MyGame  または  MyGame.dx12proj");
    ImGui::PopStyleVar();
    if (Btn(s, "##openbrowse", ICON_FOLDER_OPEN, "参照…", ImVec2(fx + fw - pad - browseW, y), ImVec2(browseW, P(36.0f)), BtnKind::Secondary, true,
            ".dx12proj ファイルを選びます", 13.5f))
    {
        std::string picked;
        if (ProjectManager::PickProjectFile(host.hwnd, picked)) { SetBuf(s.openPath, picked); s.openKey.clear(); }
    }
    (void)ch;
    y += P(36.0f) + P(16.0f);

    // 解決結果（入力が変わったときだけ判定）
    const std::string key = GetBuf(s.openPath);
    if (key != s.openKey)
    {
        s.openKey = key;
        s.openOk = false;
        s.openErr.clear();
        s.openInfo = ProjectInfo{};
        if (!L::Trim(key).empty())
            s.openOk = ProjectManager::ResolveProjectPath(key, s.openInfo, s.openErr);
    }
    if (L::Trim(key).empty())
    {
        Icon(ImVec2(fx + pad + P(8.0f), y + P(10.0f)), ICON_INFO, th::Accent, 15.0f);
        DText(FBody(), 13.0f, ImVec2(fx + pad + P(24.0f), y + P(2.5f)), U(th::TextDim), "パスを貼り付けるか、「参照…」から選んでください。引用符つきのパスもそのまま使えます。");
        y += P(30.0f);
    }
    else if (s.openOk)
    {
        dl->AddRectFilled(ImVec2(fx + pad, y), ImVec2(fx + fw - pad, y + P(66.0f)), U(th::Good, 0.08f), P(10.0f));
        dl->AddRect(ImVec2(fx + pad, y), ImVec2(fx + fw - pad, y + P(66.0f)), U(th::Good, 0.35f), P(10.0f));
        Icon(ImVec2(fx + pad + P(28.0f), y + P(33.0f)), ICON_OK, th::Good, 24.0f);
        dl->AddText(FBold(), P(16.0f), ImVec2(fx + pad + P(56.0f), y + P(11.0f)), U(White()), FitEnd(FBold(), 16.0f, s.openInfo.name, fw - pad * 2.0f - P(80.0f)).c_str());
        dl->AddText(FMono(), P(12.0f), ImVec2(fx + pad + P(56.0f), y + P(37.0f)), U(th::TextDim), FitPath(FMono(), 12.0f, s.openInfo.rootDir, fw - pad * 2.0f - P(80.0f)).c_str());
        y += P(78.0f);
    }
    else
    {
        L::Validation v;
        v.Add(L::Severity::Error, "open.err", s.openErr);
        y += DrawIssues(ImVec2(fx + pad, y), fw - pad * 2.0f, v) + P(8.0f);
    }

    const float bh = P(46.0f), bw = P(220.0f);
    y += P(6.0f);
    if (Btn(s, "##openbtn", ICON_PLAY, "プロジェクトを開く", ImVec2(fx + fw - pad - bw, y), ImVec2(bw, bh), BtnKind::Primary, s.openOk, s.openOk ? nullptr : "有効なプロジェクトを指定すると開けます", 15.0f))
    {
        out = s.openInfo;
        ProjectManager::AddToRecents(out);
        action = LauncherAction::OpenExisting;
    }
    y += bh + pad;

    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(ImVec2(fx, top), ImVec2(fx + fw, y), U(th::Bg1, 0.82f), P(14.0f));
    dl->AddLine(ImVec2(fx + P(14.0f), top + 0.5f), ImVec2(fx + fw - P(14.0f), top + 0.5f), U(White(), 0.10f));
    dl->AddRect(ImVec2(fx, top), ImVec2(fx + fw, y), U(White(), 0.08f), P(14.0f));
    split.Merge(dl);

    // ヒント
    y += P(28.0f);
    DText(FBold(), 14.0f, ImVec2(fx, y), U(th::TextMid), "ヒント");
    y += P(26.0f);
    const char* tips[] = {
        "プロジェクトのフォルダ（.dx12proj がある場所）を指定すれば、ファイル名を入れなくても開けます。",
        "最近開いたプロジェクトは「最近のプロジェクト」に並びます。サムネイルは閉じたとき・保存したときに自動で更新されます。",
        "チームのリポジトリは「Git からクローン」で取得して、そのまま開けます。",
    };
    for (const char* t : tips)
    {
        Icon(ImVec2(fx + P(7.0f), y + P(9.0f)), ICON_SPARKLES, th::AccentHover, 14.0f, 0.9f);
        const ImVec2 ts = FBody()->CalcTextSizeA(P(13.0f), FLT_MAX, fw - P(28.0f), t);
        dl->AddText(FBody(), P(13.0f), ImVec2(fx + P(24.0f), y + P(2.0f)), U(th::TextDim), t, nullptr, fw - P(28.0f));
        y += (std::max)(P(24.0f), ts.y + P(8.0f));
    }
    {
        const float sx = fx + fw + P(28.0f);
        if (x1 - sx >= P(280.0f))
        {
            static const char* kSteps[] = { "パスを貼り付けるか、「参照…」で .dx12proj を選びます。",
                                            "プロジェクト名が緑のカードで出れば準備完了です。",
                                            "「プロジェクトを開く」を押すと、そのままエディタが立ち上がります。" };
            DrawStepsCard(ImVec2(sx, top), (std::min)(x1 - sx, P(360.0f)), "開く手順", kSteps, 3);
        }
    }
    (void)Sz;
}

// ===========================================================================
// タブ: Git からクローン
// ===========================================================================
void DrawCloneTab(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz, float cx0, float cx1, float topH, ProjectInfo& out, LauncherAction& action)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    InitForm(s);
    const float x0 = cx0 + P(40.0f), x1 = cx1 - P(40.0f);
    PageHeader(s, ImVec2(x0, O.y + topH + P(24.0f)), "Git からクローン", "リポジトリの URL から取得して、そのまま開きます");

    const float W = (std::min)(x1 - x0, P(860.0f));
    const float fx = x0, fw = W, pad = P(24.0f);
    float y = O.y + topH + P(112.0f);
    const float top = y;
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);

    const int cstate = s.clone ? s.clone->state.load() : 0;
    const bool running = cstate == 1;

    y += pad;
    Label(ImVec2(fx + pad, y), "リポジトリの URL");
    y += P(20.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    ImGui::BeginDisabled(running);
    Input("##cloneurl", s.cloneUrl.data(), s.cloneUrl.size(), ImVec2(fx + pad, y), fw - pad * 2.0f, "https://github.com/owner/repo.git");
    y += P(36.0f) + P(14.0f);

    Label(ImVec2(fx + pad, y), "保存先（この中にリポジトリ名のフォルダを作ります）");
    y += P(20.0f);
    const float browseW = P(104.0f);
    Input("##cloneparent", s.cloneParent.data(), s.cloneParent.size(), ImVec2(fx + pad, y), fw - pad * 2.0f - browseW - P(10.0f), "C:\\Users\\...\\UnoProjects");
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    if (Btn(s, "##clonebrowse", ICON_FOLDER_OPEN, "参照…", ImVec2(fx + fw - pad - browseW, y), ImVec2(browseW, P(36.0f)), BtnKind::Secondary, !running, "保存先の親フォルダを選びます", 13.5f))
    {
        std::string picked;
        if (ProjectManager::PickFolder(host.hwnd, picked, L"クローン先の親フォルダを選択")) SetBuf(s.cloneParent, picked);
    }
    y += P(36.0f) + P(12.0f);

    // 検証
    const std::string url = GetBuf(s.cloneUrl), parent = GetBuf(s.cloneParent);
    const std::string key = url + "\n" + parent;
    if (key != s.cloneKey)
    {
        s.cloneKey = key;
        s.cloneVal = L::ValidateGitUrl(url);
        if (!s.cloneVal.HasError())
            s.cloneVal.Append(L::ValidateLocation(parent, L::RepoNameFromGitUrl(url), L::RealFs(), /*forClone*/ true));
    }
    if (!L::Trim(url).empty() && !s.cloneVal.HasError())
    {
        Label(ImVec2(fx + pad, y), "クローン先");
        const std::string tgt = L::JoinPath(L::Trim(parent), L::RepoNameFromGitUrl(url));
        DText(FMono(), 13.0f, ImVec2(fx + pad + P(70.0f), y - P(1.0f)), U(Mix(th::Text, th::AccentHover, 0.25f)), FitPath(FMono(), 13.0f, tgt, fw - pad * 2.0f - P(70.0f)).c_str());
        y += P(26.0f);
    }
    if (L::Trim(url).empty())
    {
        Icon(ImVec2(fx + pad + P(8.0f), y + P(10.0f)), ICON_INFO, th::Accent, 15.0f);
        DText(FBody(), 13.0f, ImVec2(fx + pad + P(24.0f), y + P(2.5f)), U(th::TextDim), "例: https://github.com/owner/repo.git");
        y += P(26.0f);
    }
    else y += DrawIssues(ImVec2(fx + pad, y), fw - pad * 2.0f, s.cloneVal, !s.cloneVal.HasError(), "クローンできます") + P(4.0f);

    if (!s.gitAvail && s.env && !s.env->running.load())
    {
        L::Validation v;
        v.Add(L::Severity::Error, "git.missing", "git が見つかりません（PATH を通してください）");
        y += DrawIssues(ImVec2(fx + pad, y), fw - pad * 2.0f, v);
    }
    if (s.ghUser.empty() && s.ghAvail)
    {
        L::Validation v;
        v.Add(L::Severity::Info, "gh.login", "非公開リポジトリは、左下の「ログイン」で GitHub にログインすると取得できます。");
        y += DrawIssues(ImVec2(fx + pad, y), fw - pad * 2.0f, v);
    }

    // 進行 / 結果
    if (running)
    {
        Spinner(dl, ImVec2(fx + pad + P(12.0f), y + P(20.0f)), P(9.0f), th::AccentHover, s.time);
        DText(FBold(), 14.0f, ImVec2(fx + pad + P(34.0f), y + P(11.0f)), U(th::Text), "クローン中…（大きなリポジトリは時間がかかります）");
        y += P(44.0f);
    }
    else if (cstate == 3)
    {
        std::string out2;
        { std::lock_guard<std::mutex> lk(s.clone->mu); out2 = s.clone->output; }
        L::Validation v;
        v.Add(L::Severity::Error, "clone.failed", out2.empty() ? "クローンに失敗しました" : ("クローンに失敗しました: " + out2.substr(0, 400)));
        y += DrawIssues(ImVec2(fx + pad, y), fw - pad * 2.0f, v) + P(4.0f);
    }
    else if (cstate == 2)
    {
        std::string repoDir;
        { std::lock_guard<std::mutex> lk(s.clone->mu); repoDir = s.clone->repoDir; }
        if (ProjectManager::ProjectFromFolder(repoDir, out))
        {
            ProjectManager::AddToRecents(out);
            action = LauncherAction::OpenExisting;
            s.clone->state.store(0);
            s.cloneUrl.fill('\0');
        }
        else s.clone->state.store(3);
    }

    y += P(6.0f);
    const float bh = P(46.0f), bw = P(200.0f);
    const bool ok = !running && !s.cloneVal.HasError() && !L::Trim(url).empty() && s.gitAvail;
    if (Btn(s, "##clonebtn", ICON_CLOUD_DOWNLOAD, running ? "取得中…" : "クローンして開く", ImVec2(fx + fw - pad - bw, y), ImVec2(bw, bh), BtnKind::Primary, ok,
            ok ? nullptr : "URL と保存先を正しく入力すると使えます", 15.0f))
        StartClone(s, L::Trim(url), L::Trim(parent));
    y += bh + pad;

    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(ImVec2(fx, top), ImVec2(fx + fw, y), U(th::Bg1, 0.82f), P(14.0f));
    dl->AddLine(ImVec2(fx + P(14.0f), top + 0.5f), ImVec2(fx + fw - P(14.0f), top + 0.5f), U(White(), 0.10f));
    dl->AddRect(ImVec2(fx, top), ImVec2(fx + fw, y), U(White(), 0.08f), P(14.0f));
    split.Merge(dl);
    {
        const float sx = fx + fw + P(28.0f);
        if (x1 - sx >= P(280.0f))
        {
            static const char* kSteps[] = { "リポジトリの URL を貼り付けます（https / ssh どちらも可）。",
                                            "保存先の親フォルダを選ぶと、その中にリポジトリ名のフォルダを作ります。",
                                            "「クローンして開く」を押すと、取得の間も画面は止まらず、終わると自動で開きます。" };
            DrawStepsCard(ImVec2(sx, top), (std::min)(x1 - sx, P(360.0f)), "クローンの手順", kSteps, 3);
        }
    }
    (void)Sz;
}

// ===========================================================================
// タブ: ニュース / バージョン / 学習
// ===========================================================================
struct LinkDef { const char* icon; const char* title; const char* sub; const char* url; int internalTab; };

void DrawNewsTab(State& s, const LauncherHost& host, ImVec2 O, ImVec2 Sz, float cx0, float cx1, float topH)
{
    (void)host;
    const float x0 = cx0 + P(40.0f), x1 = cx1 - P(40.0f);
    PageHeader(s, ImVec2(x0, O.y + topH + P(24.0f)), "ニュースと学習", "更新内容・バージョン情報・ドキュメントへの入口");

    const float cy0 = O.y + topH + P(96.0f);
    const float totalW = x1 - x0;
    const float rightW = (std::min)(P(400.0f), totalW * 0.38f);
    const float gap = P(24.0f);
    const float leftW = (std::min)(totalW - rightW - gap, P(820.0f));

    ImGui::SetCursorScreenPos(ImVec2(x0 - P(18.0f), cy0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, P(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    ImGui::BeginChild("##newsscroll", ImVec2(totalW + P(36.0f), O.y + Sz.y - cy0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavInputs);
    {
        const ImVec2 base0 = ImGui::GetCursorScreenPos();
        const ImVec2 base(base0.x + P(18.0f), base0.y + P(8.0f));
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float pad = P(22.0f);

        // ---- 左: 更新内容 ----
        float ly = base.y;
        {
            const float lx = base.x;
            const float top = ly;
            ImDrawListSplitter split;
            split.Split(cdl, 2);
            split.SetCurrentChannel(cdl, 1);
            ly += pad;
            DText(FBody(), 12.5f, ImVec2(lx + pad, ly), U(th::AccentHover), "更新内容");
            ly += P(22.0f);
            const float tw = leftW - pad * 2.0f;
            for (const auto& b : s.news)
            {
                using K = L::NewsBlock::Kind;
                if (b.kind == K::Headline)
                {
                    const ImVec2 ts = FBold()->CalcTextSizeA(P(22.0f), FLT_MAX, tw, b.text.c_str());
                    cdl->AddText(FBold(), P(22.0f), ImVec2(lx + pad, ly), U(White()), b.text.c_str(), nullptr, tw);
                    ly += ts.y + P(16.0f);
                }
                else if (b.kind == K::Section)
                {
                    ly += P(10.0f);
                    const ImVec2 ts = FBold()->CalcTextSizeA(P(15.0f), FLT_MAX, tw - P(14.0f), b.text.c_str());
                    cdl->AddRectFilled(ImVec2(lx + pad, ly + P(2.0f)), ImVec2(lx + pad + P(3.0f), ly + ts.y), U(th::Accent), P(1.5f));
                    cdl->AddText(FBold(), P(15.0f), ImVec2(lx + pad + P(12.0f), ly), U(th::Text), b.text.c_str(), nullptr, tw - P(14.0f));
                    ly += ts.y + P(8.0f);
                }
                else if (b.kind == K::Bullet)
                {
                    const ImVec2 ts = FBody()->CalcTextSizeA(P(13.5f), FLT_MAX, tw - P(30.0f), b.text.c_str());
                    cdl->AddCircleFilled(ImVec2(lx + pad + P(16.0f), ly + P(8.5f)), P(2.2f), U(th::AccentHover), 10);
                    cdl->AddText(FBody(), P(13.5f), ImVec2(lx + pad + P(28.0f), ly), U(th::TextMid), b.text.c_str(), nullptr, tw - P(30.0f));
                    ly += ts.y + P(7.0f);
                }
                else
                {
                    const ImVec2 ts = FBody()->CalcTextSizeA(P(13.5f), FLT_MAX, tw - P(14.0f), b.text.c_str());
                    cdl->AddText(FBody(), P(13.5f), ImVec2(lx + pad + P(12.0f), ly), U(th::TextMid), b.text.c_str(), nullptr, tw - P(14.0f));
                    ly += ts.y + P(9.0f);
                }
            }
            ly += pad;
            split.SetCurrentChannel(cdl, 0);
            cdl->AddRectFilled(ImVec2(lx, top), ImVec2(lx + leftW, ly), U(th::Bg1, 0.82f), P(14.0f));
            cdl->AddLine(ImVec2(lx + P(14.0f), top + 0.5f), ImVec2(lx + leftW - P(14.0f), top + 0.5f), U(Mix(th::AccentHover, White(), 0.3f), 0.45f));
            cdl->AddRect(ImVec2(lx, top), ImVec2(lx + leftW, ly), U(White(), 0.08f), P(14.0f));
            split.Merge(cdl);
        }

        // ---- 右: バージョン + 学習リンク ----
        float ry = base.y;
        const float rx = base.x + leftW + gap;   // 右カラムは左カードのすぐ隣
        {
            const float top = ry;
            const float h = P(112.0f);
            cdl->AddRectFilled(ImVec2(rx, top), ImVec2(rx + rightW, top + h), U(th::Bg1, 0.82f), P(14.0f));
            GlowRect(cdl, ImVec2(rx, top), ImVec2(rx + rightW, top + h), P(14.0f), th::Accent, 0.16f, P(16.0f));
            cdl->AddRect(ImVec2(rx, top), ImVec2(rx + rightW, top + h), U(White(), 0.08f), P(14.0f));
            const float logo = P(56.0f);
            const ImVec2 lp(rx + pad, top + (h - logo) * 0.5f);
            if (host.logo)
                cdl->AddImageRounded(static_cast<ImTextureID>(host.logo), lp, ImVec2(lp.x + logo, lp.y + logo), ImVec2(0, 0), ImVec2(1, 1), U(White()), logo * 0.22f);
            DText(FBold(), 20.0f, ImVec2(lp.x + logo + P(16.0f), top + P(22.0f)), U(White()), kEngineName);
            char ver[64];
            std::snprintf(ver, sizeof(ver), "バージョン %s", kEngineVersion);
            DText(FMono(), 13.5f, ImVec2(lp.x + logo + P(16.0f), top + P(50.0f)), U(th::TextDim), ver);
            const std::string stat = std::string("GitHub: ") + (s.ghUser.empty() ? "未ログイン" : ("@" + s.ghUser));
            DText(FBody(), 12.5f, ImVec2(lp.x + logo + P(16.0f), top + P(72.0f)), U(th::TextFaint), stat.c_str());
            ry += h + P(18.0f);
        }
        DText(FBold(), 14.0f, ImVec2(rx + P(2.0f), ry), U(th::TextMid), "学習とリンク");
        ry += P(28.0f);

        static const LinkDef kLinks[] = {
            { ICON_BOOK_OPEN, "ドキュメント", "使い方・Lua API・サンプルコード", "https://ryuto-alt.github.io/dx12/", -1 },
            { ICON_FILE_CODE, "Lua スクリプトの書き方", "コンポーネント・イベント・UI のガイド", "https://github.com/ryuto-alt/dx12/blob/master/docs/SCRIPTING.md", -1 },
            { ICON_SPARKLES, "AI と一緒に作る（MCP）", "エンジンを AI から操作するための手引き", "https://github.com/ryuto-alt/dx12/blob/master/docs/MCP.md", -1 },
            { ICON_ROCKET, "サンプルとテンプレート", "FPS / TPS / 2D / 空 をすぐ動かせます", nullptr, kNew },
            { ICON_FOLDER_GIT, "GitHub リポジトリ", "ソース・Issue・ディスカッション", "https://github.com/ryuto-alt/dx12", -1 },
            { ICON_MEGAPHONE, "リリースノート", "過去の更新内容の一覧", "https://github.com/ryuto-alt/dx12/releases", -1 },
        };
        int li = 0;
        for (const LinkDef& L_ : kLinks)
        {
            const float lh = P(66.0f);
            const ImVec2 a(rx, ry), b(rx + rightW, ry + lh);
            ImGui::PushID(li);
            ImGui::SetCursorScreenPos(a);
            const bool clicked = ImGui::InvisibleButton("##link", ImVec2(rightW, lh));
            const bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const float h = Ease(s, ImGui::GetItemID(), hov ? 1.0f : 0.0f, 0.07f);
            ImGui::PopID();
            const float rev = s.anim ? L::RevealAmount(s.tabT, li + 2, 0.05f, 0.35f) : 1.0f;
            GlowRect(cdl, a, b, P(12.0f), th::Accent, 0.5f * h * rev, P(12.0f));
            cdl->AddRectFilled(a, b, U(Mix(th::Bg1, th::Bg2, h), 0.86f * rev), P(12.0f));
            cdl->AddRect(a, b, U(Mix(White(), th::AccentHover, h), (0.08f + 0.6f * h) * rev), P(12.0f));
            const ImVec2 ic(a.x + P(32.0f), a.y + lh * 0.5f);
            cdl->AddCircleFilled(ic, P(18.0f), U(th::Accent, (0.12f + 0.16f * h) * rev), 28);
            Icon(ic, L_.icon, Mix(th::AccentHover, White(), h), 18.0f, rev);
            cdl->AddText(FBold(), P(14.5f), ImVec2(a.x + P(62.0f), a.y + P(13.0f)), U(th::Text, rev), L_.title);
            cdl->AddText(FBody(), P(12.5f), ImVec2(a.x + P(62.0f), a.y + P(36.0f)), U(th::TextDim, rev),
                         FitEnd(FBody(), 12.5f, L_.sub, rightW - P(62.0f) - P(40.0f)).c_str());
            Icon(ImVec2(b.x - P(22.0f), ic.y), L_.url ? ICON_EXTERNAL : ICON_ARROW_RIGHT, Mix(th::TextFaint, th::AccentHover, h), 16.0f, rev);
            if (clicked)
            {
                if (L_.url) OpenUrl(L_.url);
                else if (L_.internalTab >= 0) SwitchTab(s, static_cast<Tab>(L_.internalTab));
            }
            ry += lh + P(10.0f);
            ++li;
        }
        const float endY = (std::max)(ly, ry) + P(24.0f);
        ImGui::SetCursorScreenPos(ImVec2(base.x, endY));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

}  // namespace

// ===========================================================================
// 公開 API
// ===========================================================================
LauncherAction LauncherScreen::Render(ProjectInfo& outInfo, const LauncherHost& host)
{
    State& s = St();
    LauncherAction action = LauncherAction::None;
    EnsureInit(s, host);
    ++s.renderedFrames;
    s.lastRenderImGuiFrame = ImGui::GetFrameCount();

    const float dt = (std::min)(ImGui::GetIO().DeltaTime, 0.1f);
    s.time += dt;
    s.tabT += dt;
    s.loadBudget = 3;
    PumpAsync(s);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, th::Bg0);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##LauncherBG", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoDocking);

    if (ImGui::IsWindowAppearing())
    {
        s.recentsDirty = true;
        s.tabT = 0.0f;
        s.confirmRemoveKey.clear();
        KickEnvProbe(s);
        s.valKey.clear();
        s.valPending = true;
        s.valForce = true;
        s.openKey.clear();
    }
    if (s.recentsDirty) ReloadRecents(s);

    // タブ入場（フェード）。アニメ OFF は常に 1。
    g_alpha = s.anim ? L::EaseOutCubicf(s.tabT / 0.26f) : 1.0f;

    const ImVec2 O = vp->Pos, Sz = vp->Size;
    const float topH = P(40.0f);
    const float navW = Sz.x < P(900.0f) ? P(176.0f) : (Sz.x < P(1180.0f) ? P(216.0f) : P(256.0f));
    const bool hasRight = (s.tab == kRecent || s.tab == kNew) && Sz.x >= P(980.0f);   // 狭い窓では詳細パネルを畳む
    const float rightW = hasRight ? Clamp(Sz.x * 0.25f, P(330.0f), P(440.0f)) : 0.0f;

    // 背景は不透明度の影響を受けない
    const float keepAlpha = g_alpha;
    g_alpha = 1.0f;
    DrawBackground(s, host, O, Sz);
    DrawNav(s, host, O, Sz, navW);
    g_alpha = keepAlpha;

    const float cx0 = O.x + navW, cx1 = O.x + Sz.x - rightW;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    switch (s.tab)
    {
    case kRecent:
    {
        const PanelRect pr = DrawPanelFrame(s, O, Sz, rightW, topH);
        DrawRecentTab(s, host, O, Sz, cx0, cx1, topH, outInfo, action);
        DrawRecentDetail(s, host, pr, outInfo, action);
        break;
    }
    case kNew:
    {
        const PanelRect pr = DrawPanelFrame(s, O, Sz, rightW, topH);
        DrawNewTab(s, host, O, Sz, cx0, cx1, topH, outInfo, action);
        DrawNewDetail(s, host, pr);
        break;
    }
    case kOpen:  DrawOpenTab(s, host, O, Sz, cx0, cx1, topH, outInfo, action); break;
    case kClone: DrawCloneTab(s, host, O, Sz, cx0, cx1, topH, outInfo, action); break;
    case kNews:  DrawNewsTab(s, host, O, Sz, cx0, cx1, topH); break;
    default: break;
    }
    ImGui::PopStyleVar();

    // 一時メッセージ（コピーしました 等）
    if (s.time < s.flashUntil && !s.flashText.empty())
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float a = Clamp(static_cast<float>(s.flashUntil - s.time) / 0.3f, 0.0f, 1.0f);
        const float tw = TextW(FBody(), 13.5f, s.flashText.c_str()) + P(44.0f);
        const ImVec2 p((cx0 + cx1) * 0.5f - tw * 0.5f, O.y + Sz.y - P(70.0f));
        dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + P(38.0f)), U(th::Bg2, 0.96f * a), P(19.0f));
        dl->AddRect(p, ImVec2(p.x + tw, p.y + P(38.0f)), U(th::AccentHover, 0.5f * a), P(19.0f));
        Icon(ImVec2(p.x + P(20.0f), p.y + P(19.0f)), ICON_CHECK, th::Good, 15.0f, a);
        dl->AddText(FBody(), P(13.5f), ImVec2(p.x + P(34.0f), p.y + P(11.0f)), U(th::Text, a), s.flashText.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    g_alpha = 1.0f;

    // UI 自動テストからの「このプロジェクトを開く」要求
    if (!s.debugOpenPath.empty() && action == LauncherAction::None)
    {
        std::string err;
        const std::string path = s.debugOpenPath;
        s.debugOpenPath.clear();
        if (ProjectManager::ResolveProjectPath(path, outInfo, err))
        {
            ProjectManager::AddToRecents(outInfo);
            action = LauncherAction::OpenExisting;
        }
        else Logger::Warn("ランチャー(テスト): 開けませんでした: {} ({})", path, err);
    }

    if (s.recentsDirty && action == LauncherAction::None) { /* 次フレームで再読込 */ }
    return action;
}

LauncherDebugState LauncherScreen::Debug()
{
    State& s = St();
    LauncherDebugState d;
    d.tab = static_cast<int>(s.tab);
    d.templateIndex = s.tmpl;
    d.recentCount = static_cast<int>(s.recents.size());
    d.visibleRecentCount = static_cast<int>(L::FilterRecents(s.recs, GetBuf(s.search)).size());
    d.selectedRecent = -1;
    {
        const auto vis = L::FilterRecents(s.recs, GetBuf(s.search));
        for (size_t i = 0; i < vis.size(); ++i) if (s.recents[static_cast<size_t>(vis[i])].key == s.selKey) d.selectedRecent = static_cast<int>(i);
    }
    d.animations = s.anim;
    d.formHasError = s.val.HasError();
    d.formHasWarn = s.val.HasWarn();
    d.formName = GetBuf(s.name);
    d.formLocation = GetBuf(s.location);
    { const auto sorted = s.val.Sorted(); if (!sorted.empty()) d.formFirstIssueCode = sorted.front().code; }
    d.searchText = GetBuf(s.search);
    d.renderedFrames = s.renderedFrames;
    d.visible = ImGui::GetCurrentContext() && (ImGui::GetFrameCount() - s.lastRenderImGuiFrame) <= 2;
    return d;
}

void LauncherScreen::DebugSetTab(int tab)
{
    if (tab < 0 || tab >= kTabCount) return;
    SwitchTab(St(), static_cast<Tab>(tab));
}
void LauncherScreen::DebugSetForm(const std::string& name, const std::string& location)
{
    State& s = St();
    InitForm(s);
    SetBuf(s.name, name);
    SetBuf(s.location, location);
    s.valKey.clear();
    s.valPending = true;
    s.valForce = true;    // 次フレームで即検証（入力が止まるのを待たない）
}
void LauncherScreen::DebugSelectTemplate(int index)
{
    State& s = St();
    const int n = static_cast<int>(L::Templates().size());
    if (index >= 0 && index < n) s.tmpl = index;
}
void LauncherScreen::DebugSetSearch(const std::string& text) { SetBuf(St().search, text); }
void LauncherScreen::DebugSetAnimations(bool on) { St().anim = on; }
void LauncherScreen::DebugReloadRecents() { St().recentsDirty = true; }
void LauncherScreen::DebugRequestOpen(const std::string& p) { St().debugOpenPath = p; }

}  // namespace dx12e
