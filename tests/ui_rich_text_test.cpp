// UiRichText.h（UIText リッチテキストのインラインタグパーサ）の単体テスト。
// ヘッダオンリー・依存ゼロ（GPU/entt/ImGui 不要）なのでスタンドアロン構成でも動く。
// 注意: ランは元 std::string 内を指すので、必ず名前付きの string を渡す（一時は delete 済み）。
#include "ui/UiRichText.h"

#include <cstdio>
#include <string>

using namespace dx12e;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) \
    do { ++g_checks; if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// ランの本文断片を std::string で取り出す（b/e は元文字列内のポインタ範囲）
static std::string RunText(const UiRichRun& r)
{
    return std::string(r.b, r.e);
}

// 色スパン + waveスパン + 無装飾の混在（日本語混じり。デモと同じ形）
static void TestColorAndWaveSpans()
{
    const std::string text = "[c=ff5566]DANGER[/c] を [wave]避けろ[/wave]";
    const auto runs = ParseUiRichText(text);
    CHECK(runs.size() == 3);
    if (runs.size() != 3) return;
    CHECK(RunText(runs[0]) == "DANGER");
    CHECK(runs[0].hasColor);
    CHECK(runs[0].rgb == 0xff5566u);
    CHECK(runs[0].anim == -1);
    CHECK(RunText(runs[1]) == " を ");
    CHECK(!runs[1].hasColor);
    CHECK(runs[1].anim == -1);
    CHECK(RunText(runs[2]) == "避けろ");
    CHECK(!runs[2].hasColor);
    CHECK(runs[2].anim == 1);
    // ストリップ後の文字数: DANGER(6) + " を "(3) + 避けろ(3) = 12
    CHECK(UiRichStrippedCodepoints(text) == 12);
}

// 大文字16進 + shake/rainbow の種別
static void TestUpperHexAndAnimKinds()
{
    const std::string text = "[c=AABBCC]a[/c][shake]b[/shake][rainbow]c[/rainbow]";
    const auto runs = ParseUiRichText(text);
    CHECK(runs.size() == 3);
    if (runs.size() != 3) return;
    CHECK(runs[0].hasColor && runs[0].rgb == 0xAABBCCu);
    CHECK(runs[1].anim == 2);
    CHECK(runs[2].anim == 3);
}

// 不正・未知のタグはタグ文字列をそのまま文字として残す（黙って消さない）
static void TestUnknownTagsPassThrough()
{
    // 未知タグ / 桁不足 / 16進でない / 閉じ括弧なし / 単独 '['
    const std::string text = "a[x]b[c=12]d[c=zzzzzz]e[wave f[";
    const auto runs = ParseUiRichText(text);
    CHECK(runs.size() == 1);
    if (runs.size() != 1) return;
    CHECK(RunText(runs[0]) == "a[x]b[c=12]d[c=zzzzzz]e[wave f[");
    CHECK(!runs[0].hasColor);
    CHECK(runs[0].anim == -1);
    // タグとして消えるものが無い = 文字数もそのまま（ASCII 31文字）
    CHECK(UiRichStrippedCodepoints(text) == 31);
}

// 閉じ忘れは文末まで効く / 開きの無い閉じタグは状態解除として黙って消える
static void TestUnclosedAndStrayClose()
{
    const std::string text = "[shake]ドキドキ";
    const auto runs = ParseUiRichText(text);
    CHECK(runs.size() == 1);
    if (runs.size() == 1)
    {
        CHECK(RunText(runs[0]) == "ドキドキ");
        CHECK(runs[0].anim == 2);
    }
    // 閉じタグ単独: タグは消えて本文だけ残る（色/アニメは元々無し）
    const std::string text2 = "A[/c]B[/wave]C";
    const auto runs2 = ParseUiRichText(text2);
    CHECK(runs2.size() == 3);
    if (runs2.size() == 3)
    {
        CHECK(RunText(runs2[0]) == "A");
        CHECK(RunText(runs2[1]) == "B");
        CHECK(RunText(runs2[2]) == "C");
        CHECK(!runs2[2].hasColor && runs2[2].anim == -1);
    }
}

// 色とアニメの重ね掛け（フラット = 同時に1色+1アニメまで）と \n の扱い
static void TestOverlapAndNewline()
{
    const std::string text = "[c=112233][wave]両方\n2行目[/wave]素";
    const auto runs = ParseUiRichText(text);
    CHECK(runs.size() == 2);
    if (runs.size() == 2)
    {
        CHECK(RunText(runs[0]) == "両方\n2行目");
        CHECK(runs[0].hasColor && runs[0].rgb == 0x112233u);
        CHECK(runs[0].anim == 1);
        CHECK(RunText(runs[1]) == "素");
        CHECK(runs[1].hasColor);      // [/c] が無い = 色は文末まで
        CHECK(runs[1].anim == -1);
    }
    // \n も1文字と数える: 両方(2) + \n(1) + 2行目(3) + 素(1) = 7
    CHECK(UiRichStrippedCodepoints(text) == 7);
}

// タグだけ / 空文字列 / タグ無しプレーン文
static void TestEdgeCases()
{
    const std::string empty;
    const std::string onlyTags = "[wave][/wave]";
    const std::string plain = "こんにちは world";
    CHECK(ParseUiRichText(empty).empty());
    CHECK(ParseUiRichText(onlyTags).empty());
    CHECK(UiRichStrippedCodepoints(onlyTags) == 0);
    const auto runs = ParseUiRichText(plain);
    CHECK(runs.size() == 1);
    if (runs.size() == 1)
        CHECK(RunText(runs[0]) == "こんにちは world");
    CHECK(UiRichStrippedCodepoints(plain) == 11);
}

int main()
{
    TestColorAndWaveSpans();
    TestUpperHexAndAnimKinds();
    TestUnknownTagsPassThrough();
    TestUnclosedAndStrayClose();
    TestOverlapAndNewline();
    TestEdgeCases();

    std::printf("ui_rich_text: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
