// MCP の純ロジックのテスト（core/mcp/McpScenePath.h / McpCreateEntityParams.h / McpDlssWarn.h）。
#include <cstdio>
#include <string>
#include <vector>

#include "core/mcp/McpCreateEntityParams.h"
#include "core/mcp/McpDlssWarn.h"
#include "core/mcp/McpScenePath.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    // ---- シーンパス: path は assets 相対、projectPath はプロジェクト相対。基準が違うので別のキー ----
    {
        using mcpscene::Resolve;
        const std::string assets = "C:/proj/assets/";
        const std::string base   = "C:\\proj\\";   // 区切りが混ざっていても同じ結果

        auto none = Resolve("", assets, base);
        CHECK(!none.path && !none.projectPath);

        auto a = Resolve("C:\\proj\\assets\\scenes\\a.json", assets, base);
        CHECK(a.path && *a.path == "scenes/a.json");
        CHECK(a.projectPath && *a.projectPath == "assets/scenes/a.json");

        // assets の外（未保存の退避）: path は null、projectPath で戻れる
        auto u = Resolve("C:/proj/.dx12/untitled/_mcp_untitled_1.json", assets, base);
        CHECK(!u.path);
        CHECK(u.projectPath && *u.projectPath == ".dx12/untitled/_mcp_untitled_1.json");

        // <project>/scenes/x.json と <project>/assets/scenes/x.json は衝突しない
        auto o = Resolve("C:/proj/scenes/x.json", assets, base);
        CHECK(!o.path && o.projectPath && *o.projectPath == "scenes/x.json");
        auto i = Resolve("C:/proj/assets/scenes/x.json", assets, base);
        CHECK(i.path && *i.path == "scenes/x.json" && *i.projectPath == "assets/scenes/x.json");

        // プロジェクトの外の絶対パスは漏らさない
        auto x = Resolve("D:/elsewhere/x.json", assets, base);
        CHECK(!x.path && !x.projectPath);

        // 既に相対なら assets 相対として扱う
        auto r = Resolve("scenes/rel.json", assets, base);
        CHECK(r.path && *r.path == "scenes/rel.json" && !r.projectPath);
    }

    // ---- create_entity の引数検査 ----
    {
        using namespace mcpcreate;
        std::string bad;
        CHECK(Validate({"type", "name"}, "box", bad) == Check::Ok);
        CHECK(Validate({"name"}, "", bad) == Check::MissingType);
        CHECK(Validate({}, "", bad) == Check::MissingType);
        CHECK(Validate({"type", "rotation"}, "box", bad) == Check::UnknownKey && bad == "rotation");
        CHECK(Validate({"typ"}, "", bad) == Check::UnknownKey && bad == "typ");   // 綴り違いは未知キーが先
        CHECK(NearestKey("typ") == "type");
        CHECK(NearestKey("positon") == "position");
        CHECK(NearestKey("zzzzzzzzzz").empty());
        CHECK(Validate({"type", "dryRun", "idempotency_key", "expectGeneration"}, "box", bad) == Check::Ok);
        // parent / parentName は ui_canvas 以外の全種別で有効（dx12_place_decal / dx12_place_vfx が渡す）
        CHECK(Validate({"type", "parentName"}, "box", bad) == Check::Ok);
        CHECK(Validate({"type", "parentName"}, "decal", bad) == Check::Ok);
        CHECK(Validate({"type", "parent"}, "particle_emitter", bad) == Check::Ok);
        CHECK(Validate({"type", "parent"}, "ui_button", bad) == Check::Ok);
        CHECK(Validate({"type", "parent"}, "ui_canvas", bad) == Check::ParentOnCanvas && bad == "parent");
    }

    // ---- DLSS 警告: 同じ理由は 1 回。dlss → off → dlss でも 1 回。理由が変われば再度出す ----
    {
        std::string last;
        CHECK(mcpdlss::ShouldLogFallback(last, "DLSS not compiled"));
        CHECK(!mcpdlss::ShouldLogFallback(last, "DLSS not compiled"));
        CHECK(!mcpdlss::ShouldLogFallback(last, "DLSS not compiled"));
        CHECK(mcpdlss::ShouldLogFallback(last, "device lost"));
        CHECK(!mcpdlss::ShouldLogFallback(last, "device lost"));
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
