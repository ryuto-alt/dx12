#include "core/SceneBackup.h"
#include "core/SafeRemove.h"   // 再帰削除の最後の砦（ルート・ホームなどは消さない）

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace dx12e::scenebackup
{

Policy& GlobalPolicy()
{
    static Policy p;
    return p;
}

fs::path BackupDir(const fs::path& projectRoot) { return projectRoot / ".dx12" / "backups"; }

namespace
{

bool ReadAll(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// "<stem>_<YYYYmmdd_HHMMSS>[_<n>]" を分解する。stem が "_" を含んでも末尾から読むので壊れない。
bool ParseId(const std::string& id, std::string& stem, std::string& stamp, std::string* order = nullptr)
{
    auto isDigits = [](const std::string& s, size_t a, size_t n) {
        if (a + n > s.size()) return false;
        for (size_t i = a; i < a + n; ++i) if (s[i] < '0' || s[i] > '9') return false;
        return true;
    };
    std::string core = id;
    int seqNo = 1;
    // 末尾の _<n>（同秒の連番。1〜3 桁）を外す。ただし直前が "HHMMSS" の形のときだけ
    const size_t us = core.rfind('_');
    if (us != std::string::npos && us + 1 < core.size() && core.size() - us - 1 <= 3 && isDigits(core, us + 1, core.size() - us - 1) &&
        us >= 16 && isDigits(core, us - 6, 6))
    {
        seqNo = std::stoi(core.substr(us + 1));
        core = core.substr(0, us);
    }
    if (core.size() < 17) return false;
    const size_t a = core.size() - 15;   // YYYYmmdd_HHMMSS
    if (!isDigits(core, a, 8) || core[a + 8] != '_' || !isDigits(core, a + 9, 6)) return false;
    if (a < 2 || core[a - 1] != '_') return false;
    stem = core.substr(0, a - 1);
    stamp = core.substr(a);
    if (order)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "_%03d", seqNo);
        *order = stamp + buf;
    }
    return true;
}

int64_t StampToUnix(const std::string& stamp)
{
    std::tm tmv{};
    tmv.tm_year = std::stoi(stamp.substr(0, 4)) - 1900;
    tmv.tm_mon  = std::stoi(stamp.substr(4, 2)) - 1;
    tmv.tm_mday = std::stoi(stamp.substr(6, 2));
    tmv.tm_hour = std::stoi(stamp.substr(9, 2));
    tmv.tm_min  = std::stoi(stamp.substr(11, 2));
    tmv.tm_sec  = std::stoi(stamp.substr(13, 2));
    tmv.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&tmv));
}

uint64_t TreeBytes(const fs::path& p)
{
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) { const auto s = fs::file_size(p, ec); return ec ? 0 : s; }
    uint64_t n = 0;
    if (fs::is_directory(p, ec))
        for (const auto& e : fs::recursive_directory_iterator(p, ec))
            if (e.is_regular_file(ec)) { const auto s = e.file_size(ec); if (!ec) n += s; }
    return n;
}

fs::path PartsOf(const fs::path& dir, const std::string& stemOrId) { return dir / (stemOrId + ".parts"); }
fs::path InstOf (const fs::path& dir, const std::string& stemOrId) { return dir / (stemOrId + ".inst"); }
fs::path NavOf  (const fs::path& dir, const std::string& stemOrId) { return dir / (stemOrId + ".nav"); }

// プロジェクトの内側か（lexically_relative で ".." にならない）、かつ "." 始まりのフォルダ/ファイルの中ではないか
bool IsBackupTarget(const fs::path& projectRoot, const fs::path& scenePath)
{
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(projectRoot, ec);
    const fs::path abs  = fs::weakly_canonical(scenePath, ec);
    if (root.empty() || abs.empty()) return false;
    const fs::path rel = abs.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") return false;
    for (const auto& comp : rel)
    {
        const std::string c = comp.string();
        if (c.size() > 1 && c[0] == '.') return false;   // .autosave / .dx12 など生成物
    }
    return true;
}

std::string NowStamp(int seq)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmv{};
    char buf[32] = {};
    if (localtime_s(&tmv, &t) == 0) std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    std::string s = buf;
    if (seq > 1) s += "_" + std::to_string(seq);
    return s;
}


// ============================================================================
//  内容アドレス方式の置き場
//    .dx12/backups/objects/<内容ハッシュ>-<大きさ>   … ファイルの実体（同じ内容は 1 回だけ置く）
//    .dx12/backups/<id>.gen                          … 世代の目録（相対名 → ハッシュ・大きさ・更新時刻）。最後に原子的に書く
//  世代どうしは同じ実体を指すだけ（ハードリンクもコピーもしない）ので、現行ファイルをその場で上書きしても世代は変わらない。
//  旧方式の世代（<id>.json + .parts/.inst/.nav のコピー）は一覧と復元だけ引き続き扱う。
// ============================================================================

struct Entry
{
    std::string rel;       // "root" / "parts/<名前>" / "inst/<名前>" / "nav"
    std::string hash;      // 32 桁の 16 進 + "-" + 大きさ
    uint64_t    size = 0;
    int64_t     mtime = 0;
};

// 128 ビットの高速ハッシュ（暗号用ではない。内容が違えば違う値になれば十分。大きさも名前に含める）
class Hasher
{
public:
    void Update(const char* p, size_t n)
    {
        total_ += n;
        while (n > 0)
        {
            if (fill_ == 0 && n >= 16)
            {
                const size_t blocks = n / 16;
                for (size_t i = 0; i < blocks; ++i, p += 16) Block(p);
                n -= blocks * 16;
                continue;
            }
            const size_t take = (std::min)(n, 16 - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 16) { Block(buf_); fill_ = 0; }
        }
    }
    std::string Finish()
    {
        uint64_t tail = 0;
        for (size_t i = 0; i < fill_; ++i) tail = tail * 131 + static_cast<unsigned char>(buf_[i]);
        uint64_t a = Fmix(h1_ ^ tail ^ total_), b = Fmix(h2_ + tail * 31 + total_);
        char out[64];
        std::snprintf(out, sizeof(out), "%016llx%016llx-%llu", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                      static_cast<unsigned long long>(total_));
        return out;
    }
private:
    static uint64_t Fmix(uint64_t k) { k ^= k >> 33; k *= 0xFF51AFD7ED558CCDULL; k ^= k >> 33; k *= 0xC4CEB9FE1A85EC53ULL; k ^= k >> 33; return k; }
    void Block(const char* p)
    {
        uint64_t a, b;
        std::memcpy(&a, p, 8); std::memcpy(&b, p + 8, 8);
        h1_ = (h1_ ^ a) * 0x9FB21C651E98DF25ULL; h1_ ^= h1_ >> 29;
        h2_ = (h2_ ^ b) * 0xC2B2AE3D27D4EB4FULL; h2_ ^= h2_ >> 31;
        h1_ += b; h2_ += a;
    }
    uint64_t h1_ = 0x9E3779B97F4A7C15ULL, h2_ = 0x243F6A8885A308D3ULL, total_ = 0;
    char     buf_[16] = {};
    size_t   fill_ = 0;
};

fs::path ObjectsDir(const fs::path& dir) { return dir / "objects"; }
int64_t  MTimeOf(const fs::path& p) { std::error_code ec; const auto t = fs::last_write_time(p, ec); return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count()); }

std::string RelFor(const fs::path& scenePath, const fs::path& live)
{
    const std::string stem = scenePath.stem().string();
    if (live == scenePath) return "root";
    const std::string pn = live.parent_path().filename().string();
    if (pn == stem + ".parts") return "parts/" + live.filename().string();
    if (pn == stem + ".inst") return "inst/" + live.filename().string();
    if (live.extension() == ".nav") return "nav";
    return {};
}
fs::path LiveFor(const fs::path& scenePath, const std::string& rel)
{
    const std::string stem = scenePath.stem().string();
    const fs::path sdir = scenePath.parent_path();
    if (rel == "root") return scenePath;
    if (rel == "nav") return NavOf(sdir, stem);
    if (rel.rfind("parts/", 0) == 0) return PartsOf(sdir, stem) / rel.substr(6);
    if (rel.rfind("inst/", 0) == 0) return InstOf(sdir, stem) / rel.substr(5);
    return {};
}

// 実体を objects へ置く。bytes があれば（読み込み済み）それを書く。移動元 moveFrom があれば改名する（同じボリュームならコピー不要）。
// すでに同じ内容があれば何もしない（移動元は消す）。
bool StoreObject(const fs::path& dir, const std::string& hash, const char* bytes, size_t size, const fs::path& moveFrom)
{
    std::error_code ec;
    const fs::path od = ObjectsDir(dir);
    const fs::path dst = od / hash;
    if (fs::exists(dst, ec))
    {
        if (!moveFrom.empty()) fs::remove(moveFrom, ec);
        return true;
    }
    fs::create_directories(od, ec);
    if (!moveFrom.empty())
    {
        if (MoveFileExW(moveFrom.c_str(), dst.c_str(), 0)) return true;
        if (fs::exists(dst, ec)) { fs::remove(moveFrom, ec); return true; }
        if (MoveFileExW(moveFrom.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) return true;
        return false;
    }
    // 一時ファイルへ書いて改名（途中で死んだ半端な実体を名前の付いた物として残さない）。同期は不要（バックアップの実体）。
    static std::atomic<unsigned> seq{0};
    const fs::path tmp = od / (hash + "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(seq.fetch_add(1)) + ".tmp");
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o.write(bytes, static_cast<std::streamsize>(size));
        if (!o) { o.close(); fs::remove(tmp, ec); return false; }
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) { fs::remove(tmp, ec); return fs::exists(dst, ec); }
    return true;
}

// 世代の目録
bool WriteManifest(const fs::path& dir, const std::string& id, const std::vector<Entry>& es)
{
    std::string t = "DX12GEN1\n";
    for (const Entry& e : es) t += e.rel + "\t" + e.hash + "\t" + std::to_string(e.size) + "\t" + std::to_string(e.mtime) + "\n";
    t += "END\n";
    return atomicfile::WriteFile(dir / (id + ".gen"), t).ok;
}
bool ReadManifest(const fs::path& path, std::vector<Entry>& out)
{
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    std::getline(f, line);
    if (line != "DX12GEN1") return false;
    bool ended = false;
    while (std::getline(f, line))
    {
        if (line == "END") { ended = true; break; }
        std::vector<std::string> c;
        size_t a = 0;
        for (;;) { const size_t b = line.find('\t', a); c.push_back(line.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (c.size() < 4) continue;
        Entry e;
        e.rel = c[0]; e.hash = c[1];
        e.size = std::strtoull(c[2].c_str(), nullptr, 10);
        e.mtime = std::strtoll(c[3].c_str(), nullptr, 10);
        out.push_back(std::move(e));
    }
    return ended;
}

void RemoveGenFiles(const fs::path& dir, const std::string& id)
{
    std::error_code ec;
    fs::remove(dir / (id + ".gen"), ec);
    fs::remove(dir / (id + ".json"), ec);
    saferm::RemoveAll(PartsOf(dir, id), ec, dir);
    saferm::RemoveAll(InstOf(dir, id), ec, dir);
    fs::remove(NavOf(dir, id), ec);
}

template <typename Fn>
void ParallelFor(size_t n, Fn&& fn)
{
    const unsigned hw = (std::max)(1u, std::thread::hardware_concurrency());
    const unsigned workers = (std::max)(1u, (std::min)(hw / 4, static_cast<unsigned>(n)));
    std::atomic<size_t> next{0};
    auto run = [&] { for (;;) { const size_t i = next.fetch_add(1); if (i >= n) return; fn(i); } };
    if (workers <= 1) { run(); return; }
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) pool.emplace_back(run);
    for (std::thread& t : pool) t.join();
}

// 保存前の版の各ファイルを objects へ置いて目録を作る。retained にあるファイルは置き換えで退いた元の版（移動）、
// それ以外は現行のファイル（読んでハッシュ。同じ大きさ・更新時刻が直前の世代の目録にあればハッシュを再利用して読まない。
// 実体が無ければ書く）。
bool BuildEntries(const fs::path& dir, const fs::path& scenePath, const std::vector<fs::path>& live,
                  const std::map<std::string, fs::path>& oldOf, const std::vector<Entry>& newest, std::vector<Entry>& out)
{
    std::map<std::string, const Entry*> prev;
    for (const Entry& e : newest) prev[e.rel] = &e;
    auto keyOf = [](const fs::path& x) { return x.lexically_normal().generic_string(); };
    out.assign(live.size(), Entry{});
    std::atomic<bool> failed{false};
    ParallelFor(live.size(), [&](size_t i) {
        if (failed.load()) return;
        const fs::path& L = live[i];
        Entry e;
        e.rel = RelFor(scenePath, L);
        if (e.rel.empty()) return;
        std::error_code ec;
        const auto it = oldOf.find(keyOf(L));
        const bool moved = it != oldOf.end();
        const fs::path& src = moved ? it->second : L;
        if (!fs::is_regular_file(src, ec)) return;   // 保存前の一覧にあったが、いまは無い
        e.mtime = MTimeOf(src);
        e.size = fs::file_size(src, ec);
        const auto pv = prev.find(e.rel);
        if (!moved && pv != prev.end() && pv->second->size == e.size && pv->second->mtime == e.mtime && fs::exists(ObjectsDir(dir) / pv->second->hash, ec))
        {
            e.hash = pv->second->hash;   // 前の世代と同じファイル（読まない・置かない）
            out[i] = std::move(e);
            return;
        }
        std::string bytes;
        if (!ReadAll(src, bytes)) { failed.store(true); return; }
        Hasher h;
        h.Update(bytes.data(), bytes.size());
        e.hash = h.Finish();
        e.size = bytes.size();
        if (!StoreObject(dir, e.hash, bytes.data(), bytes.size(), moved ? src : fs::path{})) failed.store(true);
        out[i] = std::move(e);
    });
    if (failed.load()) return false;
    out.erase(std::remove_if(out.begin(), out.end(), [](const Entry& e) { return e.rel.empty(); }), out.end());
    return !out.empty();
}

std::vector<fs::path> ListLive(const fs::path& scenePath)
{
    std::error_code ec;
    const std::string stem = scenePath.stem().string();
    const fs::path sdir = scenePath.parent_path();
    std::vector<fs::path> live;
    live.push_back(scenePath);
    for (const fs::path& d : {PartsOf(sdir, stem), InstOf(sdir, stem)})
        if (fs::is_directory(d, ec))
            for (const auto& e : fs::directory_iterator(d, ec))
                if (e.is_regular_file(ec) && e.path().extension() != ".dx12tmp" && e.path().extension() != ".dx12old") live.push_back(e.path());
    if (fs::is_regular_file(NavOf(sdir, stem), ec)) live.push_back(NavOf(sdir, stem));
    return live;
}

} // namespace

// ---- 世代の一覧 ----
static std::vector<Generation> ListImpl(const fs::path& projectRoot, const fs::path& scenePath, bool withBytes)
{
    std::vector<Generation> out;
    const fs::path dir = BackupDir(projectRoot);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    const std::string stem = scenePath.stem().string();
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        ec.clear();
        if (!e.is_regular_file(ec)) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".gen" && ext != ".json") continue;
        const std::string id = e.path().stem().string();
        Generation g;
        if (!ParseId(id, g.stem, g.stamp, &g.order) || g.stem != stem) continue;
        g.id = id;
        g.unixTime = StampToUnix(g.stamp);
        g.root = e.path();
        g.legacy = (ext == ".json");
        if (g.legacy)
        {
            if (fs::exists(dir / (id + ".gen"), ec)) continue;
            g.hasParts = fs::is_directory(PartsOf(dir, id), ec);
            g.hasInst  = fs::is_directory(InstOf(dir, id), ec);
            g.hasNav   = fs::is_regular_file(NavOf(dir, id), ec);
            if (withBytes) g.bytes = TreeBytes(g.root) + TreeBytes(PartsOf(dir, id)) + TreeBytes(InstOf(dir, id)) + TreeBytes(NavOf(dir, id));
        }
        else if (withBytes)
        {
            std::vector<Entry> es;
            if (!ReadManifest(e.path(), es)) continue;   // 書きかけ・壊れた目録は一覧に出さない
            for (const Entry& x : es)
            {
                g.bytes += x.size;
                g.hasParts |= x.rel.rfind("parts/", 0) == 0;
                g.hasInst  |= x.rel.rfind("inst/", 0) == 0;
                g.hasNav   |= x.rel == "nav";
            }
        }
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(), [](const Generation& a, const Generation& b) { return a.order > b.order; });
    return out;
}

namespace
{
std::mutex g_jobMu;
std::thread g_job;
struct JobGuard { ~JobGuard() { WaitIdle(); } };
JobGuard g_jobGuard;
std::mutex g_forceMu;
std::set<std::string> g_forceNext;
std::string ForceKey(const fs::path& p)
{
    std::string s = p.lexically_normal().generic_string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
bool ConsumeForce(const fs::path& scenePath)
{
    std::lock_guard<std::mutex> lk(g_forceMu);
    return g_forceNext.erase(ForceKey(scenePath)) > 0;
}

std::string NewId(const fs::path& dir, const fs::path& projectRoot, const fs::path& scenePath)
{
    const std::string stem = scenePath.stem().string();
    const std::vector<Generation> gens = ListImpl(projectRoot, scenePath, false);
    const std::string nowStamp = NowStamp(1);
    int maxSeq = 0;
    for (const auto& g : gens)
        if (g.stamp == nowStamp) maxSeq = (std::max)(maxSeq, std::atoi(g.order.c_str() + g.order.size() - 3));
    std::error_code ec;
    std::string id;
    for (int seq = maxSeq + 1; seq < 1000; ++seq)
    {
        id = stem + "_" + NowStamp(seq);
        if (!fs::exists(dir / (id + ".gen"), ec) && !fs::exists(dir / (id + ".json"), ec)) break;
    }
    return id;
}

std::vector<Entry> NewestEntries(const fs::path& projectRoot, const fs::path& scenePath)
{
    std::vector<Entry> es;
    for (const Generation& g : ListImpl(projectRoot, scenePath, false))
        if (!g.legacy) { if (ReadManifest(g.root, es)) return es; es.clear(); }
    return es;
}
} // namespace

void WaitIdle()
{
    std::lock_guard<std::mutex> lk(g_jobMu);
    if (g_job.joinable()) g_job.join();
}

std::vector<Generation> List(const fs::path& projectRoot, const fs::path& scenePath, bool withBytes)
{
    WaitIdle();
    return ListImpl(projectRoot, scenePath, withBytes);
}

void ForceNext(const fs::path& scenePath)
{
    std::lock_guard<std::mutex> lk(g_forceMu);
    g_forceNext.insert(ForceKey(scenePath));
}

Pending Begin(const fs::path& projectRoot, const fs::path& scenePath, bool force, std::string* why)
{
    auto no = [&](const char* w) { if (why) *why = w; return Pending{}; };
    WaitIdle();   // 前の保存の世代作成がまだ走っていたら待つ（これから置き換えるファイルを読んでいる最中かもしれない）
    const bool forced = ConsumeForce(scenePath) || force;
    const Policy pol = GlobalPolicy();
    if (!pol.enabled) return no("バックアップは無効です");
    std::error_code ec;
    if (!fs::is_regular_file(scenePath, ec)) return no("元のシーンファイルがありません（初回の保存）");
    if (!IsBackupTarget(projectRoot, scenePath)) return no("プロジェクトの外または生成物のフォルダです");
    if (!forced && pol.minIntervalSec > 0)
    {
        const std::vector<Generation> gens = ListImpl(projectRoot, scenePath, /*withBytes=*/false);
        if (!gens.empty() && static_cast<int64_t>(std::time(nullptr)) - gens.front().unixTime < pol.minIntervalSec)
            return no("前の世代から間もないため作りません");
    }
    Pending p;
    p.active = true;
    p.projectRoot = projectRoot;
    p.scenePath = scenePath;
    p.live = ListLive(scenePath);
    return p;
}

static void ReleaseOlds(const std::vector<atomicfile::Batch::Retained>& olds)
{
    std::error_code ec;
    for (const auto& r : olds) fs::remove(r.old, ec);
}

static std::string FinishImpl(Pending& p, const std::vector<atomicfile::Batch::Retained>& retained)
{
    if (!p.active) { ReleaseOlds(retained); return {}; }
    p.active = false;
    if (retained.empty()) return {};   // 何も置き換わらなかった＝前の版と同じ

    std::error_code ec;
    const fs::path dir = BackupDir(p.projectRoot);
    fs::create_directories(dir, ec);
    if (ec) { ReleaseOlds(retained); return {}; }
    const std::string id = NewId(dir, p.projectRoot, p.scenePath);
    auto keyOf = [](const fs::path& x) { return x.lexically_normal().generic_string(); };
    std::map<std::string, fs::path> oldOf;
    for (const auto& r : retained) oldOf[keyOf(r.dst)] = r.old;

    std::vector<Entry> entries;
    const bool ok = BuildEntries(dir, p.scenePath, p.live, oldOf, NewestEntries(p.projectRoot, p.scenePath), entries)
                    && WriteManifest(dir, id, entries);   // 目録は最後（これが見えている世代は完全）
    ReleaseOlds(retained);   // 移動できなかった元のファイルの残り
    if (!ok) { RemoveGenFiles(dir, id); return {}; }
    Prune(p.projectRoot, GlobalPolicy());
    return id;
}

std::string Finish(Pending& p, atomicfile::Batch& batch)
{
    return FinishImpl(p, batch.TakeRetained());
}

void FinishAsync(Pending&& p, atomicfile::Batch& batch)
{
    if (!p.active) { batch.ReleaseRetained(); return; }
    std::vector<atomicfile::Batch::Retained> retained = batch.TakeRetained();
    std::lock_guard<std::mutex> lk(g_jobMu);
    if (g_job.joinable()) g_job.join();
    g_job = std::thread([pending = std::move(p), kept = std::move(retained)]() mutable {
        try { FinishImpl(pending, kept); } catch (...) { ReleaseOlds(kept); }
    });
}

std::string Snapshot(const fs::path& projectRoot, const fs::path& scenePath, bool force, std::string* why)
{
    auto no = [&](const char* w) { if (why) *why = w; return std::string(); };
    WaitIdle();
    const Policy pol = GlobalPolicy();
    if (!pol.enabled) return no("バックアップは無効です");
    std::error_code ec;
    if (!fs::is_regular_file(scenePath, ec)) return no("元のシーンファイルがありません（初回の保存）");
    if (!IsBackupTarget(projectRoot, scenePath)) return no("プロジェクトの外または生成物のフォルダです");
    const fs::path dir = BackupDir(projectRoot);
    const std::vector<Generation> gens = ListImpl(projectRoot, scenePath, /*withBytes=*/false);
    if (!gens.empty() && !force && pol.minIntervalSec > 0 &&
        static_cast<int64_t>(std::time(nullptr)) - gens.front().unixTime < pol.minIntervalSec)
        return no("前の世代から間もないため作りません");
    fs::create_directories(dir, ec);
    if (ec) return no("バックアップのフォルダを作れません");

    const std::vector<Entry> newest = NewestEntries(projectRoot, scenePath);
    std::vector<Entry> entries;
    if (!BuildEntries(dir, scenePath, ListLive(scenePath), {}, newest, entries)) return no("世代を書けません");
    // 最新の世代と同じ内容（同じファイル一式・同じハッシュ）なら作らない
    if (!newest.empty() && newest.size() == entries.size())
    {
        bool same = true;
        std::map<std::string, std::string> nh;
        for (const Entry& e : newest) nh[e.rel] = e.hash;
        for (const Entry& e : entries) { const auto it = nh.find(e.rel); if (it == nh.end() || it->second != e.hash) { same = false; break; } }
        if (same) { Prune(projectRoot, pol); return no("最新の世代と同じ内容です"); }
    }
    const std::string id = NewId(dir, projectRoot, scenePath);
    if (!WriteManifest(dir, id, entries)) { RemoveGenFiles(dir, id); return no("世代を書けません"); }
    Prune(projectRoot, pol);
    return id;
}

int Prune(const fs::path& projectRoot, const Policy& policy)
{
    const fs::path dir = BackupDir(projectRoot);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int removed = 0;

    // 1) 本体の無い旧方式の途中の世代・古い一時ファイルの掃除
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        ec.clear();
        if (e.is_directory(ec))
        {
            const std::string ext = e.path().extension().string();
            if (ext == ".parts" || ext == ".inst")
            {
                const std::string id = e.path().stem().string();
                if (!fs::exists(dir / (id + ".json"), ec) && !fs::exists(dir / (id + ".gen"), ec)) { saferm::RemoveAll(e.path(), ec, dir); ++removed; }
            }
        }
        else if (e.path().extension() == ".nav")
        {
            const std::string id = e.path().stem().string();
            if (!fs::exists(dir / (id + ".json"), ec)) { fs::remove(e.path(), ec); ++removed; }
        }
    }
    atomicfile::SweepStaleTmp(dir);

    // 2) シーンごとに世代数の上限
    std::set<std::string> stems;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        ec.clear();
        if (!e.is_regular_file(ec)) continue;
        const std::string x = e.path().extension().string();
        if (x != ".gen" && x != ".json") continue;
        std::string stem, stamp;
        if (ParseId(e.path().stem().string(), stem, stamp)) stems.insert(stem);
    }
    std::vector<Generation> all;
    for (const std::string& s : stems)
    {
        auto gens = ListImpl(projectRoot, fs::path(s + ".json"), /*withBytes=*/false);
        const size_t keep = static_cast<size_t>((std::max)(1, policy.generations));
        for (size_t i = 0; i < gens.size(); ++i)
        {
            if (i >= keep) { RemoveGenFiles(dir, gens[i].id); ++removed; }
            else all.push_back(gens[i]);
        }
    }

    // 3) 参照されなくなった実体だけ消す（世代の目録のどれにも載っていない objects）。合計容量も数える。
    auto gc = [&](uint64_t& total) {
        std::error_code e2;
        std::set<std::string> used;
        uint64_t legacyBytes = 0;
        for (const Generation& g : all)
        {
            if (!fs::exists(g.root, e2)) continue;
            if (g.legacy) { legacyBytes += TreeBytes(g.root) + TreeBytes(PartsOf(dir, g.id)) + TreeBytes(InstOf(dir, g.id)) + TreeBytes(NavOf(dir, g.id)); continue; }
            std::vector<Entry> es;
            if (ReadManifest(g.root, es)) for (const Entry& x : es) used.insert(x.hash);
        }
        total = legacyBytes;
        const fs::path od = ObjectsDir(dir);
        if (!fs::is_directory(od, e2)) return;
        for (const auto& o : fs::directory_iterator(od, e2))
        {
            e2.clear();
            if (!o.is_regular_file(e2)) continue;
            const std::string n = o.path().filename().string();
            if (o.path().extension() == ".tmp") continue;   // 書いている途中（SweepStaleTmp が古いものを消す）
            if (!used.count(n)) { fs::remove(o.path(), e2); ++removed; }
            else { const auto sz = o.file_size(e2); if (!e2) total += sz; }
        }
    };
    uint64_t total = 0;
    gc(total);

    // 4) 合計容量の上限: 古い世代から消す（各シーンの最新 1 世代は残す）
    if (policy.maxTotalBytes > 0 && total > policy.maxTotalBytes)
    {
        std::sort(all.begin(), all.end(), [](const Generation& a, const Generation& b) { return a.order < b.order; });
        for (size_t i = 0; i < all.size() && total > policy.maxTotalBytes; ++i)
        {
            const Generation g = all[i];
            bool newest = true;
            for (const Generation& o : all) if (o.stem == g.stem && o.order > g.order && fs::exists(o.root, ec)) { newest = false; break; }
            if (newest) continue;
            RemoveGenFiles(dir, g.id);
            all[i].root.clear();
            ++removed;
            gc(total);
        }
    }
    return removed;
}

void Remove(const fs::path& projectRoot, const std::string& id)
{
    WaitIdle();
    RemoveGenFiles(BackupDir(projectRoot), id);
}

bool ReadGenerationFile(const fs::path& projectRoot, const std::string& id, const std::string& rel, std::string& out)
{
    WaitIdle();
    const fs::path dir = BackupDir(projectRoot);
    std::vector<Entry> es;
    if (ReadManifest(dir / (id + ".gen"), es))
    {
        for (const Entry& e : es) if (e.rel == rel) return ReadAll(ObjectsDir(dir) / e.hash, out);
        return false;
    }
    // 旧方式
    if (rel == "root") return ReadAll(dir / (id + ".json"), out);
    if (rel == "nav") return ReadAll(NavOf(dir, id), out);
    if (rel.rfind("parts/", 0) == 0) return ReadAll(PartsOf(dir, id) / rel.substr(6), out);
    if (rel.rfind("inst/", 0) == 0) return ReadAll(InstOf(dir, id) / rel.substr(5), out);
    return false;
}

atomicfile::Result Restore(const fs::path& projectRoot, const fs::path& scenePath, const std::string& id, std::string* preRestoreId)
{
    atomicfile::Result r;
    WaitIdle();
    const fs::path dir = BackupDir(projectRoot);
    std::string stem, stamp;
    if (!ParseId(id, stem, stamp) || stem != scenePath.stem().string()) { r.error = "このシーンの世代ではありません: " + id; return r; }
    std::error_code ec;

    // 戻す一式（相対名 → 実体のパス）
    std::vector<std::pair<std::string, fs::path>> files;
    std::vector<Entry> es;
    if (ReadManifest(dir / (id + ".gen"), es))
        for (const Entry& e : es) files.push_back({e.rel, ObjectsDir(dir) / e.hash});
    else if (fs::is_regular_file(dir / (id + ".json"), ec))
    {
        files.push_back({"root", dir / (id + ".json")});
        for (const char* sub : {"parts", "inst"})
        {
            const fs::path d = std::string(sub) == "parts" ? PartsOf(dir, id) : InstOf(dir, id);
            if (fs::is_directory(d, ec))
                for (const auto& e : fs::directory_iterator(d, ec)) if (e.is_regular_file(ec)) files.push_back({std::string(sub) + "/" + e.path().filename().string(), e.path()});
        }
        if (fs::is_regular_file(NavOf(dir, id), ec)) files.push_back({"nav", NavOf(dir, id)});
    }
    else { r.error = "世代が見つかりません: " + id; return r; }

    // 今の版を残す（復元を取り消せるように）。復元元と同じ内容なら作られない。
    const std::string pre = Snapshot(projectRoot, scenePath, /*force=*/true);
    if (preRestoreId) *preRestoreId = pre;

    // 読んでから 1 回のコミットで置き換える（本体は最後）。実体が無い・壊れている世代は、何も書かずに失敗する。
    std::vector<std::string> blobs(files.size());
    std::vector<atomicfile::Batch::Entry> stage;
    fs::path rootBlobSrc;
    size_t rootIdx = files.size();
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!ReadAll(files[i].second, blobs[i])) { r.error = "世代の実体を読めません: " + files[i].first; return r; }
        if (files[i].first == "root") rootIdx = i;
    }
    if (rootIdx == files.size()) { r.error = "世代に本体がありません"; return r; }
    const fs::path sdir = scenePath.parent_path();
    atomicfile::Batch batch(fs::path(scenePath).concat(".dx12txn"));
    std::vector<std::string> keepParts, keepInst;
    bool hasNav = false;
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (i == rootIdx) continue;
        const std::string& rel = files[i].first;
        const fs::path dst = LiveFor(scenePath, rel);
        if (dst.empty()) continue;
        if (rel.rfind("parts/", 0) == 0) keepParts.push_back(rel.substr(6));
        else if (rel.rfind("inst/", 0) == 0) keepInst.push_back(rel.substr(5));
        else hasNav = true;
        stage.push_back({dst, std::string_view(blobs[i]), {}});
    }
    const unsigned hw = (std::max)(1u, std::thread::hardware_concurrency());
    if (!batch.AddMany(stage, (std::max)(1u, hw / 4), /*skipIdentical=*/true)) { r.error = "世代を書き出せません: " + batch.Error(); return r; }
    if (!batch.Add(scenePath, blobs[rootIdx])) { r.error = "世代を書き出せません: " + batch.Error(); return r; }
    r = batch.Commit();
    if (!r.ok) return r;

    // 世代に無いファイルは消す（食い違いを残さない）
    auto sweep = [&](const fs::path& d, const std::vector<std::string>& keep, const char* ext) {
        if (!fs::is_directory(d, ec)) return;
        for (const auto& e : fs::directory_iterator(d, ec))
        {
            if (!e.is_regular_file(ec) || e.path().extension() != ext) continue;
            if (std::find(keep.begin(), keep.end(), e.path().filename().string()) == keep.end()) fs::remove(e.path(), ec);
        }
        if (fs::is_empty(d, ec)) fs::remove(d, ec);
    };
    sweep(PartsOf(sdir, stem), keepParts, ".json");
    sweep(InstOf(sdir, stem), keepInst, ".jsonl");
    if (!hasNav) fs::remove(NavOf(sdir, stem), ec);
    r.ok = true;
    return r;
}

} // namespace dx12e::scenebackup
