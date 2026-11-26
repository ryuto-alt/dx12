// text::EllipsizeEnd（src/editor/TextFit.h）とゲージ型の進捗の刻み送り（ui::anim::StepToward）の検査（W1-A5 / RM-A3）。
// ImGui のフォントも GPU も要らない（アルゴリズムは計測関数を受け取る。決定論の時計で時間を進める）。
//   ・日本語 / 絵文字 / 結合文字 / ZWJ 連結 / 国旗 / 不正な UTF-8 を、コードポイント・クラスタの途中で切らない
//   ・結果の幅が maxW 以下、計測回数が O(log n)
//   ・StepToward: 1 刻みずつ進む / 決定論では即時 / 戻りは即時 / 参照されない状態は捨てる
//   ・黄の指針（ProgressRole::StatusJob）の使用箇所が StatusBar.cpp の 1 か所以下
#include "editor/TextFit.h"
#include "editor/UiMotion.h"
#include "gui/UiClock.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace dx12e;
namespace fs = std::filesystem;

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// 計測: 結合文字・VS・ZWJ・修飾子は幅 0、全角（U+1100 以上の主要範囲）は 2、それ以外は 1。「…」は 1。
float Measure(std::string_view s)
{
    float w = 0.0f;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end)
    {
        const text::detail::Cp d = text::detail::DecodeUtf8(p, end);
        p += d.len;
        if (text::detail::IsExtend(d.cp)) continue;
        w += (d.cp >= 0x1100 && d.cp != 0x2026) ? 2.0f : 1.0f;
    }
    return w;
}

bool ValidUtf8(std::string_view s)
{
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end)
    {
        const text::detail::Cp d = text::detail::DecodeUtf8(p, end);
        if (d.cp == 0xFFFD && d.len == 1) return false;
        p += d.len;
    }
    return true;
}

std::string Fit(std::string_view s, float maxW, text::FitResult* out = nullptr)
{
    const text::FitResult r = text::FitEndBytes(s, maxW, Measure);
    if (out) *out = r;
    if (!r.fits) return std::string();
    std::string o(s.substr(0, r.keepBytes));
    if (r.ellipsis) o += text::kEllipsis;
    return o;
}

void TestBasic()
{
    CHECK(Fit("", 5) == "");
    CHECK(Fit("abc", 3) == "abc");                       // ぴったり入る
    CHECK(Fit("abcdef", 4) == std::string("abc") + text::kEllipsis);
    CHECK(Fit("abcdef", 1) == text::kEllipsis);          // 「…」だけ入る
    CHECK(Fit("abcdef", 0.5f).empty());                  // 「…」も入らない
    text::FitResult r;
    Fit("abcdef", 0.5f, &r);
    CHECK(!r.fits);
}

void TestJapanese()
{
    const std::string s = "これは日本語のファイル名です.png";   // 全角 2 / 半角 1
    for (int w = 0; w <= 40; ++w)
    {
        const std::string o = Fit(s, static_cast<float>(w));
        CHECKF(ValidUtf8(o), "invalid utf8 at maxW=%d", w);
        CHECKF(Measure(o) <= static_cast<float>(w) + 1e-4f, "width %f > %d", Measure(o), w);
    }
    // 1 文字ずつ増える（取りこぼし = 入るのに削りすぎ、をしない）
    CHECK(Fit(s, 7) == std::string("これは") + text::kEllipsis);   // 6 + 1
    CHECK(Fit(s, 6) == std::string("これ") + text::kEllipsis);     // 4 + 1（3 文字目は 6+1 で超える）
}

void TestEmojiAndCombining()
{
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";   // 👨‍👩‍👧（ZWJ 連結）
    const std::string flag = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";                                              // 🇯🇵
    const std::string tone = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";                                               // 👍🏽（修飾子）
    const std::string kaDakuten = "\xE3\x81\x8B\xE3\x82\x99";                                                  // か + 結合濁点
    const std::string eAcute = "e\xCC\x81";                                                                    // e + 結合アクセント
    const std::string src = "a" + family + flag + tone + kaDakuten + eAcute + "z";
    for (int w = 0; w <= 40; ++w)
    {
        const std::string o = Fit(src, static_cast<float>(w));
        CHECKF(ValidUtf8(o), "invalid utf8 at maxW=%d", w);
        // 残した部分（… の前）が、連結の途中で終わっていないこと
        std::string kept = o;
        if (kept.size() >= 3 && kept.compare(kept.size() - 3, 3, text::kEllipsis) == 0) kept.resize(kept.size() - 3);
        const char* p = kept.data();
        const char* end = p + kept.size();
        char32_t last = 0;
        while (p < end) { const auto d = text::detail::DecodeUtf8(p, end); last = d.cp; p += d.len; }
        CHECKF(last != 0x200D, "cut after ZWJ at maxW=%d", w);
        // どのクラスタ境界でもない位置で終わっていない = kept は src の前方一致で、境界の上にいる
        CHECK(src.compare(0, kept.size(), kept) == 0);
        std::vector<size_t> b;
        text::detail::ClusterBoundaries(src, b);
        bool onBoundary = false;
        for (size_t x : b) if (x == kept.size()) onBoundary = true;
        CHECKF(onBoundary, "kept=%zu bytes is not a cluster boundary (maxW=%d)", kept.size(), w);
        CHECKF(Measure(o) <= static_cast<float>(w) + 1e-4f, "width over");
    }
    // クラスタの数: a / 家族 / 国旗 / 👍🏽 / か゛ / é / z = 7
    std::vector<size_t> b;
    text::detail::ClusterBoundaries(src, b);
    CHECKF(b.size() == 8, "clusters=%zu", b.size() - 1);
    // 国旗 2 つは対で数える（4 つの地域指示子 = 2 クラスタ）
    const std::string flags = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    text::detail::ClusterBoundaries(flags, b);
    CHECKF(b.size() == 3, "flag clusters=%zu", b.size() - 1);
}

void TestInvalidUtf8()
{
    std::string bad = "ab\xFF\xFE\xC3(cd\xE3\x81";   // 不正・途中切れ
    for (int w = 0; w <= 12; ++w)
    {
        text::FitResult r;
        Fit(bad, static_cast<float>(w), &r);
        CHECK(r.keepBytes <= bad.size());
    }
    CHECK(Fit(std::string(1, '\x80'), 0.0f).empty());   // 落ちない
}

void TestComplexity()
{
    for (int n : { 10, 100, 1000, 10000 })
    {
        std::string s;
        for (int i = 0; i < n; ++i) s += "あ";
        text::FitResult r;
        Fit(s, static_cast<float>(n), &r);   // 全角 2 × n の半分だけ入る
        const int bound = 3 + static_cast<int>(std::ceil(std::log2(static_cast<double>(n) + 1.0)));
        CHECKF(r.measureCalls <= bound, "n=%d calls=%d bound=%d", n, r.measureCalls, bound);
        CHECK(r.ellipsis);
    }
    // そのまま入るなら 1 回
    text::FitResult r;
    Fit("short", 100, &r);
    CHECK(r.measureCalls == 1);
}

void TestStepToward()
{
    namespace anim = ui::anim;
    ImGuiContext* ctx = ImGui::CreateContext();
    anim::Reset();
    anim::SetReduceMotionOverride(0);
    anim::SetBackground(false);
    anim::SetVirtualInput(false);
    ui::clock::SetDeterministic(false);
    const auto SetTime = [](double t) { ImGui::GetCurrentContext()->Time = t; };
    const float stepMs = ui::anim::dur::kProgressStep.ms;
    CHECK(stepMs > 0.0f);

    SetTime(10.0);
    CHECK(anim::StepToward(5, 0, stepMs).shown == 0);          // 初回は行き先そのまま
    SetTime(10.01);
    CHECK(anim::StepToward(5, 4, stepMs).shown == 1);          // 最初の 1 刻みは即時
    SetTime(10.01 + stepMs * 0.001 * 0.5);
    CHECK(anim::StepToward(5, 4, stepMs).shown == 1);          // 半刻み後はまだ
    SetTime(10.01 + stepMs * 0.001 * 1.01);
    CHECK(anim::StepToward(5, 4, stepMs).shown == 2);          // 1 刻みごとに 1 つ
    SetTime(10.01 + stepMs * 0.001 * 10.0);
    CHECK(anim::StepToward(5, 4, stepMs).shown == 4);          // 行き先を超えない
    CHECK(anim::StepToward(5, 1, stepMs).shown == 1);          // 戻りは即時
    // 経過・最後の変化
    SetTime(20.0);
    const anim::StepInfo si = anim::StepToward(5, 1, stepMs);
    CHECK(si.sinceChangeSec > 9.0 && si.sinceStartSec > 9.9);
    CHECK(anim::StepToward(5, 2, stepMs).sinceChangeSec == 0.0);

    // 決定論 / background では即時
    anim::SetBackground(true);
    CHECK(anim::StepToward(6, 0, stepMs).shown == 0);
    CHECK(anim::StepToward(6, 9, stepMs).shown == 9);
    anim::SetBackground(false);

    // Reduce Motion では送りは即時（刻みの量子化は呼び出し側が保つので、shown は整数の刻み数のまま行き先に着く）
    anim::SetReduceMotionOverride(1);
    SetTime(30.0);
    CHECK(anim::StepToward(7, 0, stepMs).shown == 0);
    SetTime(30.01);
    CHECK(anim::StepToward(7, 6, stepMs).shown == 6);
    anim::SetReduceMotionOverride(0);

    // 遅れの上限: 出来事が溜まっても、止まった後に回り続けない
    SetTime(40.0);
    CHECK(anim::StepToward(10, 0, stepMs, 12).shown == 0);
    SetTime(40.001);
    CHECK(anim::StepToward(10, 1000, stepMs, 12).shown >= 988);   // 1000 件の連続でも 12 刻み以内の遅れ
    SetTime(40.001 + stepMs * 0.001 * 13.0);
    CHECK(anim::StepToward(10, 1000, stepMs, 12).shown == 1000);  // 出来事が止まれば 13 刻みぶん（約 0.8 秒）で止まる
    SetTime(40.001 + stepMs * 0.001 * 40.0);
    CHECK(anim::StepToward(10, 1000, stepMs, 12).shown == 1000);  // 以降は動かない

    // 参照されない状態は捨てる
    anim::Reset();
    anim::StepToward(8, 3, stepMs);
    CHECK(anim::StateCount() == 1);
    for (int i = 0; i < 1300; ++i) { ui::clock::Advance(); anim::StepToward(9, 0, stepMs); }
    CHECKF(anim::StateCount() == 1, "stale step state must be purged: %d", anim::StateCount());

    anim::Reset();
    ImGui::DestroyContext(ctx);
}

// アセットブラウザ用の旧判定（CalcTextSize の切り上げ幅 + 末尾バイト削り・最低 2 バイト）と ASCII で同じ結果になること。
void TestLegacyEquivalence()
{
    const float cw[] = { 6.7f, 7.0f, 5.3f, 8.13f };
    for (const float perChar : cw)
    {
        const auto widthOf = [&](const std::string& str) { return text::CeilLikeCalcTextSize(perChar * static_cast<float>(str.size()) * 0.9f + 0.37f); };
        for (int len = 1; len <= 30; ++len)   // 空文字列は EllipsizeEnd が即座に空を返すので対象外
        {
            std::string name;
            for (int i = 0; i < len; ++i) name += static_cast<char>('a' + i % 26);
            for (float maxW = 0.0f; maxW < 260.0f; maxW += 0.6f)
            {
                std::string old = name;   // 旧ループ（AssetBrowserPanel）
                if (widthOf(old) > maxW)
                {
                    while (old.size() > 2 && widthOf(old + text::kEllipsis) > maxW) old.pop_back();
                    old += text::kEllipsis;
                }
                const float usable = std::floor(maxW);
                const text::FitResult fr = text::FitEndBytesLegacy(name, usable, [&](std::string_view sv, bool ell)
                {
                    std::string x(sv); if (ell) x += text::kEllipsis; return widthOf(x);
                });
                std::string now = name.substr(0, fr.keepBytes);
                if (fr.ellipsis) now += text::kEllipsis;
                CHECKF(now == old, "legacy mismatch len=%d maxW=%.1f perChar=%.2f", len, maxW, perChar);
            }
        }
    }
}

// 黄の指針（ProgressRole::StatusJob）を使ってよいのはステータスバー（A7）の 1 か所だけ。
void TestStatusJobBudget()
{
#ifdef UI_LINT_ROOT
    int sites = 0;
    std::string where;
    const fs::path root = fs::path(UI_LINT_ROOT) / "src";
    for (const auto& e : fs::recursive_directory_iterator(root))
    {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        const std::string name = e.path().filename().string();
        if (name == "UiWidgets.cpp" || name == "UiWidgets.h") continue;   // 定義側
        std::ifstream f(e.path(), std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        const std::string s = ss.str();
        for (size_t pos = s.find("ProgressRole::StatusJob"); pos != std::string::npos; pos = s.find("ProgressRole::StatusJob", pos + 1))
        {
            ++sites;
            where += name + " ";
            CHECKF(name == "StatusBar.cpp", "ProgressRole::StatusJob は StatusBar.cpp だけ: %s", name.c_str());
        }
    }
    CHECKF(sites <= 1, "StatusJob の使用箇所は 1 か所まで: %d (%s)", sites, where.c_str());
#endif
}
} // namespace

int main()
{
    TestBasic();
    TestJapanese();
    TestEmojiAndCombining();
    TestInvalidUtf8();
    TestComplexity();
    TestStepToward();
    TestLegacyEquivalence();
    TestStatusJobBudget();
    if (g_fail == 0) std::printf("text_fit_test: ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
