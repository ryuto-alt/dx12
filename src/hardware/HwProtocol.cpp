#include "hardware/HwProtocol.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace dx12e::hw
{

namespace
{
bool IsSpace(char c) { return c == ' ' || c == '\t'; }

std::vector<std::string_view> Split(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && IsSpace(s[i])) ++i;
        const size_t b = i;
        while (i < s.size() && !IsSpace(s[i])) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

bool IsNameChar(char c, bool allowUpper)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (allowUpper && c >= 'A' && c <= 'Z');
}

bool ValidName(std::string_view n, size_t maxLen, bool allowUpper)
{
    if (n.empty() || n.size() > maxLen) return false;
    for (char c : n) if (!IsNameChar(c, allowUpper)) return false;
    return true;
}

bool ParseInt(std::string_view s, int& out)
{
    double d = 0;
    if (!HwParseNumber(s, d)) return false;
    if (d != std::floor(d) || std::fabs(d) > 2e9) return false;
    out = static_cast<int>(d);
    return true;
}

std::string_view RestAfterFirstToken(std::string_view line)
{
    size_t i = 0;
    while (i < line.size() && IsSpace(line[i])) ++i;
    while (i < line.size() && !IsSpace(line[i])) ++i;
    while (i < line.size() && IsSpace(line[i])) ++i;
    return line.substr(i);
}
} // namespace

void HwLineAssembler::Feed(const void* data, size_t len, std::vector<std::string>& lines)
{
    const char* p = static_cast<const char*>(data);
    for (size_t i = 0; i < len; ++i)
    {
        const char c = p[i];
        if (c == '\r') continue;
        if (c == '\n')
        {
            if (m_discarding) { m_discarding = false; }
            else if (!m_buf.empty()) { lines.push_back(std::move(m_buf)); }
            m_buf.clear();
            continue;
        }
        if (m_discarding) continue;
        if (m_buf.size() >= kMaxLineBytes)
        {
            // 129 バイト目が来た: この行は捨てる（改行まで読み飛ばす）
            m_buf.clear();
            m_discarding = true;
            ++m_overflow;
            continue;
        }
        m_buf.push_back(c);
    }
}

bool HwParseNumber(std::string_view s, double& out)
{
    if (s.empty()) return false;
    if (s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

HwMessage ParseDeviceLine(std::string_view line)
{
    HwMessage m;
    m.text = std::string(line);
    const auto tok = Split(line);
    if (tok.empty()) return m;

    const std::string_view head = tok[0];

    if (head[0] == '@')
    {
        if (head == "@ready") { m.type = HwMsgType::Ready; m.text.clear(); return m; }
        if (head == "@log" || head == "@err")
        {
            m.type = (head == "@log") ? HwMsgType::Log : HwMsgType::Err;
            m.text = std::string(RestAfterFirstToken(line));
            return m;
        }
        if (head == "@pong")
        {
            int seq = 0;
            if (tok.size() == 2 && ParseInt(tok[1], seq)) { m.type = HwMsgType::Pong; m.seq = seq; m.text.clear(); }
            return m;
        }
        if (head == "@hello")
        {
            bool hasName = false;
            for (size_t i = 1; i < tok.size(); ++i)
            {
                const size_t eq = tok[i].find('=');
                if (eq == std::string_view::npos) return m;   // 不正
                const std::string_view k = tok[i].substr(0, eq), v = tok[i].substr(eq + 1);
                if (k == "name")
                {
                    if (!ValidName(v, 24, false)) return m;
                    m.name = std::string(v); hasName = true;
                }
                else if (k == "proto") { if (!ParseInt(v, m.proto)) return m; }
                else if (k == "fw")    { if (!ParseInt(v, m.fw)) return m; }
                else if (k == "board") { m.board = std::string(v); }
                // 未知のキーは無視（前方互換）
            }
            if (!hasName) { m.name.clear(); return m; }
            m.type = HwMsgType::Hello;
            m.text.clear();
            return m;
        }
        if (head == "@ch")
        {
            if (tok.size() < 4) return m;
            HwChannelDecl d;
            if (!ValidName(tok[1], 32, true)) return m;
            d.name = std::string(tok[1]);
            if (tok[2] == "in") d.isOutput = false; else if (tok[2] == "out") d.isOutput = true; else return m;
            if (tok[3] == "bool") d.type = HwChType::Bool;
            else if (tok[3] == "int") d.type = HwChType::Int;
            else if (tok[3] == "float") d.type = HwChType::Float;
            else return m;
            std::vector<double> nums;
            for (size_t i = 4; i < tok.size(); ++i)
            {
                if (tok[i].substr(0, 5) == "safe=")
                {
                    if (!HwParseNumber(tok[i].substr(5), d.safe)) return m;
                }
                else
                {
                    double v = 0;
                    if (!HwParseNumber(tok[i], v)) return m;
                    nums.push_back(v);
                }
            }
            if (nums.size() == 2) { d.hasRange = true; d.min = nums[0]; d.max = nums[1]; }
            else if (!nums.empty()) return m;
            m.type = HwMsgType::Channel;
            m.channel = std::move(d);
            m.text.clear();
            return m;
        }
        return m;   // 未知の @ 行
    }

    // 値行: 先頭が英字、全トークンが name=number
    if (!((head[0] >= 'a' && head[0] <= 'z') || (head[0] >= 'A' && head[0] <= 'Z'))) return m;
    std::vector<std::pair<std::string, double>> vals;
    for (const auto t : tok)
    {
        const size_t eq = t.find('=');
        if (eq == std::string_view::npos || eq == 0) return m;
        const std::string_view k = t.substr(0, eq);
        double v = 0;
        if (!ValidName(k, 32, true) || !HwParseNumber(t.substr(eq + 1), v)) return m;
        vals.emplace_back(std::string(k), v);
    }
    m.type = HwMsgType::Values;
    m.values = std::move(vals);
    m.text.clear();
    return m;
}

std::string HwFormatValue(double v)
{
    if (!std::isfinite(v)) return "0";
    char buf[48];
    if (v == std::floor(v) && std::fabs(v) < 1e9)
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    else
        std::snprintf(buf, sizeof(buf), "%.4g", v);
    return buf;
}

std::string HwFormatHello() { return "?hello"; }
std::string HwFormatPing(int seq) { return "?ping " + std::to_string(seq); }
std::string HwFormatSafe() { return "!safe"; }

std::string HwFormatSet(const std::vector<std::pair<std::string, double>>& kv)
{
    std::string s = "!";
    bool first = true;
    for (const auto& [k, v] : kv)
    {
        if (!first) s += ' ';
        first = false;
        s += k; s += '='; s += HwFormatValue(v);
    }
    return s;
}

std::vector<std::string> HwFormatSetLines(const std::vector<std::pair<std::string, double>>& kv)
{
    std::vector<std::string> out;
    std::vector<std::pair<std::string, double>> cur;
    size_t curLen = 1;
    for (const auto& e : kv)
    {
        const size_t add = e.first.size() + 1 + HwFormatValue(e.second).size() + 1;
        if (!cur.empty() && curLen + add > kMaxLineBytes)
        {
            out.push_back(HwFormatSet(cur));
            cur.clear(); curLen = 1;
        }
        cur.push_back(e);
        curLen += add;
    }
    if (!cur.empty()) out.push_back(HwFormatSet(cur));
    return out;
}

std::string HwFormatPin(int pin, std::string_view mode, std::string_view name)
{
    std::string s = "?pin " + std::to_string(pin) + " ";
    s += mode; s += ' '; s += name;
    return s;
}

const char* HwChTypeName(HwChType t)
{
    switch (t) { case HwChType::Bool: return "bool"; case HwChType::Int: return "int"; default: return "float"; }
}

} // namespace dx12e::hw
