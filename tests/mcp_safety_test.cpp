// MCP の副作用の安全性（M5）の単体テスト: 冪等ストア / guarded 確認トークン / ファイル書き込みジャーナル。
//
// ★エンジンをリンクしない。core/mcp/McpSafety.h（ヘッダオンリー）と core/mcp/McpJournal.h + McpJournal.cpp
//   （標準ライブラリ + std::filesystem だけ）を直接ビルドして検査する。時計は注入して決定論にしている。
//   実エンジンの dispatcher（ゲート拒否・冪等再送・dryRun・tx ロールバック）は MCP 経由の実機検証で見る（docs/MCP.md §13）。
//
// 実行: ctest --output-on-failure -R McpSafety

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/mcp/McpSafety.h"

namespace fs = std::filesystem;
using namespace dx12e;
using namespace dx12e::mcpsafety;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const std::string& label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label.c_str());
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream ifs(p, std::ios::binary);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

void WriteFile(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    for (const auto& x : v) if (x == s) return true;
    return false;
}

// ---------------------------------------------------------------------------
// 冪等ストア
// ---------------------------------------------------------------------------
void TestIdempotency()
{
    std::printf("[idempotency]\n");
    int64_t now = 1000000;
    using K = IdempotencyStore::Kind;
    IdempotencyStore st(4, 600000, 120000, [&] { return now; });

    Check(st.Find("k1", "set_transform", 11).kind == K::Miss, "未知のキーは Miss");
    st.BeginInFlight("k1", "set_transform", 11);
    Check(st.Find("k1", "set_transform", 11).kind == K::InFlight, "処理中は InFlight");
    Check(st.Find("k1", "set_transform", 12).kind == K::Conflict, "InFlight でも引数(hash)が違えば Conflict");
    st.Complete("k1", "{\"ok\":true,\"result\":{\"a\":1}}");
    {
        const auto f = st.Find("k1", "set_transform", 11);
        Check(f.kind == K::Replay && f.resp.find("\"a\":1") != std::string::npos, "完了後は Replay で前回の応答を返す");
        Check(f.firstAtMs == 1000000, "firstAtMs は最初の時刻");
    }
    Check(st.Find("k1", "set_component", 11).kind == K::Conflict, "method が違えば Conflict");
    Check(st.Find("k1", "set_transform", 99).kind == K::Conflict, "引数(hash)が違えば Conflict");
    {
        const auto f = st.Find("k1", "set_component", 11);
        Check(f.existingMethod == "set_transform", "Conflict は最初の method を返す");
    }

    // 失敗したら InFlight を消す（再送で再実行できる）
    st.BeginInFlight("k2", "save_scene", 5);
    st.Abort("k2");
    Check(st.Find("k2", "save_scene", 5).kind == K::Miss, "Abort した InFlight は Miss（再送で再実行できる）");
    // Done は Abort で消えない
    st.Abort("k1");
    Check(st.Find("k1", "set_transform", 11).kind == K::Replay, "Done は Abort で消えない");

    // TTL
    now += 599000;
    Check(st.Find("k1", "set_transform", 11).kind == K::Replay, "TTL 内は Replay");
    now += 2000;
    Check(st.Find("k1", "set_transform", 11).kind == K::Miss, "TTL(600 秒)を過ぎたら Miss");
    // InFlight は 120 秒で期限切れ（応答が来なかった要求が塞ぎ続けない）
    st.BeginInFlight("k3", "delete_entity", 7);
    now += 121000;
    Check(st.Find("k3", "delete_entity", 7).kind == K::Miss, "InFlight は 120 秒で期限切れ");

    // 容量 4・LRU
    st.Clear();
    for (int i = 0; i < 4; ++i) { st.BeginInFlight("c" + std::to_string(i), "m", 1); st.Complete("c" + std::to_string(i), "{}"); }
    Check(st.Size() == 4, "容量ちょうどまで入る");
    (void)st.Find("c0", "m", 1);   // c0 を最近使った扱いにする
    st.BeginInFlight("c4", "m", 1);
    Check(st.Size() == 4, "容量を超えない");
    Check(st.Find("c0", "m", 1).kind == K::Replay, "最近使った c0 は残る");
    Check(st.Find("c1", "m", 1).kind == K::Miss, "最も使われていない c1 が捨てられる");
    Check(st.Capacity() == 4 && st.TtlSec() == 600, "Capacity / TtlSec");

    // 既定値
    IdempotencyStore def;
    Check(def.Capacity() == 256 && def.TtlSec() == 600, "既定は容量 256・TTL 600 秒");

    // 遅延応答の結び付け
    st.Clear();
    st.BeginInFlight("d1", "delete_entity", 3);
    st.BindRequest(77, 5, "d1");
    Check(st.PendingRequests() == 1, "遅延応答を結び付けた");
    Check(!st.ResolveRequest(77, 6, true, "{}"), "別の requestId は無視");
    Check(st.ResolveRequest(77, 5, true, "{\"id\":5,\"ok\":true,\"result\":{\"deleted\":1}}"), "応答が出たら結び付けが解ける");
    Check(st.Find("d1", "delete_entity", 3).kind == K::Replay, "遅延応答の完了で Done になる");
    st.BeginInFlight("d2", "delete_entity", 3);
    st.BindRequest(77, 8, "d2");
    Check(st.ResolveRequest(77, 8, false, "{\"ok\":false}"), "失敗応答も結び付けが解ける");
    Check(st.Find("d2", "delete_entity", 3).kind == K::Miss, "遅延応答が失敗なら InFlight を消す");
    Check(st.PendingRequests() == 0, "結び付けが残らない");
}

// ---------------------------------------------------------------------------
// guarded 確認トークン
// ---------------------------------------------------------------------------
void TestGuard()
{
    std::printf("[guard token]\n");
    int64_t now = 5000;
    using R = GuardTokens::Result;
    GuardTokens g(60000, 32, [&] { return now; });

    const std::string t1 = g.Issue("eval_lua");
    Check(t1.size() == 32, "トークンは 128bit（hex 32 文字）");
    Check(g.Issue("eval_lua") != t1, "トークンは毎回違う");
    Check(g.Consume(t1, "eval_lua") == R::Ok, "発行した method で 1 回使える");
    Check(g.Consume(t1, "eval_lua") == R::Unknown, "同じトークンの再利用は拒否（1 回限り）");

    const std::string t2 = g.Issue("git_push");
    Check(g.Consume(t2, "eval_lua") == R::WrongMethod, "別 method 用のトークンは拒否");
    Check(g.Consume(t2, "git_push") == R::Unknown, "拒否されたトークンも消費される（使い回せない）");
    Check(g.Consume("", "eval_lua") == R::Unknown && g.Consume("deadbeef", "eval_lua") == R::Unknown, "空・未知のトークンは拒否");

    const std::string t3 = g.Issue("build_game");
    now += 59000;
    Check(g.Consume(t3, "build_game") == R::Ok, "TTL 内は使える");
    const std::string t4 = g.Issue("build_game");
    now += 61000;
    Check(g.Consume(t4, "build_game") == R::Expired, "TTL(60 秒)を過ぎたら Expired");

    // 上限 32（古い順に捨てる）
    GuardTokens g2(60000, 32, [&] { return now; });
    std::vector<std::string> ts;
    for (int i = 0; i < 33; ++i) ts.push_back(g2.Issue("eval_lua"));
    Check(g2.Size() == 32, "保持は最大 32 個");
    Check(g2.Consume(ts[0], "eval_lua") == R::Unknown, "33 個目を発行すると最も古い 1 個が無効になる");
    Check(g2.Consume(ts[32], "eval_lua") == R::Ok, "最新は有効");

    GuardTokens gd;
    Check(gd.TtlSec() == 60, "既定の TTL は 60 秒");
    gd.SetTtlMs(2000);
    Check(gd.TtlSec() == 2, "TTL を縮められる（実機テスト用 DX12_MCP_GUARD_TTL_SEC の元）");
}

// ---------------------------------------------------------------------------
// ジャーナル
// ---------------------------------------------------------------------------
void TestJournal(const fs::path& root)
{
    std::printf("[journal]\n");
    using namespace dx12e::mcpjournal;
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path base = root / "proj";
    fs::create_directories(base);
    int64_t clockMs = 1700000000000;
    Journal j;
    j.SetBaseDir(base);
    j.SetClock([&] { return ++clockMs; });
    Check(j.Root() == base / ".dx12" / "journal", "置き場は <baseDir>/.dx12/journal");
    Check(!j.HasScope() && !j.Backup(base / "a.txt"), "スコープが無いと Backup は何もしない");

    // 上書きの復元
    const fs::path a = base / "assets" / "scenes" / "a.json";
    WriteFile(a, "v1-original");
    Check(j.BeginCall("save_scene"), "BeginCall はスコープを開く");
    Check(j.Backup(a), "Backup できる");
    Check(j.Backup(a), "同じ path は 1 エントリで 1 回だけ（2 回目も true）");
    WriteFile(a, "v2-overwritten");
    const std::string id1 = j.EndCall();
    Check(id1 == "000001-save_scene", "id は <seq 6 桁>-<method>: " + id1);
    Check(fs::exists(j.Root() / id1 / "manifest.json") && fs::exists(j.Root() / id1 / "files" / "0.bin"), "manifest.json と files/0.bin ができる");
    {
        EntryInfo e;
        Check(j.Load(id1, e) && e.state == "committed" && e.method == "save_scene" && e.files.size() == 1, "manifest を読める(committed / 1 ファイル)");
        Check(e.files[0].path == "assets/scenes/a.json" && e.files[0].existed && e.files[0].backup == "files/0.bin" && e.files[0].bytes == 11,
              "path は baseDir 相対・区切りは '/'・bytes が入る");
    }
    {
        const auto r = j.Restore(id1, false);
        Check(r.found && r.restored.size() == 1 && ReadFile(a) == "v1-original", "上書き前の内容へ復元できる");
        const auto r2 = j.Restore(id1, false);
        Check(r2.restored.empty() && r2.unchanged.size() == 1, "既に元の内容なら noop（unchanged）");
        EntryInfo e;
        j.Load(id1, e);
        Check(e.state == "restored", "全部戻したエントリは restored");
    }

    // 不在 → 削除（新規作成の取り消し）
    const fs::path nb = base / "assets" / "components" / "new.lua";
    j.BeginCall("create_lua_component");
    j.Backup(nb);
    WriteFile(nb, "return {}");
    const std::string id2 = j.EndCall();
    Check(id2 == "000002-create_lua_component", "seq は既存の最大 + 1: " + id2);
    {
        EntryInfo e;
        j.Load(id2, e);
        Check(e.files.size() == 1 && !e.files[0].existed && e.files[0].backup.empty(), "存在しなかったファイルは existed:false");
        const auto r = j.Restore(id2, false);
        Check(!fs::exists(nb) && r.restored.size() == 1, "新規作成されたファイルは復元で消える");
    }

    // 何も退避しなければフォルダを残さない
    Check(j.BeginCall("set_transform"), "BeginCall");
    Check(j.EndCall().empty(), "1 つも退避しなかったら空 id");
    Check(j.EntryCount() == 2, "空のエントリはフォルダを作らない");

    // 一覧（新しい順）
    {
        const auto l = j.List(10);
        Check(l.size() == 2 && l[0].id == id2 && l[1].id == id1, "List は新しい順");
        Check(j.List(1).size() == 1, "List の limit");
    }

    // トランザクション: 1 エントリにまとめて rollback で戻す
    WriteFile(a, "tx-original");
    j.BeginTx("batch(3)");
    Check(j.TxActive() && j.HasScope() && j.TxLabel() == "batch(3)", "tx が開く");
    Check(!j.BeginCall("save_scene"), "tx 中の BeginCall は false（tx のエントリへ入る）");
    j.Backup(a);
    WriteFile(a, "tx-overwritten");
    const fs::path nc = base / "assets" / "shaders" / "s.hlsl";
    j.Backup(nc);
    WriteFile(nc, "float4 main() {}");
    j.Backup(nc);   // 2 回目は無視
    RestoreResult rr;
    const std::string id3 = j.RollbackTx(&rr);
    Check(!j.TxActive() && !id3.empty(), "RollbackTx で tx が閉じる: " + id3);
    Check(ReadFile(a) == "tx-original" && !fs::exists(nc), "rollback で上書きは復元・新規は削除");
    Check(rr.restored.size() == 2, "rollback の結果に復元した path が入る");
    {
        EntryInfo e;
        j.Load(id3, e);
        Check(e.state == "rolledBack" && e.hasTx && e.txLabel == "batch(3)", "state:rolledBack・txLabel が入る");
    }
    // commit は残る（後から journal_restore できる）
    j.BeginTx("t2");
    j.Backup(a);
    WriteFile(a, "tx2-new");
    const std::string id4 = j.CommitTx();
    Check(ReadFile(a) == "tx2-new" && !id4.empty(), "commit ではファイルはそのまま");
    {
        const auto r = j.Restore(id4, false);
        Check(ReadFile(a) == "tx-original" && r.restored.size() == 1, "commit 済みのエントリも後から戻せる");
    }

    // backupFirst: 復元自体も 1 エントリとして残る（やり直せる）
    {
        WriteFile(a, "before-restore");
        Check(j.BeginCall("journal_restore"), "journal_restore のスコープ");
        const auto r = j.Restore(id4, true);
        const std::string idr = j.EndCall();
        Check(!idr.empty() && ReadFile(a) == "tx-original" && r.restored.size() == 1, "backupFirst で復元して自分のエントリを残す");
        j.Restore(idr, false);
        Check(ReadFile(a) == "before-restore", "復元の復元で直前の内容に戻る");
    }

    // 64MB 超は退避しない（complete:false）
    {
        const fs::path big = base / "assets" / "big.bin";
        fs::create_directories(big.parent_path());
        {
            std::ofstream ofs(big, std::ios::binary | std::ios::trunc);
            const std::string chunk(1024 * 1024, 'x');
            for (int i = 0; i < 65; ++i) ofs.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        }
        j.BeginCall("delete_asset");
        j.Backup(big);
        const std::string idb = j.EndCall();
        EntryInfo e;
        Check(j.Load(idb, e) && !e.complete && e.files.size() == 1 && e.files[0].skipped == "too_large" && e.files[0].backup.empty(),
              "64MB 超は skipped:too_large・complete:false");
        Check(!fs::exists(j.Root() / idb / "files" / "0.bin"), "64MB 超のコピーは作らない");
        fs::remove(big, ec);
        const auto r = j.Restore(idb, false);
        Check(r.missing.size() == 1 && !r.warnings.empty() && !r.complete, "戻せないファイルは missing + 警告");
        Check(!fs::exists(big), "退避が無いので戻せない（ファイルは作られない）");
    }

    // ディレクトリの再帰（delete_asset recursive）
    {
        const fs::path dir = base / "assets" / "props";
        WriteFile(dir / "a.txt", "A");
        WriteFile(dir / "sub" / "b.txt", "BB");
        j.BeginCall("delete_asset");
        Check(j.BackupTree(dir), "BackupTree");
        const std::string idd = j.EndCall();
        fs::remove_all(dir, ec);
        Check(!fs::exists(dir), "（削除した）");
        const auto r = j.Restore(idd, false);
        Check(r.restored.size() == 2 && ReadFile(dir / "a.txt") == "A" && ReadFile(dir / "sub" / "b.txt") == "BB", "フォルダ配下を個別に復元（フォルダは作り直す）");
    }

    // MarkIncomplete
    {
        j.BeginCall("move_asset");
        j.MarkIncomplete("参照書き換えの他ファイルは退避していない");
        const std::string idm = j.EndCall();
        EntryInfo e;
        Check(!idm.empty() && j.Load(idm, e) && !e.complete && e.note.find("参照書き換え") != std::string::npos, "MarkIncomplete は complete:false + note");
    }

    // ベースの外の絶対パス
    {
        const fs::path outside = root / "outside" / "x.txt";
        WriteFile(outside, "OUT");
        j.BeginCall("import_asset");
        j.Backup(outside);
        const std::string ido = j.EndCall();
        EntryInfo e;
        j.Load(ido, e);
        Check(e.files.size() == 1 && fs::path(e.files[0].path).is_absolute(), "baseDir の外は絶対パスで記録");
        WriteFile(outside, "CHANGED");
        j.Restore(ido, false);
        Check(ReadFile(outside) == "OUT", "絶対パスも復元できる");
    }
}

// TS（dx12_scene_write）が書く形式のエントリを読んで復元できる
void TestJournalTsFormat(const fs::path& root)
{
    std::printf("[journal ts format]\n");
    using namespace dx12e::mcpjournal;
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path base = root / "proj";
    Journal j;
    j.SetBaseDir(base);
    const fs::path jr = base / ".dx12" / "journal";
    const fs::path dir = jr / "000007-scene_write";
    // TS の JSON.stringify(…, null, 2) 相当（インデント・日本語・エスケープ・null）
    WriteFile(dir / "files" / "0.bin", "{\"元のシーン\":true}\n");
    WriteFile(dir / "manifest.json",
        "{\n"
        "  \"version\": 1,\n"
        "  \"id\": \"000007-scene_write\",\n"
        "  \"method\": \"scene_write\",\n"
        "  \"label\": \"部屋 \\\"A\\\" を配置\",\n"
        "  \"createdAt\": 1790000000123,\n"
        "  \"state\": \"committed\",\n"
        "  \"txLabel\": null,\n"
        "  \"complete\": true,\n"
        "  \"files\": [\n"
        "    {\n"
        "      \"path\": \"assets/scenes/日本語のシーン.json\",\n"
        "      \"existed\": true,\n"
        "      \"backup\": \"files/0.bin\",\n"
        "      \"bytes\": 20,\n"
        "      \"skipped\": null\n"
        "    },\n"
        "    { \"path\": \"assets/scenes/new.json\", \"existed\": false, \"backup\": null, \"bytes\": 0, \"skipped\": null }\n"
        "  ]\n"
        "}\n");
    const fs::path target = base / PathFromUtf8("assets/scenes/日本語のシーン.json");
    const fs::path created = base / "assets" / "scenes" / "new.json";
    WriteFile(target, "{\"新しい\":true}");
    WriteFile(created, "{}");
    EntryInfo e;
    Check(j.Load("000007-scene_write", e), "TS 形式の manifest を読める");
    Check(e.method == "scene_write" && e.state == "committed" && !e.hasTx && e.label == "部屋 \"A\" を配置" && e.files.size() == 2, "フィールド・エスケープ・日本語・null");
    const auto r = j.Restore("000007-scene_write", false);
    Check(r.restored.size() == 2 && ReadFile(target) == "{\"元のシーン\":true}\n" && !fs::exists(created), "TS が書いたエントリを復元できる（上書きは戻り新規は消える）");
    // 次の seq は既存の最大 + 1（TS が書いたフォルダとぶつからない）
    j.BeginCall("save_scene");
    j.Backup(target);
    Check(j.EndCall() == "000008-save_scene", "seq は TS のエントリの次から採番");
    Check(j.List(10).size() == 2 && j.List(10)[1].id == "000007-scene_write", "List に TS のエントリも出る");
    // 壊れた manifest は無視して落ちない
    WriteFile(jr / "000009-broken" / "manifest.json", "{ not json");
    Check(j.List(10).size() == 2, "壊れた manifest は List から外れる（落ちない）");
    EntryInfo bad;
    Check(!j.Load("../etc", bad) && !j.Load("", bad) && !j.Load("000007-scene_write/..", bad), "id の path traversal は拒否");
    // ".." を含む相対 path は戻さない
    WriteFile(jr / "000010-evil" / "manifest.json",
        "{\"version\":1,\"id\":\"000010-evil\",\"method\":\"x\",\"label\":\"\",\"createdAt\":1,\"state\":\"committed\",\"txLabel\":null,\"complete\":true,"
        "\"files\":[{\"path\":\"../escape.txt\",\"existed\":false,\"backup\":null,\"bytes\":0,\"skipped\":null}]}");
    WriteFile(root / "escape.txt", "must-survive");
    const auto re = j.Restore("000010-evil", false);
    Check(fs::exists(root / "escape.txt") && !re.warnings.empty(), "baseDir の外へ出る相対 path は戻さない（警告）");
}

// 保持数
void TestJournalRetention(const fs::path& root)
{
    std::printf("[journal retention]\n");
    using namespace dx12e::mcpjournal;
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path base = root / "proj";
    fs::create_directories(base);
    Journal j;
    j.SetBaseDir(base);
    j.SetKeep(5);
    const fs::path f = base / "f.txt";
    WriteFile(f, "x");
    std::string last;
    for (int i = 0; i < 8; ++i)
    {
        j.BeginCall("save_scene");
        j.Backup(f);
        last = j.EndCall();
    }
    Check(j.EntryCount() == 5, "閉じたエントリは keep 件だけ残る");
    Check(!last.empty() && fs::exists(j.Root() / last), "最新は残る");
    Check(!fs::exists(j.Root() / "000001-save_scene") && fs::exists(j.Root() / "000008-save_scene"), "古い物から刈る");
    // open は刈らない
    j.BeginTx("open-one");
    j.Backup(f);
    // tx を開いたまま、別の Journal（別プロセス相当）が閉じたエントリを積んで刈りを走らせる
    Journal other;
    other.SetBaseDir(base);
    other.SetKeep(2);
    other.BeginCall("save_scene");
    other.Backup(f);
    other.EndCall();
    other.BeginCall("save_scene");
    other.Backup(f);
    other.EndCall();
    Check(fs::exists(j.Root() / "000009-transaction"), "state:open のエントリは刈られない");
    j.CommitTx();
    Check(other.EntryCount() >= 3, "open は keep の数え方から外れて残る");
}
} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / ("mcp_safety_test_" + std::to_string(static_cast<unsigned long long>(std::rand())) );
    TestIdempotency();
    TestGuard();
    TestJournal(root / "j1");
    TestJournalTsFormat(root / "j2");
    TestJournalRetention(root / "j3");
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("OK: %d checks passed\n", g_checks);
    else std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
