#include "core/LogRing.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <regex>

namespace dx12e
{

int64_t NowEpochMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string FormatHms(int64_t epochMs)
{
    const std::time_t t = static_cast<std::time_t>(epochMs / 1000);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char tb[16];
    std::snprintf(tb, sizeof(tb), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return tb;
}

int ParseLogLevelName(const std::string& name)
{
    std::string n;
    for (unsigned char c : name) n.push_back(static_cast<char>(std::tolower(c)));
    if (n == "trace") return 0;
    if (n == "debug") return 1;
    if (n == "info") return 2;
    if (n == "warn" || n == "warning") return 3;
    if (n == "error" || n == "err") return 4;
    if (n == "critical" || n == "fatal") return 5;
    return -1;
}

const char* LogLevelName(int level)
{
    switch (level)
    {
    case 0: return "trace";
    case 1: return "debug";
    case 2: return "info";
    case 3: return "warn";
    case 4: return "error";
    case 5: return "critical";
    default: return "info";
    }
}

LogRing::LogRing(size_t capacity)
    : m_cap(capacity < 1 ? 1 : capacity), m_sessionId(static_cast<uint64_t>(NowEpochMs()))
{
}

LogEntry LogRing::Push(LogEntry e)
{
    if (e.epochMs == 0) e.epochMs = NowEpochMs();
    if (e.time.empty()) e.time = FormatHms(e.epochMs);
    std::lock_guard<std::mutex> lk(m_mutex);
    e.id = m_nextId++;
    m_buf.push_back(e);
    if (m_buf.size() > m_cap) m_buf.pop_front();
    return e;
}

uint64_t LogRing::ReadLegacy(uint64_t since, std::vector<LogEntry>& out) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    uint64_t latest = since;
    for (const auto& e : m_buf)
    {
        if (e.id <= since) continue;
        latest = e.id;
        if (e.aux) continue;
        out.push_back(e);
    }
    return latest;
}

uint64_t LogRing::LatestSeq() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_nextId - 1;
}

size_t LogRing::Size() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_buf.size();
}

LogQueryResult LogRing::Query(const LogFilter& f) const
{
    LogQueryResult r;
    r.capacity = m_cap;

    std::regex re;
    const bool useGrep = !f.grep.empty();
    if (useGrep)
    {
        try { re = std::regex(f.grep, std::regex::ECMAScript | std::regex::icase); }
        catch (const std::regex_error& e)
        {
            r.error = e.what();
            return r;
        }
    }

    const size_t limit = std::max<size_t>(1, f.limit);
    auto matches = [&](const LogEntry& e) -> bool
    {
        if (f.levelMask != 0 && (e.level < 0 || e.level > 31 || (f.levelMask & (1u << e.level)) == 0)) return false;
        if (f.minLevel >= 0 && e.level < f.minLevel) return false;
        const bool catNamed = !f.categories.empty() &&
            std::find(f.categories.begin(), f.categories.end(), e.category) != f.categories.end();
        if (!f.categories.empty() && !catNamed) return false;
        if (e.aux && !f.includeAux && !catNamed) return false;
        if (useGrep && !std::regex_search(e.text, re)) return false;
        return true;
    };

    std::lock_guard<std::mutex> lk(m_mutex);
    r.latestSeq = m_nextId - 1;
    r.oldestSeq = m_buf.empty() ? m_nextId : m_buf.front().id;
    r.dropped = (r.oldestSeq > f.sinceSeq + 1) ? (r.oldestSeq - 1 - f.sinceSeq) : 0;
    if (r.latestSeq <= f.sinceSeq) r.dropped = 0;

    if (f.tail)
    {
        std::vector<const LogEntry*> hit;
        for (const auto& e : m_buf)
        {
            if (e.id <= f.sinceSeq) continue;
            if (matches(e)) hit.push_back(&e); else ++r.filteredOut;
        }
        const size_t from = hit.size() > limit ? hit.size() - limit : 0;
        r.omitted = from;
        for (size_t i = from; i < hit.size(); ++i) r.entries.push_back(*hit[i]);
        r.nextCursor = r.latestSeq;
        r.hasMore = false;
        return r;
    }

    for (const auto& e : m_buf)
    {
        if (e.id <= f.sinceSeq) continue;
        if (!matches(e)) { ++r.filteredOut; continue; }
        if (r.entries.size() < limit) { r.entries.push_back(e); continue; }
        r.hasMore = true;   // limit 件を返した後にも一致があった
        break;
    }
    r.nextCursor = r.hasMore ? r.entries.back().id : std::max<uint64_t>(r.latestSeq, f.sinceSeq);
    return r;
}

} // namespace dx12e
