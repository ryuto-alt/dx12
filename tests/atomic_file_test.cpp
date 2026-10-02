// 原子的な書き込み（core/AtomicFile.h）のテスト。
//
// 守りたいこと: 書き込みの途中のどこで失敗・中断しても
//   (1) 元のファイルが無傷（1 バイトも変わらない）
//   (2) 一時ファイル（*.dx12tmp）・退避（*.dx12old）・記録（*.dx12txn）が残らない
//   (3) 複数ファイルのコミットは「全部新しい」か「全部古い」のどちらかで、食い違いが残らない
// 故障注入（SetFaultForTest）でディスク満杯・フラッシュ失敗・検証失敗・置き換え失敗・コミット途中の死を再現する。

#include "core/AtomicFile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <windows.h>

namespace fs = std::filesystem;
using namespace dx12e::atomicfile;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) { std::printf("FAIL: %s %s (%s:%d)\n", #cond, "" __VA_ARGS__, __FILE__, __LINE__); ++g_failed; } \
    } while (0)

static std::string Slurp(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void Put(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}
static int LeftoverCount(const fs::path& dir)
{
    int n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir))
    {
        const std::string ext = e.path().extension().string();
        if (ext == ".dx12tmp" || ext == ".dx12old" || ext == ".dx12txn") ++n;
    }
    return n;
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "dx12_atomic_file_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const std::string oldText(300000, 'o');
    const std::string newText(500000, 'n');

    // ---- 1. 基本: 新規 / 上書き ----
    {
        const fs::path p = root / "a" / "basic.json";
        CHECK(WriteFile(p, "hello").ok, "新規に書ける（フォルダも作る）");
        CHECK(Slurp(p) == "hello", "新規の中身");
        CHECK(WriteFile(p, newText).ok, "上書きできる");
        CHECK(Slurp(p) == newText, "上書きの中身");
        CHECK(WriteFile(p, "").ok && Slurp(p).empty(), "空ファイルも書ける");
        CHECK(LeftoverCount(root) == 0, "基本: 一時ファイルが残らない");
    }

    // ---- 2. 各段階の失敗（元が無傷・一時ファイルなし） ----
    {
        const fs::path p = root / "fail.bin";
        Put(p, oldText);
        // ディスク満杯: 書き込みの先頭・途中・最後の 1 バイト手前
        for (int64_t at : {int64_t(0), int64_t(1), int64_t(newText.size() / 2), int64_t(newText.size() - 1)})
        {
            SetFaultForTest(Fault::DiskFullAfterBytes, at);
            const Result r = WriteFile(p, newText);
            SetFaultForTest(Fault::None);
            CHECK(!r.ok && !r.error.empty(), "ディスク満杯はエラーを返す");
            CHECK(Slurp(p) == oldText, "ディスク満杯: 元のファイルが無傷");
            CHECK(LeftoverCount(root) == 0, "ディスク満杯: 一時ファイルが残らない");
        }
        // 2MB 超（チャンクをまたぐ）の途中でも
        {
            const std::string big(3 * 1048576 + 17, 'b');
            SetFaultForTest(Fault::DiskFullAfterBytes, 2 * 1048576 + 5);
            const Result r = WriteFile(p, big);
            SetFaultForTest(Fault::None);
            CHECK(!r.ok, "大きいファイルの途中のディスク満杯");
            CHECK(Slurp(p) == oldText && LeftoverCount(root) == 0, "大きいファイル: 元が無傷・一時なし");
        }
        SetFaultForTest(Fault::FlushFails);
        CHECK(!WriteFile(p, newText).ok, "フラッシュ失敗はエラー");
        SetFaultForTest(Fault::None);
        CHECK(Slurp(p) == oldText && LeftoverCount(root) == 0, "フラッシュ失敗: 元が無傷・一時なし");

        SetFaultForTest(Fault::VerifyFails);
        CHECK(!WriteFile(p, newText).ok, "検証失敗はエラー");
        SetFaultForTest(Fault::None);
        CHECK(Slurp(p) == oldText && LeftoverCount(root) == 0, "検証失敗: 元が無傷・一時なし");

        SetFaultForTest(Fault::ReplaceFails, 0);
        CHECK(!WriteFile(p, newText).ok, "置き換え失敗はエラー");
        SetFaultForTest(Fault::None);
        CHECK(Slurp(p) == oldText && LeftoverCount(root) == 0, "置き換え失敗: 元が無傷・一時なし");

        // 検証関数（JSON 再パースの代わり）
        auto reject = [](std::string_view, std::string& err) { err = "不正"; return false; };
        const Result vr = WriteFile(p, newText, reject);
        CHECK(!vr.ok && vr.error.find("不正") != std::string::npos, "検証関数の失敗は理由つきで返る");
        CHECK(Slurp(p) == oldText && LeftoverCount(root) == 0, "検証関数の失敗: 元が無傷・一時なし");

        // 失敗の直後でも次の保存は成功する
        CHECK(WriteFile(p, newText).ok && Slurp(p) == newText, "失敗の後で保存し直せる");
    }

    // ---- 3. 読み取り専用 ----
    {
        const fs::path p = root / "readonly.json";
        Put(p, oldText);
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_READONLY);
        const Result r = WriteFile(p, newText);
        CHECK(!r.ok && !r.error.empty(), "読み取り専用はエラー（上書きしない）");
        CHECK(Slurp(p) == oldText, "読み取り専用: 元が無傷");
        CHECK(LeftoverCount(root) == 0, "読み取り専用: 一時ファイルが残らない");
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        CHECK(WriteFile(p, newText).ok && Slurp(p) == newText, "読み取り専用を外せば書ける");
    }

    // ---- 4. 別のプログラムが開いている（共有違反） ----
    {
        const fs::path p = root / "locked.json";
        Put(p, oldText);
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, 0 /*共有なし*/, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE, "ロック用ハンドル");
        const Result r = WriteFile(p, newText);
        CHECK(!r.ok, "他で開かれていれば置き換えない");
        CloseHandle(h);
        CHECK(Slurp(p) == oldText && LeftoverCount(root) == 0, "ロック: 元が無傷・一時なし");
    }

    // ---- 5. 複数ファイル Batch ----
    const fs::path d = root / "batch";
    const fs::path txn = d / "scene.json.dx12txn";
    const char* names[] = {"inst.jsonl", "cell_0.json", "cell_1.json", "scene.json"};   // 最後がルート
    auto reset = [&]() {
        fs::remove_all(d, ec);
        for (const char* n : names) Put(d / n, std::string("OLD-") + n);
    };
    auto allIs = [&](const char* prefix) {
        for (const char* n : names) if (Slurp(d / n) != std::string(prefix) + n) return false;
        return true;
    };
    auto stageAll = [&](Batch& b) {
        for (const char* n : names) if (!b.Add(d / n, std::string("NEW-") + n)) return false;
        return true;
    };
    {
        reset();
        Batch b(txn);
        CHECK(stageAll(b), "Batch: 全部ステージできる");
        CHECK(allIs("OLD-"), "Batch: Commit 前は元のまま（ステージは既存に触らない）");
        CHECK(b.Commit().ok, "Batch: Commit 成功");
        CHECK(allIs("NEW-"), "Batch: 全部新しい");
        CHECK(LeftoverCount(d) == 0, "Batch: 成功後に何も残らない");
    }
    {   // ステージ中の失敗（途中のファイルがディスク満杯）: 何も置き換えない
        reset();
        {
            Batch b(txn);
            CHECK(b.Add(d / names[0], std::string(2000, 'x')), "Batch: 1 つ目は書ける");
            SetFaultForTest(Fault::DiskFullAfterBytes, 10);
            CHECK(!b.Add(d / names[1], std::string(2000, 'y')), "Batch: 途中のファイルの失敗");
            SetFaultForTest(Fault::None);
            CHECK(!b.Add(d / names[2], "z"), "Batch: 失敗後の Add は何もしない");
            CHECK(!b.Commit().ok, "Batch: 失敗していれば Commit も失敗");
        }
        CHECK(allIs("OLD-"), "Batch: ステージ失敗で全部が古いまま");
        CHECK(LeftoverCount(d) == 0, "Batch: ステージ失敗で一時が残らない");
    }
    for (int k = 0; k < 4; ++k)
    {   // 置き換えの k 番目が失敗: 先に置き換えた分も元へ戻る
        reset();
        {
            Batch b(txn);
            CHECK(stageAll(b), "Batch: ステージ");
            SetFaultForTest(Fault::ReplaceFails, k);
            const Result r = b.Commit();
            SetFaultForTest(Fault::None);
            CHECK(!r.ok, "Batch: 置き換え失敗はエラー");
        }
        CHECK(allIs("OLD-"), "Batch: 置き換え失敗で全部が古いまま（食い違いなし）");
        CHECK(LeftoverCount(d) == 0, "Batch: 置き換え失敗で一時・退避・記録が残らない");
    }
    {   // 読み取り専用のセルがあって実際に置き換えが失敗: 食い違いなし
        reset();
        SetFileAttributesW((d / names[2]).c_str(), FILE_ATTRIBUTE_READONLY);
        {
            Batch b(txn);
            CHECK(stageAll(b), "Batch(読み取り専用): ステージ");
            CHECK(!b.Commit().ok, "Batch(読み取り専用): Commit はエラー");
        }
        SetFileAttributesW((d / names[2]).c_str(), FILE_ATTRIBUTE_NORMAL);
        CHECK(allIs("OLD-"), "Batch(読み取り専用): 全部が古いまま");
        CHECK(LeftoverCount(d) == 0, "Batch(読み取り専用): 何も残らない");
    }
    {   // 記録を書いた直後に死んだ: 次回の RecoverPending が最後まで置き換える
        reset();
        {
            Batch b(txn);
            CHECK(stageAll(b), "Batch: ステージ");
            SetFaultForTest(Fault::CrashAfterJournal);
            CHECK(b.Commit().ok, "Batch: （死んだ想定）");
            SetFaultForTest(Fault::None);
        }
        CHECK(fs::exists(txn), "死んだ後: 記録が残っている");
        CHECK(RecoverPending(txn), "RecoverPending が復旧する");
        CHECK(allIs("NEW-"), "復旧: 全部新しい");
        CHECK(LeftoverCount(d) == 0, "復旧後に何も残らない");
        CHECK(!RecoverPending(txn), "2 回目は何もしない");
    }
    for (int n = 1; n <= 3; ++n)
    {   // n 個置き換えた直後に死んだ
        reset();
        {
            Batch b(txn);
            CHECK(stageAll(b), "Batch: ステージ");
            SetFaultForTest(Fault::CrashAfterReplaces, n);
            CHECK(b.Commit().ok, "Batch: （死んだ想定）");
            SetFaultForTest(Fault::None);
        }
        RecoverPending(txn);
        CHECK(allIs("NEW-"), "途中で死んでも復旧で全部新しい");
        CHECK(LeftoverCount(d) == 0, "途中で死んだ復旧後に何も残らない");
    }
    {   // 新規ファイルだけの Batch（元が無い）の置き換え失敗 → 作ったものも消える
        fs::remove_all(d, ec);
        fs::create_directories(d);
        {
            Batch b(txn);
            CHECK(b.Add(d / "n0.json", "a") && b.Add(d / "n1.json", "b"), "Batch(新規): ステージ");
            SetFaultForTest(Fault::ReplaceFails, 1);
            CHECK(!b.Commit().ok, "Batch(新規): 失敗");
            SetFaultForTest(Fault::None);
        }
        CHECK(!fs::exists(d / "n0.json") && !fs::exists(d / "n1.json"), "Batch(新規): 失敗なら新規ファイルも残さない");
        CHECK(LeftoverCount(d) == 0, "Batch(新規): 何も残らない");
    }

    {   // 記録を書いている途中で止まった（末尾の END が無い）: 何も置き換えず、記録と書きかけの一時ファイルを捨てる
        reset();
        const fs::path t0 = d / "cell_0.json.1.0.dx12tmp";
        Put(t0, "HALF-WRITTEN");
        Put(txn, "DX12TXN1\n" + (d / "cell_0.json").string() + "\t" + t0.string() + "\t" + (d / "cell_0.json.dx12old").string() + "\t1\n");   // END なし
        CHECK(!RecoverPending(txn));
        CHECK(allIs("OLD-"));
        CHECK(!fs::exists(txn) && !fs::exists(t0));
    }
    {   // 1 ファイルだけの Batch は記録なしで原子的に置き換わる（置き換え失敗なら元のまま）
        reset();
        {
            Batch b(txn);
            CHECK(b.Add(d / names[1], "ONLY-NEW"));
            SetFaultForTest(Fault::ReplaceFails, 0);
            CHECK(!b.Commit().ok);
            SetFaultForTest(Fault::None);
        }
        CHECK(Slurp(d / names[1]) == std::string("OLD-") + names[1] && LeftoverCount(d) == 0);
        Batch b2(txn);
        CHECK(b2.Add(d / names[1], "ONLY-NEW") && b2.Commit().ok);
        CHECK(Slurp(d / names[1]) == "ONLY-NEW" && !fs::exists(txn) && LeftoverCount(d) == 0);
    }
    {   // RetainOld: コミット後も元の版を取っておき、ReleaseRetained で消す
        reset();
        Batch b(txn);
        b.RetainOld(true);
        CHECK(stageAll(b) && b.Commit().ok);
        CHECK(b.RetainedOld().size() == 4);
        for (const auto& r : b.RetainedOld()) CHECK(Slurp(r.old) == "OLD-" + r.dst.filename().string());
        b.ReleaseRetained();
        CHECK(LeftoverCount(d) == 0 && allIs("NEW-"));
    }

    {   // コピー・移動（取り込み・複製・移動）
        const fs::path cd = root / "copy";
        fs::remove_all(cd, ec);
        Put(cd / "src" / "a.bin", std::string(200000, 'A'));
        Put(cd / "src" / "sub" / "b.bin", "BBB");
        Put(cd / "one.bin", "ONE-OLD");
        CHECK(CopyFileAtomic(cd / "src" / "a.bin", cd / "dst" / "a.bin", false).ok);
        CHECK(Slurp(cd / "dst" / "a.bin") == std::string(200000, 'A'));
        CHECK(!CopyFileAtomic(cd / "src" / "a.bin", cd / "dst" / "a.bin", false).ok);   // 上書きしない指定
        // 置き換え失敗: 元のファイルが無傷・一時ファイルなし
        SetFaultForTest(Fault::ReplaceFails, 0);
        CHECK(!CopyFileAtomic(cd / "src" / "a.bin", cd / "one.bin", true).ok);
        SetFaultForTest(Fault::None);
        CHECK(Slurp(cd / "one.bin") == "ONE-OLD" && LeftoverCount(cd) == 0);
        CHECK(CopyFileAtomic(cd / "src" / "a.bin", cd / "one.bin", true).ok && Slurp(cd / "one.bin").size() == 200000);
        // フォルダ: 途中の失敗では何も現れない
        SetFaultForTest(Fault::ReplaceFails, 0);
        CHECK(!CopyTree(cd / "src", cd / "tree", false).ok);
        SetFaultForTest(Fault::None);
        CHECK(!fs::exists(cd / "tree") && LeftoverCount(cd) == 0);
        {
            bool tmpDir = false;
            for (const auto& e : fs::directory_iterator(cd)) if (e.path().extension() == ".dx12tmp") tmpDir = true;
            CHECK(!tmpDir, "失敗したフォルダのコピーの一時フォルダが残らない");
        }
        CHECK(CopyTree(cd / "src", cd / "tree", false).ok && Slurp(cd / "tree" / "sub" / "b.bin") == "BBB");
        // 移動: 同名があればエラーで元のまま。成功すれば元は無い
        CHECK(!MovePath(cd / "src", cd / "tree").ok && fs::exists(cd / "src" / "a.bin"));
        CHECK(MovePath(cd / "src", cd / "moved").ok && !fs::exists(cd / "src") && Slurp(cd / "moved" / "sub" / "b.bin") == "BBB");
    }

    // ---- 6. 古い一時ファイルの掃除 ----
    {
        const fs::path sd = root / "sweep";
        fs::create_directories(sd);
        Put(sd / "scene.json.99999999.0.dx12tmp", "dead");   // 存在しない pid（古い残骸）
        Put(sd / ("scene.json." + std::to_string(GetCurrentProcessId()) + ".7.dx12tmp"), "mine");   // 自分の途中
        Put(sd / "scene.json", "keep");
        CHECK(SweepStaleTmp(sd) == 1, "掃除: 死んだ pid の一時だけ消す");
        CHECK(fs::exists(sd / "scene.json") && fs::exists(sd / ("scene.json." + std::to_string(GetCurrentProcessId()) + ".7.dx12tmp")),
              "掃除: 本体と自分の一時には触らない");
    }

    fs::remove_all(root, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
