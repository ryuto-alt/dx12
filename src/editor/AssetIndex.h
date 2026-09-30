#pragma once

// ===== アセット一覧の非同期インデックス =====
// 以前のアセットブラウザは UI スレッドで 0.5 秒ごとに directory_iterator（検索中は最大 20000 件の再帰）を回していた。
// 数千ファイルのフォルダではそれだけで毎フレームの一部を食い、検索中は 0.5 秒ごとに固まった。
//
// 今は:
//   ・走査は専用ワーカースレッド。UI スレッドは Request()（要求を置く）と Poll()（結果を受け取る）だけ。
//   ・変更検知は ReadDirectoryChangesW（別スレッド）。ファイルが増減・更新された時だけ再走査する（ポーリングしない）。
//     ".thumbcache" 等の "." 始まりの場所と *.tmp の変更は無視する（サムネイルの書き込みで再走査が回り続けない）。
//   ・走査結果が前回と見た目に同じなら UI へ渡さない（選択・スクロール・サムネの再要求を起こさない）。
//   ・フォルダツリー用のサブフォルダ一覧もワーカーが作り、キャッシュする（毎フレームの directory_iterator を廃止）。
// スレッド境界: 渡すのは値のコピーだけ（entries は move）。GPU / ImGui には一切触れない。

#include "editor/AssetBrowserLogic.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx12e
{

class AssetIndex
{
public:
    AssetIndex() = default;
    ~AssetIndex() { Stop(); }
    AssetIndex(const AssetIndex&) = delete;
    AssetIndex& operator=(const AssetIndex&) = delete;

    void Start();   // ワーカーを起こす（多重呼び出し安全。最初の Request で自動的にも起動する）
    void Stop();    // スレッドを止めて join

    // 変更を監視するルート（assets / scripts）。差し替えで監視を張り直す。
    void SetWatchRoots(std::vector<std::filesystem::path> roots);

    // 一覧の要求。最新の 1 件だけが有効（古い要求は捨てる）。走査自体は非同期。
    void Request(const abl::ScanOptions& opt);
    // 変更通知を待たずに再走査させる（ファイル操作の直後など）。
    void ForceRescan();

    // 新しい結果があれば true（out へ move）。Request した内容に対応しない古い結果は返さない。
    bool Poll(abl::ScanResult& out);

    // フォルダツリー用: dir のサブフォルダ。キャッシュがあれば true で out を埋める（古い場合は裏で更新を要求）。
    // 無ければ要求を積んで false（次のフレーム以降に取れる）。
    bool SubDirs(const std::filesystem::path& dir, std::vector<abl::DirInfo>& out);

    uint64_t ChangeSerial() const { return m_changeSerial.load(); }   // 変更を検知するたび増える
    bool     IsScanning() const { return m_scanning.load(); }
    uint64_t ScanCount() const { return m_scanCount.load(); }         // 完了した走査の回数（テスト / 計測用）

private:
    void WorkerLoop();
    void WatchLoop(std::vector<std::filesystem::path> roots, void* stopEvent);
    void StartWatcher();
    void StopWatcher();

    // ---- ワーカー ----
    std::thread              m_worker;
    std::mutex               m_mtx;
    std::condition_variable  m_cv;
    bool                     m_stop = false;
    bool                     m_started = false;

    abl::ScanOptions         m_req;
    bool                     m_hasReq = false;          // 新しい要求が未処理
    uint64_t                 m_reqGen = 0;              // Request のたび増える
    bool                     m_forceRescan = false;
    uint64_t                 m_scannedSerial = 0;       // 走査時点の変更カウンタ
    int64_t                  m_lastChangeTickMs = 0;    // 最後に変更通知を受けた時刻（デバウンス用）

    abl::ScanResult          m_result;
    uint64_t                 m_resultGen = 0;
    bool                     m_resultReady = false;
    std::vector<abl::Entry>  m_lastEntries;             // 直近に「公開した」結果（差分判定用。ワーカー専有）
    uint64_t                 m_lastEntriesGen = 0;
    std::filesystem::path    m_lastDir;
    std::string              m_lastQuery;

    struct DirCache { std::vector<abl::DirInfo> dirs; uint64_t serial = 0; };
    std::unordered_map<std::string, DirCache> m_dirCache;
    std::unordered_set<std::string>           m_dirJobs;   // 要求済み（未処理）

    std::atomic<uint64_t> m_changeSerial{1};
    std::atomic<bool>     m_scanning{false};
    std::atomic<uint64_t> m_scanCount{0};

    // ---- 変更監視 ----
    std::thread                        m_watcher;
    void*                              m_watchStop = nullptr;   // HANDLE（Windows.h を出さないため void*）
    std::vector<std::filesystem::path> m_roots;
};

} // namespace dx12e
