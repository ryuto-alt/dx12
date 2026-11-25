// UI テストのレーン A の本体。宣言は gui/UiTestHarnessShared.h の lane-A:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-A ブロックに足す。
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/PathResolver.h"
#include "core/SceneBackup.h"
#include "editor/EditorContext.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneSettingsHash.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_engine.h>
#include <imgui_te_context.h>
#pragma warning(pop)

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace dx12e
{
namespace uitest
{

namespace
{
std::string ReadFileBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
} // namespace

// W05-02: 人の編集（仮想入力で Box を作る）のあと MCP の自動保存が走っても、シーン本体は書かれない。
//   SceneBackup に理由つきの世代が 1 件でき、MCP の editor_state に出す旗（autosaveBlockedByHumanEdits）が立つ。
void T_AutosaveHumanEditNotWritten(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);

    // 検査用のシーンを用意して「保存済み」にする（本体のバイト列を控える）
    const fs::path scenePath = fs::path(PathResolver::AssetsDir()) / "scenes" / "_autosave_probe.json";
    const fs::path root      = fs::path(PathResolver::BaseDir());
    const std::string prevPath = ed->currentScenePath;
    IM_CHECK(SceneSerializer::Save(*app->GetScene(), scenePath.string(), PathResolver::AssetsDir()));
    ed->currentScenePath = scenePath.string();
    ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
    ed->autosaveBlockedByHumanEdits = false;
    ed->humanSettingsDirty = false;
    const std::string before = ReadFileBytes(scenePath);
    const auto mtimeBefore = fs::last_write_time(scenePath);
    const size_t gensBefore = scenebackup::List(root, scenePath, false).size();

    // どこで検査が失敗して抜けても、検査用の状態（旗・保存先・退避・検査用のシーン）を戻す
    struct Cleanup
    {
        EditorContext* ed; Application* app; fs::path root, scenePath; std::string prevPath; size_t gensBefore;
        ~Cleanup()
        {
            ed->aiSessionEver = false;
            ed->mcpSaveCountdown = -1.0f;
            auto gens = scenebackup::List(root, scenePath, false);
            while (gens.size() > gensBefore) { scenebackup::Remove(root, gens.front().id); gens.erase(gens.begin()); }
            ed->currentScenePath = prevPath;
            if (app->GetScene()) ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
            std::error_code ec;
            fs::remove(scenePath, ec);
        }
    } cleanup{ed, app, root, scenePath, prevPath, gensBefore};

    LogStep(ctx, "仮想入力（人の操作）で Box を作る");
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneHumanDirty());

    LogStep(ctx, "MCP の自動保存を武装して 3 秒進める");
    ed->aiSessionEver = true;
    ed->mcpSaveCountdown = 0.5f;   // MCP の書き込みの後と同じ状態
    ctx->SleepNoSkip(3.0f, 0.1f);

    IM_CHECK(ReadFileBytes(scenePath) == before);              // 本体のハッシュ（内容）が不変
    IM_CHECK(fs::last_write_time(scenePath) == mtimeBefore);   // mtime も不変
    IM_CHECK_EQ(scenebackup::List(root, scenePath, true).size(), gensBefore + 1);   // 退避が 1 件
    const auto gens = scenebackup::List(root, scenePath, true);
    IM_CHECK(!gens.empty() && gens.front().reason.rfind("protect-human-edits", 0) == 0);
    IM_CHECK(ed->autosaveBlockedByHumanEdits);   // MCP の結果の autosave.blockedByHumanEdits の元

    // 後始末: Box は Undo で戻す（退避・保存先・検査用のシーンは Cleanup が戻す）
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
}

} // namespace uitest
} // namespace dx12e
