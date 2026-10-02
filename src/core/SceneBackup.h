#pragma once
// ============================================================================
// シーンの世代つきバックアップ（<プロジェクト>/.dx12/backups/）
//
// 保存の直前に「ディスク上にある前の版」を 1 世代として残す。書いた新しい版が壊れていた・物体が消えていた、
// というときに前の版へ戻せる。AI セッションの最初の保存の前に取る退避（Application::WriteMcpBackup）と
// 同じ置き場・同じ名前の付け方なので、二重にならない（同じ内容なら 2 世代目は作らない）。
//
// 内容アドレス方式: ファイルの実体は .dx12/backups/objects/<内容ハッシュ>-<大きさ> に 1 回だけ置き、世代は
//   <シーン名>_<YYYYmmdd_HHMMSS>.gen（相対名 → ハッシュの目録）で参照する。変わらなかったファイルは前の世代と同じ実体を指すだけ
//   （コピーもリンクもしない）なので、現行ファイルを外部のエディタ・AI・スクリプトがその場で上書きしても世代は変わらない。
//   目録は最後に原子的に書く＝一覧に出る世代は完全（途中で死んだ世代は目録が無く、次の掃除で実体ごと消える）。
//   世代を消すときは、どの目録からも参照されなくなった実体だけ消す。旧方式の世代（<id>.json + .parts/.inst/.nav のコピー）も一覧・復元できる。
//
// 標準ライブラリ + std::filesystem + core/AtomicFile だけ（tests が単体でビルドする）。
// ============================================================================

#include "core/AtomicFile.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dx12e::scenebackup
{

struct Policy
{
    bool     enabled         = true;
    int      generations     = 10;                       // シーンごとに残す世代数（1 以上）
    uint64_t maxTotalBytes   = 1024ull * 1024 * 1024;    // バックアップ置き場の合計の上限（超えたら古い世代から消す。各シーンの最新 1 世代は残す）
    int      minIntervalSec  = 60;                       // 前の世代からこの秒数が経っていなければ作らない（自動保存で世代が溢れない）
};

// プロセス全体の方針（Application がプロジェクトの設定 settings.json から設定する）
Policy& GlobalPolicy();

struct Generation
{
    std::string           id;          // "<シーン名>_<YYYYmmdd_HHMMSS>"（同秒は _2, _3…）
    std::string           stem;        // シーン名
    std::string           stamp;       // "YYYYmmdd_HHMMSS"
    std::string           order;       // 新旧の比較用（stamp + 同秒の連番 3 桁）。大きいほど新しい
    int64_t               unixTime = 0;
    uint64_t              bytes = 0;   // 世代の合計の大きさ
    bool                  hasParts = false, hasInst = false, hasNav = false;
    std::filesystem::path root;        // 世代を表すファイル（目録 <id>.gen。旧方式の世代は <id>.json）
    bool                  legacy = false;   // 旧方式（ファイルのコピー）の世代
};

std::filesystem::path BackupDir(const std::filesystem::path& projectRoot);   // <プロジェクト>/.dx12/backups

// そのシーンの世代の一覧（新しい順）。
// withBytes=false なら大きさ（bytes）を数えない（ファイルを走査しないので速い。保存のたびに呼ぶ Snapshot が使う）。
std::vector<Generation> List(const std::filesystem::path& projectRoot, const std::filesystem::path& scenePath, bool withBytes = true);

// 世代の中のファイルの中身を読む（rel: "root" / "parts/<名前>" / "inst/<名前>" / "nav"）。テスト・調査用。
bool ReadGenerationFile(const std::filesystem::path& projectRoot, const std::string& id, const std::string& rel, std::string& out);

// scenePath のディスク上の今の版を 1 世代として残す。
//   force=true: 間隔の制限を無視する（AI セッションの最初の保存・復元の直前）。同じ内容の世代がすでに最新なら作らない。
// 戻り値: 作った世代の id。作らなかった（無効・対象外・間隔内・同じ内容・ファイルが無い）ときは空（why に理由）。
// 対象外: scenePath がプロジェクトの外 / "." で始まるフォルダ（.autosave など生成物）の中。
std::string Snapshot(const std::filesystem::path& projectRoot, const std::filesystem::path& scenePath,
                     bool force = false, std::string* why = nullptr);

// ---- 保存のたびに呼ぶ軽い経路（コピーしない）----
// 保存で置き換わる元のファイルは、そのまま objects へ移動（改名。同じボリュームならコピー不要）する。置き換わらなかったファイルは
// 前の世代と同じハッシュを指す（同じ大きさ・更新時刻なら読まない。初めて見るファイルだけ読んで objects に書く）。
// ハッシュ計算と目録の書き込みは別スレッド。
//   Begin  … 世代を作る必要があるか判断し、保存前のディスク上の版のファイル一覧を控える（読まない）。
//   Batch::RetainOld(true) で Commit し、成功したら Finish / FinishAsync に渡す（孤児の掃除より前に）。
struct Pending
{
    bool                               active = false;
    std::filesystem::path              projectRoot, scenePath;
    std::vector<std::filesystem::path> live;   // 保存前のディスク上の版の全ファイル（本体・.parts/*・.inst/*・.nav）
};
Pending Begin(const std::filesystem::path& projectRoot, const std::filesystem::path& scenePath,
              bool force = false, std::string* why = nullptr);
std::string Finish(Pending& pending, atomicfile::Batch& batch);
// Finish を別スレッドで行う（保存の待ち時間に載せない）。すぐ返る。
// 次の Begin / Snapshot / Restore / List は実行中のジョブの完了を待つ（読み込み側から見えるのは完成した世代だけ）。
// 孤児の掃除（削除されるファイルも世代に入れる必要がある）が控えているときは使わず Finish（同期）を使うこと。
void FinishAsync(Pending&& pending, atomicfile::Batch& batch);
// 実行中の世代作成ジョブがあれば終わるまで待つ（プロセス終了時にも自動で待つ）。
void WaitIdle();

// 次の Begin（このシーン）は間隔の制限を無視して必ず世代を作る（AI セッションの最初の保存の前。WriteMcpBackup が立てる）。
void ForceNext(const std::filesystem::path& scenePath);

// 古い世代の掃除（世代数と合計容量）。Snapshot / Finish が呼ぶ。消した世代の数を返す。
int Prune(const std::filesystem::path& projectRoot, const Policy& policy);

// 世代 id の内容でシーンを置き換える（本体・.parts・.inst・.nav をまとめて 1 回のコミットで）。
// 置き換える前に今の版を 1 世代として残す（復元を取り消せる）。
// restoredFromBackup: 今の版を残した世代の id（空なら作らなかった）。
atomicfile::Result Restore(const std::filesystem::path& projectRoot, const std::filesystem::path& scenePath,
                           const std::string& id, std::string* preRestoreId = nullptr);

// 世代 id を丸ごと消す。
void Remove(const std::filesystem::path& projectRoot, const std::string& id);

} // namespace dx12e::scenebackup
