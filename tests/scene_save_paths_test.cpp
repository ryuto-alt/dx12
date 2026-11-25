// 保存まわりの単体テスト（W05-12）。
//   ・SceneFileNameFromInput（editor/SceneFileName.h）: 禁止文字 / 予約名 / 末尾の空白とドット / .json の重複
//   ・既存ファイルの検出（Windows は大文字小文字を区別しない）
//   ・ProjectManager::SaveLastOpenedScene が実在しないパスを記録しない
// GPU / ImGui は不要。editor_state.json は DX12E_DATA_DIR で temp 配下へ向ける。
#include "editor/SceneFileName.h"
#include "project/ProjectManager.h"

#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace dx12e;
namespace fs = std::filesystem;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                      \
    } while (0)

bool NameIs(std::string_view in, const char* expect)
{
    const auto r = SceneFileNameFromInput(in);
    return r && *r == expect;
}
bool NameRejected(std::string_view in) { return !SceneFileNameFromInput(in).has_value(); }

void TestNames()
{
    CHECK(NameIs("Forest", "Forest.json"));
    CHECK(NameIs("Forest.json", "Forest.json"));
    CHECK(NameIs("Forest.JSON", "Forest.json"));
    CHECK(NameIs("Forest.json.json", "Forest.json"));
    CHECK(NameIs("  Forest  ", "Forest.json"));
    CHECK(NameIs("Forest...", "Forest.json"));
    CHECK(NameIs("Forest. .", "Forest.json"));
    CHECK(NameIs("森 の シーン", "森 の シーン.json"));
    CHECK(NameIs("a.b", "a.b.json"));
    CHECK(NameIs("CONSOLE", "CONSOLE.json"));       // 予約名の前方一致は不可ではない
    CHECK(NameIs("COM10", "COM10.json"));
    CHECK(NameIs("LPT0", "LPT0.json"));

    CHECK(NameRejected(""));
    CHECK(NameRejected("   "));
    CHECK(NameRejected("..."));
    CHECK(NameRejected(".json"));
    CHECK(NameRejected(" .json "));
    CHECK(NameRejected("a/b"));
    CHECK(NameRejected("a\\b"));
    CHECK(NameRejected("a:b"));
    CHECK(NameRejected("a*b"));
    CHECK(NameRejected("a?b"));
    CHECK(NameRejected("a\"b"));
    CHECK(NameRejected("a<b"));
    CHECK(NameRejected("a>b"));
    CHECK(NameRejected("a|b"));
    CHECK(NameRejected(std::string_view("a\x01" "b")));
    CHECK(NameRejected("CON"));
    CHECK(NameRejected("con"));
    CHECK(NameRejected("Prn"));
    CHECK(NameRejected("AUX.json"));
    CHECK(NameRejected("nul"));
    CHECK(NameRejected("COM1"));
    CHECK(NameRejected("com9"));
    CHECK(NameRejected("LPT3"));
    CHECK(NameRejected("CON.txt"));
    CHECK(NameRejected(std::string(300, 'a')));
}

void TestExistence(const fs::path& root)
{
    const fs::path f = root / "Forest.json";
    { std::ofstream o(f, std::ios::binary); o << "{}"; }
    std::error_code ec;
    CHECK(fs::exists(f, ec));
    CHECK(!fs::exists(root / "Meadow.json", ec));
    // 大文字小文字だけ違う名前は Windows では同じファイル
    CHECK(fs::exists(root / "FOREST.JSON", ec));
    CHECK(fs::equivalent(f, root / "forest.json", ec));
}

void TestLastOpened(const fs::path& root)
{
    const fs::path real = root / "Real.json";
    { std::ofstream o(real, std::ios::binary); o << "{}"; }
    const std::string realStr = real.string();

    ProjectManager::SaveLastOpenedScene(realStr);
    CHECK(ProjectManager::LoadLastOpenedScene() == realStr);

    // 実在しないパスは記録しない（直前の値が残る）
    ProjectManager::SaveLastOpenedScene((root / "Ghost.json").string());
    CHECK(ProjectManager::LoadLastOpenedScene() == realStr);
    ProjectManager::SaveLastOpenedScene("");
    CHECK(ProjectManager::LoadLastOpenedScene() == realStr);
}
}  // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / ("scene_save_paths_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root / "data");
    SetEnvironmentVariableA("DX12E_DATA_DIR", (root / "data").string().c_str());

    TestNames();
    TestExistence(root);
    TestLastOpened(root);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("scene_save_paths_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
