// 自動更新の体験の純ロジックの単体テスト（標準ライブラリだけ）。
//   core/ReleaseNotes.*（更新内容データ・範囲・Markdown）/ core/UpdateLogic.*（GitHub 本文の整形・この版を飛ばす・進捗の表示）
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/ReleaseNotes.h"
#include "core/UpdateLogic.h"
#include "core/Version.h"

namespace rn = dx12e::relnotes;
namespace ul = dx12e::updatelogic;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static bool Contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// ---------------- データの整合 ----------------
static void TestData()
{
    const auto& all = rn::All();
    const auto errs = rn::Validate(all);
    for (const auto& e : errs) std::printf("  data error: %s\n", e.c_str());
    CHECK(errs.empty());
    CHECK(all.size() >= 2);
    // 先頭の版 == エンジンの版（版を上げたのに更新内容を足し忘れる / 逆、を検出）
    CHECK(!all.empty() && rn::CompareVersions(all.front().version, dx12e::kEngineVersion) == 0);
    CHECK(rn::Find(all, "1.19.0") != nullptr);          // 履歴として過去の版も持つ
    CHECK(rn::Find(all, "2.0.0") != nullptr && rn::Find(all, "2.0.0")->date == "2026-10-01");
    if (const rn::Release* r = rn::Find(all, "2.0.0"))
    {
        CHECK(r->highlights.size() == 4);
        CHECK(rn::CountKind(*r, rn::Kind::Feature) == 10);
        CHECK(rn::CountKind(*r, rn::Kind::Improvement) == 5);
        CHECK(rn::CountKind(*r, rn::Kind::Fix) == 8);
    }

    // Validate が実際に不整合を検出する（空の検査になっていない）
    {
        std::vector<rn::Release> bad = all;
        std::swap(bad[0], bad[1]);                       // 並びが逆
        CHECK(!rn::Validate(bad).empty());
        bad = all; bad[0].headline.clear();
        CHECK(!rn::Validate(bad).empty());
        bad = all; bad[0].highlights[0].area = "nonexistent";
        CHECK(!rn::Validate(bad).empty());
        bad = all; bad[0].date = "2026/10/01";
        CHECK(!rn::Validate(bad).empty());
        bad = all; bad[0].version = "2.0";
        CHECK(!rn::Validate(bad).empty());
        bad = all; bad[0].highlights.clear();
        CHECK(!rn::Validate(bad).empty());
        bad = all; bad[0].items.push_back(rn::Item{});
        CHECK(!rn::Validate(bad).empty());
        CHECK(!rn::Validate({}).empty());
    }
}

// ---------------- 版 / 範囲 ----------------
static void TestRange()
{
    CHECK(rn::CompareVersions("1.19.0", "2.0.0") < 0);
    CHECK(rn::CompareVersions("v2.0.0", "2.0.0") == 0);
    CHECK(rn::CompareVersions("1.9.0", "1.10.0") < 0);      // 数値比較（文字列比較ではない）
    CHECK(rn::IsNewer("2.0.1", "2.0.0") && !rn::IsNewer("2.0.0", "2.0.0"));

    // 手元の検査用データ（実データに依存しない）
    auto mk = [](const char* v) { rn::Release r; r.version = v; return r; };
    std::vector<rn::Release> all = { mk("2.0.0"), mk("1.19.0"), mk("1.18.0"), mk("1.17.0") };

    auto vers = [](const std::vector<const rn::Release*>& v) { std::string s; for (auto* r : v) s += r->version + " "; return s; };
    CHECK(vers(rn::Range(all, "1.18.0", "2.0.0")) == "2.0.0 1.19.0 ");        // 飛ばし更新: 間の全部・新しい順
    CHECK(vers(rn::Range(all, "1.19.0", "2.0.0")) == "2.0.0 ");               // 1 つだけ
    CHECK(vers(rn::Range(all, "", "2.0.0")) == "2.0.0 ");                     // 初回インストール: 今の版だけ
    CHECK(vers(rn::Range(all, "garbage", "2.0.0")) == "2.0.0 ");              // 壊れた記録
    CHECK(vers(rn::Range(all, "2.0.0", "2.0.0")) == "2.0.0 ");                // 同じ版
    CHECK(vers(rn::Range(all, "3.0.0", "2.0.0")) == "2.0.0 ");                // ダウングレード
    CHECK(vers(rn::Range(all, "1.0.0", "2.0.0")) == "2.0.0 1.19.0 1.18.0 1.17.0 ");   // 履歴より古い前回版
    CHECK(vers(rn::Range(all, "1.18.0", "1.19.0")) == "1.19.0 ");             // 今の版より新しいデータは載せない
    CHECK(vers(rn::Range(all, "1.18.0", "1.18.5")) == "");                    // 今の版のデータが無い（範囲内に何も無い）→ 空
    CHECK(vers(rn::History(all, "2.0.0")) == "2.0.0 1.19.0 1.18.0 1.17.0 ");
    CHECK(vers(rn::History(all, "1.19.0")) == "1.19.0 1.18.0 1.17.0 ");
}

// ---------------- Markdown ----------------
static void TestMarkdown()
{
    const rn::Release* r = rn::Find(rn::All(), "2.0.0");
    CHECK(r != nullptr);
    if (!r) return;
    const std::string md = rn::ToMarkdown(*r, "Uno Engine");
    CHECK(md.rfind("# Uno Engine v2.0.0（2026-10-01）\n", 0) == 0);
    CHECK(Contains(md, r->headline.c_str()));
    CHECK(Contains(md, "\n## 注目\n"));
    CHECK(Contains(md, "\n## 新機能\n") && Contains(md, "\n## 改善\n") && Contains(md, "\n## 修正\n"));
    CHECK(Contains(md, "- **新しい GI（間接光）** — "));
    CHECK(Contains(md, "- 水面（WaterBody）\n"));                       // 本文の無い項目は題だけ
    CHECK(md.find("\n\n\n") == std::string::npos);                       // 空行が重ならない
    CHECK(md.size() > 2 && md.back() == '\n' && md[md.size() - 2] != '\n');   // 末尾は改行 1 つだけ（余計な空行なし）

    // 全項目が漏れなく出る
    size_t items = 0;
    for (const auto& it : r->items) { ++items; CHECK(Contains(md, it.title.c_str())); }
    CHECK(items == 23);
    // 種類の出力順は 新機能 → 改善 → 修正
    CHECK(md.find("## 新機能") < md.find("## 改善") && md.find("## 改善") < md.find("## 修正"));

    // Markdown → 案内窓の整形を通しても「リリース名の見出し」が重複せず、箇条書きが拾える（次の更新で古い exe が見る形）
    const ul::BodyView v = ul::FormatGithubBody(md, 6);
    CHECK(v.lines.size() == 6);                       // 見出し文 + 「注目」+ 注目 4 件
    CHECK(!v.lines.empty() && v.lines[0].type == ul::BodyLine::Type::Text && v.lines[0].text == r->headline);
    CHECK(v.more == 23);                              // 残りは一覧の 23 件（新機能 10 + 改善 5 + 修正 8）
}

// ---------------- GitHub 本文の整形 ----------------
static void TestGithubBody()
{
    CHECK(ul::StripInlineMarkdown("**太字** と `code` と [リンク](https://x.y/z) と ![img](a.png) です") == "太字 と code と リンク と  です");
    CHECK(ul::StripInlineMarkdown("a<!-- hidden -->b<br>c") == "ab c");
    CHECK(ul::StripInlineMarkdown("  __x__  ") == "x");

    const std::string md =
        "# Uno Engine v2.1.0\r\n"
        "\r\n"
        "## What's Changed\r\n"
        "* **水面** を追加 by @u in https://example.com/1\r\n"
        "* [物理](https://example.com) を修正\r\n"
        "- 三つ目\r\n"
        "1. 四つ目\r\n"
        "```\r\n"
        "- コードの中は捨てる\r\n"
        "```\r\n"
        "---\r\n"
        "> 引用の段落\r\n"
        "- 五つ目\r\n"
        "- 六つ目\r\n"
        "- 七つ目\r\n"
        "- 八つ目\r\n";
    const ul::BodyView v = ul::FormatGithubBody(md, 6);
    CHECK(v.lines.size() == 6);
    if (v.lines.size() == 6)
    {
        CHECK(v.lines[0].type == ul::BodyLine::Type::Heading && v.lines[0].text == "What's Changed");   // # は捨て ## は残す
        CHECK(v.lines[1].type == ul::BodyLine::Type::Bullet && v.lines[1].text == "水面 を追加 by @u in https://example.com/1");
        CHECK(v.lines[2].text == "物理 を修正");
        CHECK(v.lines[4].type == ul::BodyLine::Type::Bullet && v.lines[4].text == "四つ目");
        CHECK(v.lines[5].type == ul::BodyLine::Type::Text && v.lines[5].text == "引用の段落");
    }
    CHECK(v.more == 4);                                   // 残りは箇条書き 4 件（五〜八つ目）

    // 短ければ全部・「ほか」なし
    const ul::BodyView s = ul::FormatGithubBody("## 修正\n- 一つ\n- 二つ\n", 6);
    CHECK(s.lines.size() == 3 && s.more == 0);
    // 見出しで終わらない（宙に浮いた見出しを外す）
    const ul::BodyView h = ul::FormatGithubBody("- a\n- b\n## 次の節\n- c\n- d\n", 3);
    CHECK(h.lines.size() == 2 && h.lines[1].text == "b" && h.more == 2);
    // 箇条書きが無いときは残りの行数
    const ul::BodyView t = ul::FormatGithubBody("一行目\n二行目\n三行目\n四行目\n", 2);
    CHECK(t.lines.size() == 2 && t.more == 2);
    // 空・空白だけ・コードだけ
    CHECK(ul::FormatGithubBody("", 6).lines.empty());
    CHECK(ul::FormatGithubBody("  \n\n", 6).lines.empty() && ul::FormatGithubBody("```\nx\n```", 6).lines.empty());
    CHECK(ul::FormatGithubBody("- a", 0).lines.empty() && ul::FormatGithubBody("- a", 0).more == 1);
}

// ---------------- この版を飛ばす ----------------
static void TestSkip()
{
    CHECK(ul::ShouldPromptUpdate("2.1.0", ""));                 // 記録なし → 案内する
    CHECK(!ul::ShouldPromptUpdate("2.1.0", "2.1.0"));           // 飛ばした版 → 聞かない
    CHECK(!ul::ShouldPromptUpdate("v2.1.0", "2.1.0"));          // v の有無は無関係
    CHECK(!ul::ShouldPromptUpdate("2.0.5", "2.1.0"));           // 飛ばした版より古い → 聞かない
    CHECK(ul::ShouldPromptUpdate("2.1.1", "2.1.0"));            // もっと新しい版が出たら聞く
    CHECK(ul::ShouldPromptUpdate("3.0.0", "2.9.9"));
    CHECK(ul::ShouldPromptUpdate("2.1.0", "garbage"));          // 壊れた記録は無視して案内
    CHECK(ul::ShouldPromptUpdate("1.10.0", "1.9.0"));           // 数値比較: 1.10.0 > 1.9.0 → 案内する
}

// ---------------- 進捗の表示 ----------------
static void TestProgress()
{
    ul::RateTracker rt(3.0);
    CHECK(rt.BytesPerSec() == 0.0 && rt.EtaSeconds(0, 100) < 0.0);
    rt.Add(0.0, 0);
    CHECK(rt.BytesPerSec() == 0.0);                             // 0.5 秒未満は見積もらない
    rt.Add(1.0, 3 * 1048576);
    CHECK(rt.BytesPerSec() > 3.0 * 1048576 * 0.99 && rt.BytesPerSec() < 3.0 * 1048576 * 1.01);
    const double eta = rt.EtaSeconds(3 * 1048576, 33 * 1048576);
    CHECK(eta > 9.9 && eta < 10.1);                             // 残り 30MB ÷ 3MB/s = 10 秒
    CHECK(rt.EtaSeconds(1, 0) < 0.0);                           // 総量不明
    // 窓（3 秒）から古い標本が落ち、直近の速度になる
    rt.Add(2.0, 3 * 1048576);
    rt.Add(5.0, 3 * 1048576);
    CHECK(rt.BytesPerSec() < 0.1 * 1048576);
    // 巻き戻り（再試行）で壊れない
    rt.Add(0.0, 0);
    CHECK(rt.BytesPerSec() == 0.0);

    CHECK(ul::FormatMB(12 * 1048576 + 1048576 * 3 / 10) == "12.3");
    CHECK(ul::FormatSpeed(3.2 * 1048576) == "3.2 MB/s");
    CHECK(ul::FormatSpeed(512 * 1024) == "512 KB/s");
    CHECK(ul::FormatSpeed(0).empty());
    CHECK(ul::FormatEta(10) == "約 10 秒" && ul::FormatEta(0.2) == "約 1 秒" && ul::FormatEta(95) == "約 2 分" && ul::FormatEta(7300) == "約 2 時間");
    CHECK(ul::FormatEta(-1).empty());
    CHECK(ul::FormatDownloadDetail(static_cast<uint64_t>(12.3 * 1048576), static_cast<uint64_t>(45.6 * 1048576), 3.2 * 1048576, 10.4)
          == "12.3 / 45.6 MB  ・  3.2 MB/s  ・  残り約 10 秒");
    CHECK(ul::FormatDownloadDetail(5 * 1048576, 0, 0, -1) == "5.0 MB");
}

int main()
{
    TestData();
    TestRange();
    TestMarkdown();
    TestGithubBody();
    TestSkip();
    TestProgress();
    std::printf("UpdateUxTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
