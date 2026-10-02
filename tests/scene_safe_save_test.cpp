// シーンの安全な保存（原子的な書き込み・複数ファイルのコミット・世代つきバックアップ・壊れたシーンの読み込み）のテスト。GPU 不要。
//
// 守りたいこと:
//   (1) 保存の途中のどこで失敗しても、シーン一式（foo.json + foo.parts/ + foo.inst/ + foo.nav）は「前の版」のまま 1 バイトも変わらない
//   (2) 置き換えの途中でプロセスが死んでも、次に開くとき「新しい版」で完全に揃う（新しいルート＋古いセルの食い違いが残らない）
//   (3) どちらの場合も一時ファイル（*.dx12tmp / *.dx12old / *.dx12txn）が残らない
//   (4) 壊れた・空・途中で切れたシーンは開けない（false）。理由が取れる
//   (5) 保存のたびに直前の版が世代として残り、世代数の上限が守られ、戻せる

#include "core/AtomicFile.h"
#include "core/PathResolver.h"
#include "core/SceneBackup.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/ScenePartition.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

using namespace dx12e;
namespace fs = std::filesystem;
namespace af = dx12e::atomicfile;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) { std::printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); ++g_failed; } \
    } while (0)

static std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void WriteAll(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

// ディレクトリ全体の中身（相対パス → バイト列）
static std::map<std::string, std::string> Snapshot(const fs::path& dir)
{
    std::map<std::string, std::string> m;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) m[fs::relative(e.path(), dir).generic_string()] = ReadAll(e.path());
    return m;
}
static int Leftovers(const fs::path& dir)
{
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    {
        const std::string x = e.path().extension().string();
        if (x == ".dx12tmp" || x == ".dx12old" || x == ".dx12txn") ++n;
    }
    return n;
}

// 40 体を 70m 間隔で並べる（セル 64m で 40 セルに分かれる）。version を変えると全エンティティの位置が変わる＝全セルが書き換わる。
static void Fill(Scene& s, int version, int count = 40)
{
    auto& reg = s.GetRegistry();
    reg.clear();
    for (int i = 0; i < count; ++i)
    {
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{"E" + std::to_string(i)});
        Transform t;
        t.position = {static_cast<float>(i) * 70.0f, static_cast<float>(version), 0.0f};
        reg.emplace<Transform>(e, t);
        BoxCollider bc;   // 「入れ物（名前と Transform だけ）」ではないのでセルへ割り当てられる
        reg.emplace<BoxCollider>(e, bc);
    }
    if (version >= 1)
    {
        // ルート（foo.json）にだけ置かれるもの（ライト）。版が変わるとルートも書き換わる＝ルートの置き換え失敗も試せる。
        const entt::entity l = reg.create();
        reg.emplace<NameTag>(l, NameTag{"Sun"});
        reg.emplace<Transform>(l, Transform{});
        DirectionalLight dl;
        dl.intensity = 1.0f + static_cast<float>(version);
        reg.emplace<DirectionalLight>(l, dl);
    }
}

static std::string GenRead(const fs::path& proj, const std::string& id, const std::string& rel)
{
    std::string out;
    scenebackup::ReadGenerationFile(proj, id, rel, out);
    return out;
}

static size_t CountEntities(Scene& s)
{
    size_t n = 0;
    for (auto e : s.GetRegistry().view<NameTag>()) { (void)e; ++n; }
    return n;
}

static void Test_SplitSaveFailures()
{
    scenebackup::GlobalPolicy().enabled = false;   // 世代の作成は別のテストで見る（故障注入と干渉させない）
    const fs::path root = fs::temp_directory_path() / "dx12_safe_save_split";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path file = root / "assets" / "scenes" / "big.json";
    const std::string assets = (root / "assets").string() + "/";

    Scene a;
    Fill(a, 0);
    a.SetPartitionCellSize(64.0f);
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    const auto rep0 = SceneSerializer::LastSaveReport();
    CHECK(rep0.partitioned && rep0.files >= 30);
    const auto v1 = Snapshot(root);
    CHECK(Leftovers(root) == 0);

    // 次の版: 全セルが書き換わる
    Fill(a, 1);
    const int cells = rep0.files;
    // 置き換えの k 番目が失敗（セル → ルートの順なので、途中・最後（ルート）の失敗も含む）
    for (int k : {0, 1, cells / 2, cells - 1, cells})
    {
        af::SetFaultForTest(af::Fault::ReplaceFails, k);
        const bool ok = SceneSerializer::Save(a, file.string(), assets);
        af::SetFaultForTest(af::Fault::None);
        CHECK(!ok);
        CHECK(Snapshot(root) == v1);          // 1 バイトも変わらない
        CHECK(Leftovers(root) == 0);
        Scene chk;
        CHECK(SceneSerializer::Load(chk, file.string(), assets));
        CHECK(CountEntities(chk) == 40);
    }
    // ステージ中のディスク満杯（最初のセルの途中で）
    for (int64_t at : {int64_t(0), int64_t(50)})
    {
        af::SetFaultForTest(af::Fault::DiskFullAfterBytes, at);
        const bool ok = SceneSerializer::Save(a, file.string(), assets);
        af::SetFaultForTest(af::Fault::None);
        CHECK(!ok);
        CHECK(Snapshot(root) == v1);
        CHECK(Leftovers(root) == 0);
    }
    // 書いた内容の検証の失敗
    af::SetFaultForTest(af::Fault::VerifyFails);
    CHECK(!SceneSerializer::Save(a, file.string(), assets));
    af::SetFaultForTest(af::Fault::None);
    CHECK(Snapshot(root) == v1 && Leftovers(root) == 0);

    // 置き換えの途中で死んだ（n 個目の直後）/ 記録を書いた直後に死んだ: 次に開くと新しい版で完全に揃う
    const std::string expect = SceneSerializer::SaveToString(a, assets);
    for (int n : {1, cells / 2, cells})
    {
        // 前の版へ戻してから試す
        fs::remove_all(root, ec);
        Scene base;
        Fill(base, 0);
        base.SetPartitionCellSize(64.0f);
        CHECK(SceneSerializer::Save(base, file.string(), assets));
        af::SetFaultForTest(af::Fault::CrashAfterReplaces, n);
        CHECK(SceneSerializer::Save(a, file.string(), assets));   // 死んだ想定（Commit は途中で打ち切る）
        af::SetFaultForTest(af::Fault::None);
        CHECK(fs::exists(fs::path(file).concat(".dx12txn")));      // 記録が残っている
        Scene b;
        CHECK(SceneSerializer::Load(b, file.string(), assets));    // 読み込みが最後まで置き換える
        CHECK(SceneSerializer::SaveToString(b, assets) == expect);
        CHECK(Leftovers(root) == 0);
    }
    {
        fs::remove_all(root, ec);
        Scene base;
        Fill(base, 0);
        base.SetPartitionCellSize(64.0f);
        CHECK(SceneSerializer::Save(base, file.string(), assets));
        af::SetFaultForTest(af::Fault::CrashAfterJournal);
        CHECK(SceneSerializer::Save(a, file.string(), assets));
        af::SetFaultForTest(af::Fault::None);
        Scene b;
        CHECK(SceneSerializer::Load(b, file.string(), assets));
        CHECK(SceneSerializer::SaveToString(b, assets) == expect);
        CHECK(Leftovers(root) == 0);
    }
    fs::remove_all(root, ec);
}

static void Test_PlainSaveFailure()
{
    scenebackup::GlobalPolicy().enabled = false;
    const fs::path root = fs::temp_directory_path() / "dx12_safe_save_plain";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path file = root / "assets" / "scenes" / "one.json";
    const std::string assets = (root / "assets").string() + "/";
    Scene a;
    Fill(a, 0, 5);
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    const auto v1 = Snapshot(root);
    Fill(a, 1, 8);
    af::SetFaultForTest(af::Fault::DiskFullAfterBytes, 100);
    CHECK(!SceneSerializer::Save(a, file.string(), assets));
    af::SetFaultForTest(af::Fault::ReplaceFails, 0);
    CHECK(!SceneSerializer::Save(a, file.string(), assets));
    af::SetFaultForTest(af::Fault::None);
    CHECK(Snapshot(root) == v1 && Leftovers(root) == 0);
    // 読み取り専用の本体
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(!SceneSerializer::Save(a, file.string(), assets));
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
    CHECK(Snapshot(root) == v1 && Leftovers(root) == 0);
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    Scene b;
    CHECK(SceneSerializer::Load(b, file.string(), assets) && CountEntities(b) == 9);   // 8 体 + ライト
    fs::remove_all(root, ec);
}

static void Test_CorruptLoad()
{
    const fs::path root = fs::temp_directory_path() / "dx12_safe_save_corrupt";
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::string assets = (root / "assets").string() + "/";
    const fs::path good = root / "assets" / "scenes" / "good.json";
    Scene a;
    Fill(a, 0, 6);
    scenebackup::GlobalPolicy().enabled = false;
    CHECK(SceneSerializer::Save(a, good.string(), assets));
    const std::string text = ReadAll(good);

    struct Case { const char* name; std::string body; };
    const Case cases[] = {
        {"空のファイル", ""},
        {"ゼロ埋め（電源断で起きる）", std::string(4096, '\0')},
        {"途中で切れた JSON", text.substr(0, text.size() / 2)},
        {"1 バイトだけ", "{"},
    };
    for (const Case& c : cases)
    {
        const fs::path p = root / "assets" / "scenes" / "bad.json";
        WriteAll(p, c.body);
        Scene s;
        CHECK(!SceneSerializer::Load(s, p.string(), assets));
        CHECK(!SceneSerializer::LastLoadError().empty());
        CHECK(CountEntities(s) == 0);   // 黙って中途半端なシーンを作らない
    }
    // 開けた後は理由が空に戻る
    Scene ok;
    CHECK(SceneSerializer::Load(ok, good.string(), assets));
    CHECK(SceneSerializer::LastLoadError().empty());
    CHECK(CountEntities(ok) == 6);
    fs::remove_all(root, ec);
}

static void Test_Backups()
{
    // 使い捨てのプロジェクトを作り、プロジェクトの根をそこへ向ける（バックアップは <根>/.dx12/backups）
    const fs::path proj = fs::temp_directory_path() / "dx12_safe_save_backups";
    const fs::path base = proj;
    std::error_code ec;
    fs::remove_all(proj, ec);
    fs::create_directories(proj);
    PathResolver::SetProjectRoot(proj.string());
    const fs::path file = proj / "assets" / "scenes" / "zzsafe.json";
    const std::string assets = (proj / "assets").string() + "/";
    auto cleanBackups = [&] {
        for (const auto& g : scenebackup::List(base, file)) scenebackup::Remove(base, g.id);
    };
    cleanBackups();

    scenebackup::Policy& pol = scenebackup::GlobalPolicy();
    pol = scenebackup::Policy{};
    pol.generations = 3;
    pol.minIntervalSec = 0;   // 毎回

    Scene a;
    Fill(a, 0, 4);
    CHECK(SceneSerializer::Save(a, file.string(), assets));                 // 初回: 前の版が無いので世代なし
    CHECK(scenebackup::List(base, file).empty());
    std::string versions[6];
    for (int v = 1; v <= 5; ++v)
    {
        versions[v - 1] = ReadAll(file);                                    // 保存の直前の版
        Fill(a, v, 4 + v);
        CHECK(SceneSerializer::Save(a, file.string(), assets));
        // 同じ秒に作ると連番になる。順序は id（stamp → 連番）で保たれる
    }
    auto gens = scenebackup::List(base, file);
    CHECK(gens.size() == 3);                                               // 上限 3 世代
    if (gens.size() != 3)
    {
        std::printf("  世代が %zu 件\n", gens.size());
        for (const auto& e : fs::recursive_directory_iterator(scenebackup::BackupDir(base), ec)) std::printf("    %s\n", e.path().filename().string().c_str());
        return;
    }
    // 最新の世代 = 保存の直前の版（v=5 の保存の直前 = versions[4]）
    if (!gens.empty()) CHECK(GenRead(base, gens.front().id, "root") == versions[4]);
    if (false) for (int i = 0; i < 5; ++i) std::printf("  version[%d] bytes=%zu\n", i, versions[i].size());

    // 変わっていない保存し直しは世代を増やさない（置き換わるファイルが無い）
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    CHECK(scenebackup::List(base, file).size() == 3);
    {
        // コピー方式の Snapshot（復元の直前・MCP の snapshot）: 今の版が世代に無ければ作り、同じ内容なら 2 回目は作らない
        std::string why;
        const std::string first = scenebackup::Snapshot(base, file, true, &why);
        if (first.empty()) std::printf("  Snapshot: %s\n", why.c_str());
        CHECK(!first.empty());
        CHECK(scenebackup::Snapshot(base, file, true, &why).empty());
        CHECK(scenebackup::List(base, file).size() == 3);   // 上限 3 世代
    }

    // 戻す: 最新の世代の版へ。戻す前の版も 1 世代残る
    const std::string newest = ReadAll(file);
    const std::string targetId = gens.front().id;
    std::string pre;
    const af::Result rr = scenebackup::Restore(base, file, targetId, &pre);
    CHECK(rr.ok);
    CHECK(ReadAll(file) == versions[4]);
    {
        // 戻す前の版（newest）は、いずれかの世代として残っている（直前に保存済みの世代があれば新しくは作らない）
        bool found = false;
        for (const auto& g : scenebackup::List(base, file)) if (GenRead(base, g.id, "root") == newest) found = true;
        CHECK(found);
    }
    Scene b;
    CHECK(SceneSerializer::Load(b, file.string(), assets));
    CHECK(CountEntities(b) == 4 + 4 + 1);   // v=4 の版（ライト 1 つ込み）

    // 方針: 無効なら作らない
    pol.enabled = false;
    Fill(a, 9, 3);
    const size_t before = scenebackup::List(base, file).size();
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    CHECK(scenebackup::List(base, file).size() == before);
    pol = scenebackup::Policy{};

    // 生成物のフォルダ（.autosave）の中は対象外
    {
        const fs::path auto_ = proj / "assets" / "scenes" / ".autosave" / "scene.json";
        Scene c;
        Fill(c, 0, 2);
        CHECK(SceneSerializer::Save(c, auto_.string(), assets));
        Fill(c, 1, 3);
        CHECK(SceneSerializer::Save(c, auto_.string(), assets));
        CHECK(scenebackup::List(base, auto_).empty());
    }

    cleanBackups();
    fs::remove_all(proj, ec);
    // バックアップの置き場に何も残さない（空になったフォルダは掃除）
    fs::remove(scenebackup::BackupDir(base), ec);
    fs::remove(base / ".dx12", ec);
}


static void Test_BackupsSplit()
{
    // 分割シーン: 変わったセルだけが置き換わるが、世代には全ファイル（変わらないセルはハードリンク）が入り、完全に戻せる
    const fs::path proj = fs::temp_directory_path() / "dx12_safe_save_backups_split";
    std::error_code ec;
    fs::remove_all(proj, ec);
    fs::create_directories(proj);
    PathResolver::SetProjectRoot(proj.string());
    const fs::path file = proj / "assets" / "scenes" / "zzsplit.json";
    const std::string assets = (proj / "assets").string() + "/";
    scenebackup::Policy& pol = scenebackup::GlobalPolicy();
    pol = scenebackup::Policy{};
    pol.minIntervalSec = 0;

    Scene a;
    Fill(a, 1, 30);
    a.SetPartitionCellSize(64.0f);
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    const auto v1 = Snapshot(proj / "assets");
    CHECK(v1.size() > 30);
    // 1 体だけ動かす（1 セルだけが書き換わる）
    for (auto [e, nt, tr] : a.GetRegistry().view<NameTag, Transform>().each())
        if (nt.name == "E3") tr.position.y += 0.5f;
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    CHECK(SceneSerializer::LastSaveReport().written == 1);   // 変わったセルだけ
    const auto gens = scenebackup::List(proj, file);
    CHECK(gens.size() == 1);
    if (gens.size() == 1)
    {
        CHECK(gens[0].hasParts);
        // 世代の一式は保存前の版そのもの
        CHECK(GenRead(proj, gens[0].id, "root") == v1.at("scenes/zzsplit.json"));
        int n = 0;
        for (const auto& [rel, bytes] : v1)
            if (rel.rfind("scenes/zzsplit.parts/", 0) == 0)
            {
                ++n;
                CHECK(GenRead(proj, gens[0].id, "parts/" + fs::path(rel).filename().string()) == bytes);
            }
        CHECK(n >= 30);
        // 戻す: 一式が保存前と完全に一致
        const auto v2 = Snapshot(proj / "assets");
        CHECK(scenebackup::Restore(proj, file, gens[0].id).ok);
        CHECK(Snapshot(proj / "assets") == v1 && Snapshot(proj / "assets") != v2);
        CHECK(Leftovers(proj / "assets") == 0);
    }
    // 世代の作成は別スレッドで行われる（保存の待ち時間に載せない）。終わってから、置き換え前の元ファイル（.dx12old）が残っていないこと
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    scenebackup::WaitIdle();
    CHECK(Leftovers(proj / "assets") == 0);
    pol = scenebackup::Policy{};
    fs::remove_all(proj, ec);
}

static int CountObjects(const fs::path& proj)
{
    std::error_code ec;
    int n = 0;
    const fs::path od = scenebackup::BackupDir(proj) / "objects";
    if (fs::is_directory(od, ec)) for (const auto& e : fs::directory_iterator(od, ec)) if (e.is_regular_file(ec) && e.path().extension() != ".tmp") ++n;
    return n;
}

static void Test_BackupsContentAddressed()
{
    // 内容アドレス方式: 現行ファイルをその場で上書きしても世代は変わらない・同じ内容は 1 つの実体・参照されなくなった実体だけ消える・旧方式の世代も戻せる
    const fs::path proj = fs::temp_directory_path() / "dx12_safe_save_cas";
    std::error_code ec;
    fs::remove_all(proj, ec);
    fs::create_directories(proj);
    PathResolver::SetProjectRoot(proj.string());
    const fs::path file = proj / "assets" / "scenes" / "zzcas.json";
    const std::string assets = (proj / "assets").string() + "/";
    scenebackup::Policy& pol = scenebackup::GlobalPolicy();
    pol = scenebackup::Policy{};
    pol.minIntervalSec = 0;

    Scene a;
    Fill(a, 1, 30);
    a.SetPartitionCellSize(64.0f);
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    const auto v1 = Snapshot(proj / "assets");
    for (auto [e, nt, tr] : a.GetRegistry().view<NameTag, Transform>().each())
        if (nt.name == "E3") tr.position.y += 0.5f;
    CHECK(SceneSerializer::Save(a, file.string(), assets));          // 世代 1 = v1
    auto gens = scenebackup::List(proj, file);
    CHECK(gens.size() == 1);
    if (gens.size() != 1) return;
    const std::string id1 = gens[0].id;

    // 変わらなかったセルを外部のエディタが「その場で」上書きしても、世代の中身は変わらない
    const fs::path victim = proj / "assets" / "scenes" / "zzcas.parts" / "cell_5_0.json";
    const std::string victimOrig = ReadAll(victim);
    CHECK(!victimOrig.empty());
    {
        std::ofstream o(victim, std::ios::binary | std::ios::trunc);
        o << "TAMPERED IN PLACE";
    }
    CHECK(GenRead(proj, id1, "parts/cell_5_0.json") == victimOrig);
    CHECK(GenRead(proj, id1, "root") == v1.at("scenes/zzcas.json"));

    // 2 世代目: 共有される実体は 1 つ（30 セル + ルート + 変わった 1 セル程度）
    for (auto [e, nt, tr] : a.GetRegistry().view<NameTag, Transform>().each())
        if (nt.name == "E7") tr.position.y += 0.5f;
    CHECK(SceneSerializer::Save(a, file.string(), assets));
    scenebackup::WaitIdle();
    gens = scenebackup::List(proj, file);
    CHECK(gens.size() == 2);
    const int objs = CountObjects(proj);
    CHECK(objs >= 31 && objs <= 36);   // 世代ごとに全ファイルを持つ方式なら 60 超

    // 世代を減らすと、参照されなくなった実体だけが消える。全部消せば実体も 0
    pol.generations = 1;
    scenebackup::Prune(proj, pol);
    CHECK(scenebackup::List(proj, file).size() == 1);
    CHECK(CountObjects(proj) < objs);
    for (const auto& g : scenebackup::List(proj, file)) scenebackup::Remove(proj, g.id);
    pol.generations = 10;
    scenebackup::Prune(proj, pol);
    CHECK(CountObjects(proj) == 0);

    // 旧方式（ファイルのコピー）の世代も一覧に出て戻せる
    {
        const fs::path bd = scenebackup::BackupDir(proj);
        fs::create_directories(bd / "zzcas_20200101_000000.parts");
        const std::string root0 = ReadAll(file);
        WriteAll(bd / "zzcas_20200101_000000.json", root0);
        std::string cell = ReadAll(proj / "assets" / "scenes" / "zzcas.parts" / "cell_0_0.json");
        WriteAll(bd / "zzcas_20200101_000000.parts" / "cell_0_0.json", cell);
        const auto lg = scenebackup::List(proj, file);
        CHECK(lg.size() == 1 && lg[0].legacy);
        WriteAll(file, "BROKEN");
        // 旧方式の世代は 1 セルだけ持つので、戻すとほかのセルは世代に無い扱いで消える（旧方式の仕様）。本体が戻ることだけ確かめる
        CHECK(scenebackup::Restore(proj, file, "zzcas_20200101_000000").ok);
        CHECK(ReadAll(file) == root0);
    }
    pol = scenebackup::Policy{};
    fs::remove_all(proj, ec);
}

#define RUN(f) do { std::printf("%s\n", #f); std::fflush(stdout); f(); } while (0)

int main()
{
    RUN(Test_PlainSaveFailure);
    RUN(Test_SplitSaveFailures);
    RUN(Test_CorruptLoad);
    RUN(Test_Backups);
    RUN(Test_BackupsSplit);
    RUN(Test_BackupsContentAddressed);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
