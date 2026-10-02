#pragma once
// ============================================================================
// 原子的なファイル書き込み（ユーザーのデータを書く全経路の共通部品）
//
// なぜ要るか: 元のファイルへ直接上書きすると、書き込み中のクラッシュ・電源断・ディスク満杯で
//   シーンが半端な状態（空・途中で切れた JSON）で残り、作品が壊れる。
//
// 単一ファイル WriteFile:
//   同じフォルダの一時ファイルへ書く → FlushFileBuffers → 読み戻して検証（サイズ・内容・任意の検証関数。
//   JSON なら再パース）→ ReplaceFileW（無ければ MoveFileExW）で置き換える。
//   どの段階で失敗しても元のファイルは無傷で、一時ファイルは消す。
//
// 複数ファイル Batch（分割シーン・インスタンス群のサイドカー・.nav をまとめて保存する）:
//   1) 全部を一時ファイルへ書き終える（ここで 1 つでも失敗したら何も置き換えない）
//   2) コミット記録（<記録ファイル>）を原子的に書く ← これ以降は「やり遂げる」側
//   3) 順番に置き換える（ルートを最後に）。元の版は .dx12old に退避しておき、置き換えの途中で
//      失敗したら元へ戻す（食い違いを残さない）
//   4) 退避と記録を消す
//   2〜3 の最中にプロセスが死んでも、次回 RecoverPending が記録を見て最後まで置き換える（ロールフォワード）。
//
// 標準ライブラリ + Win32 だけ（Logger にも nlohmann にも依存しない。tests が AtomicFile.cpp と一緒にビルドする）。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dx12e::atomicfile
{

struct Result
{
    bool        ok = false;
    std::string error;   // 標準語の説明（ok のときは空）
    explicit operator bool() const { return ok; }
};

// 書いた中身の検証（false で失敗。err に理由）。JSON なら再パースする関数を渡す。
using Verifier = std::function<bool(std::string_view bytes, std::string& err)>;

// 1 本を原子的に書く。verify が空ならサイズと内容の一致だけを確認する。
Result WriteFile(const std::filesystem::path& dst, std::string_view bytes, const Verifier& verify = {});

// 変更を 1 回のコミットにまとめる。Add は一時ファイルへ書くだけ（既存ファイルには触らない）。
class Batch
{
public:
    // txnFile: コミット記録の置き場（通常はシーン本体のパス + ".dx12txn"）。
    explicit Batch(std::filesystem::path txnFile);
    ~Batch();   // コミットしなかった分の一時ファイルを消す
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    // dst の新しい中身を一時ファイルへ書く。失敗したら以降の Add / Commit は何もせず失敗を返す。
    // 追加した順に置き換える（最後に置き換えたいルートは最後に Add）。
    bool Add(const std::filesystem::path& dst, std::string_view bytes, const Verifier& verify = {});

    // 多数のファイルを並列に一時ファイルへ書く（分割シーンの数百のセル。1 つずつだと flush・検証・ウイルス対策の待ちが積み重なる）。
    // skipIdentical なら、いまの dst と中身が同じものは書かない（written[i] = 0）。置き換えの順は entries の順。
    // 失敗したら Add と同じく以降は何もせず false。
    struct Entry { std::filesystem::path dst; std::string_view bytes; Verifier verify; };
    bool AddMany(const std::vector<Entry>& entries, unsigned workers, bool skipIdentical, std::vector<char>* written = nullptr);

    bool Failed() const { return !m_error.empty(); }
    const std::string& Error() const { return m_error; }
    size_t Count() const { return m_items.size(); }

    // 置き換えを実行する。失敗したら元に戻して false（error に理由）。Add が失敗していたら何もせず false。
    Result Commit();

    // 置き換えた元のファイル（.dx12old）を、コミットの後も消さずに取っておく（世代つきバックアップが「コピーせず移動」するため）。
    // Commit の前に呼ぶ。取っておいた分は RetainedOld() で分かり、使い終えたら（移動したら）残りを ReleaseRetained() で消す。
    // コミット後にプロセスが死ぬと残るが、次回の保存が SweepStaleOld で消す。
    struct Retained { std::filesystem::path dst, old; };
    void RetainOld(bool keep) { m_retainOld = keep; }
    const std::vector<Retained>& RetainedOld() const { return m_retained; }
    void ReleaseRetained();
    // 取っておいた元のファイルの所有を呼び出し側へ渡す（このバッチはもう消さない。別スレッドで世代へ移すため）。
    std::vector<Retained> TakeRetained() { std::vector<Retained> r; r.swap(m_retained); return r; }

private:
    struct Item { std::filesystem::path dst, tmp, old; bool hadOld = false; bool replaced = false; };
    std::filesystem::path m_txn;
    std::vector<Item>     m_items;
    std::string           m_error;
    bool                  m_committed = false;
    bool                  m_retainOld = false;
    std::vector<Retained> m_retained;
    void DiscardTmp();
};

// ---- コピー・移動（取り込み・複製・移動。途中で失敗したら元のまま・コピー先に半端なファイルを残さない）----
// CopyFile: 一時ファイルへコピー → flush → 大きさ確認 → 置き換え。overwrite=false で既存があればエラー。
Result CopyFileAtomic(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite);
// CopyTree: ファイルならCopyFile。フォルダは、新規なら一時フォルダへ全部コピーしてから改名（途中で失敗したら何も現れない）、
// 既存へ重ねる（overwrite）ならファイルごとに原子的。
Result CopyTree(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite);
// MovePath: 改名（同じボリュームは原子的）。別のボリュームはコピーを完成させてから元を消す。移動先が既にあればエラー（上書きしない）。
Result MovePath(const std::filesystem::path& src, const std::filesystem::path& dst);

// 前回の保存が置き換えの途中で止まっていたら最後まで置き換える（シーンを開く前・保存する前に呼ぶ）。
// 戻り値: 何かを復旧したら true。
bool RecoverPending(const std::filesystem::path& txnFile);

// dir 直下の拡張子 .dx12tmp（書き込み途中で残った一時ファイル）を消す。消した数を返す。
int SweepStaleTmp(const std::filesystem::path& dir);
// dir 直下の拡張子 .dx12old（コミット後に取っておいた元の版の残り）を消す。RecoverPending の後（進行中のコミットが無いとき）だけ呼ぶ。
int SweepStaleOld(const std::filesystem::path& dir);

// 書き込みの時間の内訳（保存の遅さの調査用。呼ぶたびに前回からの合計を返して 0 に戻す。メインスレッドの保存だけを数える）。
struct Stats
{
    double  writeMs = 0, flushMs = 0, readbackMs = 0, verifyMs = 0, replaceMs = 0;
    int64_t bytes = 0;
    int     files = 0;
};
Stats TakeStats();

// UTF-8 文字列 → path / path → UTF-8（コミット記録用）
std::filesystem::path PathFromUtf8(const std::string& s);
std::string           Utf8FromPath(const std::filesystem::path& p);

// ---- テスト用の故障注入（本番では常に None） ----
enum class Fault
{
    None,
    DiskFullAfterBytes,   // 一時ファイルへ n バイト書いたところでディスク満杯相当の失敗
    FlushFails,           // FlushFileBuffers の失敗
    VerifyFails,          // 検証の失敗
    ReplaceFails,         // n 番目（0 始まり）の置き換えの失敗（単一ファイルは n=0）
    CrashAfterJournal,    // コミット記録を書いた直後にプロセスが死んだ想定（Commit は置き換えずに成功扱いで返る）
    CrashAfterReplaces,   // n 個置き換えた直後にプロセスが死んだ想定（以降は何もせず返る）
};
void SetFaultForTest(Fault f, int64_t n = 0);

} // namespace dx12e::atomicfile
