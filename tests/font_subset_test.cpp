// UI 書体のサブセット（tools/subset_fonts.py の生成物）の検査（W1-A1 / GG-2a）。GPU 不要。
//   ・生成物が揃い、全体が 12MB 以下、splash_subset.ttf が 200KB 以下
//   ・コミット済みの字の一覧 third_party/fonts/charset_ui.txt（ソースから抜き出したもの。最新かどうかは見ない。ソース全体との照合は
//     tools/subset_fonts.py --check = ctest の FontSubsetCheck が本番）の字が、サブセットのどれかに入っている
//     （入れない字は charset_system_fallback.txt の許可リストにある字だけ）
//   ・PlemolJP 由来の 2 出力が Plex の字だけを持つこと（Hack 由来の字形 = Bitstream Vera License を入れない）
//   ・起動画面の書体（splash_subset.ttf）が起動画面の字を全部持ち、等幅（和欧 1:2）であること
//   ・OFL / Bitstream Vera の予約名（Plex / PlemolJP / Source / Bitstream / Vera）を、全出力（起動画面も含む）の名前表
//     nameID 1/4/6/16/21 に残していないこと / ライセンス文が同梱されていること
// 字の表は ImGui に同梱の stb_truetype（static）で読む。
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef FONT_TEST_ROOT
#error FONT_TEST_ROOT must be defined
#endif

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

std::string Path(const char* rel) { return std::string(FONT_TEST_ROOT) + "/" + rel; }

bool ReadFile(const std::string& p, std::vector<unsigned char>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// UTF-8 → コードポイント（comments が真なら # で始まる行を読み飛ばす。改行は常に読み飛ばす）
std::set<uint32_t> ReadCodepoints(const std::string& p, bool comments = false)
{
    std::vector<unsigned char> b;
    std::set<uint32_t> out;
    if (!ReadFile(p, b)) return out;
    size_t i = 0;
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) i = 3;
    bool lineStart = true, comment = false;
    while (i < b.size())
    {
        const unsigned char c = b[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < b.size())      { cp = ((c & 7u) << 18) | ((b[i + 1] & 63u) << 12) | ((b[i + 2] & 63u) << 6) | (b[i + 3] & 63u); n = 4; }
        else if (c >= 0xE0 && i + 2 < b.size()) { cp = ((c & 15u) << 12) | ((b[i + 1] & 63u) << 6) | (b[i + 2] & 63u); n = 3; }
        else if (c >= 0xC0 && i + 1 < b.size()) { cp = ((c & 31u) << 6) | (b[i + 1] & 63u); n = 2; }
        i += n;
        if (cp == '\n') { lineStart = true; comment = false; continue; }
        if (cp == '\r') continue;
        if (comments && lineStart && cp == '#') comment = true;
        lineStart = false;
        if (!comment) out.insert(cp);
    }
    return out;
}

struct Face
{
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
    bool Load(const char* rel)
    {
        if (!ReadFile(Path(rel), data)) return false;
        ok = stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)) != 0;
        return ok;
    }
    bool Has(uint32_t cp) const { return ok && stbtt_FindGlyphIndex(&info, (int)cp) != 0; }
    int Advance(uint32_t cp) const
    {
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, stbtt_FindGlyphIndex(&info, (int)cp), &adv, &lsb);
        return adv;
    }
    // name テーブルの nameID（全プラットフォーム・全言語）の文字列を小文字の ASCII 風にして返す
    // （UTF-16BE は 1 バイトおきに取り、非 ASCII は '?' にする。予約名の部分一致を見るだけなので十分）
    std::vector<std::pair<int, std::string>> Names(std::initializer_list<int> ids) const
    {
        std::vector<std::pair<int, std::string>> out;
        auto u16 = [&](size_t o) { return o + 1 < data.size() ? (unsigned)(data[o] << 8 | data[o + 1]) : 0u; };
        auto u32 = [&](size_t o) { return (u16(o) << 16) | u16(o + 2); };
        const size_t dir = stbtt_GetFontOffsetForIndex(data.data(), 0);
        const unsigned nTab = u16(dir + 4);
        size_t nameOff = 0;
        for (unsigned i = 0; i < nTab; ++i)
        {
            const size_t rec = dir + 12 + 16 * i;
            if (rec + 16 <= data.size() && std::memcmp(&data[rec], "name", 4) == 0) nameOff = u32(rec + 8);
        }
        if (!nameOff) return out;
        const unsigned cnt = u16(nameOff + 2);
        const size_t strBase = nameOff + u16(nameOff + 4);
        for (unsigned i = 0; i < cnt; ++i)
        {
            const size_t r = nameOff + 6 + 12 * i;
            const int pid = (int)u16(r), nid = (int)u16(r + 6);
            const size_t len = u16(r + 8), off = strBase + u16(r + 10);
            bool want = false;
            for (int id : ids) want = want || id == nid;
            if (!want || off + len > data.size()) continue;
            std::string s;
            if (pid == 0 || pid == 3)
            {
                for (size_t k = 0; k + 1 < len; k += 2)
                {
                    const unsigned c = u16(off + k);
                    s.push_back(c < 0x80 ? (char)std::tolower((int)c) : '?');
                }
            }
            else for (size_t k = 0; k < len; ++k) s.push_back((char)std::tolower((int)data[off + k]));
            out.emplace_back(nid, s);
        }
        return out;
    }
};

const char* kUiFonts[] = {
    "third_party/fonts/ibm-plex/subset/UnoUISansJP-Regular.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUISansJP-Medium.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUISansJP-SemiBold.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUISans-Regular.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUISans-Medium.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUISans-SemiBold.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUIMono-Regular.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUIMono-Medium.ttf",
    "third_party/fonts/ibm-plex/subset/UnoUIMono-Light.ttf",
    "third_party/fonts/plemoljp/subset/UnoUIMonoJP-Regular.ttf",
    "third_party/fonts/noto-sans-jp/subset/NotoSansJP-Regular.ttf",
};
}

int main()
{
    // 1) 生成物とサイズ
    std::vector<Face> faces(sizeof(kUiFonts) / sizeof(kUiFonts[0]));
    size_t total = 0;
    for (size_t i = 0; i < faces.size(); ++i)
    {
        CHECKF(faces[i].Load(kUiFonts[i]), "load %s", kUiFonts[i]);
        total += faces[i].data.size();
    }
    CHECKF(total <= 12u * 1024u * 1024u, "fonts total %zu bytes > 12MB", total);

    // 2) 抜き出した UI 文字が、サブセットのどれかに入っている
    const std::set<uint32_t> need = ReadCodepoints(Path("third_party/fonts/charset_ui.txt"));
    const std::set<uint32_t> allow = ReadCodepoints(Path("third_party/fonts/charset_system_fallback.txt"), true);
    CHECK(need.size() > 1000);   // 空読みの検出（抜き出した字は 1000 字を超える）
    size_t missing = 0;
    for (uint32_t cp : need)
    {
        bool any = false;
        for (const Face& f : faces) any = any || f.Has(cp);
        if (!any && !allow.count(cp))
        {
            if (missing++ < 20) std::printf("  not covered: U+%04X\n", cp);
        }
    }
    CHECKF(missing == 0, "%zu UI chars are in no subset font and not in charset_system_fallback.txt", missing);
    // 必須の字（第 1 水準の代表、かな、図番の字）
    for (uint32_t cp : { 0x4E9Cu /*亜*/, 0x5F37u /*強*/, 0x30A4u /*イ*/, 0x3042u /*あ*/, (uint32_t)'U', (uint32_t)'N', (uint32_t)'-', (uint32_t)'.' })
        CHECKF(faces[0].Has(cp), "Sans JP Regular lacks U+%04X", cp);

    // 2b) PlemolJP 由来の出力（UI 用 / 起動画面）の収録字は、Plex 由来の出力（Sans JP Regular / Mono Regular）にもある
    //     （Plex に無い字は PlemolJP が Hack から借りた字形。Bitstream Vera License は許可ライセンスの外）
    {
        Face splashF;
        CHECK(splashF.Load("assets/editor/splash/fonts/splash_subset.ttf"));
        for (uint32_t cp = 0x20; cp <= 0xFFFF; ++cp)
        {
            const bool plex = faces[0].Has(cp) || faces[6].Has(cp);
            CHECKF(!faces[9].Has(cp) || plex, "UnoUIMonoJP has U+%04X, which Plex lacks (Hack-derived?)", cp);
            CHECKF(!splashF.Has(cp) || plex, "splash_subset.ttf has U+%04X, which Plex lacks (Hack-derived?)", cp);
        }
    }

    // 3) 欧文の面（Sans / Mono）は ASCII を全部持つ
    for (size_t idx : { (size_t)3, (size_t)4, (size_t)5, (size_t)6, (size_t)7, (size_t)8 })
        for (uint32_t cp = 0x20; cp < 0x7F; ++cp)
            CHECKF(faces[idx].Has(cp), "%s lacks U+%04X", kUiFonts[idx], cp);

    // 4) 予約フォント名（RFN）を名前表に残さない（OFL の改変版の規則）。全出力が対象（起動画面の書体も、Noto / PlemolJP も）
    Face splash;
    CHECK(splash.Load("assets/editor/splash/fonts/splash_subset.ttf"));
    {
        const char* kRfn[] = { "plex", "plemoljp", "source", "bitstream", "vera" };
        std::vector<std::pair<std::string, const Face*>> all;
        for (size_t i = 0; i < faces.size(); ++i) all.emplace_back(kUiFonts[i], &faces[i]);
        all.emplace_back("splash_subset.ttf", &splash);
        for (const auto& f : all)
        {
            const auto names = f.second->Names({ 1, 4, 6, 16, 21 });
            CHECKF(!names.empty(), "no name records in %s", f.first.c_str());
            for (const auto& n : names)
                for (const char* r : kRfn)
                    CHECKF(n.second.find(r) == std::string::npos, "%s nameID %d '%s' contains reserved name '%s'", f.first.c_str(), n.first, n.second.c_str(), r);
        }
    }
    for (const char* lic : { "third_party/licenses/IBM-Plex-OFL.txt", "third_party/licenses/Noto-OFL.txt", "third_party/licenses/PlemolJP-OFL.txt" })
    {
        std::vector<unsigned char> b;
        CHECKF(ReadFile(Path(lic), b) && b.size() > 500, "license %s", lic);
    }

    // 5) 起動画面の書体
    CHECKF(splash.data.size() <= 200u * 1024u, "splash_subset.ttf %zu bytes > 200KB", splash.data.size());
    for (uint32_t cp : ReadCodepoints(Path("third_party/fonts/charset_splash.txt")))
        CHECKF(splash.Has(cp), "splash_subset.ttf lacks U+%04X", cp);
    if (splash.ok)
    {
        // 等幅（ASCII は同じ幅、全角は半角のちょうど 2 倍）
        const int w = splash.Advance('0');
        CHECK(w > 0);
        for (uint32_t cp : { (uint32_t)'i', (uint32_t)'W', (uint32_t)'%', (uint32_t)'.', (uint32_t)'1' })
            CHECKF(splash.Advance(cp) == w, "splash not monospaced at U+%04X", cp);
        CHECK(splash.Has(0x3042) && splash.Advance(0x3042) == 2 * w);
    }

    if (g_fail == 0) std::printf("font_subset_test: OK (fonts %.2f MB)\n", total / 1048576.0);
    return g_fail == 0 ? 0 : 1;
}
