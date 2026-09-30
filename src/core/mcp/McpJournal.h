#pragma once
// ===========================================================================
// MCP ファイル書き込みジャーナル（M5。docs/MCP.md §13）
// ---------------------------------------------------------------------------
// ★標準ライブラリ + std::filesystem だけ（nlohmann にもエンジンにも依存しない）。
//   tests/mcp_safety_test.cpp が McpJournal.cpp と一緒にビルドして単体で検査する。
//
// 何のためにあるか
//   MCP がファイルを書く操作（save_scene / create_lua_component / create_shader / move_asset / delete_asset /
//   import_asset / create_prefab）は Undo の対象外で、トランザクションを rollback してもファイルは戻らなかった。
//   書く直前に「上書きされる（または消される）ファイルの元の内容」を <project>/.dx12/journal/ へ退避し、
//   rollback / journal_restore で書き戻せるようにする。
//
// エントリ = フォルダ <root>/<seq 6 桁>-<method>/
//     manifest.json  {"version":1,"id":"000012-save_scene","method":"save_scene","label":"…","createdAt":<ms>,
//                     "state":"open|committed|rolledBack|restored","txLabel":"…"|null,"complete":true|false,
//                     "files":[{"path":"assets/scenes/a.json","existed":true,"backup":"files/0.bin","bytes":1234,"skipped":null}]}
//     files/<n>.bin  上書き・削除される直前の内容
//   TS 側（dx12_scene_write）も同じ形で書く。path は project baseDir 相対（外なら絶対）。
//
// 適用単位
//   ・トランザクション中は 1 つの tx エントリに全部まとめる（BeginTx → CommitTx / RestoreTx）。
//   ・それ以外は journal 対応 method の 1 呼び出し = 1 エントリ（BeginCall → EndCall。ディスパッチャが行う）。
//   ・ハンドラは書く直前に Backup(path) を呼ぶだけ。スコープが無ければ何もしない。
// ===========================================================================

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dx12e
{
namespace mcpjournal
{

constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024;         // これを超えるファイルは退避せず complete:false
constexpr size_t   kKeepEntries  = 50;                          // 閉じたエントリの保持数
constexpr size_t   kMaxTreeFiles = 2000;                        // BackupTree の 1 エントリあたり上限
constexpr uint64_t kMaxTreeBytes = 256ull * 1024 * 1024;

struct FileRec
{
    std::string path;          // baseDir 相対（外なら絶対）。区切りは '/'
    bool        existed = false;
    std::string backup;        // エントリフォルダからの相対（"files/0.bin"）。existed:false / skipped のときは空
    uint64_t    bytes = 0;
    std::string skipped;       // "" | "too_large"
};

struct EntryInfo
{
    std::string id;
    std::string method;
    std::string label;
    std::string state;         // open | committed | rolledBack | restored
    std::string txLabel;
    bool        hasTx = false;
    int64_t     createdAt = 0;
    bool        complete = true;
    std::string note;
    std::vector<FileRec> files;
};

struct RestoreResult
{
    bool                     found = false;
    bool                     complete = true;     // false = 元のエントリが不完全（退避できなかったファイルがあった）
    std::vector<std::string> restored;            // 書き戻した / 消したパス
    std::vector<std::string> unchanged;           // 既に元の内容だった
    std::vector<std::string> missing;             // バックアップが無く戻せなかった
    std::vector<std::string> warnings;
};

// UTF-8 <-> std::filesystem::path（C++20 で u8path が非推奨なので自前）。
std::filesystem::path PathFromUtf8(const std::string& s);
std::string           Utf8FromPath(const std::filesystem::path& p);   // 区切りは '/'

class Journal
{
public:
    using Clock = std::function<int64_t()>;   // epoch ms（テストで差し替える）

    Journal();

    // baseDir=プロジェクトのルート。journal 置き場は <baseDir>/.dx12/journal。
    void SetBaseDir(const std::filesystem::path& baseDir);
    // テスト用: 置き場を明示する。
    void Configure(const std::filesystem::path& baseDir, const std::filesystem::path& journalRoot);
    void SetClock(Clock c) { m_clock = std::move(c); }
    void SetKeep(size_t keep) { m_keep = keep; }
    const std::filesystem::path& Root() const { return m_root; }
    const std::filesystem::path& BaseDir() const { return m_base; }
    bool Configured() const { return !m_root.empty(); }

    // ---- 1 呼び出し = 1 エントリ ----
    // true を返したら呼び出し側が EndCall() する責任を持つ。トランザクション中は tx エントリへ入るので false。
    bool BeginCall(const std::string& method, const std::string& label = {});
    // 閉じる。1 つもファイルを退避しなかったら何も残さない。成功で退避したエントリ id を返す（無ければ空）。
    std::string EndCall();

    // ---- トランザクション ----
    void        BeginTx(const std::string& label);
    bool        TxActive() const { return m_tx != nullptr; }
    std::string TxLabel() const;
    std::string CommitTx();                          // 確定（state:committed で保持）
    std::string RollbackTx(RestoreResult* out);      // 元へ戻して state:rolledBack

    // ---- 退避（ハンドラが書く直前に呼ぶ）----
    // 現在のスコープ（tx か 1 呼び出し）へ、path の今の内容を 1 度だけ退避する。存在しなければ「無かった」を記録する。
    // スコープが無ければ何もしない。戻り値: 記録した（または既に記録済み）なら true。
    bool Backup(const std::filesystem::path& absPath);
    // ディレクトリなら配下の全ファイルを個別に退避（上限超過は complete:false）。ファイル / 不在なら Backup と同じ。
    bool BackupTree(const std::filesystem::path& absPath);
    // 退避しきれないことを記録する（例: move_asset が参照書き換えで触った他のファイル）。
    void MarkIncomplete(const std::string& reason);
    bool HasScope() const { return m_tx != nullptr || m_cur != nullptr; }

    // ---- 一覧・復元 ----
    std::vector<EntryInfo> List(size_t limit = 20) const;   // 新しい順
    bool                   Load(const std::string& id, EntryInfo& out) const;
    // backupFirst=true なら書き換える前に現在のスコープへ Backup する（journal_restore を journal 対応にするため）。
    RestoreResult          Restore(const std::string& id, bool backupFirst);
    size_t                 EntryCount() const;

private:
    struct Entry;
    struct Impl;
    bool        Materialize(Entry& e);
    void        WriteManifest(const Entry& e) const;
    bool        BackupInto(Entry& e, const std::filesystem::path& absPath);
    std::string Finish(std::unique_ptr<Entry>& slot, const char* state);
    RestoreResult RestoreEntry(const EntryInfo& info, bool backupFirst);
    void        Prune();
    std::string RelPath(const std::filesystem::path& absPath) const;
    std::filesystem::path Resolve(const std::string& p) const;
    int64_t     Now() const;

    std::filesystem::path   m_base;
    std::filesystem::path   m_root;
    Clock                   m_clock;
    size_t                  m_keep = kKeepEntries;
    uint32_t                m_lastSeq = 0;
    std::unique_ptr<Entry>  m_cur;
    std::unique_ptr<Entry>  m_tx;

public:
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
};

// プロセス内シングルトン（エンジンのディスパッチャとハンドラが共有する）。
Journal& Instance();

} // namespace mcpjournal
} // namespace dx12e
