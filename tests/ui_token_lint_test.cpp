// 直書き色の lint（W1-A3 / RM-A5）。GPU 不要。
//   src/editor/** と src/gui/** の `IM_COL32(` と、色のリテラルの `ImVec4(r, g, b, a)`（先頭の 3 つが [0,1] の数値リテラル）を数え、
//   tests/ui_token_lint_allow.txt の「ファイルごとの件数」を超えたら赤にする（増やせない棚。減る分には通り、--print で棚を締め直せる）。
//   ・黄系の直書き（色相が Graphite の accent から ±5° 以内、かつ彩度 0.6 以上）は、新規には 0 件。
//     W1-A3 の時点で既にあった分だけ [yellow_legacy]（ファイルごとの件数。増えたら赤、減る一方）に載せ、A14/W2 の置き換えで 0 にする。
//     accent の色相は gg の生成物（kGraphitePalette.accent）から取り、数値を lint に書かない。
//   ・黄の線を引く呼び出し（AddLine / AddRect / AddPolyline / AddTriangle / AddQuad / AddBezier* / AddCircle の 1 行に
//     Accent 系の識別子が出る行）を数え、ファイルごとの件数を許可リストの [accent_lines] で棚にする（増えたら赤）。
//     W1-A7 の `deco::Needle`（src/editor/deco/Needle.*）の中は数えない。
// 使い方:  UiTokenLintTests            検査（ctest）
//          UiTokenLintTests --print    現状の件数を許可リストの書式で標準出力へ（棚を締め直すとき）
#include "editor/EditorTheme.h"
#include "editor/ThemeGraphite.gen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef UI_LINT_ROOT
#error UI_LINT_ROOT must be defined
#endif

namespace
{
namespace fs = std::filesystem;
namespace th = dx12e::theme;

int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// ---- 色相・彩度 ----
struct Hsv { float h, s, v; };
Hsv ToHsv(float r, float g, float b)
{
    const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 0.0f)
    {
        if (mx == r)      h = std::fmod((g - b) / d, 6.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else              h = (r - g) / d + 4.0f;
        h *= 60.0f;
        if (h < 0.0f) h += 360.0f;
    }
    return { h, mx > 0.0f ? d / mx : 0.0f, mx };
}
float HueDiff(float a, float b)
{
    float d = std::fabs(a - b);
    return d > 180.0f ? 360.0f - d : d;
}
constexpr float kYellowHueTolDeg = 5.0f;   // EXEC §2.4 A3: accent の色相から ±5°
constexpr float kYellowMinSat = 0.6f;      // PROGRAM_PLAN A3: 彩度 0.6 以上

bool IsYellow(float r, float g, float b)
{
    const auto& a = th::gg::kGraphitePalette.accent;
    const Hsv ha = ToHsv(a.x, a.y, a.z);
    const Hsv hc = ToHsv(r, g, b);
    return hc.s >= kYellowMinSat && HueDiff(hc.h, ha.h) <= kYellowHueTolDeg;
}

// ---- 字句 ----
bool IsIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// コメントと文字列・文字リテラルの中身を空白にする（改行と長さは保つので行番号がずれない）。
std::string Blank(const std::string& s)
{
    std::string o = s;
    const size_t n = s.size();
    size_t i = 0;
    auto blank = [&](size_t a, size_t b) { for (size_t k = a; k < b && k < n; ++k) if (o[k] != '\n') o[k] = ' '; };
    while (i < n)
    {
        const char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/')
        {
            size_t e = i;
            while (e < n && s[e] != '\n') ++e;
            blank(i, e);
            i = e;
        }
        else if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            size_t e = s.find("*/", i + 2);
            e = (e == std::string::npos) ? n : e + 2;
            blank(i, e);
            i = e;
        }
        else if (c == '"')
        {
            // 生文字列 R"d( ... )d"
            if (i > 0 && s[i - 1] == 'R')
            {
                const size_t p = s.find('(', i + 1);
                if (p != std::string::npos && p - i - 1 <= 16)
                {
                    const std::string close = ")" + s.substr(i + 1, p - i - 1) + "\"";
                    size_t e = s.find(close, p + 1);
                    e = (e == std::string::npos) ? n : e + close.size();
                    blank(i + 1, e - 1);
                    i = e;
                    continue;
                }
            }
            size_t e = i + 1;
            while (e < n && s[e] != '"' && s[e] != '\n') { if (s[e] == '\\') ++e; ++e; }
            blank(i + 1, e);
            i = (e < n) ? e + 1 : n;
        }
        else if (c == '\'')
        {
            // 数字区切り（1'000）は文字リテラルではない
            if (i > 0 && std::isxdigit(static_cast<unsigned char>(s[i - 1])) && i + 1 < n && std::isxdigit(static_cast<unsigned char>(s[i + 1])))
            { ++i; continue; }
            size_t e = i + 1;
            while (e < n && s[e] != '\'' && s[e] != '\n') { if (s[e] == '\\') ++e; ++e; }
            blank(i + 1, e);
            i = (e < n) ? e + 1 : n;
        }
        else ++i;
    }
    return o;
}

// `(` の位置から対応する `)` までの引数を、トップレベルのカンマで割って返す。見つからなければ空。
std::vector<std::string> SplitArgs(const std::string& s, size_t open, size_t* closeOut = nullptr)
{
    std::vector<std::string> args;
    int depth = 0;
    std::string cur;
    for (size_t i = open; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '(' || c == '[' || c == '{') { if (depth++ == 0) continue; }
        else if (c == ')' || c == ']' || c == '}')
        {
            if (--depth == 0)
            {
                args.push_back(cur);
                if (closeOut) *closeOut = i;
                return args;
            }
        }
        else if (c == ',' && depth == 1) { args.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    return {};
}

std::string Trim(std::string v)
{
    size_t a = 0, b = v.size();
    while (a < b && std::isspace(static_cast<unsigned char>(v[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(v[b - 1]))) --b;
    return v.substr(a, b - a);
}

// 数値リテラル（10 進・16 進の整数、小数。接尾辞 f/F/u/U/l/L 可、符号なし）だけを値にする。式は不可。
bool ParseNumber(const std::string& raw, double& out)
{
    std::string t = Trim(raw);
    if (t.empty()) return false;
    while (!t.empty() && (t.back() == 'f' || t.back() == 'F' || t.back() == 'u' || t.back() == 'U' || t.back() == 'l' || t.back() == 'L'))
    {
        // 16 進の f（0xFF）を接尾辞と取り違えない
        if (t.size() > 2 && (t[1] == 'x' || t[1] == 'X')) break;
        t.pop_back();
    }
    if (t.empty()) return false;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
    {
        for (size_t i = 2; i < t.size(); ++i) if (!std::isxdigit(static_cast<unsigned char>(t[i]))) return false;
        out = static_cast<double>(std::strtoul(t.c_str() + 2, nullptr, 16));
        return true;
    }
    bool dot = false, digit = false;
    for (char c : t)
    {
        if (c == '.') { if (dot) return false; dot = true; }
        else if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
        else return false;
    }
    if (!digit) return false;
    out = std::strtod(t.c_str(), nullptr);
    return true;
}

// ---- 走査 ----
struct ScanResult
{
    int colorLiterals = 0;                 // IM_COL32( と色の ImVec4( の数
    std::vector<int> yellowLines;          // 黄系の直書きの行（1 始まり）
    int accentLineDraws = 0;               // 黄の線を引く呼び出しの数
};

int LineOf(const std::string& s, size_t pos)
{
    return 1 + static_cast<int>(std::count(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
}

bool HasIdent(const std::string& s, const char* id)
{
    const size_t L = std::char_traits<char>::length(id);
    for (size_t p = s.find(id); p != std::string::npos; p = s.find(id, p + 1))
    {
        const bool l = (p == 0) || !IsIdent(s[p - 1]);
        const bool r = (p + L >= s.size()) || !IsIdent(s[p + L]);
        if (l && r) return true;
    }
    return false;
}

ScanResult ScanText(const std::string& raw, bool countAccentLines)
{
    ScanResult r;
    const std::string s = Blank(raw);

    // IM_COL32(
    static const char kCol32[] = "IM_COL32(";
    for (size_t p = s.find(kCol32); p != std::string::npos; p = s.find(kCol32, p + 1))
    {
        if (p > 0 && IsIdent(s[p - 1])) continue;
        // `#define IM_COL32(` の定義行は数えない
        size_t ls = s.rfind('\n', p);
        ls = (ls == std::string::npos) ? 0 : ls + 1;
        if (s.compare(ls, 7, "#define") == 0) continue;
        auto a = SplitArgs(s, p + sizeof(kCol32) - 2);
        ++r.colorLiterals;
        if (a.size() == 4)
        {
            double v[3];
            if (ParseNumber(a[0], v[0]) && ParseNumber(a[1], v[1]) && ParseNumber(a[2], v[2]) && IsYellow(float(v[0] / 255.0), float(v[1] / 255.0), float(v[2] / 255.0)))
                r.yellowLines.push_back(LineOf(s, p));
        }
    }

    // ImVec4(r, g, b, a): 先頭の 3 つが [0,1] の数値リテラルのもの（矩形や位置の ImVec4 は数えない）
    static const char kVec4[] = "ImVec4(";
    for (size_t p = s.find(kVec4); p != std::string::npos; p = s.find(kVec4, p + 1))
    {
        if (p > 0 && IsIdent(s[p - 1])) continue;
        auto a = SplitArgs(s, p + sizeof(kVec4) - 2);
        if (a.size() != 4) continue;
        double v[3];
        if (!(ParseNumber(a[0], v[0]) && ParseNumber(a[1], v[1]) && ParseNumber(a[2], v[2]))) continue;
        if (v[0] < 0 || v[0] > 1 || v[1] < 0 || v[1] > 1 || v[2] < 0 || v[2] > 1) continue;
        ++r.colorLiterals;
        if (IsYellow(float(v[0]), float(v[1]), float(v[2]))) r.yellowLines.push_back(LineOf(s, p));
    }

    // 黄の線を引く呼び出し: 1 行に「線・縁取りの描画」と「Accent 系の識別子」が並ぶもの（塗りつぶし Filled は数えない）
    if (countAccentLines)
    {
        static const char* kDraw[] = { "AddLine(", "AddRect(", "AddPolyline(", "AddTriangle(", "AddQuad(", "AddBezierCubic(", "AddBezierQuadratic(", "AddCircle(", "AddNgon(" };
        static const char* kAcc[] = { "Accent", "AccentHover", "AccentPressed", "AccentLight", "AccentDim", "AccentDim2", "NeedleDim" };
        std::istringstream is(s);
        std::string line;
        while (std::getline(is, line))
        {
            bool draw = false;
            for (const char* d : kDraw)
            {
                const size_t p = line.find(d);
                if (p != std::string::npos && (p == 0 || line[p - 1] == '>' || line[p - 1] == '.' || !IsIdent(line[p - 1]))) { draw = true; break; }
            }
            if (!draw) continue;
            bool acc = false;
            for (const char* a : kAcc) if (HasIdent(line, a)) { acc = true; break; }
            if (acc) ++r.accentLineDraws;
        }
    }
    return r;
}

// ---- 許可リスト ----
struct Allow
{
    std::map<std::string, int> colors;        // [colors]
    std::map<std::string, int> accentLines;   // [accent_lines]
    std::map<std::string, int> yellowLegacy;  // [yellow_legacy]
    bool ok = false;
};

Allow LoadAllow(const fs::path& p)
{
    Allow a;
    std::ifstream f(p);
    if (!f) return a;
    a.ok = true;
    std::string line, sect;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { sect = line; continue; }
        std::istringstream ls(line);
        std::string path;
        int n = -1;
        ls >> path >> n;
        if (path.empty() || n < 0) { a.ok = false; std::printf("allow: bad line: %s\n", line.c_str()); continue; }
        if (sect == "[colors]") a.colors[path] = n;
        else if (sect == "[accent_lines]") a.accentLines[path] = n;
        else if (sect == "[yellow_legacy]") a.yellowLegacy[path] = n;
        else { a.ok = false; std::printf("allow: unknown section: %s\n", sect.c_str()); }
    }
    return a;
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool IsSource(const fs::path& p)
{
    const std::string e = p.extension().string();
    return e == ".h" || e == ".hpp" || e == ".cpp" || e == ".inc" || e == ".inl";
}

// deco::Needle の実装（W1-A7）の中は「黄の線の呼び出し箇所」に数えない。
bool IsNeedleImpl(const std::string& rel)
{
    return rel.rfind("src/editor/deco/Needle.", 0) == 0;
}

struct Totals
{
    std::map<std::string, ScanResult> files;
};

Totals ScanTree(const fs::path& root)
{
    Totals t;
    for (const char* sub : { "src/editor", "src/gui" })
    {
        const fs::path base = root / sub;
        if (!fs::exists(base)) continue;
        for (auto it = fs::recursive_directory_iterator(base); it != fs::recursive_directory_iterator(); ++it)
        {
            if (!it->is_regular_file() || !IsSource(it->path())) continue;
            std::string rel = fs::relative(it->path(), root).generic_string();
            ScanResult r = ScanText(ReadAll(it->path()), !IsNeedleImpl(rel));
            if (r.colorLiterals > 0 || !r.yellowLines.empty() || r.accentLineDraws > 0) t.files[rel] = std::move(r);
        }
    }
    return t;
}

// ---- スキャナの自己検査 ----
void SelfTest()
{
    // 数える: IM_COL32 は引数が変数でも数える。色の ImVec4 だけ数える（矩形は [0,1] の外・変数混じりで外れる）
    {
        auto r = ScanText("auto a = IM_COL32(10, 20, 30, alpha);\nImVec4 b(0.1f, 0.2f, 0.3f, 1.0f);\nImVec4 rc = ImVec4(0, 0, 640, 480);\nImVec4 d = ImVec4(c.x, c.y, c.z, 0.5f);\n", true);
        CHECKF(r.colorLiterals == 1, "self: IM_COL32 only (ImVec4 b( is a declaration, not ImVec4( call) got %d", r.colorLiterals);
    }
    {
        auto r = ScanText("x = ImVec4(0.1f, 0.2f, 0.3f, 1.0f);\ny = ImVec4(0,0,640,480);\nz = ImVec4(c.x,c.y,c.z,1);\n", true);
        CHECKF(r.colorLiterals == 1, "self: ImVec4 literal count got %d", r.colorLiterals);
    }
    // コメントと文字列は数えない
    {
        auto r = ScanText("// IM_COL32(1,2,3,4)\n/* ImVec4(0,0,0,1) */\nconst char* s = \"IM_COL32(1,2,3,4)\";\nauto raw = R\"x(IM_COL32(1,2,3,4))x\";\n", true);
        CHECKF(r.colorLiterals == 0, "self: comments/strings got %d", r.colorLiterals);
    }
    // 識別子の一部・#define は数えない
    {
        auto r = ScanText("#define IM_COL32(R,G,B,A) 0\nint MY_IM_COL32(1,2,3,4);\nauto w = IM_COL32_WHITE;\n", true);
        CHECKF(r.colorLiterals == 0, "self: define/ident got %d", r.colorLiterals);
    }
    // 黄: accent と同じ色は検出、灰・青・暗い黄土色は対象外。16 進と小数
    {
        auto r = ScanText("a = IM_COL32(240, 194, 59, 255);\nb = IM_COL32(0xF0, 0xC2, 0x3B, 255);\nc = ImVec4(0.94f, 0.76f, 0.23f, 1);\n"
                          "d = IM_COL32(128, 128, 128, 255);\ne = IM_COL32(60, 120, 240, 255);\nf = IM_COL32(100, 90, 80, 255);\n", true);
        CHECKF(r.yellowLines.size() == 3, "self: yellow hits got %zu", r.yellowLines.size());
        CHECKF(r.colorLiterals == 6, "self: yellow total got %d", r.colorLiterals);
        CHECK(!r.yellowLines.empty() && r.yellowLines[0] == 1);
    }
    // 引数に変数があると黄の判定はできない（件数には入る）
    {
        auto r = ScanText("a = IM_COL32(240, g, 59, 255);\n", true);
        CHECK(r.colorLiterals == 1 && r.yellowLines.empty());
    }
    // 黄の線: 線・縁取りの描画 + Accent 系が 1 行にあるものだけ（Filled は数えない・Needle 実装は呼び出し側が外す）
    {
        auto r = ScanText("dl->AddLine(a, b, U(th::Accent), 2);\ndl->AddRectFilled(a, b, U(th::Accent));\ndl->AddRect(a, b, U(th::AccentHover));\ndl->AddLine(a, b, U(th::Border));\nint AccentCount = 0;\n", true);
        CHECKF(r.accentLineDraws == 2, "self: accent line draws got %d", r.accentLineDraws);
        auto r2 = ScanText("dl->AddLine(a, b, U(th::Accent), 2);\n", false);
        CHECK(r2.accentLineDraws == 0);
    }
    // 色相の環状差
    CHECK(HueDiff(359.0f, 1.0f) == 2.0f);
    // accent 自身は黄
    {
        const auto& a = th::gg::kGraphitePalette.accent;
        CHECK(IsYellow(a.x, a.y, a.z));
    }
}
} // namespace

int main(int argc, char** argv)
{
    const fs::path root = UI_LINT_ROOT;
    const fs::path allowPath = root / "tests" / "ui_token_lint_allow.txt";

    if (argc > 1 && std::string(argv[1]) == "--print")
    {
        const Totals t = ScanTree(root);
        std::printf("[colors]\n");
        for (const auto& [k, v] : t.files) if (v.colorLiterals > 0) std::printf("%s %d\n", k.c_str(), v.colorLiterals);
        std::printf("\n[yellow_legacy]\n");
        for (const auto& [k, v] : t.files) if (!v.yellowLines.empty()) std::printf("%s %zu\n", k.c_str(), v.yellowLines.size());
        std::printf("\n[accent_lines]\n");
        for (const auto& [k, v] : t.files) if (v.accentLineDraws > 0) std::printf("%s %d\n", k.c_str(), v.accentLineDraws);
        return 0;
    }

    SelfTest();

    const Allow allow = LoadAllow(allowPath);
    CHECKF(allow.ok, "許可リストを読めない / 書式が不正: %s", allowPath.generic_string().c_str());

    const Totals t = ScanTree(root);
    int total = 0, accentTotal = 0;
    for (const auto& [rel, r] : t.files)
    {
        total += r.colorLiterals;
        accentTotal += r.accentLineDraws;

        // 黄系の直書き: 棚（[yellow_legacy]）を超えたら赤。棚の既定は 0 件
        if (!r.yellowLines.empty())
        {
            const auto it = allow.yellowLegacy.find(rel);
            const int cap = it == allow.yellowLegacy.end() ? 0 : it->second;
            if (static_cast<int>(r.yellowLines.size()) > cap)
                for (int ln : r.yellowLines)
                    CHECKF(false, "%s:%d  黄系（accent の色相 ±%.0f°、彩度 %.1f 以上）の直書き（棚 %d 件を超過）。accent 系のトークンを使うこと",
                           rel.c_str(), ln, kYellowHueTolDeg, kYellowMinSat, cap);
        }

        if (r.colorLiterals > 0)
        {
            const auto it = allow.colors.find(rel);
            const int cap = it == allow.colors.end() ? 0 : it->second;
            CHECKF(r.colorLiterals <= cap, "%s: 直書き色 %d 件（許可 %d 件）。色は EditorTheme / gg のトークンから取ること。件数を増やす場合はメインに依頼",
                   rel.c_str(), r.colorLiterals, cap);
            if (r.colorLiterals < cap)
                std::printf("note: %s は許可 %d 件に対し %d 件に減った（UiTokenLintTests --print で棚を締められる）\n", rel.c_str(), cap, r.colorLiterals);
        }
        if (r.accentLineDraws > 0)
        {
            const auto it = allow.accentLines.find(rel);
            const int cap = it == allow.accentLines.end() ? 0 : it->second;
            CHECKF(r.accentLineDraws <= cap, "%s: 黄の線の呼び出し %d 件（許可 %d 件）。線は deco::Needle 経由で引くこと",
                   rel.c_str(), r.accentLineDraws, cap);
        }
    }
    // 許可リストにあるのに存在しないファイル（改名・削除の取り残し）は、棚が緩むので赤にする
    for (const auto& [rel, n] : allow.colors)
        CHECKF(fs::exists(root / rel), "許可リスト [colors] のファイルが無い: %s", rel.c_str());
    for (const auto& [rel, n] : allow.yellowLegacy)
        CHECKF(fs::exists(root / rel), "許可リスト [yellow_legacy] のファイルが無い: %s", rel.c_str());
    for (const auto& [rel, n] : allow.accentLines)
        CHECKF(fs::exists(root / rel), "許可リスト [accent_lines] のファイルが無い: %s", rel.c_str());

    std::printf("ui_token_lint: 直書き色 %d 件（%zu ファイル）、黄の線の呼び出し %d 件。\n", total, t.files.size(), accentTotal);
    if (g_fail)
    {
        std::printf("UiTokenLintTests: %d 件の失敗\n", g_fail);
        return 1;
    }
    std::printf("UiTokenLintTests: OK\n");
    return 0;
}
