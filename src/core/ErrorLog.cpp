#include "core/ErrorLog.h"
#include "core/LogRing.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace dx12e
{
namespace
{

std::string Lower(const std::string& s)
{
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) o.push_back(static_cast<char>(std::tolower(c)));
    return o;
}

bool Has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// ".png" のような拡張子が「語の終わり」で現れるか（".pngx" や ".hfoo" を拾わない）。
bool HasExt(const std::string& lower, const char* ext)
{
    const size_t n = std::char_traits<char>::length(ext);
    size_t pos = 0;
    while ((pos = lower.find(ext, pos)) != std::string::npos)
    {
        const size_t end = pos + n;
        if (end >= lower.size()) return true;
        const unsigned char c = static_cast<unsigned char>(lower[end]);
        if (!(std::isalnum(c) || c == '_')) return true;
        pos = end;
    }
    return false;
}

bool IsHex(unsigned char c) { return std::isxdigit(c) != 0; }

} // namespace

std::string ClassifyLogCategory(int /*level*/, const std::string& text)
{
    if (Has(text, "[D3D12")) return "d3d12";
    const std::string lo = Lower(text);
    if (Has(text, "Luaエラー") || Has(text, "[Lua]") || Has(text, "LuaScript") || Has(lo, "lua error") || HasExt(lo, ".lua"))
        return "lua";
    if (Has(text, "シェーダ") || Has(lo, "shader") || HasExt(lo, ".hlsl") || HasExt(lo, ".cso") || Has(text, "コンパイル"))
        return "shader";
    if (Has(text, "読み込み") || Has(text, "ロード") || Has(lo, "load") || Has(text, "見つかりません") || Has(text, "開けません"))
        return "asset";
    static const char* const kAssetExt[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr", ".glb", ".gltf",
                                            ".fbx", ".obj", ".hf", ".smsh", ".vgeo", ".wav", ".ogg", ".mp3", ".flac"};
    for (const char* e : kAssetExt) if (HasExt(lo, e)) return "asset";
    if (Has(lo, "jolt")) return "physics";
    return "engine";
}

void SplitLuaStack(const std::string& full, std::string& text, std::string& stack)
{
    static const char kMark[] = "stack traceback:";
    const size_t p = full.find(kMark);
    if (p == std::string::npos) { text = full; stack.clear(); return; }
    text = full.substr(0, p);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
    stack = full.substr(p);
}

std::string NormalizeErrorMessage(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    bool lastSpace = false;
    for (size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '0' && i + 2 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X') && IsHex(static_cast<unsigned char>(text[i + 2])))
        {
            i += 2;
            while (i < text.size() && IsHex(static_cast<unsigned char>(text[i]))) ++i;
            out += "0x?";
            lastSpace = false;
            continue;
        }
        if (std::isdigit(c))
        {
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
            out.push_back('#');
            lastSpace = false;
            continue;
        }
        if (c == '\\') { out.push_back('/'); ++i; lastSpace = false; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            if (!lastSpace) out.push_back(' ');
            lastSpace = true;
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
        lastSpace = false;
        ++i;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    constexpr size_t kMax = 300;
    if (out.size() > kMax)
    {
        size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;   // UTF-8 の途中で切らない
        out.resize(cut);
    }
    return out;
}

bool ExtractScriptLocation(const std::string& text, std::string& file, int& line)
{
    size_t pos = 0;
    while ((pos = text.find(".lua:", pos)) != std::string::npos)
    {
        size_t d = pos + 5;
        size_t e = d;
        while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e]))) ++e;
        if (e > d)
        {
            size_t b = pos;
            auto pathChar = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '/' || c == '\\' || c == '-' || c >= 0x80; };
            while (b > 0 && pathChar(static_cast<unsigned char>(text[b - 1]))) --b;
            file = text.substr(b, pos + 4 - b);
            std::replace(file.begin(), file.end(), '\\', '/');
            line = std::atoi(text.substr(d, e - d).c_str());
            return true;
        }
        pos += 5;
    }
    return false;
}

std::string ErrorFingerprint(const std::string& category, const std::string& normalized, const std::string& scriptFile, int scriptLine)
{
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    auto mix = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= '|'; h *= 1099511628211ull; };
    mix(category);
    mix(normalized);
    mix(scriptFile + ":" + std::to_string(scriptLine));
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf, 12);
}

// ---------------------------------------------------------------------------

ErrorLog::ErrorLog(size_t maxGroups) : m_maxGroups(std::max<size_t>(1, maxGroups)), m_startedAt(NowEpochMs())
{
}

ErrorLog& ErrorLog::Global()
{
    static ErrorLog g;
    return g;
}

bool ErrorLog::IsAggregated(const LogEntry& e)
{
    if (e.level >= 4) return true;
    if (e.category == "d3d12" && e.level >= 3) return true;
    if (e.category == "mcp") return true;
    return false;
}

void ErrorLog::Observe(const LogEntry& e)
{
    if (!IsAggregated(e)) return;
    std::string file; int line = 0;
    const bool hasScript = ExtractScriptLocation(e.text, file, line);
    const std::string norm = NormalizeErrorMessage(e.text);
    const std::string fp = ErrorFingerprint(e.category, norm, hasScript ? file : std::string(), hasScript ? line : 0);

    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_groups.find(fp);
    if (it == m_groups.end())
    {
        if (m_groups.size() >= m_maxGroups)
        {
            auto oldest = m_groups.begin();
            for (auto i = m_groups.begin(); i != m_groups.end(); ++i)
                if (i->second.lastSeq < oldest->second.lastSeq) oldest = i;
            m_groups.erase(oldest);
            ++m_evicted;
        }
        ErrorGroup g;
        g.fingerprint = fp;
        g.category = e.category;
        g.message = norm;
        g.sample = e.text.size() > 600 ? e.text.substr(0, 600) : e.text;
        g.firstSeq = e.id; g.firstAt = e.epochMs; g.firstFrame = e.frame;
        g.hasScript = hasScript; g.scriptFile = file; g.scriptLine = line;
        it = m_groups.emplace(fp, std::move(g)).first;
    }
    ErrorGroup& g = it->second;
    ++g.count;
    g.level = std::max(g.level, e.level);
    g.lastSeq = e.id; g.lastAt = e.epochMs; g.lastFrame = e.frame;
    if (e.playing) ++g.playCount;
    if (!e.stack.empty()) g.stack = e.stack.size() > 2000 ? e.stack.substr(0, 2000) : e.stack;
    g.samples.push_back(e.text.size() > 600 ? e.text.substr(0, 600) : e.text);
    while (g.samples.size() > kMaxSamples) g.samples.pop_front();
    ++m_total;
    ++m_byCategory[e.category];
}

ErrorQueryResult ErrorLog::Query(const ErrorQuery& q) const
{
    ErrorQueryResult r;
    std::lock_guard<std::mutex> lk(m_mutex);
    r.totalGroups = m_groups.size();
    r.totalOccurrences = m_total;
    r.byCategory = m_byCategory;
    r.evicted = m_evicted;
    r.sessionId = m_session;
    r.clearedAtMs = m_clearedAt;
    r.startedAtMs = m_startedAt;

    std::vector<const ErrorGroup*> hit;
    for (const auto& kv : m_groups)
    {
        const ErrorGroup& g = kv.second;
        if (g.lastSeq <= q.sinceSeq) continue;
        if (!q.categories.empty() && std::find(q.categories.begin(), q.categories.end(), g.category) == q.categories.end()) continue;
        if (g.count < q.minCount) continue;
        hit.push_back(&g);
    }
    r.matchedGroups = hit.size();
    r.hasNew = !hit.empty();
    std::sort(hit.begin(), hit.end(), [&](const ErrorGroup* a, const ErrorGroup* b)
    {
        if (q.sortByCount && a->count != b->count) return a->count > b->count;
        return a->lastSeq > b->lastSeq;
    });
    const size_t n = std::min(hit.size(), std::max<size_t>(1, q.limit));
    for (size_t i = 0; i < n; ++i)
    {
        ErrorGroup g = *hit[i];
        const size_t keep = std::min(q.samples, g.samples.size());
        while (g.samples.size() > keep) g.samples.pop_front();
        r.groups.push_back(std::move(g));
    }
    return r;
}

size_t ErrorLog::Clear(int64_t nowMs)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const size_t n = m_groups.size();
    m_groups.clear();
    m_byCategory.clear();
    m_total = 0;
    m_evicted = 0;
    ++m_session;
    m_clearedAt = nowMs;
    m_startedAt = nowMs;
    return n;
}

uint64_t ErrorLog::SessionId() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_session;
}

} // namespace dx12e
