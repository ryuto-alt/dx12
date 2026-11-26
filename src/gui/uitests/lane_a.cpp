// UI テストのレーン A の本体。宣言は gui/UiTestHarnessShared.h の lane-A:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-A ブロックに足す。
#include <algorithm>
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/AppAccess.h"
#include "core/DpiScale.h"
#include "core/PathResolver.h"
#include "core/SceneBackup.h"
#include "editor/panels/MaterialGraphPanel.h"
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

// DF-W05-R2-02: 人の編集が続いて自動保存が退避を繰り返しても、退避は 1 枠に畳まれ、世代数の上限で
//   AI の最初の保存の前の原本（強制で取った世代）を押し出さない。最短間隔はテスト用の口で飛ばす。
void T_AutosaveProtectRollingSlot(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);

    const fs::path scenePath = fs::path(PathResolver::AssetsDir()) / "scenes" / "_autosave_roll_probe.json";
    const fs::path root      = fs::path(PathResolver::BaseDir());
    const std::string prevPath = ed->currentScenePath;
    const scenebackup::Policy prevPolicy = scenebackup::GlobalPolicy();
    IM_CHECK(SceneSerializer::Save(*app->GetScene(), scenePath.string(), PathResolver::AssetsDir()));
    ed->currentScenePath = scenePath.string();
    ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
    ed->autosaveBlockedByHumanEdits = false;
    ed->humanSettingsDirty = false;
    scenebackup::GlobalPolicy().enabled = true;
    scenebackup::GlobalPolicy().generations = 2;
    scenebackup::GlobalPolicy().minIntervalSec = 60;

    const std::vector<std::string> idsBefore = [&] {
        std::vector<std::string> v;
        for (const auto& g : scenebackup::List(root, scenePath, false)) v.push_back(g.id);
        return v;
    }();
    struct Cleanup
    {
        EditorContext* ed; Application* app; fs::path root, scenePath; std::string prevPath; scenebackup::Policy prevPolicy; std::vector<std::string> idsBefore;
        ~Cleanup()
        {
            ed->aiSessionEver = false;
            ed->mcpSaveCountdown = -1.0f;
            for (const auto& g : scenebackup::List(root, scenePath, false))
                if (std::find(idsBefore.begin(), idsBefore.end(), g.id) == idsBefore.end()) scenebackup::Remove(root, g.id);
            scenebackup::GlobalPolicy() = prevPolicy;
            ed->currentScenePath = prevPath;
            if (app->GetScene()) ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
            std::error_code ec;
            fs::remove(scenePath, ec);
        }
    } cleanup{ed, app, root, scenePath, prevPath, prevPolicy, idsBefore};

    // AI の最初の保存の前の原本（強制の世代）
    std::string why;
    const std::string original = scenebackup::Snapshot(root, scenePath, /*force=*/true, &why);
    IM_CHECK(!original.empty());

    ed->aiSessionEver = true;
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        LogStep(ctx, "退避の最短間隔の起点を消してから、人の編集 -> 自動保存");
        AppAccess::ResetAutoProtectThrottleForTest();
        AddEntity(ctx, "Box");
        IM_CHECK(ed->IsSceneHumanDirty());
        ed->mcpSaveCountdown = 0.5f;
        ctx->SleepNoSkip(3.0f, 0.1f);
    }

    const auto gens = scenebackup::List(root, scenePath, true);
    bool hasOriginal = false;
    int autoCount = 0;
    for (const auto& g : gens)
    {
        if (g.id == original) hasOriginal = true;
        if (g.reason == "protect-human-edits:Autosave") ++autoCount;
    }
    IM_CHECK(hasOriginal);          // 原本は押し出されない
    IM_CHECK_EQ(autoCount, 1);      // 自動保存の退避は 1 枠
    IM_CHECK_EQ(gens.size(), size_t(2));

    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
}

// DF-W05-R3-01: 強制の退避（OpenScene）が重複排除で自動保存の世代へ合流したあと、次の自動保存がその世代を消さない
//   （破棄された編集の唯一の写し）。人の編集 -> 自動保存 -> OpenScene の退避 -> 人の編集 -> 自動保存。
void T_AutosaveProtectKeepsForcedMerge(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);

    const fs::path scenePath = fs::path(PathResolver::AssetsDir()) / "scenes" / "_autosave_merge_probe.json";
    const fs::path root      = fs::path(PathResolver::BaseDir());
    const std::string prevPath = ed->currentScenePath;
    const scenebackup::Policy prevPolicy = scenebackup::GlobalPolicy();
    IM_CHECK(SceneSerializer::Save(*app->GetScene(), scenePath.string(), PathResolver::AssetsDir()));
    ed->currentScenePath = scenePath.string();
    ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
    ed->autosaveBlockedByHumanEdits = false;
    ed->humanSettingsDirty = false;
    scenebackup::GlobalPolicy().enabled = true;
    scenebackup::GlobalPolicy().generations = 8;
    scenebackup::GlobalPolicy().minIntervalSec = 60;

    std::vector<std::string> idsBefore;
    for (const auto& g : scenebackup::List(root, scenePath, false)) idsBefore.push_back(g.id);
    struct Cleanup
    {
        EditorContext* ed; Application* app; fs::path root, scenePath; std::string prevPath; scenebackup::Policy prevPolicy; std::vector<std::string> idsBefore;
        ~Cleanup()
        {
            ed->aiSessionEver = false;
            ed->mcpSaveCountdown = -1.0f;
            for (const auto& g : scenebackup::List(root, scenePath, false))
                if (std::find(idsBefore.begin(), idsBefore.end(), g.id) == idsBefore.end()) scenebackup::Remove(root, g.id);
            scenebackup::GlobalPolicy() = prevPolicy;
            ed->currentScenePath = prevPath;
            if (app->GetScene()) ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
            std::error_code ec;
            fs::remove(scenePath, ec);
        }
    } cleanup{ed, app, root, scenePath, prevPath, prevPolicy, idsBefore};

    ed->aiSessionEver = true;
    AppAccess::ResetAutoProtectThrottleForTest();
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneHumanDirty());
    ed->mcpSaveCountdown = 0.5f;
    ctx->SleepNoSkip(3.0f, 0.1f);

    std::string firstId;
    for (const auto& g : scenebackup::List(root, scenePath, true))
        if (g.reason == "protect-human-edits:Autosave") firstId = g.id;
    IM_CHECK(!firstId.empty());

    // OpenScene の強制の退避（同じ内容なので firstId へ合流する）
    IM_CHECK(AppAccess::ProtectBeforeSceneReplace(*app, EditorContext::DiscardReason::OpenScene));

    AppAccess::ResetAutoProtectThrottleForTest();
    AddEntity(ctx, "Sphere");
    ed->mcpSaveCountdown = 0.5f;
    ctx->SleepNoSkip(3.0f, 0.1f);

    bool stillThere = false;
    for (const auto& g : scenebackup::List(root, scenePath, true))
        if (g.id == firstId) stillThere = true;
    IM_CHECK(stillThere);   // 破棄された編集の写しを自動保存が消さない

    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
}

// MOD-ED-01 / MOD-ED-14: マテリアルグラフを DPI 1.0 / 1.5 / 2.0 で開き、ホイールでズームを 40 回往復する。
//   フォントのサイズは 6 段に量子化され、ImGui のテクスチャ用 SRV は DescriptorHeap の遅延返却つき確保で賄う。
//   ImGui の assert（テストエンジンが失敗に変える）が 0、往復のあとも UI（ツールバー・ヒエラルキー）が残ること。
//   アトラスの枚数は多サイズのグリフを焼いて 2 枚以上へ育てたうえで、描画が崩れない（UI が消えない）ことを見る。
void T_MatgraphZoomStress(ImGuiTestContext* ctx)
{
    EditorContext* ed  = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    const bool  bg = app->IsBackground();
    const float origOverride = dpi::Override();

    ed->showMaterialGraph = true;
    ctx->Yield(10);
    MaterialGraphPanel::DebugCall(*ed, "{\"op\":\"new\",\"template\":\"sample\"}");
    ctx->Yield(10);

    static const float kScales[] = {1.0f, 1.5f, 2.0f, 1.0f};
    for (float sc : kScales)
    {
        app->SetUiScaleOverride(sc);
        for (int f = 0; f < 90; ++f)
        {
            ctx->Yield(1);
            const bool sizeOk = !bg || (std::fabs(static_cast<float>(app->GetClientWidthPx()) - 1920.0f * sc) < 1.5f
                                        && std::fabs(static_cast<float>(app->GetClientHeightPx()) - 1080.0f * sc) < 1.5f);
            if (std::fabs(app->GetUiScale() - sc) < 0.001f && sizeOk && f >= 4) break;
        }
        ctx->Yield(10);

        // 多サイズのグリフを焼いて、アトラスのテクスチャを増やす（日本語＋ラテン）
        {
            ImFont* font = ImGui::GetFont();
            for (float px = 8.0f; px <= 96.0f; px += 4.0f)
                font->CalcTextSizeA(px, FLT_MAX, 0.0f, "Material Graph ãããªã¢ã«ã°ã©ã");
            ctx->Yield(3);
        }

        ImGuiWindow* gw = ImGui::FindWindowByName("###MaterialGraph");
        IM_CHECK(gw != nullptr);
        const ImVec2 c(gw->Pos.x + gw->Size.x * 0.5f, gw->Pos.y + gw->Size.y * 0.5f);
        ctx->MouseMoveToPos(c);
        for (int i = 0; i < 40; ++i)
        {
            ctx->MouseWheelY((i % 20) < 10 ? 2.0f : -2.0f);   // 10 回拡大 → 10 回縮小 を 2 往復
            ctx->Yield(2);
        }
        ctx->Yield(6);
        ctx->LogInfo("dpi %.2f: atlas textures=%d", sc, ImGui::GetPlatformIO().Textures.Size);
        IM_CHECK(ImGui::FindWindowByName("###MaterialGraph") != nullptr);
        ImGuiWindow* tb = ImGui::FindWindowByName("##Toolbar");
        ImGuiWindow* hw = ImGui::FindWindowByName("ヒエラルキー");
        IM_CHECK(tb != nullptr && tb->Active && tb->Size.x > 50.0f);   // UI が全部消えていない
        IM_CHECK(hw != nullptr && hw->Active && hw->Size.x > 50.0f);
    }

    app->SetUiScaleOverride(origOverride);
    ctx->Yield(20);
    ed->showMaterialGraph = false;
    ctx->Yield(6);
}

} // namespace uitest
} // namespace dx12e
