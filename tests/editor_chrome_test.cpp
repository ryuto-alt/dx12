// 計器のクロム（W1-A7）の単体テスト。GPU 不要。
//   ・主尺: 目盛りの位置・間隔・主目盛りの周期（倍率 1.0 / 1.5 / 2.0）
//   ・キャリパー: 顎 2x8 + 2px の線が整数 px、主目盛りの上に乗る、狭すぎれば出さない
//   ・ステータスバー: 状態の和文、図番 UN-x.y.z、ゲージの刻み、GPU 名の短縮、VRAM の段階
//   ・黄の呼び出し箇所: deco::Caliper は EditorChrome.cpp の 1 か所、NeedleBar / Caliper の合計は kGgSpec.budget.budgetLineCount 以下
//     （数値はテストに書かず tokens から生成した kGgSpec を読む）
#include "editor/EditorChromeLogic.h"
#include "editor/ThemeGraphite.gen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace
{
namespace fs = std::filesystem;
namespace ch = dx12e::chrome;
namespace gg = dx12e::theme::gg;

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

void TestRuler()
{
    const gg::GgDeco d{};
    for (float scale : { 1.0f, 1.25f, 1.5f, 2.0f })
    {
        std::vector<ch::RulerTick> t;
        const float w = 1000.0f * scale;
        ch::RulerTicks(10.0f, 10.0f + w, scale, d.scaleMinorStep, d.scaleMajorStep, t);
        CHECK(!t.empty());
        CHECK(t.front().major && t.front().x == 10.0f);   // 左端は主目盛り
        int majors = 0;
        for (size_t i = 0; i < t.size(); ++i)
        {
            CHECKF(t[i].x == std::floor(t[i].x), "tick x は整数 px (%.2f)", t[i].x);
            CHECK(t[i].x < 10.0f + w);
            if (i) CHECK(t[i].x > t[i - 1].x);
            if (t[i].major) { ++majors; CHECK(i % static_cast<size_t>(d.scaleMajorStep / d.scaleMinorStep) == 0); }
        }
        // 論理 1000px に細目盛り 125 本・主目盛り 25 本
        CHECKF(static_cast<int>(t.size()) == 1000 / d.scaleMinorStep, "scale %.2f: ticks=%zu", scale, t.size());
        CHECK(majors == 1000 / d.scaleMajorStep);
        // 間隔は 1 目盛りあたり minorStep*scale ±1px
        for (size_t i = 1; i < t.size(); ++i)
            CHECK(std::fabs((t[i].x - t[i - 1].x) - d.scaleMinorStep * scale) <= 1.0f);
    }
    std::vector<ch::RulerTick> t;
    ch::RulerTicks(5.0f, 5.0f, 1.0f, 8, 40, t);   CHECK(t.empty());
    ch::RulerTicks(0.0f, 100.0f, 0.0f, 8, 40, t); CHECK(t.empty());
    ch::RulerTicks(0.0f, 100.0f, 1.0f, 0, 40, t); CHECK(t.empty());
}

void TestCaliper()
{
    const gg::GgDeco d{};
    for (float scale : { 1.0f, 1.5f, 2.0f })
    {
        const float accent = std::max(2.0f, std::floor(2.0f * scale + 0.5f));
        const ch::CaliperGeom g = ch::CaliperGeometry(100.4f, 600.6f, 32.0f * scale, d.scaleMajorH * scale, accent,
                                                      d.caliperJawW * scale, d.caliperJawH * scale);
        CHECK(g.valid);
        for (const ch::PxRect* r : { &g.line, &g.jawL, &g.jawR })
            CHECKF(r->x0 == std::floor(r->x0) && r->y0 == std::floor(r->y0) && r->x1 == std::floor(r->x1) && r->y1 == std::floor(r->y1),
                   "整数 px: %.2f %.2f %.2f %.2f", r->x0, r->y0, r->x1, r->y1);
        CHECK(g.line.H() == accent);
        CHECK(g.jawL.W() == std::floor(d.caliperJawW * scale + 0.5f));
        CHECK(g.jawL.H() == std::floor(d.caliperJawH * scale + 0.5f));
        CHECK(g.jawL.x0 == g.line.x0 && g.jawR.x1 == g.line.x1);   // 顎は線の両端
        CHECK(g.jawL.y0 == g.line.y0);                              // 線の上端から下へ
        CHECK(g.line.y1 == std::floor(32.0f * scale - d.scaleMajorH * scale + 0.5f));   // 主目盛りの上
        CHECK(g.jawL.y1 <= 32.0f * scale + 1.0f);                   // タイトルバーの下端を越えない
    }
    CHECK(!ch::CaliperGeometry(10.0f, 12.0f, 32.0f, 6.0f, 2.0f, 2.0f, 8.0f).valid);   // 顎が 2 本入らない
}

void TestStatus()
{
    CHECK(std::string(ch::StateLabel(ch::RunState::Edit)) == "編集");
    CHECK(std::string(ch::StateLabel(ch::RunState::Play)) == "再生中");
    CHECK(std::string(ch::StateLabel(ch::RunState::Paused)) == "一時停止");
    CHECK(ch::DrawingNumber("2.6.1") == "UN-2.6.1");
    CHECK(ch::DrawingNumber("v2.7.0") == "UN-2.7.0");
    CHECK(ch::GaugeCells(0.0f, 100.0f, 8) == 0);
    CHECK(ch::GaugeCells(0.5f, 100.0f, 8) == 1);    // 0 より大きければ最低 1 刻み
    CHECK(ch::GaugeCells(50.0f, 100.0f, 8) == 4);
    CHECK(ch::GaugeCells(100.0f, 100.0f, 8) == 8);
    CHECK(ch::GaugeCells(500.0f, 100.0f, 8) == 8);
    CHECK(ch::GaugeCells(std::nanf(""), 100.0f, 8) == 0);
    CHECK(ch::GaugeCells(5.0f, 0.0f, 8) == 0);
    CHECK(ch::ShortGpuName("NVIDIA GeForce RTX 5060") == "RTX 5060");
    CHECK(ch::ShortGpuName("Intel(R) UHD Graphics") == "UHD Graphics");
    CHECK(ch::ShortGpuName("Microsoft Basic Render Driver") == "Microsoft Basic Render Driver");
    CHECK(ch::VramRatio(-1.0, 8192.0) < 0.0f && ch::VramRatio(10.0, 0.0) < 0.0f);
    CHECK(ch::VramLevel(0.5f) == 0 && ch::VramLevel(0.9f) == 1 && ch::VramLevel(0.97f) == 2);
}

// ---- 黄の呼び出し箇所 ----
std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void TestYellowCallSites()
{
#ifdef UI_LINT_ROOT
    const fs::path root = fs::path(UI_LINT_ROOT) / "src";
    const std::regex re(R"(deco::(NeedleBar|Caliper)\()");
    int caliperSites = 0, needleSites = 0;
    std::string caliperWhere;
    for (const auto& e : fs::recursive_directory_iterator(root))
    {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        const std::string s = ReadAll(e.path());
        for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
        {
            if ((*it)[1].str() == "Caliper") { ++caliperSites; caliperWhere += e.path().filename().string() + " "; }
            else ++needleSites;
        }
    }
    CHECKF(caliperSites == 1, "deco::Caliper の呼び出しは 1 か所: %d (%s)", caliperSites, caliperWhere.c_str());
    CHECKF(caliperWhere.find("EditorChrome.cpp") != std::string::npos, "Caliper は EditorChrome.cpp から: %s", caliperWhere.c_str());
    const int budget = gg::kGgSpec.budget.budgetLineCount;
    CHECKF(needleSites + caliperSites <= budget, "黄の線の呼び出し箇所 %d 以下（予算 %d）", needleSites + caliperSites, budget);
    std::printf("yellow call sites: NeedleBar=%d Caliper=%d (budget %d)\n", needleSites, caliperSites, budget);

    // ロゴの矩形とダークタイトルバーの入口が存在する（W2 の BA-7 が読む / MOD-ED-19）
    CHECK(ReadAll(root / "editor" / "EditorChrome.h").find("LogoRectScreen") != std::string::npos);
    CHECK(ReadAll(root / "core" / "Window.cpp").find("DwmSetWindowAttribute") != std::string::npos);
    // クロムの黄は Accent / NeedleDim のトークンだけ（直書きの黄を持ち込まない）
    for (const char* f : { "editor/EditorChrome.cpp", "editor/StatusBar.cpp" })
    {
        const std::string s = ReadAll(root / f);
        CHECKF(s.find("F0C23B") == std::string::npos && s.find("IM_COL32") == std::string::npos, "%s に直書きの色がある", f);
    }
#endif
}
} // namespace

int main()
{
    TestRuler();
    TestCaliper();
    TestStatus();
    TestYellowCallSites();
    if (g_fail == 0) std::printf("editor_chrome_test: ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}
