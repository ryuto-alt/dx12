#include "core/UpdateLogic.h"

#include <cmath>
#include <cstdio>

#include "core/ReleaseNotes.h"

namespace dx12e::updatelogic
{
namespace
{
std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
bool StartsWith(const std::string& s, const char* p)
{
    for (size_t i = 0; p[i]; ++i) if (i >= s.size() || s[i] != p[i]) return false;
    return true;
}
} // namespace

std::string StripInlineMarkdown(const std::string& in)
{
    std::string s;
    // HTML コメントと <br> を先に落とす
    for (size_t i = 0; i < in.size();)
    {
        if (in.compare(i, 4, "<!--") == 0)
        {
            const size_t e = in.find("-->", i + 4);
            i = (e == std::string::npos) ? in.size() : e + 3;
        }
        else if (in.compare(i, 4, "<br>") == 0 || in.compare(i, 5, "<br/>") == 0 || in.compare(i, 6, "<br />") == 0)
        {
            s += ' ';
            i += (in[i + 3] == '>') ? 4 : (in[i + 3] == '/' ? 5 : 6);
        }
        else s += in[i++];
    }
    std::string o;
    for (size_t i = 0; i < s.size();)
    {
        // ![alt](url) は丸ごと捨てる
        if (s[i] == '!' && i + 1 < s.size() && s[i + 1] == '[')
        {
            const size_t c = s.find("](", i + 2);
            const size_t e = (c == std::string::npos) ? std::string::npos : s.find(')', c + 2);
            if (e != std::string::npos) { i = e + 1; continue; }
        }
        // [題](url) → 題
        if (s[i] == '[')
        {
            const size_t c = s.find("](", i + 1);
            const size_t e = (c == std::string::npos) ? std::string::npos : s.find(')', c + 2);
            if (e != std::string::npos)
            {
                o += s.substr(i + 1, c - i - 1);
                i = e + 1;
                continue;
            }
        }
        if ((s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') || (s[i] == '_' && i + 1 < s.size() && s[i + 1] == '_')) { i += 2; continue; }
        if (s[i] == '`') { ++i; continue; }
        o += s[i++];
    }
    return Trim(o);
}

BodyView FormatGithubBody(const std::string& md, int maxLines)
{
    std::vector<BodyLine> all;
    bool fence = false, firstH1Done = false;
    size_t pos = 0;
    while (pos <= md.size())
    {
        size_t e = md.find('\n', pos);
        if (e == std::string::npos) e = md.size();
        std::string line = Trim(md.substr(pos, e - pos));
        pos = e + 1;

        if (StartsWith(line, "```") || StartsWith(line, "~~~")) { fence = !fence; continue; }
        if (fence || line.empty()) continue;
        if (line == "---" || line == "***" || line == "___" || StartsWith(line, "<!--")) continue;
        if (line[0] == '>') line = Trim(line.substr(1));

        BodyLine bl;
        if (line[0] == '#')
        {
            size_t n = 0;
            while (n < line.size() && line[n] == '#') ++n;
            const std::string t = StripInlineMarkdown(Trim(line.substr(n)));
            if (n == 1 && !firstH1Done) { firstH1Done = true; continue; }   // リリース名の繰り返し
            if (t.empty()) continue;
            bl.type = BodyLine::Type::Heading;
            bl.text = t;
        }
        else if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line.size() > 1 && line[1] == ' ')
        {
            bl.type = BodyLine::Type::Bullet;
            bl.text = StripInlineMarkdown(line.substr(2));
        }
        else if (line[0] >= '0' && line[0] <= '9')
        {
            size_t n = 0;
            while (n < line.size() && line[n] >= '0' && line[n] <= '9') ++n;
            if (n < line.size() && line[n] == '.' && n + 1 < line.size() && line[n + 1] == ' ')
            {
                bl.type = BodyLine::Type::Bullet;
                bl.text = StripInlineMarkdown(line.substr(n + 2));
            }
            else { bl.type = BodyLine::Type::Text; bl.text = StripInlineMarkdown(line); }
        }
        else
        {
            bl.type = BodyLine::Type::Text;
            bl.text = StripInlineMarkdown(line);
        }
        if (bl.text.empty()) continue;
        all.push_back(std::move(bl));
    }

    BodyView v;
    size_t take = all.size() < static_cast<size_t>(maxLines < 0 ? 0 : maxLines) ? all.size() : static_cast<size_t>(maxLines < 0 ? 0 : maxLines);
    if (take < all.size())
        while (take > 0 && all[take - 1].type == BodyLine::Type::Heading) --take;   // 見出しで終わらせない
    v.lines.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(take));
    int bullets = 0;
    for (size_t i = take; i < all.size(); ++i)
        if (all[i].type == BodyLine::Type::Bullet) ++bullets;
    if (take < all.size())
    {
        if (bullets > 0) v.more = bullets;
        else
        {
            int rest = 0;
            for (size_t i = take; i < all.size(); ++i)
                if (all[i].type != BodyLine::Type::Heading) ++rest;
            v.more = rest;
        }
    }
    return v;
}

bool ShouldPromptUpdate(const std::string& latest, const std::string& skipped)
{
    int tmp[3];
    if (skipped.empty() || !relnotes::ParseVersion(skipped, tmp)) return true;
    return relnotes::IsNewer(latest, skipped);
}

void RateTracker::Add(double nowSec, uint64_t doneBytes)
{
    if (!m_s.empty() && (nowSec < m_s.back().t || doneBytes < m_s.back().b)) m_s.clear();   // 巻き戻り（再試行）
    m_s.push_back({ nowSec, doneBytes });
    while (m_s.size() > 2 && nowSec - m_s.front().t > m_window) m_s.pop_front();
}

double RateTracker::BytesPerSec() const
{
    if (m_s.size() < 2) return 0.0;
    const double dt = m_s.back().t - m_s.front().t;
    if (dt < 0.5) return 0.0;
    return static_cast<double>(m_s.back().b - m_s.front().b) / dt;
}

double RateTracker::EtaSeconds(uint64_t done, uint64_t total) const
{
    const double r = BytesPerSec();
    if (r <= 1.0 || total == 0 || done > total) return -1.0;
    return static_cast<double>(total - done) / r;
}

std::string FormatMB(uint64_t bytes)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", static_cast<double>(bytes) / 1048576.0);
    return b;
}

std::string FormatSpeed(double bps)
{
    if (!(bps > 0.0)) return {};
    char b[48];
    if (bps >= 1048576.0) std::snprintf(b, sizeof(b), "%.1f MB/s", bps / 1048576.0);
    else                  std::snprintf(b, sizeof(b), "%.0f KB/s", bps / 1024.0);
    return b;
}

std::string FormatEta(double sec)
{
    if (!(sec >= 0.0)) return {};
    const long s = std::lround(sec);
    char b[48];
    if (s < 60)        std::snprintf(b, sizeof(b), "約 %ld 秒", s < 1 ? 1L : s);
    else if (s < 3600) std::snprintf(b, sizeof(b), "約 %ld 分", (s + 30) / 60);
    else               std::snprintf(b, sizeof(b), "約 %ld 時間", (s + 1800) / 3600);
    return b;
}

std::string FormatDownloadDetail(uint64_t done, uint64_t total, double bps, double eta)
{
    std::string o = total > 0 ? FormatMB(done) + " / " + FormatMB(total) + " MB" : FormatMB(done) + " MB";
    const std::string sp = FormatSpeed(bps);
    if (!sp.empty()) o += "  ・  " + sp;
    const std::string et = FormatEta(eta);
    if (!et.empty()) o += "  ・  残り" + et;
    return o;
}

} // namespace dx12e::updatelogic
