#pragma once
// ===========================================================================
// MCP マニフェストのメタ情報（McpMeta）と、依存ゼロの補助関数
// ---------------------------------------------------------------------------
// ★nlohmann にも entt にも依存しない（標準ライブラリだけ）。tests/mcp_manifest_test.cpp が
//   エンジンをリンクせずにこのヘッダだけを include して単体テストできるようにするため。
//   JSON への変換は ApplicationMcpManifest.cpp 側（nlohmann を使える TU）で行う。
//
// 何のためにあるか（docs/MCP_ENHANCEMENT_DESIGN.md §4.2.2）
//   Application::McpDefine(names, paramSpec, fn) の paramSpec は "key:type,..." の文字列だけで、
//   カテゴリ・副作用・タイムアウト・モード・別名などが TS 側の手書き表に散らばっていた。
//   McpMeta はそれらを method 単位で 1 か所に集める。TS サーバはこれを describe_mcp_manifest で
//   引き、エンジンを再ビルドしても TS を再起動せずに新しい method を呼べる。
//
// 使い方
//   - 新しい method は McpDefine(names, McpMeta{...}, DX12E_MCP_HANDLER {...}) で登録する。
//     meta を直接渡した method は params の必須/型/列挙/範囲がディスパッチャで中央検査される。
//   - 既存の McpDefine(names, "key:type,...", fn) は書き換えない。それらの meta は
//     ApplicationMcpManifestData.inc（データ表）が起動時に流し込む。
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <string>
#include <vector>

namespace dx12e
{

// 副作用の分類（設計書 §4.3.4）。文字列は "read|write_scene|write_setting|write_file|runtime|guarded"。
enum class McpEffect
{
    Read,          // 何も変えない（get / list / describe など）
    WriteScene,    // シーンのデータを変える（Undo に積まれる）
    WriteSetting,  // 描画 / シーン / ライト / エディタカメラなど設定を変える
    WriteFile,     // ファイルを書く（Undo 対象外）
    Runtime,       // Play/Stop・入力・時間送り・ベンチなど実行状態に効く
    Guarded,       // 取り返しがつかない / 外部へ出る（git push・eval_lua・delete_asset など）
};

inline const char* McpEffectName(McpEffect e)
{
    switch (e)
    {
    case McpEffect::Read:         return "read";
    case McpEffect::WriteScene:   return "write_scene";
    case McpEffect::WriteSetting: return "write_setting";
    case McpEffect::WriteFile:    return "write_file";
    case McpEffect::Runtime:      return "runtime";
    case McpEffect::Guarded:      return "guarded";
    }
    return "write_scene";
}

inline bool McpEffectFromName(const std::string& s, McpEffect& out)
{
    static const McpEffect kAll[] = {McpEffect::Read, McpEffect::WriteScene, McpEffect::WriteSetting,
                                     McpEffect::WriteFile, McpEffect::Runtime, McpEffect::Guarded};
    for (McpEffect e : kAll)
        if (s == McpEffectName(e)) { out = e; return true; }
    return false;
}

// 引数の型名。"key:type" の type と McpParam::type はこの集合のどれか。
inline bool McpParamTypeValid(const std::string& t)
{
    static const char* const kTypes[] = {"bool", "int", "number", "string", "vec2", "vec3", "vec4",
                                         "entityRef", "assetPath", "enum", "object", "array", "any"};
    for (const char* k : kTypes)
        if (t == k) return true;
    return false;
}

inline bool McpModeValid(const std::string& m) { return m == "any" || m == "editor" || m == "playing"; }
inline bool McpDryRunValid(const std::string& d) { return d == "none" || d == "native"; }
// expose: MCP サーバ(TS)が tools/list に直接載せるか。"" = 載せない(dx12_tool_search / dx12_call で使う)、"core" = Core に昇格
// (TS 側は manifestHash の変化を検知して notifications/tools/list_changed を送り、再起動なしで tools/list に出す)。
inline bool McpExposeValid(const std::string& e) { return e.empty() || e == "core"; }

struct McpParam
{
    std::string              name;
    std::string              type = "any";
    bool                     required = false;
    std::vector<std::string> enumValues;      // type=="enum" のときの有効値
    bool                     hasMin = false;
    double                   min = 0.0;
    bool                     hasMax = false;
    double                   max = 0.0;
    bool                     hasDefault = false;
    std::string              defaultJson;     // JSON のリテラル文字列（例 "0.5" / "true" / "\"abc\""）
    std::string              desc;
    // true の引数だけディスパッチャが中央検査する（必須 / 型 / 列挙 / 範囲）。
    // meta を McpDefine へ直接渡した method は全部 true。データ表由来（既存 method）は false。
    bool                     enforce = false;
};

struct McpNext    { std::string tool, when; };            // 次に呼ぶと良いツールと理由
struct McpExample { std::string argsJson, note; };        // 撃ち方の例（args は JSON 文字列）

struct McpMeta
{
    std::string              summary;                     // 1〜2 文（約 140 字まで）
    std::string              keywords;                    // 検索用の語（空白区切り）
    std::string              category;                    // entity / scene / render / ...
    std::string              group;                       // 例 "render_setting" / "terrain" / "navmesh"
    std::string              target;                      // group 内の対象または操作（例 "ssao" / "sculpt"）
    McpEffect                effect = McpEffect::WriteScene;
    std::string              mode = "any";                // "any" | "editor" | "playing"
    int                      timeoutMs = 10000;
    bool                     idempotent = false;
    bool                     deferred = false;            // 応答がフレーム境界まで遅れる（遅延同期）
    std::string              dryRun = "none";             // "none" | "native"
    std::vector<std::string> aliases;                     // 旧 TS ツール名（dx12_xxx）
    std::vector<McpParam>    params;
    std::vector<McpNext>     next;
    std::vector<McpExample>  examples;
    std::string              expose;                      // "" | "core"（末尾に足した。位置指定の初期化を壊さない）
};

// ---------------------------------------------------------------------------
// paramSpec "key:type,key:type,..." → McpParam の並び。
// type が無い / 未知なら "any"。"親.子"（入れ子のキー）はそのまま名前に残す。
// 空要素は捨てる。同じキーが複数あれば最初のもの。
// ---------------------------------------------------------------------------
inline std::vector<McpParam> McpParseParamSpec(const std::string& spec)
{
    std::vector<McpParam> out;
    size_t pos = 0;
    while (pos <= spec.size() && !spec.empty())
    {
        const size_t comma = spec.find(',', pos);
        std::string one = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        // 前後の空白を落とす
        while (!one.empty() && (one.front() == ' ' || one.front() == '\t' || one.front() == '\n')) one.erase(one.begin());
        while (!one.empty() && (one.back() == ' ' || one.back() == '\t' || one.back() == '\n')) one.pop_back();
        if (!one.empty())
        {
            const size_t colon = one.find(':');
            McpParam p;
            p.name = one.substr(0, colon);
            if (colon != std::string::npos)
            {
                std::string t = one.substr(colon + 1);
                p.type = McpParamTypeValid(t) ? t : std::string("any");
            }
            const bool dup = std::any_of(out.begin(), out.end(),
                                         [&](const McpParam& q) { return q.name == p.name; });
            if (!p.name.empty() && !dup) out.push_back(std::move(p));
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

// McpParam の並び → "key:type,..."（describe_mcp_params 互換の申告表を meta から作る）。
inline std::string McpParamsToSpec(const std::vector<McpParam>& params)
{
    std::string s;
    for (const McpParam& p : params)
    {
        if (!s.empty()) s += ',';
        s += p.name;
        s += ':';
        // 申告表の型語彙は従来の bool/int/number/string/vec3/object/any。新しい型は any へ丸める。
        static const char* const kLegacy[] = {"bool", "int", "number", "string", "vec3", "object", "any"};
        const bool legacy = std::any_of(std::begin(kLegacy), std::end(kLegacy),
                                        [&](const char* k) { return p.type == k; });
        s += legacy ? p.type : std::string(p.type == "enum" || p.type == "assetPath" ? "string" : "any");
    }
    return s;
}

// ---------------------------------------------------------------------------
// FNV-1a 64 とマニフェストハッシュ
// ---------------------------------------------------------------------------
constexpr uint64_t kMcpFnvOffset = 14695981039346656037ull;
constexpr uint64_t kMcpFnvPrime  = 1099511628211ull;

inline uint64_t McpFnv1a64(const std::string& s, uint64_t h = kMcpFnvOffset)
{
    for (unsigned char c : s)
    {
        h ^= static_cast<uint64_t>(c);
        h *= kMcpFnvPrime;
    }
    return h;
}

inline std::string McpHex16(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

// JSON 文字列リテラル（引用符付き）。制御文字は \u00XX、UTF-8 の多バイトはそのまま通す。
inline std::string McpJsonQuote(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
                o += b;
            }
            else o += static_cast<char>(c);
        }
    }
    o += '"';
    return o;
}

inline std::string McpNumberText(double v)
{
    char b[40];
    std::snprintf(b, sizeof(b), "%.17g", v);
    return std::string(b);
}

// メタ 1 件を「キー順固定」の JSON 文字列にする（ハッシュ用）。
// ★unordered_map の走査順にも nlohmann の実装にも依存しない。キーは下に書いた順で固定。
//   フィールドを足したら末尾へ足すこと（順序を変えるとハッシュが全部変わる）。
inline std::string McpMetaCanonical(const std::string& name, const McpMeta& m, const std::string& paramSpec)
{
    auto strArr = [](const std::vector<std::string>& v)
    {
        std::string o = "[";
        for (size_t i = 0; i < v.size(); ++i) { if (i) o += ','; o += McpJsonQuote(v[i]); }
        return o + "]";
    };
    std::string o = "{";
    o += "\"name\":" + McpJsonQuote(name);
    o += ",\"spec\":" + McpJsonQuote(paramSpec);
    o += ",\"summary\":" + McpJsonQuote(m.summary);
    o += ",\"keywords\":" + McpJsonQuote(m.keywords);
    o += ",\"category\":" + McpJsonQuote(m.category);
    o += ",\"group\":" + McpJsonQuote(m.group);
    o += ",\"target\":" + McpJsonQuote(m.target);
    o += ",\"effect\":" + McpJsonQuote(McpEffectName(m.effect));
    o += ",\"mode\":" + McpJsonQuote(m.mode);
    o += ",\"timeoutMs\":" + std::to_string(m.timeoutMs);
    o += std::string(",\"idempotent\":") + (m.idempotent ? "true" : "false");
    o += std::string(",\"deferred\":") + (m.deferred ? "true" : "false");
    o += ",\"dryRun\":" + McpJsonQuote(m.dryRun);
    o += ",\"aliases\":" + strArr(m.aliases);
    o += ",\"params\":[";
    for (size_t i = 0; i < m.params.size(); ++i)
    {
        const McpParam& p = m.params[i];
        if (i) o += ',';
        o += "{\"name\":" + McpJsonQuote(p.name);
        o += ",\"type\":" + McpJsonQuote(p.type);
        o += std::string(",\"required\":") + (p.required ? "true" : "false");
        o += ",\"enum\":" + strArr(p.enumValues);
        o += ",\"min\":" + (p.hasMin ? McpNumberText(p.min) : std::string("null"));
        o += ",\"max\":" + (p.hasMax ? McpNumberText(p.max) : std::string("null"));
        o += ",\"default\":" + (p.hasDefault ? p.defaultJson : std::string("null"));
        o += ",\"desc\":" + McpJsonQuote(p.desc);
        o += std::string(",\"enforce\":") + (p.enforce ? "true" : "false") + "}";
    }
    o += "],\"next\":[";
    for (size_t i = 0; i < m.next.size(); ++i)
    {
        if (i) o += ',';
        o += "{\"tool\":" + McpJsonQuote(m.next[i].tool) + ",\"when\":" + McpJsonQuote(m.next[i].when) + "}";
    }
    o += "],\"examples\":[";
    for (size_t i = 0; i < m.examples.size(); ++i)
    {
        if (i) o += ',';
        o += "{\"args\":" + McpJsonQuote(m.examples[i].argsJson) + ",\"note\":" + McpJsonQuote(m.examples[i].note) + "}";
    }
    o += "]";
    // expose は空のときは何も足さない（既存 method のハッシュを変えない。フィールドを足したら末尾へ）
    if (!m.expose.empty()) o += ",\"expose\":" + McpJsonQuote(m.expose);
    o += "}";
    return o;
}

// ---------------------------------------------------------------------------
// 近い名前の提案（did-you-mean）。編集距離 + 大文字小文字 + 前方/部分一致。
// ---------------------------------------------------------------------------

// UTF-8 → コードポイント列（不正なバイトは 1 バイト 1 文字として通す）。日本語名の編集距離が
// バイト単位で 3 倍に膨らまないようにする。
inline std::u32string McpDecodeUtf8(const std::string& s)
{
    std::u32string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        char32_t cp = c;
        if (c >= 0xF0 && c < 0xF8)      { len = 4; cp = c & 0x07u; }
        else if (c >= 0xE0)             { len = (c < 0xF0) ? 3 : 1; cp = c & 0x0Fu; }
        else if (c >= 0xC0)             { len = 2; cp = c & 0x1Fu; }
        if (len == 1 || i + len > s.size()) { out.push_back(c); ++i; continue; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) { out.push_back(c); ++i; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// コードポイント列 → UTF-8。
inline std::string McpEncodeUtf8(const std::u32string& s)
{
    std::string o;
    for (char32_t c : s)
    {
        if (c < 0x80)         { o += static_cast<char>(c); }
        else if (c < 0x800)   { o += static_cast<char>(0xC0 | (c >> 6));
                                o += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += static_cast<char>(0xE0 | (c >> 12));
                                o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                                o += static_cast<char>(0x80 | (c & 0x3F)); }
        else                  { o += static_cast<char>(0xF0 | (c >> 18));
                                o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                                o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                                o += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return o;
}

// ASCII だけ小文字化（それ以外はそのまま）。
inline std::u32string McpFoldCase(std::u32string s)
{
    for (char32_t& c : s)
        if (c >= U'A' && c <= U'Z') c = static_cast<char32_t>(c - U'A' + U'a');
    return s;
}

// 隣接入れ替えを 1 手と数える編集距離（optimal string alignment）。
inline int McpEditDistance(const std::u32string& a, const std::u32string& b)
{
    const size_t n = a.size(), m = b.size();
    if (n == 0) return static_cast<int>(m);
    if (m == 0) return static_cast<int>(n);
    std::vector<int> prev2(m + 1), prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j)
        {
            const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int v = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                v = (std::min)(v, prev2[j - 2] + 1);
            cur[j] = v;
        }
        prev2.swap(prev);
        prev.swap(cur);
    }
    return prev[m];
}

inline int McpEditDistance(const std::string& a, const std::string& b)
{
    return McpEditDistance(McpDecodeUtf8(a), McpDecodeUtf8(b));
}

// target に近い候補を最大 maxN 件、近い順に返す。
//   0    大文字小文字だけ違う（最優先）
//   d*10 編集距離 d（d <= max(2, len/3）まで）
//   15+差 前方 / 部分一致（短い方が 3 文字以上のとき）
// 同点は候補の並び順。target と完全一致する候補は含めない（打ち間違いの提案なので）。
// 候補が 1 万個あっても 1 走査（O(N * len^2) で len は名前の長さ）で済む素朴な実装。
inline std::vector<std::string> McpSuggest(const std::string& target,
                                           const std::vector<std::string>& candidates,
                                           size_t maxN = 5)
{
    std::vector<std::string> out;
    if (target.empty() || maxN == 0) return out;
    const std::u32string t  = McpDecodeUtf8(target);
    const std::u32string tl = McpFoldCase(t);
    const int th = (std::max)(2, static_cast<int>(t.size()) / 3);

    struct Hit { int score; size_t idx; };
    std::vector<Hit> hits;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const std::string& c = candidates[i];
        if (c == target) continue;
        const std::u32string cu = McpDecodeUtf8(c);
        const std::u32string cl = McpFoldCase(cu);
        if (cl == tl) { hits.push_back({0, i}); continue; }

        int best = 1 << 30;
        const int lenDiff = static_cast<int>(cu.size() > t.size() ? cu.size() - t.size() : t.size() - cu.size());
        // 長さが離れすぎているものは DP を回さない（部分一致は下で別に見る）
        if (lenDiff <= th)
        {
            const int d = McpEditDistance(tl, cl);
            if (d <= th) best = d * 10 + (lenDiff > 0 ? 1 : 0);
        }
        const std::u32string& shortS = (cl.size() <= tl.size()) ? cl : tl;
        const std::u32string& longS  = (cl.size() <= tl.size()) ? tl : cl;
        if (shortS.size() >= 3 && longS.find(shortS) != std::u32string::npos)
            best = (std::min)(best, 15 + lenDiff);
        if (best < (1 << 30)) hits.push_back({best, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score < b.score; });
    for (const Hit& h : hits)
    {
        if (out.size() >= maxN) break;
        out.push_back(candidates[h.idx]);
    }
    return out;
}

} // namespace dx12e
