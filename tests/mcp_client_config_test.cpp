// editor/McpClientConfig.h（AI クライアントの設定ファイルへ MCP サーバを足す純ロジック）の単体テスト。
// ファイル I/O も Win32 も使わない。実行: ctest --output-on-failure
//
// ここで守りたい回帰:
//   ・他のサーバ・他の設定を 1 文字も壊さない（ユーザーの設定ファイルを丸ごと上書きしない）
//   ・同じエントリを何度足しても 1 つのまま（再実行で TOML のキー重複を作ると Codex が起動しない）
//   ・Windows のパス（\）と日本語ユーザー名がそのまま正しく書ける
//   ・壊れた JSON は書き換えない

#include "editor/McpClientConfig.h"

#include <cstdio>
#include <string>

using namespace dx12e::mcpcfg;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{

size_t Count(const std::string& s, const std::string& sub)
{
    size_t n = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) ++n;
    return n;
}

ServerSpec Spec()
{
    ServerSpec s;
    s.command = "C:\\Users\\りゅうと\\AppData\\Local\\UnoEngine\\node\\node.exe";
    s.args = {"C:\\Users\\りゅうと\\AppData\\Local\\UnoEngine\\mcp-server\\index.ts"};
    s.env = {{"DX12_MCP_SURFACE", "core"}};
    s.startupTimeoutSec = 30;
    return s;
}

void TestTomlString()
{
    CHECK(TomlString("a\\b\"c") == "\"a\\\\b\\\"c\"");
    CHECK(TomlString("x\ny") == "\"x\\ny\"");
    CHECK(TomlString("\x01") == "\"\\u0001\"");
    CHECK(TomlString("日本語") == "\"日本語\"");
    CHECK(TomlKey("dx12-engine") == "dx12-engine");
    CHECK(TomlKey("a.b") == "\"a.b\"");
}

void TestTomlEmpty()
{
    const std::string out = UpsertCodexToml("", Spec());
    CHECK(out.rfind("[mcp_servers.dx12-engine]\n", 0) == 0);
    CHECK(out.find("command = \"C:\\\\Users\\\\りゅうと\\\\AppData\\\\Local\\\\UnoEngine\\\\node\\\\node.exe\"") != std::string::npos);
    CHECK(out.find("args = [\"C:\\\\Users") != std::string::npos);
    CHECK(out.find("env = { DX12_MCP_SURFACE = \"core\" }") != std::string::npos);
    CHECK(out.find("startup_timeout_sec = 30") != std::string::npos);
}

void TestTomlAppendKeepsOthers()
{
    const std::string in =
        "model = \"gpt-5\"\n"
        "\n"
        "[mcp_servers.other]\n"
        "command = \"npx\"\n"
        "args = [\"-y\", \"other\"]\n";
    const std::string out = UpsertCodexToml(in, Spec());
    CHECK(out.rfind(in, 0) == 0);   // 既存部分は先頭にそのまま残る
    CHECK(Count(out, "[mcp_servers.dx12-engine]") == 1);
    CHECK(out.find("\n\n[mcp_servers.dx12-engine]") != std::string::npos);
}

void TestTomlReplaceInPlaceAndIdempotent()
{
    const std::string in =
        "model = \"gpt-5\"\n"
        "\n"
        "[mcp_servers.dx12-engine]\n"
        "command = \"node\"\n"
        "args = [\"C:/old/index.ts\"]\n"
        "\n"
        "[mcp_servers.dx12-engine.env]\n"
        "DX12_MCP_PORT = \"8790\"\n"
        "\n"
        "[mcp_servers.other]\n"
        "command = \"npx\"\n";
    const std::string once = UpsertCodexToml(in, Spec());
    CHECK(Count(once, "[mcp_servers.dx12-engine]") == 1);
    CHECK(once.find("C:/old/index.ts") == std::string::npos);
    CHECK(once.find("DX12_MCP_PORT") == std::string::npos);        // 古いサブテーブルも消える
    CHECK(once.find("[mcp_servers.other]\ncommand = \"npx\"\n") != std::string::npos);
    CHECK(once.find("[mcp_servers.dx12-engine]") < once.find("[mcp_servers.other]"));   // 元の場所で置き換え
    CHECK(once.find("startup_timeout_sec = 30\n\n[mcp_servers.other]") != std::string::npos);

    const std::string twice = UpsertCodexToml(once, Spec());
    CHECK(twice == once);   // 何度やっても同じ
}

void TestTomlQuotedAndInline()
{
    const std::string in =
        "[mcp_servers.\"dx12-engine\"]  # 手で書いたもの\n"
        "command = \"node\"\n"
        "[mcp_servers]\n"
        "dx12-engine = { command = \"node\", args = [\"x\"] }\n"
        "keep-me = { command = \"y\" }\n";
    const std::string out = UpsertCodexToml(in, Spec());
    CHECK(Count(out, "dx12-engine") == 1);
    CHECK(out.find("keep-me = { command = \"y\" }") != std::string::npos);
    CHECK(out.find("[mcp_servers]\n") != std::string::npos);
}

void TestTomlCrlfAndMultilineArray()
{
    const std::string in =
        "[mcp_servers.other]\r\n"
        "args = [\r\n"
        "  [\"nested\"],\r\n"
        "]\r\n";
    const std::string out = UpsertCodexToml(in, Spec());
    CHECK(out.find("  [\"nested\"],\r\n]\r\n") != std::string::npos);   // 配列の続きを見出しと誤認しない
    CHECK(out.find("[mcp_servers.dx12-engine]\r\n") != std::string::npos);
    CHECK(Count(out, "\n") == Count(out, "\r\n"));                       // 改行コードが混ざらない
}

void TestJson()
{
    std::string out, err;
    CHECK(UpsertJsonText("", Spec(), false, out, &err));
    {
        const auto j = nlohmann::ordered_json::parse(out);
        CHECK(j["mcpServers"]["dx12-engine"]["command"] == Spec().command);
        CHECK(j["mcpServers"]["dx12-engine"]["args"][0] == Spec().args[0]);
        CHECK(j["mcpServers"]["dx12-engine"]["env"]["DX12_MCP_SURFACE"] == "core");
        CHECK(!j["mcpServers"]["dx12-engine"].contains("type"));
    }

    const std::string in = R"({"numStartups": 5, "mcpServers": {"a": {"command": "x"}, "dx12-engine": {"command": "old"}}, "zeta": true})";
    CHECK(UpsertJsonText(in, Spec(), true, out, &err));
    {
        const auto j = nlohmann::ordered_json::parse(out);
        CHECK(j["numStartups"] == 5);
        CHECK(j["zeta"] == true);
        CHECK(j["mcpServers"]["a"]["command"] == "x");
        CHECK(j["mcpServers"]["dx12-engine"]["type"] == "stdio");
        CHECK(j["mcpServers"]["dx12-engine"]["command"] == Spec().command);
        CHECK(j["mcpServers"].size() == 2);
        // キーの順序を保つ（ordered_json）
        CHECK(out.find("numStartups") < out.find("mcpServers"));
        CHECK(out.find("mcpServers") < out.find("zeta"));
    }
    std::string again;
    CHECK(UpsertJsonText(out, Spec(), true, again, &err));
    CHECK(again == out);

    CHECK(!UpsertJsonText("{ broken", Spec(), false, out, &err));
    CHECK(!UpsertJsonText("[1,2]", Spec(), false, out, &err));
    CHECK(!UpsertJsonText(R"({"mcpServers": []})", Spec(), false, out, &err));
    CHECK(UpsertJsonText("\xEF\xBB\xBF{}", Spec(), false, out, &err));   // BOM 付き
}

void TestNodeHelpers()
{
    CHECK(NodeMajorFromVersion("v24.3.0\r\n") == 24);
    CHECK(NodeMajorFromVersion("v18.20.1") == 18);
    CHECK(NodeMajorFromVersion("") == 0);
    CHECK(NodeMajorFromVersion("not node") == 0);

    const std::string sums =
        "1111111111111111111111111111111111111111111111111111111111111111  node-v24.9.0-darwin-arm64.tar.gz\n"
        "2222222222222222222222222222222222222222222222222222222222222222  node-v24.9.0-win-arm64.zip\n"
        "3333333333333333333333333333333333333333333333333333333333333333  node-v24.9.0-win-x64.7z\n"
        "4444444444444444444444444444444444444444444444444444444444444444  node-v24.9.0-win-x64.zip\n"
        "5555555555555555555555555555555555555555555555555555555555555555  win-x64/node.exe\n";
    std::string name, hash;
    CHECK(FindNodeZipInShasums(sums, "-win-x64.zip", name, hash));
    CHECK(name == "node-v24.9.0-win-x64.zip");
    CHECK(hash == "4444444444444444444444444444444444444444444444444444444444444444");
    CHECK(FindNodeZipInShasums(sums, "-win-arm64.zip", name, hash));
    CHECK(name == "node-v24.9.0-win-arm64.zip");
    CHECK(!FindNodeZipInShasums("garbage\n", "-win-x64.zip", name, hash));
}

} // namespace

int main()
{
    TestTomlString();
    TestTomlEmpty();
    TestTomlAppendKeepsOthers();
    TestTomlReplaceInPlaceAndIdempotent();
    TestTomlQuotedAndInline();
    TestTomlCrlfAndMultilineArray();
    TestJson();
    TestNodeHelpers();
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
