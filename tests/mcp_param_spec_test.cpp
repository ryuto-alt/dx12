// MCP ディスパッチ表の「申告したキー表」と「ハンドラ本文が実際に読むキー」がズレていないか
// を機械的に見張るテスト（#20-7 / #30）。
//
// ★なぜ要るか
//   `dx12_describe_mcp_params` は McpDefine の第 2 引数（"key:type,..."）をそのまま返す。
//   本文で `params.value("newKey", ...)` を足したのに申告表へ書き忘れると、
//   describe_mcp_params が嘘をつき、TS 側のドリフト検出も AI への説明もそこで壊れる。
//   逆向き（申告表にあるのに本文が読まない）は、入れ子キー("親.子")やキー名を引数で受ける
//   ラムダ経由の読みが正当に存在するので**見ない**。ここは「本文 ⊆ 申告」だけを固定する。
//
// エンジンをリンクせず Application.cpp を**テキストとして**読む。GPU も entt も要らない。
//
// 実行: ctest --output-on-failure -R McpParamSpec

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool cond, const std::string& label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label.c_str());
}

bool IsIdentChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// pos から連続する C++ 文字列リテラル（"a" "b" の連結）を読み、中身をつなげて返す。
// 読み終わった位置を pos へ返す。先頭が '"' でなければ false。
bool ReadStringLiteral(const std::string& s, size_t& pos, std::string& out)
{
    out.clear();
    bool any = false;
    for (;;)
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t')) ++pos;
        if (pos >= s.size() || s[pos] != '"') return any;
        ++pos;
        while (pos < s.size() && s[pos] != '"')
        {
            if (s[pos] == '\\' && pos + 1 < s.size()) { out += s[pos + 1]; pos += 2; continue; }
            out += s[pos++];
        }
        if (pos < s.size()) ++pos;   // 閉じ '"'
        any = true;
    }
}

// `needle` に続く `"..."` を全部拾う（needle は '(' や ',' まで含めた固定文字列）。
void CollectQuotedAfter(const std::string& body, const std::string& needle, std::set<std::string>& out)
{
    size_t at = 0;
    while ((at = body.find(needle, at)) != std::string::npos)
    {
        size_t p = at + needle.size();
        while (p < body.size() && (body[p] == ' ' || body[p] == '\n' || body[p] == '\r' || body[p] == '\t')) ++p;
        if (p < body.size() && body[p] == '"')
        {
            std::string k;
            ++p;
            while (p < body.size() && body[p] != '"') k += body[p++];
            if (!k.empty()) out.insert(k);
        }
        at += needle.size();
    }
}

// `Mcp*(params, "key"` 系ヘルパ（McpFloatParam / McpIntParam / McpEnumParam / McpVec3Required ...）。
void CollectHelperKeys(const std::string& body, std::set<std::string>& out)
{
    size_t at = 0;
    while ((at = body.find("Mcp", at)) != std::string::npos)
    {
        // 直前が識別子文字なら別の名前の一部（例: ResolveMcpEntity）
        if (at > 0 && IsIdentChar(body[at - 1])) { at += 3; continue; }
        size_t p = at + 3;
        while (p < body.size() && IsIdentChar(body[p])) ++p;
        // テンプレート引数を飛ばす
        if (p < body.size() && body[p] == '<')
        {
            const size_t gt = body.find('>', p);
            if (gt == std::string::npos) { at += 3; continue; }
            p = gt + 1;
        }
        while (p < body.size() && body[p] == ' ') ++p;
        if (p >= body.size() || body[p] != '(') { at += 3; continue; }
        ++p;
        while (p < body.size() && body[p] == ' ') ++p;
        if (body.compare(p, 6, "params") != 0) { at += 3; continue; }
        p += 6;
        while (p < body.size() && (body[p] == ' ' || body[p] == ',' || body[p] == '\n' || body[p] == '\r')) ++p;
        if (p < body.size() && body[p] == '"')
        {
            std::string k;
            ++p;
            while (p < body.size() && body[p] != '"') k += body[p++];
            if (!k.empty()) out.insert(k);
        }
        at += 3;
    }
}

std::set<std::string> ScanBodyKeys(const std::string& body)
{
    std::set<std::string> keys;
    CollectQuotedAfter(body, "params.value(", keys);
    CollectQuotedAfter(body, "params.contains(", keys);
    CollectQuotedAfter(body, "params.at(", keys);
    CollectQuotedAfter(body, "params.find(", keys);
    CollectQuotedAfter(body, "params[", keys);
    CollectHelperKeys(body, keys);
    // キー名リテラルを引数に持たないヘルパ（schemaDrift.ts の IMPLICIT_HELPER_KEYS と同じ表）
    if (body.find("ResolveMcpEntity(") != std::string::npos ||
        body.find("ResolveMcpComponentEntity<") != std::string::npos)
    {
        keys.insert("entity");
        keys.insert("name");
    }
    if (body.find("ParseMcpVk(") != std::string::npos) keys.insert("key");
    return keys;
}

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, sep))
        if (!cur.empty()) out.push_back(cur);
    return out;
}
} // namespace

int main()
{
#ifndef DX12E_APPLICATION_CPP
    std::printf("SKIP: Application.cpp のパスが渡されていない\n");
    return 0;
#else
    const char* srcPath = DX12E_APPLICATION_CPP;
    std::ifstream ifs(srcPath, std::ios::binary);
    if (!ifs)
    {
        std::printf("SKIP: %s を開けない（配布ツリー等）\n", srcPath);
        return 0;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    const std::string src = ss.str();

    std::printf("MCP ディスパッチ表: 申告キー表 ⊇ ハンドラ本文が読むキー\n");

    int methodCount = 0;
    size_t at = 0;
    const std::string kMark = "\n    McpDefine(\"";   // 登録行だけ（定義・コメント例は列が違う）
    while ((at = src.find(kMark, at)) != std::string::npos)
    {
        size_t p = at + kMark.size() - 1;   // '"' の位置
        std::string names;
        if (!ReadStringLiteral(src, p, names)) { at += kMark.size(); continue; }
        while (p < src.size() && (src[p] == ' ' || src[p] == ',' || src[p] == '\n' || src[p] == '\r')) ++p;

        // 第 2 引数: 文字列リテラルなら申告表、そうでなければ X マクロ生成（Post/SSAO）なので飛ばす
        std::string spec;
        const bool literalSpec = (p < src.size() && src[p] == '"') && ReadStringLiteral(src, p, spec);

        // 本文: DX12E_MCP_HANDLER の直後の '{' から対応する "\n        });" まで
        const size_t hAt = src.find("DX12E_MCP_HANDLER", p);
        if (hAt == std::string::npos) break;
        const size_t bodyBegin = src.find('{', hAt);
        const size_t bodyEnd   = src.find("\n        });", bodyBegin);
        if (bodyBegin == std::string::npos || bodyEnd == std::string::npos) break;
        const std::string body = src.substr(bodyBegin, bodyEnd - bodyBegin);
        at = bodyEnd;
        ++methodCount;

        if (!literalSpec) continue;   // X マクロ生成の分は別テスト（schemaDrift）に任せる

        std::set<std::string> declared;
        for (const std::string& one : Split(spec, ','))
        {
            const size_t colon = one.find(':');
            std::string key = (colon == std::string::npos) ? one : one.substr(0, colon);
            declared.insert(key);
            // 入れ子 "親.子" は親も宣言済みとみなす
            const size_t dot = key.find('.');
            if (dot != std::string::npos) declared.insert(key.substr(0, dot));
        }

        std::string missing;
        for (const std::string& k : ScanBodyKeys(body))
            if (declared.find(k) == declared.end()) missing += (missing.empty() ? "" : ", ") + k;

        Check(missing.empty(),
              "McpDefine(\"" + names + "\") の申告表に無いキーを本文が読んでいる → " + missing +
              "  （第 2 引数の \"key:type,...\" へ足すこと）");
    }

    Check(methodCount >= 110, "McpDefine を十分に検出できている（検出 " + std::to_string(methodCount) + " 本）");

    std::printf("%s: %d checks / %d methods / %d failures\n",
                g_failures == 0 ? "OK" : "NG", g_checks, methodCount, g_failures);
    return g_failures == 0 ? 0 : 1;
#endif
}
