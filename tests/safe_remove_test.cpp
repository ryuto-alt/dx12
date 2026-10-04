// 再帰削除の最後の砦（core/SafeRemove.h）のテスト。
//
// 守りたいこと: どこから来たパスでも、ドライブのルート・ルート直下・ホームとその直下・システムのフォルダ・
// 実行中の exe のフォルダ（とそれらの祖先）は消さない。base を渡したら base の中だけ。
// ★危ないパスには RemoveAll を絶対に呼ばない（柵に穴があればテストが事故を起こす）。判定（WhyUnsafe）だけを見る。
//   実際に消す・断るの確認は一時フォルダの中に作った使い捨てのフォルダだけで行う。

#include "core/SafeRemove.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { std::printf("FAIL: %s %s (%s:%d)\n", #cond, "" __VA_ARGS__, __FILE__, __LINE__); ++g_failed; } \
    } while (0)

static bool Refused(const fs::path& p, const fs::path& base = {}) { return !saferm::WhyUnsafe(p, base).empty(); }

static fs::path EnvPath(const char* name)
{
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) return {};
    fs::path p = v;
    std::free(v);
    return p;
}

static void Put(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

static int g_hookCalls = 0;
static std::string g_hookMsg;

int main()
{
    // ---- 断るべきパス（判定だけ） ----------------------------------------------------------
    CHECK(Refused(fs::path()), "空");
    CHECK(Refused("foo"), "相対");
    CHECK(Refused("foo\\bar\\baz"), "相対（深くても）");
    CHECK(Refused("\\"), "現在のドライブのルート（ルート名なし）");
    CHECK(Refused("\\Users\\someone\\x"), "ルート名なしは絶対パスではない");
    CHECK(Refused("C:"), "ドライブ相対");
    CHECK(Refused("C:foo\\bar"), "ドライブ相対");
    CHECK(Refused("C:\\"), "C ドライブのルート");
    CHECK(Refused("C:/"), "C ドライブのルート（/）");
    CHECK(Refused("D:\\"), "別ドライブのルート");
    CHECK(Refused("C:\\vcpkg"), "ルート直下");
    CHECK(Refused("C:\\vcpkg\\"), "ルート直下（末尾の区切り）");
    CHECK(Refused("C:\\Users"), "ルート直下（ホームの親）");
    CHECK(Refused("c:\\USERS"), "大文字小文字を区別しない");
    CHECK(Refused("C:\\a\\.."), "正規化するとルート");
    CHECK(Refused("C:\\a\\b\\..\\.."), "正規化するとルート");
    CHECK(Refused("\\\\server\\share"), "共有のルート");

    const fs::path home = EnvPath("USERPROFILE");
    CHECK(!home.empty(), "USERPROFILE が取れる");
    if (!home.empty())
    {
        CHECK(Refused(home), "ホーム");
        CHECK(Refused(home.wstring() + L"\\"), "ホーム（末尾の区切り）");
        CHECK(Refused(home / "Documents"), "ホーム直下（Documents）");
        CHECK(Refused(home / "AppData"), "ホーム直下（AppData）");
        CHECK(Refused(home / "some-project"), "ホーム直下（プロジェクトのフォルダ）");
        CHECK(Refused(home / "AppData" / "Local"), "LocalAppData");
        CHECK(Refused(home / "AppData" / "Roaming"), "RoamingAppData");
        CHECK(Refused(home / "some-project" / ".."), "正規化するとホーム");
        CHECK(Refused(home / "x" / ".." / ".."), "正規化するとホームの親");
        // ホームの直下より深いところは消してよい（プロジェクトの中のビルド出力など）
        CHECK(!Refused(home / "some-project" / "build"), "ホームの 2 段下は可");
    }
    const fs::path windir = EnvPath("SystemRoot");
    if (!windir.empty()) CHECK(Refused(windir), "Windows");
    const fs::path temp = fs::temp_directory_path();
    CHECK(Refused(temp), "Temp 自体");
    CHECK(Refused(temp.wstring() + L"\\"), "Temp 自体（末尾の区切り）");
    CHECK(Refused(temp.parent_path()), "Temp の親");

    // 実行中の exe のフォルダとその祖先
    {
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        CHECK(n > 0, "exe のパスが取れる");
        const fs::path exeDir = fs::path(exe).parent_path();
        CHECK(Refused(exeDir), "exe のフォルダ");
        CHECK(Refused(exeDir.parent_path()), "exe のフォルダの親");
    }

    // ---- 実際に消す・断る（一時フォルダの中だけ） --------------------------------------------
    const fs::path root = temp / ("dx12e_saferm_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(root, ec);
    CHECK(!ec, "作業フォルダを作れる");
    CHECK(!Refused(root), "Temp の中の作業フォルダは消してよい");

    saferm::SetRefusalHook([](const std::string& m) { ++g_hookCalls; g_hookMsg = m; });

    // 普通に消せる
    {
        const fs::path d = root / "plain";
        Put(d / "a.txt", "a");
        Put(d / "sub" / "b.txt", "b");
        ec.clear();
        const auto n = saferm::RemoveAll(d, ec);
        CHECK(!ec, "安全なフォルダは消せる");
        CHECK(n == 4, "フォルダ 2 + ファイル 2");
        CHECK(!fs::exists(d), "消えている");
        CHECK(saferm::LastRefusal().empty(), "断っていない");
        CHECK(g_hookCalls == 0, "フックは呼ばれない");
    }
    // base の中なら消せる
    {
        const fs::path base = root / "base";
        const fs::path d = base / "inner";
        Put(d / "a.txt", "a");
        ec.clear();
        saferm::RemoveAll(d, ec, base);
        CHECK(!ec && !fs::exists(d), "base の中は消せる");
        CHECK(fs::exists(base), "base 自体は残る");
    }
    // base 自体・base の外・名前が前方一致するだけの隣は断る（何も消えない）
    {
        const fs::path base = root / "ab";
        Put(base / "keep.txt", "k");
        Put(root / "abc" / "keep.txt", "k");
        Put(root / "other" / "keep.txt", "k");

        ec.clear();
        CHECK(saferm::RemoveAll(base, ec, base) == 0, "base 自体");
        CHECK(ec == std::errc::operation_not_permitted, "断ったら operation_not_permitted");
        CHECK(fs::exists(base / "keep.txt"), "base 自体は消えない");
        CHECK(!saferm::LastRefusal().empty(), "理由が残る");
        CHECK(g_hookCalls == 1, "フックが 1 回呼ばれる");
        CHECK(g_hookMsg.find("再帰削除を断りました") != std::string::npos, "フックに理由が渡る");

        ec.clear();
        saferm::RemoveAll(root / "abc", ec, base);
        CHECK(ec && fs::exists(root / "abc" / "keep.txt"), "前方一致するだけの隣は base の中ではない");

        ec.clear();
        saferm::RemoveAll(root / "other", ec, base);
        CHECK(ec && fs::exists(root / "other" / "keep.txt"), "base の外");

        ec.clear();
        saferm::RemoveAll(base / ".." / "other", ec, base);
        CHECK(ec && fs::exists(root / "other" / "keep.txt"), ".. で base の外へ出るパス");

        ec.clear();
        saferm::RemoveAll(base / "x", ec, fs::path("relative\\base"));
        CHECK(ec, "base が相対パスなら断る");
    }
    // 相対パスは、指す先が実在していても断る
    {
        const fs::path d = root / "rel";
        Put(d / "a.txt", "a");
        const fs::path cwd = fs::current_path();
        fs::current_path(root);
        ec.clear();
        saferm::RemoveAll(fs::path("rel"), ec);
        fs::current_path(cwd);
        CHECK(ec && fs::exists(d / "a.txt"), "相対パスは断る");
    }
    // 例外版
    {
        bool threw = false;
        try { saferm::RemoveAll(root / "other", root / "ab"); }
        catch (const fs::filesystem_error& e) { threw = std::string(e.what()).find("再帰削除を断りました") != std::string::npos; }
        CHECK(threw, "例外版は filesystem_error（理由つき）");
        CHECK(fs::exists(root / "other" / "keep.txt"), "例外版でも消えない");
    }

    saferm::SetRefusalHook(nullptr);
    ec.clear();
    saferm::RemoveAll(root, ec);
    CHECK(!ec && !fs::exists(root), "後片付け");

    // ---- src/ に再帰削除の直書きが無いこと（柵を迂回する経路を作らせない） -------------------
    {
        const std::string needle = std::string("remove") + "_all(";
        int scanned = 0;
        for (fs::recursive_directory_iterator it(fs::path(DX12E_SRC_DIR), ec), end; !ec && it != end; it.increment(ec))
        {
            if (!it->is_regular_file()) continue;
            const fs::path& p = it->path();
            const std::string ext = p.extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".inc" && ext != ".hpp") continue;
            if (p.filename() == "SafeRemove.h") continue;
            ++scanned;
            std::ifstream f(p, std::ios::binary);
            std::string line;
            int ln = 0;
            while (std::getline(f, line))
            {
                ++ln;
                if (line.find(needle) == std::string::npos) continue;
                std::printf("FAIL: 再帰削除の直書き（saferm::RemoveAll を使う）: %s:%d\n", p.string().c_str(), ln);
                ++g_failed;
            }
        }
        CHECK(scanned > 100, "src/ を走査できている");
    }

    std::printf("SafeRemoveTests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
