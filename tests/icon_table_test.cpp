// アイコンの種別表（src/editor/IconTable.h）と GG の製図ペンのアイコン（GgIcons.gen.h）の検査（W1-A6 / RM-A4）。GPU・フォント不要。
//   ・シリアライザ / entt meta / インスペクタの見出しに現れる全コンポーネント名が表にある
//   ・表に重複が無い。名前の「完全一致」だけで引く（"UI" を含むだけの名前は UI アイコンにならない）
//   ・全 UI コンポーネントは UI 系の色。エンティティの優先順、アセットの全種別、Graphite の色の差し替え
//   ・表が参照する GG アイコンの id が実在する。gg_icons.txt / .json / .gen.h の個数が一致する
//   ・strstr による判定が InspectorPanel / EntityGlyph に残っていない
#include "editor/IconTable.h"
#include "editor/GgIconDraw.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace dx12e;
namespace fs = std::filesystem;
namespace ic = dx12e::icons;

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECKF(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

std::string ReadAll(const char* rel)
{
    std::ifstream f(fs::path(UI_LINT_ROOT) / rel, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::set<std::string> Matches(const std::string& text, const char* re, int group = 1)
{
    std::set<std::string> out;
    std::regex rx(re);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), rx); it != std::sregex_iterator(); ++it)
        out.insert((*it)[group].str());
    return out;
}

bool Found(const std::string& name)
{
    bool f = false;
    ic::ForComponent(name, &f);
    return f;
}

void TestAllComponentNamesCovered()
{
    std::set<std::string> names;
    const std::string ser = ReadAll("src/scene/SceneSerializer.cpp");
    const std::string meta = ReadAll("src/ecs/ComponentMeta.cpp");
    const std::string vfx = ReadAll("src/scene/VfxComponentSerializer.cpp");
    CHECK(!ser.empty() && !meta.empty() && !vfx.empty());
    for (auto& n : Matches(ser, "MakeReflectedInfo<\\w+>\\(\"(\\w+)\"")) names.insert(n);
    for (auto& n : Matches(ser, "R\\.Register\\(\\{ \"(\\w+)\", ComponentSource")) names.insert(n);
    for (auto& n : Matches(ser, "info\\.typeName = \"(\\w+)\"")) names.insert(n);
    for (auto& n : Matches(meta, "\\.type\\(\"(\\w+)\"\\)")) if (n != "Vec3" && n != "T") names.insert(n);   // T はコメントの例
    for (auto& n : Matches(vfx, "typeName = \"(\\w+)\"")) names.insert(n);
    CHECKF(names.size() > 40, "抽出できた名前が少なすぎる（正規表現の見直しが要る）: %zu", names.size());
    for (const auto& n : names) CHECKF(Found(n), "メタ名 %s が IconTable に無い", n.c_str());

    // インスペクタの見出し（コンポーネント）
    const std::string insp = ReadAll("src/editor/panels/InspectorPanel.cpp");
    const auto labels = Matches(insp, "IconHeader\\(ic, ic \\? ic->\\w+ : 0, \"([^\"]+)\"");
    CHECKF(labels.size() > 30, "見出しが少なすぎる: %zu", labels.size());
    for (const auto& l : labels) CHECKF(Found(l), "インスペクタの見出し %s が IconTable に無い", l.c_str());
}

void TestNoDuplicatesAndExactMatch()
{
    std::set<std::string> seen;
    for (const auto& r : ic::detail::kComponentNames)
        CHECKF(seen.insert(r.name).second, "重複: %s", r.name);

    // 空白の正規化
    CHECK(Found("Box Collider"));
    CHECK(Found("Audio Reverb Zone"));
    CHECK(Found("Shader##Sprite2D") == false);   // ## より前（"Shader"）は表に無い

    // 「UI」を名前に含むだけのものは UI アイコンにならない（汎用の Empty）
    for (const char* n : { "GUIDebug", "QuickUIThing", "BUILD_UI", "UIUnknown", "XUIRect", "UIRectX", "Guid" })
    {
        bool f = true;
        const auto v = ic::ForComponent(n, &f);
        CHECKF(!f, "%s が一致扱いになった", n);
        CHECKF(v.tint == &theme::TypeEmpty && std::strcmp(v.glyph, ICON_T_EMPTY) == 0, "%s が汎用にならない", n);
    }
    // 本物の UI コンポーネントは全部 UI 系の色
    for (const auto& r : ic::detail::kComponentNames)
    {
        if (std::strncmp(r.name, "UI", 2) != 0) continue;
        const auto v = ic::ForComponent(r.name);
        CHECKF(v.tint == &theme::TypeUi, "%s が UI の色でない", r.name);
    }
    CHECK(std::strcmp(ic::ForComponent("UIText").glyph, ICON_T_TEXT) == 0);
    CHECK(std::strcmp(ic::ForComponent("UIButton").glyph, ICON_T_BUTTON) == 0);
    CHECK(std::strcmp(ic::ForComponent("UICanvas").glyph, ICON_T_UI) == 0);
    // 従来のインスペクタの見出しと同じ結果（UI ゴールデンを変えない）
    CHECK(ic::ForComponent("Transform").tint == &theme::TypeEmpty);
    CHECK(std::strcmp(ic::ForComponent("Camera").glyph, ICON_T_CAMERA) == 0);
    CHECK(ic::ForComponent("Particle Emitter").tint == &theme::TypeLight);
    CHECK(ic::ForComponent("Network Transform").tint == &theme::TypeEmpty);
    CHECK(std::strcmp(ic::ForComponent("Character Controller").glyph, ICON_T_CHARACTER) == 0);
}

void TestEntity()
{
    entt::registry reg;
    auto e = reg.create();
    reg.emplace<CameraComponent>(e);
    CHECK(std::strcmp(ic::ForEntity(reg, e).glyph, ICON_T_CAMERA) == 0);
    e = reg.create();
    reg.emplace<DirectionalLight>(e);
    CHECK(std::strcmp(ic::ForEntity(reg, e).glyph, ICON_T_SUN) == 0);
    e = reg.create();
    reg.emplace<PointLight>(e);
    CHECK(ic::ForEntity(reg, e).tint == &theme::TypeLight);
    e = reg.create();
    reg.emplace<UIText>(e);
    CHECK(std::strcmp(ic::ForEntity(reg, e).glyph, ICON_T_UI) == 0);
    e = reg.create();
    reg.emplace<UISlider>(e);
    CHECK(std::strcmp(ic::ForEntity(reg, e).glyph, ICON_T_UI) == 0);
    e = reg.create();
    reg.emplace<MeshRenderer>(e);
    CHECK(ic::ForEntity(reg, e).tint == &theme::TypeMesh);
    e = reg.create();
    reg.emplace<RigidBody>(e);
    reg.emplace<MeshRenderer>(e);
    CHECK(ic::ForEntity(reg, e).tint == &theme::TypeMesh);   // メッシュが物理より先
    e = reg.create();
    reg.emplace<BoxCollider>(e);
    CHECK(ic::ForEntity(reg, e).tint == &theme::TypePhysics);
    e = reg.create();
    CHECK(std::strcmp(ic::ForEntity(reg, e, true).glyph, ICON_T_GROUP) == 0);
    CHECK(std::strcmp(ic::ForEntity(reg, e, false).glyph, ICON_T_EMPTY) == 0);
    CHECK(ic::ForEntity(reg, e).color == ImGui::ColorConvertFloat4ToU32(theme::TypeEmpty));
}

void TestAsset()
{
    using K = ic::AssetKind;
    for (int i = 0; i < abl::kKindCount; ++i)
    {
        const auto v = ic::ForAsset(static_cast<K>(i));
        CHECKF(v.glyph && v.glyph[0] && v.tint && v.label, "Kind %d の表が空", i);
    }
    CHECK(ic::ForAsset(K::Model).tint == &theme::TypeCamera);
    CHECK(ic::ForAsset(K::Texture).tint == &theme::TypePhysics);
    CHECK(ic::ForAsset(K::Material).tint == &theme::TypeLight);
    CHECK(ic::ForAsset(K::Scene).tint == &theme::TypeScene);
    CHECK(ic::ForAsset(K::Vfx).tint == &theme::TypeFolder);   // 無彩色（ライトと混ぜない）
    CHECK(std::strcmp(ic::ForAsset(K::Folder).glyph, ICON_FOLDER) == 0);
    CHECK(std::strcmp(ic::ForAsset(K::Audio).glyph, ICON_MUSIC) == 0);
    CHECK(std::strcmp(ic::ForAsset(K::Sequence).glyph, ICON_T_SEQUENCE) == 0);

    // Graphite のときだけ GG 仕様の kind.material / kind.texture
    const auto saved = theme::g_variant;
    theme::g_variant = theme::Variant::Graphite;
    CHECK(ic::ForAsset(K::Texture).tint == ic::GraphiteKindTint(1));
    CHECK(ic::ForAsset(K::Material).tint == ic::GraphiteKindTint(0));
    CHECK(ic::ForAsset(K::Model).tint == &theme::TypeCamera);
    theme::g_variant = saved;
    CHECK(ic::ForAsset(K::Texture).tint == &theme::TypePhysics);
}

void TestGgIcons()
{
    namespace gi = dx12e::gg_icons;
    CHECK(gi::kIconCount > 100);
    for (int i = 1; i < gi::kIconCount; ++i)
        CHECKF(std::strcmp(gi::kIcons[i - 1].id, gi::kIcons[i].id) < 0, "id が昇順でない: %s", gi::kIcons[i].id);
    for (int i = 0; i < gi::kIconCount; ++i)
    {
        const gi::Icon* f = gi::Find(gi::kIcons[i].id);
        CHECKF(f == &gi::kIcons[i], "Find が自分を返さない: %s", gi::kIcons[i].id);
        CHECKF(gi::kIcons[i].pathCount > 0, "経路が空: %s", gi::kIcons[i].id);
        for (int p = 0; p < gi::kIcons[i].pathCount; ++p)
        {
            const auto& path = gi::kPaths[gi::kIcons[i].firstPath + p];
            CHECK(path.count >= 2 || path.fill);
            for (int k = 0; k < path.count; ++k)
            {
                const float x = gi::kPts[path.first + k][0], y = gi::kPts[path.first + k][1];
                CHECKF(x > -1.0f && x < gi::kIcons[i].vb + 1.0f && y > -1.0f && y < gi::kIcons[i].vb + 1.0f,
                       "グリッドの外: %s (%g,%g)", gi::kIcons[i].id, x, y);
            }
        }
    }
    CHECK(gi::Find("nope") == nullptr);
    CHECK(gi::Find("i-mes") == nullptr);      // 前方一致で拾わない
    CHECK(gi::Find("i-meshx") == nullptr);
    CHECK(gi::Find("mark") == nullptr);       // ブランドのマークは対象外

    // 表が参照する gg の id が実在する
    for (int i = 0; i < static_cast<int>(ic::Id::kCount); ++i)
    {
        const auto r = ic::detail::RowOf(static_cast<ic::Id>(i));
        CHECKF(r.gg && gi::Find(r.gg), "Id %d の gg アイコン %s が無い", i, r.gg ? r.gg : "(null)");
    }

    // 入力（.txt）・.json・.gen.h の個数が一致
    const std::string txt = ReadAll("assets/editor/icons/gg_icons.txt");
    const std::string js = ReadAll("assets/editor/icons/gg_icons.json");
    size_t symbols = 0, p = 0;
    while ((p = txt.find("<symbol id=\"", p)) != std::string::npos) { ++symbols; p += 5; }
    size_t jsonIds = 0;
    p = 0;
    while ((p = js.find("\"id\":\"", p)) != std::string::npos) { ++jsonIds; p += 5; }
    CHECKF(symbols == static_cast<size_t>(gi::kIconCount) + 2, "symbol %zu 個 / gen.h %d 個（ブランドの 2 個を除く）", symbols, gi::kIconCount);
    CHECKF(jsonIds == static_cast<size_t>(gi::kIconCount), "json %zu 個 / gen.h %d 個", jsonIds, gi::kIconCount);
}

void TestNoStrstrLeft()
{
    const std::string insp = ReadAll("src/editor/panels/InspectorPanel.cpp");
    CHECK(insp.find("strstr(label") == std::string::npos);
    const std::string glyph = ReadAll("src/editor/EntityGlyph.h");
    CHECK(glyph.find("strstr") == std::string::npos);
    const std::string ab = ReadAll("src/editor/panels/AssetBrowserPanel.cpp");
    CHECK(ab.find("case Kind::Folder:        return th::TypeFolder") == std::string::npos);   // 種別表は IconTable.h だけ
}
} // namespace

int main()
{
    TestAllComponentNamesCovered();
    TestNoDuplicatesAndExactMatch();
    TestEntity();
    TestAsset();
    TestGgIcons();
    TestNoStrstrLeft();
    if (g_fail) { std::printf("icon_table_test: %d 件失敗\n", g_fail); return 1; }
    std::printf("icon_table_test: OK\n");
    return 0;
}
