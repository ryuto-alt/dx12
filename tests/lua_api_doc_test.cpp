// Lua に束縛したのに dx12_describe_lua_api に載せ忘れた API を見張るテスト。
//
// ★なぜ要るか
//   describe_lua_api（ApplicationInternal.cpp の McpLuaApi()）は手書きの文字列表で、
//   ScriptEngine.cpp の束縛と自動では同期しない。実際に `display`（映像設定 9 個）と
//   `net`（マルチプレイ 17 個）がテーブルごと丸ごと抜けていた。
//   載っていない API は AI からは存在しないのと同じ＝実装があるのに誰も呼ばない。
//
//   逆向き（doc にあるのに束縛が無い＝呼ぶとエラー）は別問題だが、こちらは
//   「書いた人が実在を確かめて書く」前提で見ない（束縛の書き方が多様すぎて
//   テキストからの全列挙が信用できないため。嘘の検出より見落としの検出を優先する）。
//
// エンジンをリンクせずソースを**テキストとして**読む。GPU も Lua も要らない。
//
// 実行: ctest --output-on-failure -R LuaApiDoc

#include <algorithm>
#include <cctype>
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

std::string ReadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("  NG  ファイルが開けない: %s\n", path.c_str()); ++g_failures; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// src の pos 以降から `prefix"名前"` の「名前」を全部拾う。
std::set<std::string> CollectQuoted(const std::string& src, const std::string& prefix)
{
    std::set<std::string> out;
    for (std::size_t i = src.find(prefix); i != std::string::npos; i = src.find(prefix, i + 1))
    {
        const std::size_t b = i + prefix.size();
        const std::size_t e = src.find('"', b);
        if (e == std::string::npos) break;
        const std::string name = src.substr(b, e - b);
        if (!name.empty()) out.insert(name);
    }
    return out;
}

// new_usertype<T>("Name", "key", &T::member, ...) の「key」を拾う。
//
// ★usertype の中身は「行頭に "識別子", が来て、その後ろにメンバポインタかラムダが続く」
//   という書き方で揃っている。この形だけを拾えば、prose の文字列や
//   kSafeLibs のような**文字列だけが並ぶ配列**（"unpack", "rawget", ... ）を巻き込まない。
//   実測: 190 個ヒットして誤検出ゼロ。
std::set<std::string> CollectUsertypeKeys(const std::string& src)
{
    std::set<std::string> out;
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line))
    {
        std::size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line[i] != '"') continue;
        const std::size_t b = i + 1;
        const std::size_t e = line.find('"', b);
        if (e == std::string::npos || e == b) continue;

        const std::string name = line.substr(b, e - b);
        const bool ident = (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')
            && std::all_of(name.begin(), name.end(), [](char c) {
                   return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
               });
        if (!ident) continue;

        std::size_t j = line.find_first_not_of(" \t", e + 1);
        if (j == std::string::npos || line[j] != ',') continue;
        j = line.find_first_not_of(" \t", j + 1);
        // 残りが空 or さらに文字列 = 束縛ではなくただの文字列配列
        if (j == std::string::npos || line[j] == '"') continue;
        out.insert(name);
    }
    return out;
}

// McpLuaApi() の本体だけ切り出す（ファイル全体を見ると "spawn" 等が
// 無関係な場所に当たって素通りしてしまう）。
std::string ExtractMcpLuaApi(const std::string& src)
{
    const std::size_t b = src.find("nlohmann::json McpLuaApi()");
    if (b == std::string::npos) return {};
    const std::size_t e = src.find("\n}\n", b);
    return (e == std::string::npos) ? src.substr(b) : src.substr(b, e - b);
}

void Expect(bool cond, const std::string& label)
{
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::printf("  NG  %s\n", label.c_str());
}

// 「KEY_W を1個ずつ書く」のは doc として読みにくいので、族でまとめてよいものは除外する。
// PAD_* は個別に列挙してある（ボタン名は族名から推測できないため）。
bool IsDocumentedAsFamily(const std::string& name)
{
    return name.rfind("KEY_", 0) == 0 || name.rfind("MOTION_", 0) == 0;
}
}

int main()
{
    // パスは CMake から絶対パスで渡す（ctest の作業ディレクトリに依存させない）。
    const std::string engineSrc = ReadFile(DX12E_SCRIPT_ENGINE_CPP);
    const std::string doc = ExtractMcpLuaApi(ReadFile(DX12E_APP_INTERNAL_CPP));
    if (engineSrc.empty() || doc.empty())
    {
        std::printf("NG: ソースが読めない(McpLuaApi の切り出しに失敗した可能性)\n");
        return 1;
    }

    const auto mentions = [&doc](const std::string& n) { return doc.find(n) != std::string::npos; };

    // [1] create_named_table で生やしたグローバルテーブルは、doc のどこかに名前が出ること。
    //     display / net はこれが丸ごと無かった。
    //     ※ "net" "ui" "fx" のような短い名前は無関係な単語に部分一致して素通りする。
    //       そこは [2] のメソッド検査（host/rpc/…）が実質的に受け持つ。
    std::printf("[1] グローバルテーブル\n");
    for (const auto& t : CollectQuoted(engineSrc, "create_named_table(\""))
        Expect(mentions(t), "テーブル '" + t + "' が describe_lua_api に無い");

    // [2] そのテーブルにぶら下げた関数（`net.set_function("host", ...)` 等）。
    std::printf("[2] テーブルのメソッド\n");
    for (const auto& fn : CollectQuoted(engineSrc, ".set_function(\""))
        Expect(mentions(fn), "メソッド '" + fn + "' が describe_lua_api に無い");

    // [3] グローバル（`lua["savePersist"] = ...`）。KEY_*/MOTION_* は族でまとめてよい。
    std::printf("[3] グローバル関数・定数\n");
    for (const auto& g : CollectQuoted(engineSrc, "lua[\""))
    {
        if (IsDocumentedAsFamily(g)) continue;
        Expect(mentions(g), "グローバル '" + g + "' が describe_lua_api に無い");
    }

    // [4] 族でまとめた分も、族名そのものは載っていること（KEY_* を丸ごと消されないように）。
    std::printf("[4] 族でまとめた定数\n");
    Expect(mentions("KEY_"), "KEY_* の記載が消えている");
    Expect(mentions("MOTION_"), "MOTION_* の記載が消えている");

    // [5] new_usertype のメンバ（scene: / entity: / input: / audio: / camera: など）。
    //     実際に抜けていた 28 個（scene の連番アニメ 12・ゲームパッド 13・音量 get 3）は
    //     ここでしか捕まらない。
    std::printf("[5] usertype のメンバ\n");
    const auto keys = CollectUsertypeKeys(engineSrc);
    Expect(keys.size() > 100, "usertype キーの抽出が壊れている(件数 "
                              + std::to_string(keys.size()) + ")");
    for (const auto& k : keys)
        Expect(mentions(k), "usertype メンバ '" + k + "' が describe_lua_api に無い");

    if (g_failures != 0)
    {
        std::printf("\nlua_api_doc: %d チェック中 %d 件 NG\n", g_checks, g_failures);
        std::printf("→ src/core/ApplicationInternal.cpp の McpLuaApi() に追記すること。\n");
        return 1;
    }
    std::printf("\nlua_api_doc: %d チェックすべて通過\n", g_checks);
    return 0;
}
