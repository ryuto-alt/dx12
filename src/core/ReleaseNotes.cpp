#include "core/ReleaseNotes.h"

namespace dx12e::relnotes
{
namespace
{
struct AreaDef { const char* id; const char* label; };
const AreaDef kAreas[] = {
    { "lighting", "ライティング" }, { "editor", "エディタ" },   { "physics", "物理" },
    { "world",    "表現" },         { "render",  "描画" },       { "audio",   "オーディオ" },
    { "ai",       "ゲーム AI" },    { "script",  "スクリプト" }, { "mcp",     "AI 連携" },
    { "perf",     "性能" },         { "general", "全般" },
};

std::vector<Release> BuildAll()
{
    using K = Kind;
    std::vector<Release> v;
#include "core/ReleaseNotesData.inc"
    return v;
}
} // namespace

const std::vector<Release>& All()
{
    static const std::vector<Release> s = BuildAll();
    return s;
}

const Release* Find(const std::vector<Release>& all, const std::string& version)
{
    for (const Release& r : all)
        if (CompareVersions(r.version, version) == 0) return &r;
    return nullptr;
}

bool ParseVersion(const std::string& s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    size_t i = 0;
    while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;
    if (i >= s.size()) return false;
    int idx = 0;
    while (i < s.size() && idx < 3)
    {
        int v = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = true; }
        if (any) out[idx++] = v;
        while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) ++i;
    }
    return true;
}

int CompareVersions(const std::string& a, const std::string& b)
{
    int x[3], y[3];
    ParseVersion(a, x);
    ParseVersion(b, y);
    for (int i = 0; i < 3; ++i)
    {
        if (x[i] < y[i]) return -1;
        if (x[i] > y[i]) return 1;
    }
    return 0;
}

bool IsNewer(const std::string& a, const std::string& b) { return CompareVersions(a, b) > 0; }

std::vector<const Release*> Range(const std::vector<Release>& all, const std::string& from, const std::string& to)
{
    std::vector<const Release*> out;
    int f[3];
    const bool fromOk = ParseVersion(from, f) && CompareVersions(from, to) < 0;
    for (const Release& r : all)
    {
        if (CompareVersions(r.version, to) > 0) continue;            // 今の版より新しいものは載せない
        if (fromOk)
        {
            if (CompareVersions(r.version, from) > 0) out.push_back(&r);
        }
        else if (CompareVersions(r.version, to) == 0)
        {
            out.push_back(&r);
        }
    }
    return out;
}

std::vector<const Release*> History(const std::vector<Release>& all, const std::string& to)
{
    std::vector<const Release*> out;
    for (const Release& r : all)
        if (CompareVersions(r.version, to) <= 0) out.push_back(&r);
    return out;
}

const char* KindLabel(Kind k)
{
    switch (k)
    {
    case Kind::Feature:     return "新機能";
    case Kind::Improvement: return "改善";
    default:                return "修正";
    }
}

int CountKind(const Release& r, Kind k)
{
    int n = 0;
    for (const Item& it : r.items) if (it.kind == k) ++n;
    return n;
}

bool IsKnownArea(const std::string& area)
{
    for (const AreaDef& a : kAreas) if (area == a.id) return true;
    return false;
}

const char* AreaLabel(const std::string& area)
{
    for (const AreaDef& a : kAreas) if (area == a.id) return a.label;
    return "全般";
}

std::vector<std::string> Validate(const std::vector<Release>& all)
{
    std::vector<std::string> err;
    if (all.empty()) err.push_back("リリースが 1 件もありません");
    auto validSemver = [](const std::string& s)
    {
        int dots = 0;
        if (s.empty() || s.front() == '.' || s.back() == '.') return false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '.') { ++dots; if (i > 0 && s[i - 1] == '.') return false; }
            else if (!(s[i] >= '0' && s[i] <= '9')) return false;
        }
        return dots == 2;
    };
    auto validDate = [](const std::string& s)
    {
        if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
        for (size_t i = 0; i < s.size(); ++i)
            if (i != 4 && i != 7 && !(s[i] >= '0' && s[i] <= '9')) return false;
        const int m = (s[5] - '0') * 10 + (s[6] - '0'), d = (s[8] - '0') * 10 + (s[9] - '0');
        return m >= 1 && m <= 12 && d >= 1 && d <= 31;
    };
    for (size_t i = 0; i < all.size(); ++i)
    {
        const Release& r = all[i];
        const std::string tag = "v" + r.version + ": ";
        if (!validSemver(r.version)) err.push_back(tag + "版の形式が X.Y.Z ではありません");
        if (i > 0 && CompareVersions(all[i - 1].version, r.version) <= 0)
            err.push_back(tag + "新しい順に並んでいません（直前の版より新しいか同じ）");
        if (!validDate(r.date)) err.push_back(tag + "日付が YYYY-MM-DD ではありません");
        if (i > 0 && validDate(r.date) && validDate(all[i - 1].date) && all[i - 1].date < r.date)
            err.push_back(tag + "日付が新しい版より後になっています");
        if (r.headline.empty()) err.push_back(tag + "見出し(headline)が空です");
        if (r.highlights.empty() || r.highlights.size() > 4) err.push_back(tag + "注目(highlights)は 1〜4 件にしてください");
        for (const Highlight& h : r.highlights)
        {
            if (h.title.empty() || h.body.empty()) err.push_back(tag + "注目の題か本文が空です");
            if (!IsKnownArea(h.area)) err.push_back(tag + "注目「" + h.title + "」の分野 ID が不明です: " + h.area);
        }
        if (r.items.empty()) err.push_back(tag + "一覧(items)が空です");
        for (const Item& it : r.items)
            if (it.title.empty()) err.push_back(tag + "一覧に題が空の項目があります");
    }
    return err;
}

std::string ToMarkdown(const Release& r, const std::string& engineName)
{
    std::string o;
    o += "# " + engineName + " v" + r.version + "（" + r.date + "）\n\n";
    o += r.headline + "\n\n";
    if (!r.highlights.empty())
    {
        o += "## 注目\n\n";
        for (const Highlight& h : r.highlights)
        {
            o += "- **" + h.title + "** — " + h.body;
            if (!h.detail.empty()) o += " " + h.detail;
            o += "\n";
        }
        o += "\n";
    }
    const Kind kinds[] = { Kind::Feature, Kind::Improvement, Kind::Fix };
    for (Kind k : kinds)
    {
        if (CountKind(r, k) == 0) continue;
        o += std::string("## ") + KindLabel(k) + "\n\n";
        for (const Item& it : r.items)
        {
            if (it.kind != k) continue;
            o += "- " + it.title;
            if (!it.body.empty()) o += " — " + it.body;
            o += "\n";
        }
        o += "\n";
    }
    while (o.size() >= 2 && o[o.size() - 1] == '\n' && o[o.size() - 2] == '\n') o.pop_back();
    return o;
}

} // namespace dx12e::relnotes
