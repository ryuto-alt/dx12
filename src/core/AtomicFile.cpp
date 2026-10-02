#include "core/AtomicFile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace dx12e::atomicfile
{

namespace
{
Stats g_stats;
std::mutex g_statsMu;   // AddMany が複数スレッドから Stage を呼ぶ
template <typename T> void StatAdd(T& field, T v) { std::lock_guard<std::mutex> lk(g_statsMu); field += v; }
double NowMs()
{
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}
std::atomic<int>     g_fault{static_cast<int>(Fault::None)};
std::atomic<int64_t> g_faultN{0};
std::atomic<unsigned> g_seq{0};

bool FaultIs(Fault f) { return g_fault.load() == static_cast<int>(f); }

// 強制終了テスト用: 環境変数 DX12E_ATOMIC_COMMIT_DELAY_MS（ミリ秒）があれば、コミットの各段（記録を書いた後・1 ファイル置き換えるごと）で
// その時間だけ眠る。コミットは本来ミリ秒で終わるので、保存中にプロセスを殺す試験が「置き換えの途中」を狙えるようにするための窓。既定は無効。
int CommitDelayMs()
{
    static const int v = [] {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA("DX12E_ATOMIC_COMMIT_DELAY_MS", buf, sizeof(buf));
        return (n > 0 && n < sizeof(buf)) ? (std::max)(0, std::atoi(buf)) : 0;
    }();
    return v;
}
void CommitDelay() { if (const int ms = CommitDelayMs()) Sleep(static_cast<DWORD>(ms)); }

std::string Win32Msg(DWORD e)
{
    char* buf = nullptr;
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string s = "エラー " + std::to_string(e);
    if (n && buf)
    {
        std::string m(buf, n);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
        s += " (" + m + ")";
    }
    if (buf) LocalFree(buf);
    return s;
}

fs::path MakeTmpPath(const fs::path& dst)
{
    std::wstring name = dst.filename().wstring();
    name += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(g_seq.fetch_add(1)) + L".dx12tmp";
    return dst.parent_path() / name;
}

// 一時ファイルへ書く → flush → 読み戻して比較 → 検証関数。失敗したら一時ファイルを消す。
Result Stage(const fs::path& tmp, std::string_view bytes, const Verifier& verify)
{
    Result r;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        r.error = "一時ファイルを作れません: " + Utf8FromPath(tmp) + " " + Win32Msg(GetLastError());
        return r;
    }
    auto fail = [&](std::string msg) {
        CloseHandle(h);
        std::error_code ec;
        fs::remove(tmp, ec);
        r.ok = false;
        r.error = std::move(msg);
        return r;
    };

    const size_t kChunk = 1u << 20;
    size_t off = 0;
    double t0 = NowMs();
    StatAdd(g_stats.bytes, static_cast<int64_t>(bytes.size()));
    StatAdd(g_stats.files, 1);
    const bool diskFull = FaultIs(Fault::DiskFullAfterBytes);
    const int64_t limit = g_faultN.load();
    while (off < bytes.size())
    {
        size_t n = (std::min)(kChunk, bytes.size() - off);
        if (diskFull)
        {
            const int64_t left = limit - static_cast<int64_t>(off);
            if (left <= 0) return fail("ディスクの空き容量が足りません（書き込み中に失敗）: " + Utf8FromPath(tmp));
            n = (std::min)(n, static_cast<size_t>(left));
        }
        DWORD wrote = 0;
        if (!::WriteFile(h, bytes.data() + off, static_cast<DWORD>(n), &wrote, nullptr) || wrote != n)
            return fail("ファイルへ書き込めません: " + Utf8FromPath(tmp) + " " + Win32Msg(GetLastError()));
        off += n;
        if (diskFull && static_cast<int64_t>(off) >= limit && off < bytes.size())
            return fail("ディスクの空き容量が足りません（書き込み中に失敗）: " + Utf8FromPath(tmp));
    }
    {
        const double t1 = NowMs();
        StatAdd(g_stats.writeMs, t1 - t0);
        t0 = t1;
    }
    if (FaultIs(Fault::FlushFails) || !FlushFileBuffers(h))
        return fail("ディスクへの書き出し（フラッシュ）に失敗しました: " + Utf8FromPath(tmp));
    {
        const double t1 = NowMs();
        StatAdd(g_stats.flushMs, t1 - t0);
        t0 = t1;
    }

    // 読み戻して比較（サイズと中身）
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || static_cast<uint64_t>(sz.QuadPart) != bytes.size())
        return fail("書き込んだファイルの大きさが一致しません: " + Utf8FromPath(tmp));
    if (SetFilePointer(h, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return fail("書き込んだファイルを読み戻せません: " + Utf8FromPath(tmp));
    {
        std::string buf(kChunk, '\0');
        size_t pos = 0;
        while (pos < bytes.size())
        {
            const size_t n = (std::min)(kChunk, bytes.size() - pos);
            DWORD got = 0;
            if (!::ReadFile(h, buf.data(), static_cast<DWORD>(n), &got, nullptr) || got != n)
                return fail("書き込んだファイルを読み戻せません: " + Utf8FromPath(tmp));
            if (std::memcmp(buf.data(), bytes.data() + pos, n) != 0)
                return fail("書き込んだ内容が一致しません: " + Utf8FromPath(tmp));
            pos += n;
        }
    }
    CloseHandle(h);
    {
        const double t1 = NowMs();
        StatAdd(g_stats.readbackMs, t1 - t0);
        t0 = t1;
    }

    std::string verr;
    const bool verifyBad = FaultIs(Fault::VerifyFails) || (verify && !verify(bytes, verr));
    StatAdd(g_stats.verifyMs, NowMs() - t0);
    if (verifyBad)
    {
        std::error_code ec;
        fs::remove(tmp, ec);
        r.error = "書き込んだ内容の検証に失敗しました: " + Utf8FromPath(tmp) + (verr.empty() ? std::string() : " " + verr);
        return r;
    }
    r.ok = true;
    return r;
}

bool TransientErr(DWORD e)
{
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_UNABLE_TO_REMOVE_REPLACED;
}

// dst を tmp の中身に置き換える。backup が非空なら元の dst をそこへ残す。
Result ReplaceOneImpl(const fs::path& dst, const fs::path& tmp, const fs::path& backup, bool dstExists, int index)
{
    Result r;
    if (FaultIs(Fault::ReplaceFails) && g_faultN.load() == index)
    {
        r.error = "ファイルを置き換えられません（故障注入）: " + Utf8FromPath(dst);
        return r;
    }
    if (!dstExists)
    {
        if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            r.error = "ファイルを置き換えられません: " + Utf8FromPath(dst) + " " + Win32Msg(GetLastError());
            return r;
        }
        r.ok = true;
        return r;
    }
    if (!backup.empty()) { std::error_code ec; fs::remove(backup, ec); }
    DWORD err = 0;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        // ReplaceFileW（属性・ACL を引き継ぐ。実測で MoveFileExW(REPLACE_EXISTING) より速い: 4.9MB で 3.5ms 対 6.5ms）。
        // backup があれば、元の版はそこへ移る（世代つきバックアップが .dx12old を「コピーせず移動」する）。
        if (ReplaceFileW(dst.c_str(), tmp.c_str(), backup.empty() ? nullptr : backup.c_str(),
                         REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
        {
            r.ok = true;
            return r;
        }
        err = GetLastError();
        if (!TransientErr(err)) break;
        Sleep(30);   // 検索インデックス・ウイルス対策が一瞬握っていることがある
    }
    if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
    {
        // 読み取り専用・他で開いている: 置き換えない（元のファイルは無傷）
        r.error = "ファイルを置き換えられません（読み取り専用か、他のプログラムが使用中）: " + Utf8FromPath(dst) + " " + Win32Msg(err);
        return r;
    }
    // ReplaceFileW が使えない場所（ネットワーク共有など）: 退避 → 移動の 2 段で置き換える。
    if (!backup.empty())
    {
        if (!MoveFileExW(dst.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            r.error = "ファイルを置き換えられません: " + Utf8FromPath(dst) + " " + Win32Msg(GetLastError());
            return r;
        }
        if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD e2 = GetLastError();
            MoveFileExW(backup.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);   // 元へ戻す
            r.error = "ファイルを置き換えられません: " + Utf8FromPath(dst) + " " + Win32Msg(e2);
            return r;
        }
        r.ok = true;
        return r;
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        r.error = "ファイルを置き換えられません: " + Utf8FromPath(dst) + " " + Win32Msg(GetLastError());
        return r;
    }
    r.ok = true;
    return r;
}

Result ReplaceOne(const fs::path& dst, const fs::path& tmp, const fs::path& backup, bool dstExists, int index)
{
    const double t0 = NowMs();
    Result r = ReplaceOneImpl(dst, tmp, backup, dstExists, index);
    StatAdd(g_stats.replaceMs, NowMs() - t0);
    return r;
}

fs::path OldPathFor(const fs::path& dst)
{
    return fs::path(dst).concat(".dx12old");
}

} // namespace

std::filesystem::path PathFromUtf8(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string Utf8FromPath(const std::filesystem::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

void SetFaultForTest(Fault f, int64_t n)
{
    g_faultN.store(n);
    g_fault.store(static_cast<int>(f));
}

Result WriteFile(const fs::path& dst, std::string_view bytes, const Verifier& verify)
{
    Result r;
    std::error_code ec;
    if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = MakeTmpPath(dst);
    r = Stage(tmp, bytes, verify);
    if (!r.ok) return r;
    const bool exists = fs::exists(dst, ec);
    r = ReplaceOne(dst, tmp, {}, exists, 0);
    if (!r.ok) { fs::remove(tmp, ec); return r; }
    return r;
}

// ============================================================================
//  コピー・移動（取り込み・複製・移動。途中で失敗したら元のまま）
// ============================================================================
Result CopyFileAtomic(const fs::path& src, const fs::path& dst, bool overwrite)
{
    Result r;
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) { r.error = "コピー元がありません: " + Utf8FromPath(src); return r; }
    if (!overwrite && fs::exists(dst, ec)) { r.error = "コピー先が既にあります: " + Utf8FromPath(dst); return r; }
    if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = MakeTmpPath(dst);
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE))
    {
        r.error = "コピーできません: " + Utf8FromPath(src) + " " + Win32Msg(GetLastError());
        fs::remove(tmp, ec);
        return r;
    }
    {   // ディスクへ書き出し、大きさを確かめる
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = h != INVALID_HANDLE_VALUE;
        LARGE_INTEGER sz{};
        if (ok) ok = FlushFileBuffers(h) && GetFileSizeEx(h, &sz) && static_cast<uint64_t>(sz.QuadPart) == fs::file_size(src, ec);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) { fs::remove(tmp, ec); r.error = "コピーした内容を確かめられません: " + Utf8FromPath(dst); return r; }
    }
    if (FaultIs(Fault::ReplaceFails) && g_faultN.load() == 0) { fs::remove(tmp, ec); r.error = "ファイルを置き換えられません（故障注入）"; return r; }
    r = ReplaceOne(dst, tmp, {}, fs::exists(dst, ec), 0);
    if (!r.ok) fs::remove(tmp, ec);
    return r;
}

Result CopyTree(const fs::path& src, const fs::path& dst, bool overwrite)
{
    Result r;
    std::error_code ec;
    if (fs::is_regular_file(src, ec)) return CopyFileAtomic(src, dst, overwrite);
    if (!fs::is_directory(src, ec)) { r.error = "コピー元がありません: " + Utf8FromPath(src); return r; }
    if (fs::exists(dst, ec) && !overwrite) { r.error = "コピー先が既にあります: " + Utf8FromPath(dst); return r; }
    if (!fs::exists(dst, ec))
    {
        // 新しいフォルダ: 一時フォルダへ全部コピーしてから改名する（途中で失敗したら何も現れない）
        const fs::path tmp = MakeTmpPath(dst);
        fs::create_directories(tmp, ec);
        for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path rel = fs::relative(it->path(), src, ec);
            if (it->is_directory(ec)) { fs::create_directories(tmp / rel, ec); continue; }
            if (!it->is_regular_file(ec)) continue;
            const Result cr = CopyFileAtomic(it->path(), tmp / rel, true);
            if (!cr.ok) { fs::remove_all(tmp, ec); return cr; }
        }
        if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
        if (!MoveFileExW(tmp.c_str(), dst.c_str(), 0))
        {
            r.error = "フォルダを置けません: " + Utf8FromPath(dst) + " " + Win32Msg(GetLastError());
            fs::remove_all(tmp, ec);
            return r;
        }
        r.ok = true;
        return r;
    }
    // 既存のフォルダへ重ねる: ファイルごとに原子的（置き換えるファイルは元か新しい中身のどちらか）
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::path rel = fs::relative(it->path(), src, ec);
        if (it->is_directory(ec)) { fs::create_directories(dst / rel, ec); continue; }
        if (!it->is_regular_file(ec)) continue;
        const Result cr = CopyFileAtomic(it->path(), dst / rel, true);
        if (!cr.ok) return cr;
    }
    r.ok = true;
    return r;
}

Result MovePath(const fs::path& src, const fs::path& dst)
{
    Result r;
    std::error_code ec;
    if (!fs::exists(src, ec)) { r.error = "移動元がありません: " + Utf8FromPath(src); return r; }
    if (fs::exists(dst, ec)) { r.error = "移動先が既にあります: " + Utf8FromPath(dst); return r; }
    if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
    if (MoveFileExW(src.c_str(), dst.c_str(), 0)) { r.ok = true; return r; }
    const DWORD err = GetLastError();
    if (err != ERROR_NOT_SAME_DEVICE) { r.error = "移動できません: " + Utf8FromPath(src) + " " + Win32Msg(err); return r; }
    // 別のボリューム: 先にコピーを完成させ、成功してから元を消す（途中で失敗したら元のまま）
    r = CopyTree(src, dst, false);
    if (!r.ok) return r;
    fs::remove_all(src, ec);
    return r;
}

// ============================================================================
Batch::Batch(std::filesystem::path txnFile) : m_txn(std::move(txnFile)) {}

Batch::~Batch()
{
    if (!m_committed) DiscardTmp();
}

void Batch::ReleaseRetained()
{
    std::error_code ec;
    for (const Retained& r : m_retained) fs::remove(r.old, ec);
    m_retained.clear();
}

void Batch::DiscardTmp()
{
    std::error_code ec;
    for (Item& it : m_items)
        if (!it.replaced) fs::remove(it.tmp, ec);
}

bool Batch::Add(const std::filesystem::path& dst, std::string_view bytes, const Verifier& verify)
{
    if (!m_error.empty()) return false;
    std::error_code ec;
    if (!dst.parent_path().empty()) fs::create_directories(dst.parent_path(), ec);
    Item it;
    it.dst = dst;
    it.tmp = MakeTmpPath(dst);
    it.old = OldPathFor(dst);
    const Result r = Stage(it.tmp, bytes, verify);
    if (!r.ok)
    {
        m_error = r.error;
        DiscardTmp();
        m_items.clear();
        return false;
    }
    m_items.push_back(std::move(it));
    return true;
}

bool Batch::AddMany(const std::vector<Entry>& entries, unsigned workers, bool skipIdentical, std::vector<char>* written)
{
    if (written) written->assign(entries.size(), 0);
    if (!m_error.empty()) return false;
    if (entries.empty()) return true;
    std::error_code ec;
    // 親フォルダは先に作る（スレッドの中で作ると競合する）
    {
        std::vector<fs::path> dirs;
        for (const Entry& e : entries)
            if (!e.dst.parent_path().empty() && (dirs.empty() || dirs.back() != e.dst.parent_path())) dirs.push_back(e.dst.parent_path());
        for (const fs::path& d : dirs) fs::create_directories(d, ec);
    }

    struct Slot { Item item; Result res; bool wrote = false; };
    std::vector<Slot> slots(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
    {
        slots[i].item.dst = entries[i].dst;
        slots[i].item.tmp = MakeTmpPath(entries[i].dst);
        slots[i].item.old = OldPathFor(entries[i].dst);
        slots[i].res.ok = true;
    }

    auto work = [&](size_t i) {
        Slot& sl = slots[i];
        const Entry& e = entries[i];
        std::error_code e2;
        if (skipIdentical && fs::is_regular_file(e.dst, e2))
        {
            const auto sz = fs::file_size(e.dst, e2);
            if (!e2 && sz == e.bytes.size())
            {
                std::ifstream in(e.dst, std::ios::binary);
                std::string cur(e.bytes.size(), '\0');
                in.read(cur.data(), static_cast<std::streamsize>(cur.size()));
                if (in.gcount() == static_cast<std::streamsize>(cur.size()) && std::string_view(cur) == e.bytes) return;   // 同じ中身: 触らない
            }
        }
        sl.res = Stage(sl.item.tmp, e.bytes, e.verify);
        sl.wrote = sl.res.ok;
    };
    workers = (std::max)(1u, (std::min)(workers, static_cast<unsigned>(entries.size())));
    if (workers <= 1)
    {
        for (size_t i = 0; i < entries.size(); ++i) { work(i); if (!slots[i].res.ok) break; }
    }
    else
    {
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        std::vector<std::thread> pool;
        for (unsigned w = 0; w < workers; ++w)
            pool.emplace_back([&] {
                for (;;)
                {
                    const size_t i = next.fetch_add(1);
                    if (i >= entries.size() || failed.load()) return;
                    work(i);
                    if (!slots[i].res.ok) failed.store(true);
                }
            });
        for (std::thread& t : pool) t.join();
    }

    for (Slot& sl : slots)
        if (!sl.res.ok && m_error.empty()) m_error = sl.res.error;
    if (!m_error.empty())
    {
        for (Slot& sl : slots) if (sl.wrote) fs::remove(sl.item.tmp, ec);
        DiscardTmp();
        m_items.clear();
        return false;
    }
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].wrote)
        {
            m_items.push_back(std::move(slots[i].item));
            if (written) (*written)[i] = 1;
        }
    return true;
}

Result Batch::Commit()
{
    Result r;
    if (!m_error.empty()) { r.error = m_error; return r; }
    if (m_items.empty()) { m_committed = true; r.ok = true; return r; }
    std::error_code ec;

    for (Item& it : m_items) it.hadOld = fs::exists(it.dst, ec);

    // 1) コミット記録（これが見えている間は、次回起動でも最後まで置き換える）。
    //    1 ファイルだけの置き換えは ReplaceFileW 1 回で原子的なので記録は要らない（25MB のシーンでも待ち時間を増やさない）。
    //    記録は一時ファイル経由にせず直接書く（置き換えの往復を省く）。途中で切れた記録は末尾の END 行が無いので RecoverPending が
    //    無効として捨てる（この時点ではまだ何も置き換えていない）。
    if (m_items.size() > 1)
    {
        std::string txt = "DX12TXN1\n";
        for (const Item& it : m_items)
            txt += Utf8FromPath(it.dst) + "\t" + Utf8FromPath(it.tmp) + "\t" + Utf8FromPath(it.old) + "\t" + (it.hadOld ? "1" : "0") + "\n";
        txt += "END\n";
        bool jok = false;
        HANDLE jh = CreateFileW(m_txn.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (jh != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            jok = ::WriteFile(jh, txt.data(), static_cast<DWORD>(txt.size()), &wrote, nullptr) && wrote == txt.size()
                  && FlushFileBuffers(jh);
            CloseHandle(jh);
        }
        if (!jok)
        {
            fs::remove(m_txn, ec);
            DiscardTmp();
            m_error = "保存の記録を書けません: " + Utf8FromPath(m_txn);
            r.error = m_error;
            return r;
        }
        if (FaultIs(Fault::CrashAfterJournal)) { m_committed = true; r.ok = true; return r; }
        CommitDelay();
    }

    // 2) 順番に置き換える
    int done = 0;
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        Item& it = m_items[i];
        const Result rr = ReplaceOne(it.dst, it.tmp, it.hadOld ? it.old : fs::path{}, it.hadOld, static_cast<int>(i));
        if (!rr.ok)
        {
            // 元へ戻す（食い違いを残さない）
            for (size_t j = 0; j < i; ++j)
            {
                Item& b = m_items[j];
                if (b.hadOld) MoveFileExW(b.old.c_str(), b.dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                else DeleteFileW(b.dst.c_str());
                b.replaced = false;
            }
            DiscardTmp();
            for (size_t j = 0; j < m_items.size(); ++j) fs::remove(m_items[j].old, ec);
            fs::remove(m_txn, ec);
            m_error = rr.error;
            r.error = rr.error;
            m_committed = true;   // 後始末は済んだ
            return r;
        }
        it.replaced = true;
        ++done;
        CommitDelay();
        if (FaultIs(Fault::CrashAfterReplaces) && g_faultN.load() == done)
        {
            m_committed = true;
            r.ok = true;
            return r;
        }
    }

    // 3) 後始末
    for (const Item& it : m_items)
    {
        if (!it.hadOld) continue;
        if (m_retainOld) m_retained.push_back({it.dst, it.old});
        else fs::remove(it.old, ec);
    }
    fs::remove(m_txn, ec);
    m_committed = true;
    r.ok = true;
    return r;
}

// ============================================================================
bool RecoverPending(const std::filesystem::path& txnFile)
{
    std::error_code ec;
    if (!fs::exists(txnFile, ec)) return false;
    struct Row { fs::path dst, tmp, old; };
    std::vector<Row> rows;
    bool ended = false;
    {
        std::ifstream f(txnFile, std::ios::binary);
        if (!f) return false;
        std::string line;
        std::getline(f, line);
        if (line != "DX12TXN1")
        {
            f.close();
            fs::remove(txnFile, ec);
            return false;
        }
        while (std::getline(f, line))
        {
            if (line.empty()) continue;
            if (line == "END") { ended = true; break; }
            std::vector<std::string> cols;
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, '\t')) cols.push_back(c);
            if (cols.size() < 4) continue;
            rows.push_back({PathFromUtf8(cols[0]), PathFromUtf8(cols[1]), PathFromUtf8(cols[2])});
        }
    }
    if (!ended)
    {
        // 記録を書いている途中で止まった（この時点ではまだ何も置き換えていない）。記録を捨て、書きかけの一時ファイルを消す。
        for (const Row& r : rows) fs::remove(r.tmp, ec);
        fs::remove(txnFile, ec);
        return false;
    }
    // 記録は完全: 最後まで置き換える（ロールフォワード）。
    bool did = false;
    for (const Row& r : rows)
    {
        if (fs::exists(r.tmp, ec))
        {
            if (MoveFileExW(r.tmp.c_str(), r.dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) did = true;
        }
        else if (!fs::exists(r.dst, ec) && fs::exists(r.old, ec))
        {
            // 2 段置き換えの途中で止まった（dst を退避した直後）: 元へ戻す
            MoveFileExW(r.old.c_str(), r.dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            did = true;
        }
    }
    for (const Row& r : rows) fs::remove(r.old, ec);
    fs::remove(txnFile, ec);
    return did;
}

Stats TakeStats()
{
    std::lock_guard<std::mutex> lk(g_statsMu);
    const Stats s = g_stats;
    g_stats = Stats{};
    return s;
}

int SweepStaleOld(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    std::vector<fs::path> olds;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".dx12old") olds.push_back(it->path());
    int n = 0;
    for (const fs::path& p : olds) if (fs::remove(p, ec)) ++n;
    return n;
}

int SweepStaleTmp(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    int n = 0;
    const DWORD me = GetCurrentProcessId();
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::path p = it->path();
        if (p.extension() != ".dx12tmp") continue;
        // <名前>.<pid>.<連番>.dx12tmp の pid が、動いている別プロセスなら触らない
        const std::string stem = p.stem().string();            // <名前>.<pid>.<連番>
        const size_t d2 = stem.rfind('.');
        const size_t d1 = d2 == std::string::npos ? std::string::npos : stem.rfind('.', d2 - 1);
        if (d1 != std::string::npos && d2 > d1 + 1)
        {
            const DWORD pid = static_cast<DWORD>(std::strtoul(stem.substr(d1 + 1, d2 - d1 - 1).c_str(), nullptr, 10));
            if (pid == me) continue;   // 自分の保存の途中かもしれない
            if (pid != 0)
            {
                HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
                if (h)
                {
                    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
                    CloseHandle(h);
                    if (alive) continue;
                }
            }
        }
        if (it->is_directory(ec) ? fs::remove_all(p, ec) > 0 : fs::remove(p, ec)) ++n;
    }
    return n;
}

} // namespace dx12e::atomicfile
