// C14: MCP 取りこぼし修正の純ロジック（core/mcp/McpC14Logic.h）のテスト。
#include <cstdio>
#include <string>
#include <vector>

#include "core/mcp/McpC14Logic.h"

using namespace mcpc14;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    const std::string assets = "C:/proj/assets/";
    const std::string base   = "C:\\proj\\";   // 区切りが混ざっていても同じ結果

    // シーンパス: assets 配下 / 未保存の退避 / 空 / 範囲外
    CHECK(SceneRel("", assets, base).empty());
    CHECK(SceneRel("C:\\proj\\assets\\scenes\\a.json", assets, base) == "scenes/a.json");
    CHECK(SceneRel("C:/proj/.dx12/untitled/_mcp_untitled_1.json", assets, base) == ".dx12/untitled/_mcp_untitled_1.json");
    CHECK(SceneRel("D:/elsewhere/x.json", assets, base) == "x.json");          // 絶対パスは漏らさない
    CHECK(SceneRel("scenes/rel.json", assets, base) == "scenes/rel.json");     // 既に相対ならそのまま
    {
        const std::string r = SceneRel("C:/proj/.dx12/untitled/u.json", assets, base);
        CHECK(r.find(':') == std::string::npos && r.find("proj") == std::string::npos);
    }

    // create_entity の引数検査
    std::string bad;
    CHECK(ValidateCreateEntityKeys({"type", "name"}, "box", bad) == CreateEntityCheck::Ok);
    CHECK(ValidateCreateEntityKeys({"name"}, "", bad) == CreateEntityCheck::MissingType);
    CHECK(ValidateCreateEntityKeys({}, "", bad) == CreateEntityCheck::MissingType);
    CHECK(ValidateCreateEntityKeys({"type", "rotation"}, "box", bad) == CreateEntityCheck::UnknownKey && bad == "rotation");
    CHECK(ValidateCreateEntityKeys({"typ"}, "", bad) == CreateEntityCheck::UnknownKey && bad == "typ");   // 綴り違いは未知キーが先
    CHECK(ValidateCreateEntityKeys({"type", "dryRun", "idempotency_key", "expectGeneration"}, "box", bad) == CreateEntityCheck::Ok);
    CHECK(ValidateCreateEntityKeys({"type", "parentName"}, "box", bad) == CreateEntityCheck::ParentOnNonUi && bad == "parentName");
    CHECK(ValidateCreateEntityKeys({"type", "parent"}, "ui_canvas", bad) == CreateEntityCheck::ParentOnNonUi);
    CHECK(ValidateCreateEntityKeys({"type", "parent"}, "ui_button", bad) == CreateEntityCheck::Ok);

    // DLSS 警告: 同じ理由は 1 回。dlss → off → dlss でも 1 回。理由が変われば再度出す
    std::string last;
    CHECK(ShouldLogDlssFallback(last, "DLSS not compiled"));
    CHECK(!ShouldLogDlssFallback(last, "DLSS not compiled"));
    CHECK(!ShouldLogDlssFallback(last, "DLSS not compiled"));
    CHECK(ShouldLogDlssFallback(last, "device lost"));
    CHECK(!ShouldLogDlssFallback(last, "device lost"));

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
