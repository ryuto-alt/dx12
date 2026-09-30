#include "core/CrashReport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>

namespace dx12e
{
namespace fs = std::filesystem;
namespace
{

std::string g_lastCaptured;

std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> SplitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) { if (cur.back() == '\r') cur.pop_back(); out.push_back(cur); }
    return out;
}

std::string TrimTrailingBlank(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string JsonEsc(const std::string& s)
{
    std::string o;
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// crash.log(CrashHandler が書く固定書式)を読む。
CrashInfo ParseCrashLog(const std::string& text)
{
    CrashInfo c;
    enum class Sec { Head, Crumbs, Stack, Done } sec = Sec::Head;
    std::string crumbs, stack;
    for (const std::string& ln : SplitLines(text))
    {
        if (StartsWith(ln, "--- 直前の操作")) { sec = Sec::Crumbs; continue; }
        if (StartsWith(ln, "--- スタックトレース")) { sec = Sec::Stack; continue; }
        if (StartsWith(ln, "ミニダンプ")) { sec = Sec::Done; continue; }
        switch (sec)
        {
        case Sec::Head:
            if      (StartsWith(ln, "time    : ")) c.at = ln.substr(10);
            else if (StartsWith(ln, "version : ")) c.engineVersion = ln.substr(10);
            else if (StartsWith(ln, "exe     : ")) c.exe = ln.substr(10);
            else if (StartsWith(ln, "thread  : ")) c.thread = ln.substr(10);
            else if (StartsWith(ln, "except  : ")) c.exception = ln.substr(10);
            else if (StartsWith(ln, "address : ")) c.address = ln.substr(10);
            else if (StartsWith(ln, "detail  : ")) c.detail = ln.substr(10);
            break;
        case Sec::Crumbs: crumbs += ln + "\n"; break;
        case Sec::Stack:  stack += ln + "\n"; break;
        case Sec::Done: break;
        }
    }
    c.breadcrumbs = TrimTrailingBlank(crumbs);
    c.stack = TrimTrailingBlank(stack);
    return c;
}

// "YYYY-MM-DD HH:MM:SS" -> "YYYYMMDD-HHMMSS"。読めなければ現在時刻。
std::string StampFrom(const std::string& at)
{
    int y, mo, d, h, mi, s;
    if (std::sscanf(at.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%04d%02d%02d-%02d%02d%02d", y, mo, d, h, mi, s);
        return b;
    }
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char b[32];
    std::snprintf(b, sizeof(b), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

bool MoveFile(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(from, ec);
    return true;
}

std::vector<std::string> TailLines(const fs::path& p, size_t n)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    const std::streamoff want = std::min<std::streamoff>(size, 2 * 1024 * 1024);
    f.seekg(size - want);
    std::string buf(static_cast<size_t>(want), '\0');
    f.read(buf.data(), want);
    std::vector<std::string> lines = SplitLines(buf);
    if (want < size && !lines.empty()) lines.erase(lines.begin());   // 先頭は途中で切れている
    if (lines.size() > n) lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(n));
    return lines;
}

} // namespace

fs::path CrashReport::DefaultRoot()
{
    if (const char* d = std::getenv("DX12E_DATA_DIR"); d && *d) return fs::path(d) / "crash";
    if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) return fs::path(l) / "UnoEngine" / "crash";
    return fs::temp_directory_path() / "UnoEngine" / "crash";
}

std::string CrashReport::LastCapturedId() { return g_lastCaptured; }

bool CrashReport::CaptureIfPresent(const fs::path& cwd, const fs::path& destRoot, std::string* capturedId, size_t tailLines)
{
    try
    {
        const fs::path log = cwd / "dx12_crash.log";
        std::error_code ec;
        if (!fs::exists(log, ec)) return false;

        const std::string text = ReadAll(log);
        CrashInfo info = ParseCrashLog(text);
        const std::string base = StampFrom(info.at);
        fs::create_directories(destRoot, ec);
        std::string id = base;
        for (int i = 2; fs::exists(destRoot / id, ec); ++i) id = base + "-" + std::to_string(i);
        const fs::path dir = destRoot / id;
        fs::create_directories(dir, ec);

        // 直前ログ（このあと Logger が truncate するので、いま保存する）
        const std::vector<std::string> tail = TailLines(cwd / "dx12_engine.log", tailLines);
        {
            std::ofstream f(dir / "log_tail.txt", std::ios::binary);
            for (const auto& l : tail) f << l << "\n";
        }
        bool hasDmp = false;
        const fs::path dmp = cwd / "dx12_crash.dmp";
        if (fs::exists(dmp, ec)) hasDmp = MoveFile(dmp, dir / "dx12_crash.dmp");
        MoveFile(log, dir / "crash.log");

        {
            std::ofstream f(dir / "report.json", std::ios::binary);
            f << "{\n"
              << "  \"id\": \"" << JsonEsc(id) << "\",\n"
              << "  \"at\": \"" << JsonEsc(info.at) << "\",\n"
              << "  \"exception\": \"" << JsonEsc(info.exception) << "\",\n"
              << "  \"address\": \"" << JsonEsc(info.address) << "\",\n"
              << "  \"thread\": \"" << JsonEsc(info.thread) << "\",\n"
              << "  \"engineVersion\": \"" << JsonEsc(info.engineVersion) << "\",\n"
              << "  \"exe\": \"" << JsonEsc(info.exe) << "\",\n"
              << "  \"detail\": \"" << JsonEsc(info.detail) << "\",\n"
              << "  \"dmp\": " << (hasDmp ? "\"dx12_crash.dmp\"" : "null") << ",\n"
              << "  \"stack\": \"" << JsonEsc(info.stack) << "\",\n"
              << "  \"breadcrumbs\": \"" << JsonEsc(info.breadcrumbs) << "\",\n"
              << "  \"logTailLines\": " << tail.size() << "\n"
              << "}\n";
        }

        // 古い報告を 20 件までに（名前 = 日時なので辞書順が古い順）
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(destRoot, ec)) if (e.is_directory()) dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end());
        while (dirs.size() > 20) { fs::remove_all(dirs.front(), ec); dirs.erase(dirs.begin()); }

        g_lastCaptured = id;
        if (capturedId) *capturedId = id;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::vector<CrashInfo> CrashReport::LoadAll(const fs::path& destRoot, bool includeLog, size_t maxCount)
{
    std::vector<CrashInfo> out;
    try
    {
        std::error_code ec;
        if (!fs::exists(destRoot, ec)) return out;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(destRoot, ec)) if (e.is_directory()) dirs.push_back(e.path());
        std::sort(dirs.begin(), dirs.end(), std::greater<>());   // 新しい順
        for (const auto& d : dirs)
        {
            if (out.size() >= maxCount) break;
            const fs::path cl = d / "crash.log";
            if (!fs::exists(cl, ec)) continue;
            CrashInfo c = ParseCrashLog(ReadAll(cl));
            c.id = d.filename().string();
            c.dir = d.string();
            if (fs::exists(d / "dx12_crash.dmp", ec)) c.dmp = (d / "dx12_crash.dmp").string();
            c.acknowledged = fs::exists(d / "ack", ec);
            c.newThisLaunch = (c.id == g_lastCaptured);
            if (includeLog) c.logTail = TailLines(d / "log_tail.txt", 200);
            out.push_back(std::move(c));
        }
    }
    catch (...) {}
    return out;
}

size_t CrashReport::AcknowledgeAll(const fs::path& destRoot)
{
    size_t n = 0;
    try
    {
        std::error_code ec;
        if (!fs::exists(destRoot, ec)) return 0;
        for (const auto& e : fs::directory_iterator(destRoot, ec))
        {
            if (!e.is_directory()) continue;
            const fs::path ack = e.path() / "ack";
            if (fs::exists(ack, ec) || !fs::exists(e.path() / "crash.log", ec)) continue;
            std::ofstream f(ack, std::ios::binary);
            f << "acknowledged\n";
            ++n;
        }
    }
    catch (...) {}
    return n;
}

} // namespace dx12e
