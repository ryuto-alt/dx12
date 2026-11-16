#pragma once
// ===========================================================================
// AI クライアント（Codex / Claude Code / Claude Desktop / Cursor）の設定ファイルへ
// MCP サーバ dx12-engine を「足す / 置き換える」純ロジック。ファイル I/O も Win32 も使わない。
//   - Codex        : ~/.codex/config.toml の [mcp_servers.<name>]（TOML をテキストとして編集）
//   - それ以外      : JSON の "mcpServers".<name>（nlohmann::ordered_json で既存キーの順序を保つ）
// ★方針: 他のサーバ・他の設定は 1 文字も変えない。自分の名前のエントリだけを差し替える。
//   読めない（壊れた JSON / mcpServers が object でない）ときは書かずに失敗を返す＝丸ごと上書きしない。
// テスト: tests/mcp_client_config_test.cpp
// ===========================================================================
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dx12e::mcpcfg
{

struct ServerSpec
{
    std::string name = "dx12-engine";
    std::string command;                                        // node.exe の絶対パス
    std::vector<std::string> args;                              // { index.ts の絶対パス }
    std::vector<std::pair<std::string, std::string>> env;       // 例 { "DX12_MCP_SURFACE", "core" }
    int startupTimeoutSec = 0;                                  // Codex の startup_timeout_sec（0 = 書かない）
};

// TOML の基本文字列（"..."）。\ と " と制御文字をエスケープする。UTF-8 はそのまま通す。
inline std::string TomlString(const std::string& s)
{
    std::string o = "\"";
    for (const char ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c)
        {
        case '\\': o += "\\\\"; break;
        case '"':  o += "\\\""; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7F)
            {
                static const char* hex = "0123456789ABCDEF";
                o += "\\u00";
                o += hex[(c >> 4) & 0xF];
                o += hex[c & 0xF];
            }
            else o += ch;
        }
    }
    o += '"';
    return o;
}

// TOML のキー（裸で書けるなら裸、だめなら "..."）
inline std::string TomlKey(const std::string& k)
{
    bool bare = !k.empty();
    for (const char c : k)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            bare = false;
    return bare ? k : TomlString(k);
}

namespace detail
{

inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

// ドット区切りのキー列を分解する（"a".b.'c d' → {a, b, c d}）。失敗なら空。
// 引用キーのエスケープは \" と \\ だけ解釈する（設定ファイルのテーブル名にはそれで十分）。
inline std::vector<std::string> SplitDottedKey(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    const size_t n = s.size();
    auto skipWs = [&]() { while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i; };
    while (true)
    {
        skipWs();
        if (i >= n) return {};
        std::string part;
        if (s[i] == '"' || s[i] == '\'')
        {
            const char q = s[i++];
            bool closed = false;
            while (i < n)
            {
                if (q == '"' && s[i] == '\\' && i + 1 < n) { part += s[i + 1]; i += 2; continue; }
                if (s[i] == q) { closed = true; ++i; break; }
                part += s[i++];
            }
            if (!closed) return {};
        }
        else
        {
            while (i < n && s[i] != '.' && s[i] != ' ' && s[i] != '\t') part += s[i++];
            if (part.empty()) return {};
        }
        out.push_back(part);
        skipWs();
        if (i >= n) return out;
        if (s[i] != '.') return {};
        ++i;
    }
}

// 1 行が [table] ヘッダならそのキー列を返す（[[array]] や配列の続きの行は対象外 = 空）。
inline std::vector<std::string> TableHeaderKeys(std::string line)
{
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
        line = line.substr(3);   // UTF-8 BOM
    line = Trim(line);
    if (line.size() < 3 || line[0] != '[' || line[1] == '[') return {};
    // 閉じ括弧（引用の外にある最初の ]）を探す
    size_t i = 1;
    char q = 0;
    for (; i < line.size(); ++i)
    {
        const char c = line[i];
        if (q) { if (c == '\\' && q == '"') { ++i; continue; } if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == ']') break;
    }
    if (i >= line.size()) return {};
    const std::string rest = Trim(line.substr(i + 1));
    if (!rest.empty() && rest[0] != '#') return {};   // "[...]," のような配列の続きは見出しではない
    return SplitDottedKey(line.substr(1, i - 1));
}

// "key = ..." の行のキー（引用を外したもの）。キー/値の行でなければ空。
inline std::string KeyOfAssignment(const std::string& rawLine)
{
    const std::string line = Trim(rawLine);
    if (line.empty() || line[0] == '#' || line[0] == '[') return {};
    char q = 0;
    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (q) { if (c == '\\' && q == '"') { ++i; continue; } if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '=')
        {
            const std::vector<std::string> keys = SplitDottedKey(Trim(line.substr(0, i)));
            return keys.size() == 1 ? keys[0] : std::string();
        }
    }
    return {};
}

} // namespace detail

// Codex の config.toml へ [mcp_servers.<name>] を差し込む（あれば同じ場所で置き換え、無ければ末尾へ）。
//   - [mcp_servers.<name>] と [mcp_servers.<name>.*]（env などのサブテーブル）を丸ごと取り除く
//   - [mcp_servers] テーブル直下の "<name> = { ... }"（1 行のインライン表）も取り除く（キー重複で Codex が読めなくなるため）
//   - 改行コード（CRLF / LF）は元のファイルに合わせる
inline std::string UpsertCodexToml(const std::string& existing, const ServerSpec& spec)
{
    const bool crlf = existing.find("\r\n") != std::string::npos;
    const char* eol = crlf ? "\r\n" : "\n";

    // 行へ分解（各行の末尾の \r は Trim 系が無視する。出力時は元の行をそのまま使う）
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start < existing.size())
        {
            const size_t nl = existing.find('\n', start);
            if (nl == std::string::npos) { lines.push_back(existing.substr(start)); break; }
            lines.push_back(existing.substr(start, nl - start));
            start = nl + 1;
        }
        for (std::string& l : lines)
            if (!l.empty() && l.back() == '\r') l.pop_back();
    }

    std::vector<std::string> kept;
    kept.reserve(lines.size());
    size_t insertAt = static_cast<size_t>(-1);
    enum class Where { Other, Ours, McpServersRoot } where = Where::Other;
    for (const std::string& l : lines)
    {
        const std::vector<std::string> keys = detail::TableHeaderKeys(l);
        if (!keys.empty())
        {
            if (keys.size() >= 2 && keys[0] == "mcp_servers" && keys[1] == spec.name)
            {
                if (insertAt == static_cast<size_t>(-1)) insertAt = kept.size();
                where = Where::Ours;
                continue;
            }
            where = (keys.size() == 1 && keys[0] == "mcp_servers") ? Where::McpServersRoot : Where::Other;
        }
        if (where == Where::Ours) continue;
        if (where == Where::McpServersRoot && detail::KeyOfAssignment(l) == spec.name) continue;
        kept.push_back(l);
    }

    // 差し込むブロック
    std::vector<std::string> block;
    block.push_back("[mcp_servers." + TomlKey(spec.name) + "]");
    block.push_back("command = " + TomlString(spec.command));
    {
        std::string a = "args = [";
        for (size_t i = 0; i < spec.args.size(); ++i) a += (i ? ", " : "") + TomlString(spec.args[i]);
        block.push_back(a + "]");
    }
    if (!spec.env.empty())
    {
        std::string e = "env = { ";
        for (size_t i = 0; i < spec.env.size(); ++i)
            e += (i ? ", " : "") + TomlKey(spec.env[i].first) + " = " + TomlString(spec.env[i].second);
        block.push_back(e + " }");
    }
    if (spec.startupTimeoutSec > 0)
        block.push_back("startup_timeout_sec = " + std::to_string(spec.startupTimeoutSec));

    if (insertAt == static_cast<size_t>(-1))
    {
        // 末尾の空行を 1 つに揃えてから足す
        while (!kept.empty() && detail::Trim(kept.back()).empty()) kept.pop_back();
        if (!kept.empty()) kept.push_back("");
        insertAt = kept.size();
        block.push_back("");
    }
    else
    {
        // 置き換え: 次のテーブルとの間に空行を 1 つ残す
        block.push_back("");
        while (insertAt < kept.size() && detail::Trim(kept[insertAt]).empty())
            kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(insertAt));
        if (insertAt == kept.size()) block.pop_back();   // ファイル末尾なら空行は不要
    }
    kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(insertAt), block.begin(), block.end());

    std::string out;
    for (const std::string& l : kept) { out += l; out += eol; }
    return out;
}

// JSON の "mcpServers".<name> を差し替える（Claude Code の ~/.claude.json / Claude Desktop / Cursor 共通の形）。
//   withType: Claude Code は "type": "stdio" を持つ形で書く
// 失敗（ルートが object でない / mcpServers が object でない）なら false を返し root は変えない。
inline bool UpsertJsonServer(nlohmann::ordered_json& root, const ServerSpec& spec, bool withType, std::string* error = nullptr)
{
    if (root.is_null()) root = nlohmann::ordered_json::object();
    if (!root.is_object()) { if (error) *error = "設定ファイルの中身が JSON のオブジェクトではありません"; return false; }
    if (root.contains("mcpServers") && !root["mcpServers"].is_object())
    {
        if (error) *error = "\"mcpServers\" がオブジェクトではありません";
        return false;
    }
    nlohmann::ordered_json entry = nlohmann::ordered_json::object();
    if (withType) entry["type"] = "stdio";
    entry["command"] = spec.command;
    entry["args"] = spec.args;
    nlohmann::ordered_json env = nlohmann::ordered_json::object();
    for (const auto& [k, v] : spec.env) env[k] = v;
    entry["env"] = env;
    root["mcpServers"][spec.name] = std::move(entry);
    return true;
}

// テキスト → 差し替え後のテキスト（JSON）。空のテキストは {} とみなす。読めなければ false。
inline bool UpsertJsonText(const std::string& text, const ServerSpec& spec, bool withType, std::string& out, std::string* error = nullptr)
{
    nlohmann::ordered_json root;
    std::string t = text;
    if (t.size() >= 3 && static_cast<unsigned char>(t[0]) == 0xEF && static_cast<unsigned char>(t[1]) == 0xBB &&
        static_cast<unsigned char>(t[2]) == 0xBF)
        t = t.substr(3);
    if (detail::Trim(t).empty()) root = nlohmann::ordered_json::object();
    else
    {
        root = nlohmann::ordered_json::parse(t, nullptr, false);
        if (root.is_discarded()) { if (error) *error = "JSON として読めません（壊れているため書き換えません）"; return false; }
    }
    if (!UpsertJsonServer(root, spec, withType, error)) return false;
    out = root.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
    return true;
}

// Node の `--version` 出力（"v24.3.0"）から major を取る。読めなければ 0。
inline int NodeMajorFromVersion(const std::string& v)
{
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\r' || v[i] == '\n')) ++i;
    if (i < v.size() && (v[i] == 'v' || v[i] == 'V')) ++i;
    int major = 0;
    bool any = false;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') { major = major * 10 + (v[i] - '0'); any = true; ++i; if (major > 100000) return 0; }
    return any ? major : 0;
}

// nodejs.org の SHASUMS256.txt から、指定サフィックス（"-win-x64.zip" など）のファイル名とハッシュを探す。
inline bool FindNodeZipInShasums(const std::string& shasums, const std::string& suffix, std::string& fileName, std::string& sha256Hex)
{
    size_t start = 0;
    while (start < shasums.size())
    {
        size_t nl = shasums.find('\n', start);
        if (nl == std::string::npos) nl = shasums.size();
        const std::string line = detail::Trim(shasums.substr(start, nl - start));
        start = nl + 1;
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        const std::string hash = line.substr(0, sp);
        const std::string name = detail::Trim(line.substr(sp));
        if (hash.size() != 64) continue;
        if (name.size() > suffix.size() && name.compare(0, 5, "node-") == 0 &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 && name.find('/') == std::string::npos &&
            name.find('\\') == std::string::npos)
        {
            fileName = name;
            sha256Hex = hash;
            return true;
        }
    }
    return false;
}

} // namespace dx12e::mcpcfg
