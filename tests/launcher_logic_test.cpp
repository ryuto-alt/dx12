// プロジェクトランチャーの単体テスト。
//   ・project/LauncherLogic.h  : テンプレート登録表 / 名前・保存場所・Git URL の検証 / 最近のプロジェクト / サムネイル
//   ・project/LauncherMotion.h : 補間 / スプリング / 入場スタッガー / 背景の粒
// 依存は std だけ（ImGui も Win32 も GPU も使わない）。テンプレの実体表だけ ProjectTemplates.cpp をリンクする。
#include "project/LauncherLogic.h"
#include "project/LauncherMotion.h"
#include "project/ProjectTemplates.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double aa_ = (a), bb_ = (b); if (!(std::fabs(aa_ - bb_) <= (eps))) { \
    std::printf("FAIL %s:%d  %s=%.5f vs %s=%.5f\n", __FILE__, __LINE__, #a, aa_, #b, bb_); ++g_fail; } } while (0)

namespace L = dx12e::launcher;
using L::Severity;

// ---- 偽のファイルシステム: パス → 種別（"dir" / "file" / "emptydir" / "proj"(.dx12proj を持つ dir)） ----
struct FakeFs
{
    std::map<std::string, std::string> nodes;
    std::set<std::string> readOnly;   // 書けないディレクトリ

    L::FsProbe Probe() const
    {
        L::FsProbe p;
        p.exists      = [this](const std::string& s) { return nodes.count(L::PathKey(s)) > 0; };
        p.isDirectory = [this](const std::string& s) { auto it = nodes.find(L::PathKey(s)); return it != nodes.end() && it->second != "file"; };
        p.isEmptyDir  = [this](const std::string& s) { auto it = nodes.find(L::PathKey(s)); return it != nodes.end() && it->second == "emptydir"; };
        p.canWriteDir = [this](const std::string& s) { return readOnly.count(L::PathKey(s)) == 0; };
        p.hasProjectFile = [this](const std::string& s) { auto it = nodes.find(L::PathKey(s)); return it != nodes.end() && it->second == "proj"; };
        return p;
    }
    void Add(const std::string& path, const std::string& kind) { nodes[L::PathKey(path)] = kind; }
};

FakeFs BaseFs()
{
    FakeFs f;
    f.Add("C:\\", "dir");
    f.Add("C:\\Users", "dir");
    f.Add("C:\\Users\\me", "dir");
    f.Add("C:\\Users\\me\\Documents", "dir");
    f.Add("C:\\Users\\me\\Documents\\Games", "dir");
    return f;
}

// ---------------------------------------------------------------- 文字列
void TestStrings()
{
    CHECK(L::Trim("  a b \t") == "a b");
    CHECK(L::Trim("") == "");
    CHECK(L::PathKey("C:\\Users\\Me\\Proj\\") == "c:/users/me/proj");
    CHECK(L::PathKey("C:\\") == "c:/");
    CHECK(L::PathKey("C:/A/b") == L::PathKey("c:\\a\\B"));
    CHECK(L::PathLeaf("C:\\a\\b\\") == "b");
    CHECK(L::PathLeaf("C:/a/proj") == "proj");
    CHECK(L::JoinPath("C:\\a", "b") == "C:\\a\\b");
    CHECK(L::JoinPath("C:\\a\\", "b") == "C:\\a\\b");

    CHECK(L::Utf8Length("abc") == 3);
    CHECK(L::Utf8Length("日本語") == 3);
    CHECK(L::ContainsFullWidthSpace("a　b"));
    CHECK(!L::ContainsFullWidthSpace("a b"));
    CHECK(L::ContainsNonAscii("é"));
    CHECK(!L::ContainsNonAscii("abc"));

    CHECK(L::MiddleEllipsis("short", 10) == "short");
    const std::string e = L::MiddleEllipsis("C:\\Users\\someone\\Documents\\Projects\\MyGame", 20);
    CHECK(L::Utf8Length(e) == 20);
    CHECK(e.find("…") != std::string::npos);
    CHECK(e.rfind("Game") == e.size() - 4);            // 末尾（フォルダ名）は残る
    CHECK(e.compare(0, 2, "C:") == 0);                   // 先頭（ドライブ）は残る
    const std::string j = L::MiddleEllipsis("日本語のとても長いフォルダ名のパスです", 8);
    CHECK(L::Utf8Length(j) == 8);                         // マルチバイトでも文字数で切る（バイトの途中で切らない）
    CHECK(L::MiddleEllipsis("abc", 1) == "…");
}

// ---------------------------------------------------------------- 時刻
void TestTime()
{
    int y, m, d;
    L::CivilFromDays(0, y, m, d);            CHECK(y == 1970 && m == 1 && d == 1);
    L::CivilFromDays(10957, y, m, d);        CHECK(y == 2000 && m == 1 && d == 1);
    L::CivilFromDays(11016, y, m, d);        CHECK(y == 2000 && m == 2 && d == 29);   // うるう日
    L::CivilFromDays(11017, y, m, d);        CHECK(y == 2000 && m == 3 && d == 1);
    CHECK(L::FormatDate(946684800, 0) == "2000/01/01");
    CHECK(L::FormatDate(946684800 - 1, 0) == "1999/12/31");
    CHECK(L::FormatDate(946684800, 9 * 3600) == "2000/01/01");
    CHECK(L::FormatDate(946684800 - 3600, 9 * 3600) == "2000/01/01");                 // JST では 1 時間前でもまだ元日
    CHECK(L::FormatDate(946684800 + 15 * 3600 + 1, 9 * 3600) == "2000/01/02");        // 日付をまたぐ

    const int64_t now = 1'800'000'000;
    CHECK(L::FormatRelativeTime(now, 0) == "");
    CHECK(L::FormatRelativeTime(now, now - 10) == "たった今");
    CHECK(L::FormatRelativeTime(now, now - 59) == "たった今");
    CHECK(L::FormatRelativeTime(now, now - 60) == "1 分前");
    CHECK(L::FormatRelativeTime(now, now - 3599) == "59 分前");
    CHECK(L::FormatRelativeTime(now, now - 3600) == "1 時間前");
    CHECK(L::FormatRelativeTime(now, now - 86399) == "23 時間前");
    CHECK(L::FormatRelativeTime(now, now - 86400) == "昨日");
    CHECK(L::FormatRelativeTime(now, now - 2 * 86400) == "2 日前");
    CHECK(L::FormatRelativeTime(now, now - 29 * 86400) == "29 日前");
    CHECK(L::FormatRelativeTime(now, now - 30 * 86400, 0) == L::FormatDate(now - 30 * 86400, 0));
    CHECK(L::FormatRelativeTime(now, now + 100) == "たった今");   // 未来（時計ずれ）は「たった今」に丸める
}

// ---------------------------------------------------------------- 名前
void TestName()
{
    CHECK(!L::ValidateProjectName("MyGame").HasError());
    CHECK(!L::ValidateProjectName("MyGame").HasWarn());
    CHECK(!L::ValidateProjectName("My Game 2").HasError());
    CHECK(L::ValidateProjectName("").HasCode("name.empty"));
    CHECK(L::ValidateProjectName("   ").HasCode("name.empty"));
    CHECK(L::ValidateProjectName(" abc").HasCode("name.edge"));
    CHECK(L::ValidateProjectName("abc ").HasCode("name.edge"));
    CHECK(L::ValidateProjectName("abc.").HasCode("name.edge"));
    for (const char* bad : { "a<b", "a>b", "a:b", "a\"b", "a/b", "a\\b", "a|b", "a?b", "a*b" })
        CHECK(L::ValidateProjectName(bad).HasCode("name.chars"));
    CHECK(L::ValidateProjectName(std::string("a") + '\x01' + "b").HasCode("name.chars"));
    for (const char* r : { "CON", "con", "PRN", "aux", "NUL", "COM1", "com9", "LPT3", "con.txt", "CON " })
        CHECK(L::ValidateProjectName(r).HasError());
    CHECK(!L::ValidateProjectName("COM0").HasError());      // COM0 は予約ではない
    CHECK(!L::ValidateProjectName("CONSOLE").HasError());   // 前方一致では予約扱いしない
    CHECK(!L::ValidateProjectName("COM10").HasError());
    // 長さ
    CHECK(L::ValidateProjectName(std::string(65, 'a')).HasCode("name.long"));
    CHECK(!L::ValidateProjectName(std::string(65, 'a')).HasError());   // 警告だけ
    CHECK(!L::ValidateProjectName(std::string(64, 'a')).HasWarn());
    CHECK(L::ValidateProjectName(std::string(121, 'a')).HasCode("name.too_long"));
    CHECK(L::ValidateProjectName(std::string(121, 'a')).HasError());
    // 日本語 / 全角スペースは警告（エラーにはしない）
    { auto v = L::ValidateProjectName("私のゲーム"); CHECK(v.HasCode("name.non_ascii")); CHECK(!v.HasError()); CHECK(v.Worst() == Severity::Warn); }
    { auto v = L::ValidateProjectName("my　game"); CHECK(v.HasCode("name.fullwidth_space")); CHECK(!v.HasCode("name.non_ascii")); CHECK(!v.HasError()); }
    CHECK(L::ValidateProjectName(".hidden").HasCode("name.dot_start"));
    // 重い順の並べ替え
    { auto v = L::ValidateProjectName("私: 名"); auto s = v.Sorted(); CHECK(!s.empty() && s.front().sev == Severity::Error); }
}

// ---------------------------------------------------------------- 保存場所
void TestLocation()
{
    FakeFs f = BaseFs();
    const auto fs = f.Probe();
    const std::string good = "C:\\Users\\me\\Documents\\Games";

    { auto v = L::ValidateLocation(good, "Proj", fs); CHECK(v.Worst() == Severity::Ok); }
    CHECK(L::ValidateLocation("", "Proj", fs).HasCode("loc.empty"));
    CHECK(L::ValidateLocation("   ", "Proj", fs).HasCode("loc.empty"));
    CHECK(L::ValidateLocation("Games\\Proj", "Proj", fs).HasCode("loc.relative"));
    CHECK(L::ValidateLocation("\\Games", "Proj", fs).HasCode("loc.relative"));
    CHECK(L::ValidateLocation("C:Games", "Proj", fs).HasCode("loc.relative"));
    CHECK(L::ValidateLocation("C:\\Ga|mes", "Proj", fs).HasCode("loc.chars"));
    CHECK(L::ValidateLocation("C:\\Ga?mes", "Proj", fs).HasCode("loc.chars"));
    CHECK(L::ValidateLocation("C:\\Ga:mes", "Proj", fs).HasCode("loc.chars"));      // ドライブ以外の ':'
    CHECK(!L::ValidateLocation(good, "Proj", fs).HasCode("loc.chars"));              // ドライブの ':' は正常
    CHECK(L::ValidateLocation("C:\\Users\\me \\Proj", "P", fs).HasCode("loc.segment_edge"));
    // UNC は絶対パスとして受ける（存在しなければ no_root）
    CHECK(!L::ValidateLocation("\\\\server\\share\\x", "Proj", fs).HasCode("loc.relative"));

    // 存在しないドライブ / 親フォルダ
    CHECK(L::ValidateLocation("Z:\\Nowhere\\Deep", "Proj", fs).HasCode("loc.no_root"));
    { auto v = L::ValidateLocation("C:\\Users\\me\\NewFolder\\Sub", "Proj", fs);
      CHECK(v.HasCode("loc.will_create_parents")); CHECK(!v.HasError()); }
    // 保存場所がファイル
    { FakeFs g = BaseFs(); g.Add("C:\\Users\\me\\Documents\\afile", "file");
      CHECK(L::ValidateLocation("C:\\Users\\me\\Documents\\afile", "Proj", g.Probe()).HasCode("loc.not_dir")); }
    // 書き込み権限
    { FakeFs g = BaseFs(); g.readOnly.insert(L::PathKey(good));
      auto v = L::ValidateLocation(good, "Proj", g.Probe()); CHECK(v.HasCode("loc.not_writable")); CHECK(v.HasError()); }
    // 親が無くて、最寄りの祖先が書き込み不可
    { FakeFs g = BaseFs(); g.readOnly.insert(L::PathKey("C:\\Users\\me"));
      CHECK(L::ValidateLocation("C:\\Users\\me\\Nope\\Deeper", "P", g.Probe()).HasCode("loc.not_writable")); }

    // 作成先が既にある
    { FakeFs g = BaseFs(); g.Add(good + "\\Proj", "file");
      CHECK(L::ValidateLocation(good, "Proj", g.Probe()).HasCode("target.is_file")); }
    { FakeFs g = BaseFs(); g.Add(good + "\\Proj", "dir");
      auto v = L::ValidateLocation(good, "Proj", g.Probe()); CHECK(v.HasCode("target.exists_nonempty")); CHECK(v.HasError()); }
    { FakeFs g = BaseFs(); g.Add(good + "\\Proj", "emptydir");
      auto v = L::ValidateLocation(good, "Proj", g.Probe()); CHECK(v.HasCode("target.exists_empty")); CHECK(!v.HasError()); }
    { FakeFs g = BaseFs(); g.Add(good + "\\Proj", "proj");
      auto v = L::ValidateLocation(good, "Proj", g.Probe()); CHECK(v.HasCode("target.already_project")); CHECK(v.HasError());
      auto c = L::ValidateLocation(good, "Proj", g.Probe(), /*forClone*/ true); CHECK(!c.HasCode("target.already_project")); CHECK(c.HasError()); }
    // 大文字小文字違いも同じフォルダ（Windows）
    { FakeFs g = BaseFs(); g.Add(good + "\\proj", "dir");
      CHECK(L::ValidateLocation(good, "PROJ", g.Probe()).HasCode("target.exists_nonempty")); }

    // 警告: 日本語 / 全角スペース / 長さ / システム / OneDrive / 入れ子
    { FakeFs g = BaseFs(); g.Add("C:\\Users\\me\\ドキュメント", "dir");
      auto v = L::ValidateLocation("C:\\Users\\me\\ドキュメント", "Proj", g.Probe());
      CHECK(v.HasCode("path.non_ascii")); CHECK(!v.HasError()); }
    { FakeFs g = BaseFs(); g.Add("C:\\Users\\me\\a　b", "dir");
      auto v = L::ValidateLocation("C:\\Users\\me\\a　b", "Proj", g.Probe());
      CHECK(v.HasCode("path.fullwidth_space")); CHECK(!v.HasCode("path.non_ascii")); CHECK(!v.HasError()); }
    { const std::string longP = "C:\\Users\\me\\Documents\\Games\\" + std::string(170, 'x');
      FakeFs g = BaseFs(); g.Add(longP, "dir");
      auto v = L::ValidateLocation(longP, "Proj", g.Probe()); CHECK(v.HasCode("path.long")); CHECK(!v.HasError()); }
    { const std::string longP = "C:\\Users\\me\\Documents\\Games\\" + std::string(230, 'x');
      FakeFs g = BaseFs(); g.Add(longP, "dir");
      auto v = L::ValidateLocation(longP, "Proj", g.Probe()); CHECK(v.HasCode("path.too_long")); CHECK(v.HasError()); }
    { FakeFs g = BaseFs(); g.Add("C:\\Program Files", "dir"); g.Add("C:\\Program Files\\Uno", "dir");
      CHECK(L::ValidateLocation("C:\\Program Files\\Uno", "Proj", g.Probe()).HasCode("loc.protected")); }
    { FakeFs g = BaseFs(); g.Add("C:\\Users\\me\\OneDrive", "dir"); g.Add("C:\\Users\\me\\OneDrive\\Documents", "dir");
      CHECK(L::ValidateLocation("C:\\Users\\me\\OneDrive\\Documents", "Proj", g.Probe()).HasCode("loc.onedrive")); }
    { FakeFs g = BaseFs(); g.Add(good + "\\Outer", "proj");
      CHECK(L::ValidateLocation(good + "\\Outer", "Inner", g.Probe()).HasCode("loc.inside_project")); }
    // 末尾の区切りは許す
    CHECK(!L::ValidateLocation(good + "\\", "Proj", fs).HasError());
}

void TestNewProjectAndSuggest()
{
    FakeFs f = BaseFs();
    const auto fs = f.Probe();
    const std::string good = "C:\\Users\\me\\Documents\\Games";
    std::vector<L::RecentRecord> rec;
    rec.push_back({ "Other", "C:\\x\\Other", 10, false });

    CHECK(!L::ValidateNewProject("Proj", good, fs, rec).HasError());
    CHECK(L::ValidateNewProject("", good, fs, rec).HasCode("name.empty"));
    CHECK(L::ValidateNewProject("Proj", "", fs, rec).HasCode("loc.empty"));
    CHECK(L::ValidateNewProject("a/b", good, fs, rec).HasError());
    // 最近の一覧に同名（別の場所）→ 警告
    { auto v = L::ValidateNewProject("other", good, fs, rec); CHECK(v.HasCode("dup.recent_name")); CHECK(!v.HasError()); }
    // 同じ場所の同名は「重複」ではなく target 側で扱う
    { rec.push_back({ "Proj", good + "\\Proj", 5, false });
      CHECK(!L::ValidateNewProject("Proj", good, fs, rec).HasCode("dup.recent_name")); }

    // 既定名の連番
    CHECK(L::SuggestUniqueName(good, "MyGame", fs) == "MyGame");
    f.Add(good + "\\MyGame", "dir");
    CHECK(L::SuggestUniqueName(good, "MyGame", f.Probe()) == "MyGame2");
    f.Add(good + "\\MyGame2", "dir");
    CHECK(L::SuggestUniqueName(good, "MyGame", f.Probe()) == "MyGame3");
    CHECK(L::SuggestUniqueName("", "MyGame", f.Probe()) == "MyGame");
    CHECK(L::SuggestUniqueName("Z:\\none", "MyGame", f.Probe()) == "MyGame");
}

// 実ファイルシステム（一時フォルダ）で FsProbe と検証が噛み合うこと
void TestRealFs()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "uno_launcher_test_tmp";
    fs::remove_all(root, ec);
    fs::create_directories(root / "parent", ec);
    fs::create_directories(root / "parent" / "emptyproj", ec);
    fs::create_directories(root / "parent" / "full", ec);
    { std::ofstream(root / "parent" / "full" / "a.txt") << "x"; }
    fs::create_directories(root / "parent" / "isproj", ec);
    { std::ofstream(root / "parent" / "isproj" / "Foo.dx12proj") << "{}"; }
    { std::ofstream(root / "parent" / "afile") << "x"; }

    const auto real = L::RealFs();
    const std::string parent = L::PathToUtf8(root / "parent");
    CHECK(real.exists(parent));
    CHECK(real.isDirectory(parent));
    CHECK(!real.exists(parent + "\\nothing"));
    CHECK(real.canWriteDir(parent));
    CHECK(!real.canWriteDir(parent + "\\afile"));                    // ファイルには「書ける」と言わない
    CHECK(!real.canWriteDir(parent + "\\nothing"));
    CHECK(real.isEmptyDir(parent + "\\emptyproj"));
    CHECK(!real.isEmptyDir(parent + "\\full"));
    CHECK(real.hasProjectFile(parent + "\\isproj"));
    CHECK(!real.hasProjectFile(parent + "\\full"));
    // 書き込みプローブは後始末する（一時ファイルを残さない）
    { size_t n = 0; for (auto it = fs::directory_iterator(root / "parent", ec); it != fs::directory_iterator(); ++it) ++n;
      CHECK(n == 4); }

    CHECK(!L::ValidateLocation(parent, "NewOne", real).HasError());
    CHECK(L::ValidateLocation(parent, "full", real).HasCode("target.exists_nonempty"));
    CHECK(L::ValidateLocation(parent, "emptyproj", real).HasCode("target.exists_empty"));
    CHECK(L::ValidateLocation(parent, "isproj", real).HasCode("target.already_project"));
    CHECK(L::ValidateLocation(parent, "afile", real).HasCode("target.is_file"));
    CHECK(L::ValidateLocation(parent + "\\afile", "x", real).HasCode("loc.not_dir"));
    CHECK(L::ValidateLocation(parent + "\\a\\b\\c", "x", real).HasCode("loc.will_create_parents"));
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------- Git URL
void TestGitUrl()
{
    CHECK(L::RepoNameFromGitUrl("https://github.com/owner/repo.git") == "repo");
    CHECK(L::RepoNameFromGitUrl("https://github.com/owner/repo") == "repo");
    CHECK(L::RepoNameFromGitUrl("https://github.com/owner/repo/") == "repo");
    CHECK(L::RepoNameFromGitUrl("git@github.com:owner/repo.git") == "repo");
    CHECK(L::RepoNameFromGitUrl("  https://x/y/Z.GIT ") == "Z");
    CHECK(!L::ValidateGitUrl("https://github.com/owner/repo.git").HasError());
    CHECK(!L::ValidateGitUrl("git@github.com:owner/repo.git").HasError());
    CHECK(!L::ValidateGitUrl("ssh://git@host/x/y.git").HasError());
    CHECK(L::ValidateGitUrl("").HasCode("url.empty"));
    CHECK(L::ValidateGitUrl("owner/repo").HasCode("url.scheme"));
    CHECK(L::ValidateGitUrl("ftp://x/y").HasCode("url.scheme"));
    CHECK(L::ValidateGitUrl("https://x/y z").HasCode("url.space"));
    CHECK(L::ValidateGitUrl("https://").HasError());
}

// ---------------------------------------------------------------- 最近のプロジェクト
void TestRecents()
{
    using R = L::RecentRecord;
    std::vector<R> v = { { "a", "C:\\p\\a", 100, false }, { "b", "C:\\p\\b", 300, false },
                         { "c", "C:\\p\\c", 200, true }, { "d", "C:\\p\\d", 0, false }, { "e", "C:\\p\\e", 0, false } };
    L::SortRecents(v);
    CHECK(v[0].name == "c");                 // ピン留めが先頭
    CHECK(v[1].name == "b" && v[2].name == "a");   // 新しい順
    CHECK(v[3].name == "d" && v[4].name == "e");   // 記録なしは元の並びを保つ

    // 開いた → 先頭・時刻更新・ピン維持・重複排除（大文字小文字/区切り違いでも同じパス）
    L::UpsertRecent(v, "a2", "c:/P/A/", 999);
    CHECK(v.size() == 5);
    { int n = 0; for (auto& r : v) if (L::PathKey(r.path) == "c:/p/a") ++n; CHECK(n == 1); }
    CHECK(v[1].name == "a2" && v[1].lastOpened == 999);   // ピン留め(c)の次に来る
    L::UpsertRecent(v, "c", "C:\\p\\c", 1000);
    CHECK(v[0].name == "c" && v[0].pinned && v[0].lastOpened == 1000);

    // ピン留めの切替と削除
    CHECK(L::SetPinned(v, "C:\\p\\b", true));
    CHECK(v[0].pinned && v[1].pinned);
    CHECK(!L::SetPinned(v, "C:\\none", true));
    CHECK(L::SetPinned(v, "C:\\p\\b", false));
    CHECK(!v[0].pinned || v[0].name == "c");
    CHECK(L::RemoveRecent(v, "C:\\p\\d"));
    CHECK(!L::RemoveRecent(v, "C:\\p\\d"));
    CHECK(v.size() == 4);

    // 上限: ピン無しは 24 件まで・ピンは落とさない
    std::vector<R> big;
    L::UpsertRecent(big, "pinned", "C:\\keep", 1);
    L::SetPinned(big, "C:\\keep", true);
    for (int i = 0; i < 40; ++i) L::UpsertRecent(big, "p" + std::to_string(i), "C:\\proj\\" + std::to_string(i), 100 + i);
    CHECK(big.size() == L::kMaxUnpinnedRecents + 1);
    CHECK(big.front().name == "pinned");
    CHECK(big[1].name == "p39");
    CHECK(big.back().name == "p" + std::to_string(40 - static_cast<int>(L::kMaxUnpinnedRecents)));

    // 検索
    std::vector<R> s = { { "MikuOblivion", "C:\\g\\MikuOblivion", 3, false }, { "Nocturne", "C:\\g\\Nocturne", 2, false },
                         { "horror", "D:\\work\\horror", 1, false }, { "私のゲーム", "C:\\g\\私のゲーム", 0, false } };
    CHECK(L::FilterRecents(s, "").size() == 4);
    CHECK(L::FilterRecents(s, "   ").size() == 4);
    CHECK(L::FilterRecents(s, "miku") == std::vector<int>({ 0 }));            // 大文字小文字を区別しない
    CHECK(L::FilterRecents(s, "NOCT") == std::vector<int>({ 1 }));
    CHECK(L::FilterRecents(s, "d:\\work") == std::vector<int>({ 2 }));        // 区切りは \ でも / でも同じ
    CHECK(L::FilterRecents(s, "d:/work") == std::vector<int>({ 2 }));
    CHECK(L::FilterRecents(s, "work") == std::vector<int>({ 2 }));             // パスにも効く
    CHECK(L::FilterRecents(s, "g mi") == std::vector<int>({ 0 }));             // 語の AND
    CHECK(L::FilterRecents(s, "ゲーム") == std::vector<int>({ 3 }));           // 日本語
    CHECK(L::FilterRecents(s, "zzz").empty());
}

// ---------------------------------------------------------------- サムネイル
void TestThumbnail()
{
    CHECK(L::ThumbnailPath("C:\\proj\\Foo") == "C:\\proj\\Foo/.dx12/thumbnail.png");
    CHECK(L::ThumbnailPath("C:\\proj\\Foo\\") == "C:\\proj\\Foo/.dx12/thumbnail.png");
    CHECK(L::ThumbnailPath("C:/proj/Foo/") == "C:/proj/Foo/.dx12/thumbnail.png");
    CHECK(L::ThumbnailRelPath() == ".dx12/thumbnail.png");

    auto c = L::CenterCrop16x9(1920, 1080); CHECK(c.x == 0 && c.y == 0 && c.w == 1920 && c.h == 1080);
    c = L::CenterCrop16x9(1000, 1000);      CHECK(c.w == 1000 && c.h == 562 && c.x == 0 && c.y == 219);   // 正方形 → 上下を切る
    c = L::CenterCrop16x9(3440, 1440);      CHECK(c.h == 1440 && c.w == 2560 && c.x == 440 && c.y == 0);   // ウルトラワイド → 左右を切る
    c = L::CenterCrop16x9(0, 10);           CHECK(c.w == 0 && c.h == 0);
    c = L::CenterCrop16x9(801, 451);        CHECK(c.w <= 801 && c.h <= 451 && c.x >= 0 && c.y >= 0 && c.x + c.w <= 801 && c.y + c.h <= 451);

    // 単色 → 縮小しても同じ色
    { std::vector<uint8_t> src(64 * 36 * 4);
      for (size_t i = 0; i < 64 * 36; ++i) { src[i * 4] = 10; src[i * 4 + 1] = 20; src[i * 4 + 2] = 30; src[i * 4 + 3] = 255; }
      const auto out = L::DownscaleBgra(src.data(), 64, 36, 64 * 4, L::CenterCrop16x9(64, 36), 16, 9);
      CHECK(out.size() == 16 * 9 * 4);
      bool same = true;
      for (size_t i = 0; i < 16 * 9; ++i) same = same && out[i * 4] == 10 && out[i * 4 + 1] == 20 && out[i * 4 + 2] == 30 && out[i * 4 + 3] == 255;
      CHECK(same); }
    // 白黒の市松（1px）を 2 倍縮小 → 灰（箱フィルタ）
    { const int W = 32, H = 18; std::vector<uint8_t> src(W * H * 4);
      for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { const uint8_t v = ((x + y) & 1) ? 255 : 0;
        uint8_t* p = &src[(y * W + x) * 4]; p[0] = p[1] = p[2] = v; p[3] = 255; }
      const auto out = L::DownscaleBgra(src.data(), W, H, W * 4, L::CenterCrop16x9(W, H), 16, 9);
      CHECK(std::abs(static_cast<int>(out[0]) - 127) <= 1); }
    // 切り出しの位置: 左半分が赤・右半分が青の正方形を 16:9 へ → 端は切れるが左右の色は保たれる
    { const int W = 100, H = 100; std::vector<uint8_t> src(W * H * 4);
      for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { uint8_t* p = &src[(y * W + x) * 4];
        p[0] = x < 50 ? 0 : 255; p[1] = 0; p[2] = x < 50 ? 255 : 0; p[3] = 255; }
      const auto out = L::DownscaleBgra(src.data(), W, H, W * 4, L::CenterCrop16x9(W, H), 32, 18);
      CHECK(out[2] == 255 && out[0] == 0);                                  // 左端 = 赤（BGRA の R は [2]）
      CHECK(out[(31) * 4 + 0] == 255 && out[(31) * 4 + 2] == 0); }          // 右端 = 青
    // 範囲外を読まない（ピッチが幅より大きい・端の丸め）
    { const int W = 45, H = 27, pitch = 256; std::vector<uint8_t> src(static_cast<size_t>(pitch) * H, 200);
      const auto out = L::DownscaleBgra(src.data(), W, H, pitch, L::CenterCrop16x9(W, H), 20, 11);
      CHECK(out.size() == 20 * 11 * 4); CHECK(out[0] == 200); }

    // 真っ黒 / 単色の失敗画は保存しない
    { std::vector<uint8_t> black(16 * 9 * 4, 0); CHECK(L::ThumbnailLooksBlank(black, 16, 9)); }
    { std::vector<uint8_t> flat(16 * 9 * 4, 90); CHECK(L::ThumbnailLooksBlank(flat, 16, 9)); }
    { std::vector<uint8_t> img(16 * 9 * 4); for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>((i * 37) & 0xFF);
      CHECK(!L::ThumbnailLooksBlank(img, 16, 9)); }
    CHECK(L::ThumbnailLooksBlank({}, 16, 9));
}

// ---------------------------------------------------------------- テンプレート登録表
void TestTemplates()
{
    const auto& t = L::Templates();
    CHECK(t.size() >= 4);
    std::set<std::string> ids;
    for (const auto& d : t)
    {
        CHECK(d.id && d.id[0]);
        CHECK(ids.insert(d.id).second);                               // ID は一意
        CHECK(d.name && d.name[0] && d.subtitle && d.tagline && d.description);
        CHECK(!d.features.empty() && !d.includes.empty());
        CHECK(d.iconGlyph && d.iconGlyph[0]);
        // ProjectTemplates.cpp に実体があり、開始シーンを含む（空扱いへ黙って落ちない）
        const auto& files = dx12e::templates::GetFiles(d.id);
        bool hasMain = false;
        for (const auto& f : files) if (std::string(f.relPath) == "assets/scenes/main.json") hasMain = true;
        CHECK(hasMain);
        // 画像ファイルが実在する（指定があれば）
        if (d.cardImage && d.cardImage[0])
        {
            const std::filesystem::path p = std::filesystem::path(LAUNCHER_ASSET_DIR) / d.cardImage;
            std::error_code ec;
            const bool exists = std::filesystem::exists(p, ec);
            if (!exists) std::printf("  missing image: %s\n", p.string().c_str());
            CHECK(exists);
        }
    }
    CHECK(ids.count("fps") && ids.count("tps") && ids.count("2d") && ids.count("empty"));
    CHECK(L::FindTemplate("fps") != nullptr);
    CHECK(L::FindTemplate("nope") == nullptr);
    // 標準語チェック（関西弁の語尾を持ち込まない）
    for (const auto& d : t)
        for (const char* s : { d.tagline, d.description })
        {
            const std::string x = s;
            CHECK(x.find("やで") == std::string::npos && x.find("へん") == std::string::npos && x.find("やん") == std::string::npos);
        }
}

// ---------------------------------------------------------------- 動き
void TestMotion()
{
    // 半減期
    CHECK_NEAR(L::SmoothTo(0.0f, 1.0f, 0.1f, 0.1f), 0.5, 1e-5);
    CHECK_NEAR(L::SmoothTo(0.0f, 1.0f, 0.2f, 0.1f), 0.75, 1e-5);
    CHECK(L::SmoothTo(3.0f, 5.0f, 0.0f, 0.1f) == 3.0f);       // dt=0 は動かない
    CHECK(L::SmoothTo(3.0f, 5.0f, 0.016f, 0.0f) == 5.0f);     // 半減期 0 = 即座（アニメ OFF）
    // フレームレート非依存: 1 回の 0.2s と 20 回の 0.01s は同じ
    { float a = 0.0f, b = 0.0f; a = L::SmoothTo(a, 1.0f, 0.2f, 0.07f); for (int i = 0; i < 20; ++i) b = L::SmoothTo(b, 1.0f, 0.01f, 0.07f);
      CHECK_NEAR(a, b, 1e-5); }
    // 単調に目標へ・行き過ぎない
    { float x = 0.0f; float prev = 0.0f; bool mono = true;
      for (int i = 0; i < 200; ++i) { x = L::SmoothTo(x, 1.0f, 1.0f / 60.0f, 0.08f); mono = mono && x >= prev && x <= 1.0f; prev = x; }
      CHECK(mono); CHECK_NEAR(x, 1.0, 1e-3); }

    // 臨界減衰スプリング: 目標に収束・行き過ぎない
    { L::SpringCD s; s.Snap(0.0f); float maxX = 0.0f;
      for (int i = 0; i < 300; ++i) { s.Step(100.0f, 18.0f, 1.0f / 60.0f); maxX = (std::max)(maxX, s.x); }
      CHECK(maxX <= 100.0001f); CHECK_NEAR(s.x, 100.0, 0.05); CHECK_NEAR(s.v, 0.0, 0.05); }
    // dt を刻み直しても同じ（閉形式）
    { L::SpringCD a, b; a.Snap(0); b.Snap(0);
      a.Step(50.0f, 12.0f, 0.2f); for (int i = 0; i < 20; ++i) b.Step(50.0f, 12.0f, 0.01f);
      CHECK_NEAR(a.x, b.x, 1e-3); CHECK_NEAR(a.v, b.v, 1e-2); }
    { L::SpringCD s; s.Snap(7.0f); s.Step(7.0f, 12.0f, 0.5f); CHECK_NEAR(s.x, 7.0, 1e-6); }   // 目標にいれば動かない

    // 入場スタッガー: 遅らせた要素は後から始まり、0..1 に収まり、単調
    CHECK(L::RevealAmount(0.0f, 0, 0.05f, 0.3f) == 0.0f);
    CHECK(L::RevealAmount(0.1f, 3, 0.05f, 0.3f) == 0.0f);     // index 3 は 0.15s まで始まらない
    CHECK(L::RevealAmount(0.5f, 0, 0.05f, 0.3f) == 1.0f);
    { float prev = 0.0f; bool mono = true;
      for (int i = 0; i <= 60; ++i) { const float r = L::RevealAmount(i / 100.0f, 1, 0.05f, 0.3f); mono = mono && r >= prev && r <= 1.0f; prev = r; }
      CHECK(mono); }
    CHECK(L::EaseOutCubicf(0.5f) > 0.5f);                     // out は前半で進む（linear ではない）
    CHECK(L::EaseOutCubicf(-1.0f) == 0.0f && L::EaseOutCubicf(2.0f) == 1.0f);

    // 粒: 決定論・数・範囲
    L::ParticleField a, b;
    a.Reset(1234, 48, 1920.0f, 1080.0f);
    b.Reset(1234, 48, 1920.0f, 1080.0f);
    CHECK(a.p.size() == 48);
    bool same = true;
    for (size_t i = 0; i < a.p.size(); ++i) same = same && a.p[i].x == b.p[i].x && a.p[i].y == b.p[i].y && a.p[i].vy == b.p[i].vy;
    CHECK(same);
    L::ParticleField c; c.Reset(999, 48, 1920.0f, 1080.0f);
    bool differ = false;
    for (size_t i = 0; i < a.p.size(); ++i) differ = differ || a.p[i].x != c.p[i].x;
    CHECK(differ);
    for (int i = 0; i < 6000; ++i) { a.Update(1.0f / 60.0f); }
    bool inBounds = true;
    for (const auto& q : a.p) inBounds = inBounds && q.x >= -4.5f && q.x <= 1924.5f && q.y >= -4.5f && q.y <= 1084.5f && q.size > 0.0f;
    CHECK(inBounds);
    CHECK(a.p.size() == 48);
    { L::ParticleField z; z.Reset(1, 8, 100, 100); const auto before = z.p; z.Update(0.0f);
      CHECK(z.p[0].x == before[0].x && z.p[0].y == before[0].y); }
    { L::ParticleField z; z.Reset(1, 8, 100, 100); z.Update(5.0f); CHECK(z.time <= 0.1f + 1e-6f); }   // 大きな dt は丸める
    { L::ParticleField z; z.Reset(1, 8, 100, 100); const float x0 = z.p[0].x; z.Resize(200, 50); CHECK_NEAR(z.p[0].x, x0 * 2.0f, 1e-4); }
    { L::ParticleField z; z.Reset(1, 0, 100, 100); z.Update(0.016f); CHECK(z.p.empty()); }   // 0 個でも落ちない
    { L::ParticleField z; z.Reset(5, 4, 100, 100); float lo = 1, hi = 0; for (int i = 0; i < 400; ++i) { z.Update(0.05f); for (auto& q : z.p) { const float t = z.Twinkle(q); lo = (std::min)(lo, t); hi = (std::max)(hi, t); } }
      CHECK(lo >= 0.25f - 1e-4f && hi <= 1.0f + 1e-4f); }
}
// ---------------------------------------------------------------- ニュース本文
void TestNews()
{
    using K = L::NewsBlock::Kind;
    const std::string body =
        "v1.2.3: 見出しです\n"
        "\n"
        "■【新機能】あいうえお\n"
        "  一行目の途中で\n"
        "  折り返した続き。\n"
        "\n"
        "■【修正】Foo Bar\n"
        "  Hello\n"
        "  World です。\n"
        "  ・箇条書きその 1\n"
        "  ・箇条書きその 2\n"
        "\n"
        "おしまいの一言\n";
    const auto b = L::ParseNewsBody(body);
    CHECK(b.size() == 8);
    if (b.size() == 8)
    {
        CHECK(b[0].kind == K::Headline && b[0].text == "v1.2.3: 見出しです");
        CHECK(b[1].kind == K::Section && b[1].text == "【新機能】あいうえお");
        CHECK(b[2].kind == K::Paragraph && b[2].text == "一行目の途中で折り返した続き。");   // 日本語の折り返しは空白なしで結合
        CHECK(b[3].kind == K::Section);
        CHECK(b[4].kind == K::Paragraph && b[4].text == "Hello World です。");                // 英単語どうしは空白を補う
        CHECK(b[5].kind == K::Bullet && b[5].text == "箇条書きその 1");
        CHECK(b[6].kind == K::Bullet && b[6].text == "箇条書きその 2");
        CHECK(b[7].kind == K::Paragraph && b[7].text == "おしまいの一言");
    }
    CHECK(L::ParseNewsBody("").empty());
    CHECK(L::ParseNewsBody("\n\n  \n").empty());
    CHECK(L::ParseNewsBody("見出し\r\n■ 節\r\n  本文\r\n").size() == 3);                   // CRLF
    // 実物（Version.cpp の本文）を通しても落ちない・空にならない
}

}  // namespace

int main()
{
    TestStrings();
    TestTime();
    TestName();
    TestLocation();
    TestNewProjectAndSuggest();
    TestRealFs();
    TestGitUrl();
    TestRecents();
    TestThumbnail();
    TestTemplates();
    TestMotion();
    TestNews();
    if (g_fail == 0) std::printf("LauncherTests: all passed\n");
    else std::printf("LauncherTests: %d failed\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
