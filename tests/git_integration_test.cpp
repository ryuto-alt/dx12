// GitIntegration::ToWebUrl（remote URL → ブラウザで開ける https URL）の単体テスト。
//
// 外部テストフレームワークに依存しない (ctest が終了コードで合否を判定する)。
// 実際に git/gh プロセスを起動する関数は対象外（純粋な文字列変換のみ検証）。
//
// 実行: ctest --output-on-failure (失敗があれば終了コード 1)

#include "project/GitIntegration.h"

#include <cstdio>
#include <string>

using dx12e::GitIntegration;

namespace
{
int g_failures = 0;
int g_checks   = 0;
} // namespace

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        ++g_checks;                                                                      \
        std::string a_ = (actual);                                                       \
        std::string e_ = (expected);                                                     \
        if (a_ != e_) {                                                                  \
            std::printf("FAIL %s:%d: got \"%s\", want \"%s\"\n",                         \
                __FILE__, __LINE__, a_.c_str(), e_.c_str());                             \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

// オフバイワンの再発防止（"ryuto-alt" の先頭 'r' が消えて "yuto-alt" になっていたバグの実例）。
static void Test_ToWebUrl_Https()
{
    CHECK_EQ(GitIntegration::ToWebUrl("https://github.com/ryuto-alt/dx12.git"),
             "https://github.com/ryuto-alt/dx12");
    CHECK_EQ(GitIntegration::ToWebUrl("https://github.com/ryuto-alt/dx12"),
             "https://github.com/ryuto-alt/dx12");
}

static void Test_ToWebUrl_Http()
{
    CHECK_EQ(GitIntegration::ToWebUrl("http://github.com/owner/repo.git"),
             "https://github.com/owner/repo");
}

static void Test_ToWebUrl_Scp()
{
    CHECK_EQ(GitIntegration::ToWebUrl("git@github.com:owner/repo.git"),
             "https://github.com/owner/repo");
}

static void Test_ToWebUrl_Ssh()
{
    CHECK_EQ(GitIntegration::ToWebUrl("ssh://git@github.com/owner/repo.git"),
             "https://github.com/owner/repo");
}

static void Test_ToWebUrl_TrailingSlashAndWhitespace()
{
    CHECK_EQ(GitIntegration::ToWebUrl("  https://github.com/owner/repo/  \n"),
             "https://github.com/owner/repo");
}

static void Test_ToWebUrl_Invalid()
{
    CHECK_EQ(GitIntegration::ToWebUrl(""), "");
    CHECK_EQ(GitIntegration::ToWebUrl("https://gitlab.com/owner/repo.git"), "");
    CHECK_EQ(GitIntegration::ToWebUrl("not a url"), "");
}

int main()
{
    Test_ToWebUrl_Https();
    Test_ToWebUrl_Http();
    Test_ToWebUrl_Scp();
    Test_ToWebUrl_Ssh();
    Test_ToWebUrl_TrailingSlashAndWhitespace();
    Test_ToWebUrl_Invalid();

    std::printf("GitIntegration: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
