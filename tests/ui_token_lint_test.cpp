// 直書き色の lint（W1-A3 / RM-A5）。GPU 不要。
//   src/editor/** と src/gui/** の `IM_COL32(` と、色のリテラルの `ImVec4(r, g, b, a)`（先頭の 3 つが [0,1] の数値リテラル）を数え、
//   tests/ui_token_lint_allow.txt の「ファイルごとの件数」を超えたら赤にする（増やせない棚。減る分には通り、--print で棚を締め直せる）。
//   ・数える色の形: IM_COL32( / ImVec4 の色リテラル（`ImVec4(` `ImVec4{` `ImVec4 x(` `ImVec4 x{`）/ ImColor( / th::Hex(0x...)（テーマ本体 EditorTheme.h・ThemeVariants.h・*.gen.h の Hex は除く）。
//   ・黄系の直書き（色相が Graphite の accent から ±5° 以内、かつ彩度 0.6 以上）は、新規には 0 件。
//     W1-A3 の時点で既にあった分だけ [yellow_legacy]（ファイルごとの件数。増えたら赤、減る一方）に載せ、A14/W2 の置き換えで 0 にする。
//     accent の色相は gg の生成物（kGraphitePalette.accent = F0C23B、色相 約 44.75°）から取り、数値を lint に書かない。
//     （EXEC は gg::kGgSpec と書くが、kGgSpec にパレットの accent は無く、生成パレットの accent が同じ値の唯一の出どころ。±5° は PP の 40〜50° 相当）
//   ・黄の線を引く呼び出し（AddLine / AddRect / AddPolyline / AddTriangle / AddQuad / AddBezier* / AddCircle / AddNgon / PathStroke と、
//     名前に line / edge / stroke / glow / border を含むラッパー（EdgeLine / TopHairline / DrawResolvedOutline / GlowRect 等。Filled を含む名前と Needle* は除く））の
//     引数のどこかに、accent を語に含む識別子（Accent* / accent* / kAccentCol / pal.accent 等。ImGuiKey_GraveAccent と onAccent は除く）が出るものを数える。
//     コメント・文字列を空白にした全文に対し、'(' から対応する ')' までを引数として見るので、複数行にまたがる呼び出しも数える。ネストした呼び出しは外側の 1 件。
//     関数定義（引数の後ろが '{'）は数えない。W1-A7 の `deco::Needle` の本体（名前が Needle で始まる関数定義の {} の中）は数えない（置き場所のファイルに依らない）。
//     ファイルごとの件数を許可リストの [accent_lines] で棚にする（増えたら赤。減ったら棚を締め直すまで赤）。
//     [yellow_legacy] も同じく、減ったら棚を締め直すまで赤（棚は減る一方）。
//   ・未対応: float[4] の色配列（SelectionOutline.h の selColor[4] / プレビューの clearColor[4] 等）。生の ImU32 0xAABBGGRR。
//     これらは「直書き 0」の指標に入らない（GG-5 の数字は下限）。
// 使い方:  UiTokenLintTests            検査（ctest）
//          UiTokenLintTests --print    現状の件数を許可リストの書式で標準出力へ（棚を締め直すとき）
#include "editor/EditorTheme.h"
#include "editor/ThemeGraphite.gen.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
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

// 識別子が「accent」を語として含むか（AccentHover / accentDim / kUiAccentCol / m_tok.accentHover / NeedleDim）。
// ImGuiKey_GraveAccent のような ImGui のキー名と、onAccent（アクセント上の文字色）は除く。
bool IsAccentIdent(const std::string& id)
{
    if (id.rfind("ImGuiKey_", 0) == 0) return false;
    if (id == "NeedleDim") return true;
    std::vector<std::string> seg;
    std::string cur;
    for (size_t i = 0; i < id.size(); ++i)
    {
        const char c = id[i];
        if (c == '_') { if (!cur.empty()) seg.push_back(cur); cur.clear(); continue; }
        const bool up = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLow = i > 0 && std::islower(static_cast<unsigned char>(id[i - 1]));
        if (up && prevLow && !cur.empty()) { seg.push_back(cur); cur.clear(); }
        cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (!cur.empty()) seg.push_back(cur);
    // 末尾が Px の識別子（th::AccentPx() 等）は線の太さ（整数 px）を返す関数で、色ではない。
    if (seg.size() > 1 && seg.back() == "px") return false;
    for (size_t i = 0; i < seg.size(); ++i)
        if (seg[i] == "accent") return !(i > 0 && seg[i - 1] == "on");
    return false;
}

bool LineHasAccentIdent(const std::string& line)
{
    size_t i = 0;
    while (i < line.size())
    {
        if (IsIdent(line[i]) && !std::isdigit(static_cast<unsigned char>(line[i])))
        {
            size_t e = i;
            while (e < line.size() && IsIdent(line[e])) ++e;
            if (IsAccentIdent(line.substr(i, e - i))) return true;
            i = e;
        }
        else if (IsIdent(line[i])) { while (i < line.size() && IsIdent(line[i])) ++i; }
        else ++i;
    }
    return false;
}

size_t SkipSpace(const std::string& s, size_t i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

// IM_COL32 の定義そのもの（`#define IM_COL32(`、# や define の前後の空白可）だけを除く。他のマクロの本体に出る IM_COL32 は数える。
bool IsImCol32Definition(const std::string& s, size_t p)
{
    size_t ls = s.rfind('\n', p);
    ls = (ls == std::string::npos) ? 0 : ls + 1;
    size_t i = SkipSpace(s, ls);
    if (i >= p || s[i] != '#') return false;
    i = SkipSpace(s, i + 1);
    if (s.compare(i, 6, "define") != 0) return false;
    i = SkipSpace(s, i + 6);
    return i == p;
}

// 色の hex リテラル用: 0xRRGGBB
bool IsHexColorLiteral(const std::string& a, float& r, float& g, float& b)
{
    double v;
    if (!ParseNumber(a, v) || v < 0 || v > 16777215.0) return false;
    const unsigned long u = static_cast<unsigned long>(v);
    r = float((u >> 16) & 0xFF) / 255.0f; g = float((u >> 8) & 0xFF) / 255.0f; b = float(u & 0xFF) / 255.0f;
    return true;
}

// 線・縁取り・光を引く呼び出しの名前か（Filled と Needle* は除く）。
bool IsLineishCallName(const std::string& name)
{
    std::string l;
    for (char c : name) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (l.rfind("needle", 0) == 0 || l.find("filled") != std::string::npos) return false;
    static const char* kExact[] = { "addrect", "addcircle", "addtriangle", "addquad", "addngon", "addbeziercubic", "addbezierquadratic", "addpolyline", "pathstroke" };
    for (const char* e : kExact) if (l == e) return true;
    static const char* kSub[] = { "line", "edge", "stroke", "glow", "border" };
    for (const char* e : kSub) if (l.find(e) != std::string::npos) return true;
    return false;
}

size_t MatchBrace(const std::string& s, size_t open)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}' && --depth == 0) return i + 1;
    }
    return s.size();
}

// 呼び出し（引数は複数行可）の引数のどこかに accent 識別子があるものの数。s は Blank 済みの全文。
int CountAccentLineCalls(const std::string& s)
{
    int n = 0;
    size_t i = 0;
    while (i < s.size())
    {
        if (!IsIdent(s[i])) { ++i; continue; }
        size_t e = i;
        while (e < s.size() && IsIdent(s[e])) ++e;
        if (std::isdigit(static_cast<unsigned char>(s[i]))) { i = e; continue; }
        const std::string id = s.substr(i, e - i);
        const size_t o = SkipSpace(s, e);
        if (o < s.size() && s[o] == '(')
        {
            size_t close = 0;
            const auto args = SplitArgs(s, o, &close);
            if (!args.empty())
            {
                size_t k = SkipSpace(s, close + 1);
                // const / noexcept を飛ばして、本体の { があれば関数定義
                for (;;)
                {
                    if (s.compare(k, 5, "const") == 0) k = SkipSpace(s, k + 5);
                    else if (s.compare(k, 8, "noexcept") == 0) k = SkipSpace(s, k + 8);
                    else break;
                }
                const bool isDef = k < s.size() && s[k] == '{';
                if (id.rfind("Needle", 0) == 0)
                {
                    if (isDef) { i = MatchBrace(s, k); continue; }   // deco::Needle の本体は数えない
                }
                else if (!isDef && IsLineishCallName(id))
                {
                    bool hit = false;
                    for (const auto& a : args) if (LineHasAccentIdent(a)) { hit = true; break; }
                    if (hit) { ++n; i = close + 1; continue; }
                }
            }
        }
        i = e;
    }
    return n;
}

ScanResult ScanText(const std::string& raw, bool countAccentLines, bool countHex = true)
{
    ScanResult r;
    const std::string s = Blank(raw);

    // IM_COL32(
    static const char kCol32[] = "IM_COL32(";
    for (size_t p = s.find(kCol32); p != std::string::npos; p = s.find(kCol32, p + 1))
    {
        if (p > 0 && IsIdent(s[p - 1])) continue;
        if (IsImCol32Definition(s, p)) continue;
        auto a = SplitArgs(s, p + sizeof(kCol32) - 2);
        ++r.colorLiterals;
        if (a.size() == 4)
        {
            double v[3];
            if (ParseNumber(a[0], v[0]) && ParseNumber(a[1], v[1]) && ParseNumber(a[2], v[2]) && IsYellow(float(v[0] / 255.0), float(v[1] / 255.0), float(v[2] / 255.0)))
                r.yellowLines.push_back(LineOf(s, p));
        }
    }

    // ImVec4 の色リテラル: 先頭の 3 つが [0,1] の数値リテラルのもの（矩形や位置の ImVec4 は数えない）。
    //   ImVec4(r,g,b,a) / ImVec4{r,g,b,a} / ImVec4 name(r,g,b,a) / ImVec4 name{r,g,b,a} を数える。
    //   ImVec4 name = ImVec4(...) は右辺の ImVec4( で 1 回だけ数える。
    static const char kVec4[] = "ImVec4";
    for (size_t p = s.find(kVec4); p != std::string::npos; p = s.find(kVec4, p + 1))
    {
        if (p > 0 && IsIdent(s[p - 1])) continue;
        size_t i = p + sizeof(kVec4) - 1;
        if (i < s.size() && IsIdent(s[i])) continue;
        i = SkipSpace(s, i);
        if (i < s.size() && IsIdent(s[i]))
        {
            // 宣言: ImVec4 <ident> ( or {
            while (i < s.size() && IsIdent(s[i])) ++i;
            i = SkipSpace(s, i);
            // コピーリスト初期化 ImVec4 name = { r, g, b, a }
            if (i < s.size() && s[i] == '=')
            {
                const size_t j = SkipSpace(s, i + 1);
                if (j < s.size() && s[j] == '{') i = j;
            }
        }
        if (i >= s.size() || (s[i] != '(' && s[i] != '{')) continue;
        auto a = SplitArgs(s, i);
        if (a.size() != 4) continue;
        double v[3];
        if (!(ParseNumber(a[0], v[0]) && ParseNumber(a[1], v[1]) && ParseNumber(a[2], v[2]))) continue;
        if (v[0] < 0 || v[0] > 1 || v[1] < 0 || v[1] > 1 || v[2] < 0 || v[2] > 1) continue;
        ++r.colorLiterals;
        if (IsYellow(float(v[0]), float(v[1]), float(v[2]))) r.yellowLines.push_back(LineOf(s, p));
    }

    // ImColor(r,g,b[,a]): 3 つが数値リテラル。小数点があれば [0,1]、無ければ 0..255 として黄を判定する。
    {
        static const char kImColor[] = "ImColor(";
        for (size_t p = s.find(kImColor); p != std::string::npos; p = s.find(kImColor, p + 1))
        {
            if (p > 0 && IsIdent(s[p - 1])) continue;
            auto a = SplitArgs(s, p + sizeof(kImColor) - 2);
            if (a.size() < 3 || a.size() > 4) continue;
            double v[3];
            if (!(ParseNumber(a[0], v[0]) && ParseNumber(a[1], v[1]) && ParseNumber(a[2], v[2]))) continue;
            ++r.colorLiterals;
            const bool isFloat = a[0].find('.') != std::string::npos || a[1].find('.') != std::string::npos || a[2].find('.') != std::string::npos;
            const double k = isFloat ? 1.0 : 255.0;
            if (IsYellow(float(v[0] / k), float(v[1] / k), float(v[2] / k))) r.yellowLines.push_back(LineOf(s, p));
        }
    }

    // th::Hex(0xRRGGBB[, alpha]) / Hex(0x...): 数値リテラルを渡すものだけ（テーマ本体 EditorTheme.h・ThemeVariants.h・*.gen.h は呼び出し側が外す）
    if (countHex)
    {
        static const char kHex[] = "Hex(";
        for (size_t p = s.find(kHex); p != std::string::npos; p = s.find(kHex, p + 1))
        {
            if (p > 0 && IsIdent(s[p - 1])) continue;
            auto a = SplitArgs(s, p + sizeof(kHex) - 2);
            if (a.empty()) continue;
            float cr, cg, cb;
            if (!IsHexColorLiteral(a[0], cr, cg, cb)) continue;
            ++r.colorLiterals;
            if (IsYellow(cr, cg, cb)) r.yellowLines.push_back(LineOf(s, p));
        }
    }

    // 黄の線を引く呼び出し（仕様はファイル先頭の説明）
    if (countAccentLines) r.accentLineDraws = CountAccentLineCalls(s);
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
        std::string extra;
        if (path.empty() || ls.fail() || n < 0 || (ls >> extra)) { a.ok = false; std::printf("allow: bad line: %s\n", line.c_str()); continue; }
        if (sect == "[colors]") a.colors[path] = n;
        else if (sect == "[accent_lines]") a.accentLines[path] = n;
        else if (sect == "[yellow_legacy]") a.yellowLegacy[path] = n;
        else { a.ok = false; std::printf("allow: unknown section: %s\n", sect.c_str()); }
    }
    return a;
}

fs::path ToPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

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

// テーマ本体（Hex を定義・使って表を作る場所）は Hex( の数に入れない。
bool IsThemeSource(const std::string& rel)
{
    if (rel == "src/editor/EditorTheme.h" || rel == "src/editor/ThemeVariants.h") return true;
    return rel.size() > 6 && rel.compare(rel.size() - 6, 6, ".gen.h") == 0;
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
            const auto rel8 = fs::relative(it->path(), root).generic_u8string();
            const std::string rel(rel8.begin(), rel8.end());
            ScanResult r = ScanText(ReadAll(it->path()), true, !IsThemeSource(rel));
            if (r.colorLiterals > 0 || !r.yellowLines.empty() || r.accentLineDraws > 0) t.files[rel] = std::move(r);
        }
    }
    return t;
}

// ---- 棚の判定（走査結果 x 許可リスト）。失敗の件数を返す。quiet なら表示しない（自己検査用）----
int Evaluate(const Totals& t, const Allow& allow, const fs::path& root, bool quiet, int* totalOut = nullptr, int* accentOut = nullptr)
{
    int fails = 0;
    auto fail = [&](const std::string& msg) { ++fails; if (!quiet) std::printf("FAIL %s\n", msg.c_str()); };
    auto fmt = [](const char* f, auto... args) { char buf[1024]; std::snprintf(buf, sizeof(buf), f, args...); return std::string(buf); };
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
                    fail(fmt("%s:%d  黄系（accent の色相 ±%.0f°、彩度 %.1f 以上）の直書き（棚 %d 件を超過）。accent 系のトークンを使うこと",
                             rel.c_str(), ln, kYellowHueTolDeg, kYellowMinSat, cap));
        }

        if (r.colorLiterals > 0)
        {
            const auto it = allow.colors.find(rel);
            const int cap = it == allow.colors.end() ? 0 : it->second;
            if (r.colorLiterals > cap)
                fail(fmt("%s: 直書き色 %d 件（許可 %d 件）。色は EditorTheme / gg のトークンから取ること。件数を増やす場合はメインに依頼", rel.c_str(), r.colorLiterals, cap));
            else if (r.colorLiterals < cap && !quiet)
                std::printf("note: %s は許可 %d 件に対し %d 件に減った（UiTokenLintTests --print で棚を締められる）\n", rel.c_str(), cap, r.colorLiterals);
        }
        if (r.accentLineDraws > 0)
        {
            const auto it = allow.accentLines.find(rel);
            const int cap = it == allow.accentLines.end() ? 0 : it->second;
            if (r.accentLineDraws > cap)
                fail(fmt("%s: 黄の線の呼び出し %d 件（許可 %d 件）。線は deco::Needle 経由で引くこと", rel.c_str(), r.accentLineDraws, cap));
        }
    }
    // 許可リストにあるのに存在しないファイル（改名・削除の取り残し）は、棚が緩むので赤にする
    for (const auto& [rel, n] : allow.colors)
        if (!fs::exists(root / ToPath(rel))) fail(fmt("許可リスト [colors] のファイルが無い: %s", rel.c_str()));
    // [yellow_legacy] と [accent_lines] は減る一方: 実測が棚より少なければ、空いた枠に別の分を足せてしまうので、棚を締めるまで赤にする
    auto strict = [&](const char* sect, const std::map<std::string, int>& shelf, auto count) {
        for (const auto& [rel, cap] : shelf)
        {
            if (!fs::exists(root / ToPath(rel))) { fail(fmt("許可リスト %s のファイルが無い: %s", sect, rel.c_str())); continue; }
            const auto f = t.files.find(rel);
            const int actual = f == t.files.end() ? 0 : count(f->second);
            if (actual < cap) fail(fmt("%s: %s は棚 %d 件に対し %d 件に減った。UiTokenLintTests --print で棚を締めること（棚は減る一方）", rel.c_str(), sect, cap, actual));
        }
    };
    strict("[yellow_legacy]", allow.yellowLegacy, [](const ScanResult& r) { return static_cast<int>(r.yellowLines.size()); });
    strict("[accent_lines]", allow.accentLines, [](const ScanResult& r) { return r.accentLineDraws; });
    if (totalOut) *totalOut = total;
    if (accentOut) *accentOut = accentTotal;
    return fails;
}

// ---- スキャナの自己検査 ----
void SelfTest()
{
    // 数える: IM_COL32 は引数が変数でも数える。色の ImVec4 だけ数える（矩形は [0,1] の外・変数混じりで外れる）
    {
        auto r = ScanText("auto a = IM_COL32(10, 20, 30, alpha);\nImVec4 b(0.1f, 0.2f, 0.3f, 1.0f);\nImVec4 rc = ImVec4(0, 0, 640, 480);\nImVec4 d = ImVec4(c.x, c.y, c.z, 0.5f);\n", true);
        CHECKF(r.colorLiterals == 2, "self: IM_COL32 + direct-init ImVec4 b(...) got %d", r.colorLiterals);
        auto r2 = ScanText("const ImVec4 kWarn (1.00f, 0.80f, 0.35f, 1.0f);\nImVec4 q{0.1f,0.2f,0.3f,1};\nauto z = ImVec4{0.1f,0.2f,0.3f,1};\nImVec4 c = ImVec4(0.1f,0.2f,0.3f,1);\nImVec4 Foo(float a, float b);\nImVec4 rc(0,0,640,480);\n", true);
        CHECKF(r2.colorLiterals == 4, "self: ImVec4 decl/brace/assign forms got %d", r2.colorLiterals);
        CHECKF(r2.yellowLines.size() == 1, "self: yellow kWarn (direct init) got %zu", r2.yellowLines.size());
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
        auto r = ScanText("#define IM_COL32(R,G,B,A) 0\n  #  define IM_COL32(R,G,B,A) 0\nint MY_IM_COL32(1,2,3,4);\nauto w = IM_COL32_WHITE;\n", true);
        CHECKF(r.colorLiterals == 0, "self: define/ident got %d", r.colorLiterals);
        // 他のマクロの本体に出る IM_COL32 は数える（黄も拾う）
        auto r2 = ScanText("#define COL_WARN IM_COL32(240,194,59,255)\n", true);
        CHECKF(r2.colorLiterals == 1 && r2.yellowLines.size() == 1, "self: macro body IM_COL32 got %d/%zu", r2.colorLiterals, r2.yellowLines.size());
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
    // 太さの関数 th::AccentPx() は色ではないので数えない（A5 の指摘 FU-E-8）。色の Accent が同じ呼び出しにあれば数える
    {
        auto r = ScanText("dl->AddLine(a, b, U(th::Border), th::AccentPx());\ndl->AddRect(a, b, U(th::Border), 0, 0, accentPx);\ndl->AddLine(a, b, U(th::Accent), th::AccentPx());\n", true);
        CHECKF(r.accentLineDraws == 1, "self: AccentPx is width not color got %d", r.accentLineDraws);
    }
    // 小文字・メンバ・k 接頭辞の accent 識別子も拾う。ImGuiKey_GraveAccent / onAccent は拾わない。PathStroke も数える
    {
        auto r = ScanText("dl->AddRect(a,b,U(pal.accent));\ndl->AddLine(a,b,U(gg::kGraphitePalette.accent),1);\ndl->AddLine(a,b,U(m_tok.accentHover));\n"
                          "dl->AddRect(a,b,kAccentCol);\ndl->AddRect(a,b,kUiAccentCol);\ndl->AddCircle(c,r,accentDim);\n"
                          "dl->PathStroke(U(th::Accent), 0, 2);\n"
                          "dl->AddLine(a,b,U(ImGuiKey_GraveAccent));\ndl->AddLine(a,b,U(onAccent));\ndl->AddLine(a,b,U(th::Border));\n", true);
        CHECKF(r.accentLineDraws == 7, "self: accent ident forms got %d", r.accentLineDraws);
        // 複数行にまたがる呼び出しも数える
        auto r2 = ScanText("dl->AddLine(a, b,\n    U(th::Accent), 2);\ndl->AddRect(a, b,\n  ColA(th::Accent, 0.55f), 0);\n", true);
        CHECKF(r2.accentLineDraws == 2, "self: multi-line calls got %d", r2.accentLineDraws);
    }
    // ラッパー（EdgeLine / TopHairline / DrawResolvedOutline / Glow*）も数える。ネストは 1 件。関数定義・Needle の本体と呼び出しは数えない
    {
        auto r = ScanText("deco::EdgeLine(dl, a, b, th::AccentHover, 0.35f);\nTopHairline(dl, r, th::AccentHover, 1);\n"
                          "uiedit::DrawResolvedOutline(dl, *rect, kAccentCol, 2);\ndeco::Glow(dl, r, th::Accent, 4);\nGlowRect(dl, r, accentCol);\n"
                          "EdgeLine(dl, Glow(a, th::Accent), th::Accent);\n"
                          "FillRect(dl, a, b, th::Accent);\nEdgeLine(dl, a, b, th::Border);\n", true);
        CHECKF(r.accentLineDraws == 6, "self: wrapper calls got %d", r.accentLineDraws);
        auto d = ScanText("void EdgeLine(ImDrawList* dl, ImU32 accent)\n{\n  dl->AddLine(a, b, accent);\n}\n", true);
        CHECKF(d.accentLineDraws == 1, "self: definition not counted, body AddLine counted got %d", d.accentLineDraws);
        auto n = ScanText("void Needle(ImDrawList* dl, float x)\n{\n  dl->AddLine(a, b, th::Accent);\n  dl->AddRect(a, b, th::Accent);\n}\n"
                          "void f() { deco::Needle(dl, th::Accent); dl->AddLine(a, b, th::Accent); }\n", true);
        CHECKF(n.accentLineDraws == 1, "self: Needle body excluded got %d", n.accentLineDraws);
    }
    // ImVec4 のコピーリスト初期化
    {
        auto r = ScanText("ImVec4 a = {0.1f, 0.2f, 0.3f, 1.0f};\nconst ImVec4 b = { 0.94f, 0.76f, 0.23f, 1.0f };\nImVec4 c = {};\n", true);
        CHECKF(r.colorLiterals == 2 && r.yellowLines.size() == 1, "self: ImVec4 copy-list-init got %d/%zu", r.colorLiterals, r.yellowLines.size());
    }
    // Hex(0x...) と ImColor(...) の直書き。変数引数の Hex・識別子の一部（FormatXHex）は数えない。theme ファイルは countHex=false
    {
        auto r = ScanText("a = th::Hex(0xF0C23B);\nb = Hex(0x0A1630, 0.5f);\nc = th::Hex(d.gradA);\nd = FormatGuidHex(1);\ne = ImColor(240,194,59);\nf = ImColor(0.1f,0.2f,0.3f,1.0f);\n", true);
        CHECKF(r.colorLiterals == 4, "self: Hex/ImColor count got %d", r.colorLiterals);
        CHECKF(r.yellowLines.size() == 2, "self: Hex/ImColor yellow got %zu", r.yellowLines.size());
        auto r2 = ScanText("a = Hex(0xF0C23B);\n", true, false);
        CHECKF(r2.colorLiterals == 0, "self: countHex=false got %d", r2.colorLiterals);
    }
    // 棚の判定（許可リストの読み込みと上限の比較）
    {
        // 他レーンの ctest と %TEMP% を共有するので、実行ごとに一意の作業ディレクトリにする
        std::random_device rd;
        const fs::path dir = fs::temp_directory_path() / ("ui_token_lint_selftest_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(rd()));
        std::error_code ec;
        fs::create_directories(dir / "src", ec);
        { std::ofstream f(dir / "src" / "a.cpp"); f << "x\n"; }
        auto writeAllow = [&](const char* txt) { const fs::path ap = dir / "allow.txt"; { std::ofstream f(ap); f << txt; } return LoadAllow(ap); };

        const Allow good = writeAllow("[colors]\nsrc/a.cpp 2\n[yellow_legacy]\nsrc/a.cpp 1\n[accent_lines]\nsrc/a.cpp 1\n");
        CHECK(good.ok && good.colors.at("src/a.cpp") == 2 && good.yellowLegacy.at("src/a.cpp") == 1 && good.accentLines.at("src/a.cpp") == 1);
        CHECK(!writeAllow("[bogus]\nsrc/a.cpp 1\n").ok);          // 未知の節
        CHECK(!writeAllow("[colors]\nsrc/a.cpp\n").ok);           // 件数が無い
        CHECK(!writeAllow("[colors]\nsrc/a.cpp x\n").ok);         // 件数が数でない
        { Allow none = LoadAllow(dir / "nonexistent.txt"); CHECK(!none.ok); }

        auto mk = [](int col, size_t yel, int acc) { Totals t; ScanResult r; r.colorLiterals = col; r.yellowLines.assign(yel, 1); r.accentLineDraws = acc; t.files["src/a.cpp"] = r; return t; };
        CHECK(Evaluate(mk(2, 1, 1), good, dir, true) == 0);        // ちょうど棚
        CHECK(Evaluate(mk(1, 1, 1), good, dir, true) == 0);        // colors は減る分は通る（note のみ）
        CHECK(Evaluate(mk(2, 0, 1), good, dir, true) > 0);         // yellow_legacy が棚より減ったら棚を締めるまで赤
        CHECK(Evaluate(mk(2, 1, 0), good, dir, true) > 0);         // accent_lines も同じ
        CHECK(Evaluate(mk(3, 1, 1), good, dir, true) > 0);         // colors 超過
        CHECK(Evaluate(mk(2, 2, 1), good, dir, true) > 0);         // yellow_legacy 超過
        CHECK(Evaluate(mk(2, 1, 2), good, dir, true) > 0);         // accent_lines 超過
        CHECK(Evaluate(mk(1, 1, 0), Allow{}, dir, true) > 0);      // 棚に載っていないファイルは 0 件扱い
        const Allow missing = writeAllow("[colors]\nsrc/gone.cpp 1\n");
        CHECK(Evaluate(Totals{}, missing, dir, true) > 0);         // 棚にあるファイルが無い
        fs::remove_all(dir, ec);
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
    const fs::path root = fs::path(std::u8string(reinterpret_cast<const char8_t*>(UI_LINT_ROOT)));
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
    g_fail += Evaluate(t, allow, root, false, &total, &accentTotal);

    std::printf("ui_token_lint: 直書き色 %d 件（%zu ファイル）、黄の線の呼び出し %d 件。\n", total, t.files.size(), accentTotal);
    if (g_fail)
    {
        std::printf("UiTokenLintTests: %d 件の失敗\n", g_fail);
        return 1;
    }
    std::printf("UiTokenLintTests: OK\n");
    return 0;
}
