#include "editor/AssetIndex.h"

#include <array>
#include <chrono>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace dx12e
{

namespace
{
int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 変更通知の相対パスを見て「無視してよい変更か」を判定する。
// "." 始まりの成分（.thumbcache / .texcache / .autosave）や *.tmp はエディタ自身が書く物。ここで再走査を起こすと
// サムネイルを 1 枚書くたびに一覧を引き直す無限ループになる。
bool IgnorableChange(const wchar_t* name, size_t len)
{
    size_t compStart = 0;
    for (size_t i = 0; i <= len; ++i)
    {
        if (i == len || name[i] == L'\\' || name[i] == L'/')
        {
            if (i > compStart && name[compStart] == L'.') return true;
            compStart = i + 1;
        }
    }
    if (len >= 4)
    {
        const wchar_t* e = name + len - 4;
        if ((e[0] == L'.') && (e[1] == L't' || e[1] == L'T') && (e[2] == L'm' || e[2] == L'M') && (e[3] == L'p' || e[3] == L'P'))
            return true;
    }
    return false;
}
} // namespace

void AssetIndex::Start()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_started) return;
    m_started = true;
    m_stop = false;
    m_worker = std::thread([this] { WorkerLoop(); });
}

void AssetIndex::Stop()
{
    StopWatcher();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_started) return;
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_started = false;
}

void AssetIndex::SetWatchRoots(std::vector<std::filesystem::path> roots)
{
    StopWatcher();
    m_roots = std::move(roots);
    StartWatcher();
}

void AssetIndex::Request(const abl::ScanOptions& opt)
{
    Start();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_req = opt;
        m_hasReq = true;
        ++m_reqGen;
    }
    m_cv.notify_all();
}

void AssetIndex::ForceRescan()
{
    Start();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_forceRescan = true;
        m_changeSerial.fetch_add(1);   // ツリーのキャッシュも古くする
    }
    m_cv.notify_all();
}

bool AssetIndex::Poll(abl::ScanResult& out)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_resultReady) return false;
    m_resultReady = false;
    if (m_resultGen != m_reqGen) return false;   // 要求後にさらに新しい要求が来た（古い結果は捨てる）
    out = std::move(m_result);
    m_result = abl::ScanResult{};
    return true;
}

bool AssetIndex::SubDirs(const std::filesystem::path& dir, std::vector<abl::DirInfo>& out)
{
    Start();
    const std::string key = dir.string();
    bool have = false;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_dirCache.find(key);
        if (it != m_dirCache.end())
        {
            out = it->second.dirs;
            have = true;
            if (it->second.serial != m_changeSerial.load() && m_dirJobs.insert(key).second) wake = true;   // 古い → 裏で更新
        }
        else if (m_dirJobs.insert(key).second) wake = true;
    }
    if (wake) m_cv.notify_all();
    return have;
}

void AssetIndex::WorkerLoop()
{
    std::unique_lock<std::mutex> lk(m_mtx);
    for (;;)
    {
        auto changeOnly = [&] { return m_reqGen > 0 && m_scannedSerial != m_changeSerial.load(); };
        auto pending = [&] { return m_stop || m_hasReq || m_forceRescan || !m_dirJobs.empty() || changeOnly(); };
        m_cv.wait(lk, pending);
        if (m_stop) break;

        const bool urgent = m_hasReq || m_forceRescan || !m_dirJobs.empty();
        if (!urgent)
        {
            // 変更通知だけが理由: 連続する変更（コピー中の大量ファイル等）を 250ms まとめてから 1 回だけ走査する
            const int64_t due = m_lastChangeTickMs + 250;
            const int64_t now = NowMs();
            if (now < due)
            {
                m_cv.wait_for(lk, std::chrono::milliseconds(due - now));
                continue;
            }
        }

        std::vector<std::string> dirJobs(m_dirJobs.begin(), m_dirJobs.end());
        m_dirJobs.clear();
        const bool byRequest = m_hasReq;
        const bool doScan = m_reqGen > 0 && (m_hasReq || m_forceRescan || changeOnly());
        const abl::ScanOptions opt = m_req;
        const uint64_t gen = m_reqGen;
        const uint64_t serial = m_changeSerial.load();
        m_hasReq = false;
        m_forceRescan = false;
        lk.unlock();

        // ---- フォルダツリーのサブフォルダ一覧 ----
        std::vector<std::pair<std::string, std::vector<abl::DirInfo>>> dirResults;
        for (const std::string& j : dirJobs)
            dirResults.emplace_back(j, abl::ListSubDirs(std::filesystem::path(j)));

        // ---- 一覧の走査 ----
        abl::ScanResult res;
        if (doScan)
        {
            m_scanning.store(true);
            res = abl::ScanDirectory(opt);
            m_scanning.store(false);
        }

        lk.lock();
        for (auto& dr : dirResults)
        {
            DirCache& c = m_dirCache[dr.first];
            c.dirs = std::move(dr.second);
            c.serial = serial;
        }
        if (doScan)
        {
            m_scannedSerial = serial;
            m_scanCount.fetch_add(1);
            const bool stale = (gen != m_reqGen);   // 走査中に新しい要求が来た → この結果は使わない
            if (!stale)
            {
                const bool sameContext = (opt.dir == m_lastDir && opt.query == m_lastQuery && m_lastEntriesGen != 0);
                if (byRequest || !sameContext || res.dirMissing || !abl::SameListing(m_lastEntries, res.entries))
                {
                    m_lastEntries = res.entries;   // 差分判定用に控える（UI へは move で渡すのでコピー）
                    m_lastEntriesGen = gen;
                    m_lastDir = opt.dir;
                    m_lastQuery = opt.query;
                    m_result = std::move(res);
                    m_resultGen = gen;
                    m_resultReady = true;
                }
            }
        }
    }
}

// ============================================================ 変更監視

void AssetIndex::StartWatcher()
{
    if (m_roots.empty()) return;
    HANDLE stopEv = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEv) return;
    m_watchStop = stopEv;
    std::vector<std::filesystem::path> roots = m_roots;
    m_watcher = std::thread([this, roots, stopEv] { WatchLoop(roots, stopEv); });
}

void AssetIndex::StopWatcher()
{
    if (m_watchStop) ::SetEvent(static_cast<HANDLE>(m_watchStop));
    if (m_watcher.joinable()) m_watcher.join();
    if (m_watchStop) { ::CloseHandle(static_cast<HANDLE>(m_watchStop)); m_watchStop = nullptr; }
}

void AssetIndex::WatchLoop(std::vector<std::filesystem::path> roots, void* stopEventV)
{
    HANDLE stopEv = static_cast<HANDLE>(stopEventV);
    struct Watch
    {
        HANDLE       dir = INVALID_HANDLE_VALUE;
        OVERLAPPED   ov{};
        alignas(DWORD) std::array<char, 65536> buf{};
        bool         pending = false;
    };
    std::vector<std::unique_ptr<Watch>> watches;
    for (const auto& r : roots)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(r, ec)) continue;
        HANDLE h = ::CreateFileW(r.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        auto w = std::make_unique<Watch>();
        w->dir = h;
        w->ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        watches.push_back(std::move(w));
    }

    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
                            | FILE_NOTIFY_CHANGE_LAST_WRITE;
    auto issue = [&](Watch& w) {
        ::ResetEvent(w.ov.hEvent);
        DWORD ret = 0;
        w.pending = ::ReadDirectoryChangesW(w.dir, w.buf.data(), static_cast<DWORD>(w.buf.size()), TRUE, kFilter, &ret, &w.ov, nullptr) != 0;
    };
    for (auto& w : watches) issue(*w);

    auto noteChange = [&] {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_lastChangeTickMs = NowMs();
            m_changeSerial.fetch_add(1);
        }
        m_cv.notify_all();
    };

    std::vector<HANDLE> waits;
    for (auto& w : watches) waits.push_back(w->ov.hEvent);
    waits.push_back(stopEv);

    while (!watches.empty())
    {
        const DWORD r = ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + waits.size() - 1 || r == WAIT_FAILED) break;   // 停止
        const size_t idx = r - WAIT_OBJECT_0;
        if (idx >= watches.size()) break;
        Watch& w = *watches[idx];
        DWORD bytes = 0;
        if (!::GetOverlappedResult(w.dir, &w.ov, &bytes, FALSE)) { w.pending = false; break; }
        bool relevant = false;
        if (bytes == 0)
        {
            relevant = true;   // バッファ溢れ（変更が多すぎて内訳が失われた）→ 念のため再走査
        }
        else
        {
            const char* p = w.buf.data();
            for (;;)
            {
                const auto* fi = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                if (!IgnorableChange(fi->FileName, fi->FileNameLength / sizeof(wchar_t))) { relevant = true; break; }
                if (fi->NextEntryOffset == 0) break;
                p += fi->NextEntryOffset;
            }
        }
        if (relevant) noteChange();
        issue(w);
    }

    for (auto& w : watches)
    {
        if (w->pending)
        {
            ::CancelIoEx(w->dir, &w->ov);
            DWORD b = 0;
            ::GetOverlappedResult(w->dir, &w->ov, &b, TRUE);   // 完了（キャンセル）を待ってからバッファを解放する
        }
        if (w->ov.hEvent) ::CloseHandle(w->ov.hEvent);
        ::CloseHandle(w->dir);
    }
}

} // namespace dx12e
