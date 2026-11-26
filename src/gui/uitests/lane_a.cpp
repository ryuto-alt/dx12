// UI テストのレーン A の本体。宣言は gui/UiTestHarnessShared.h の lane-A:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-A ブロックに足す。
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/AppAccess.h"
#include "core/DpiScale.h"
#include "core/PathResolver.h"
#include "core/SceneBackup.h"
#include "core/SafeRemove.h"
#include "core/Logger.h"
#include "project/ProjectManager.h"
#include "input/VirtualInput.h"
#include "editor/AssetBrowserLogic.h"
#include "editor/BottomDock.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "editor/EditorCommands.h"
#include "editor/WorkspaceLogic.h"
#include "editor/nodegraph/GraphTextQuant.h"
#include "editor/panels/MaterialGraphPanel.h"
#include "editor/EditorContext.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
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
#include <set>
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
//   フォントのサイズは 9 段に量子化され、ImGui のテクスチャ用 SRV は DescriptorHeap の遅延返却つき確保で賄う。
//   ImGui の assert（テストエンジンが失敗に変える）が 0、往復のあとも UI（ツールバー・ヒエラルキー）が残ること。
//   アトラスの枚数は多サイズのグリフを焼いて 2 枚以上へ育てたうえで、描画が崩れない（UI が消えない）ことを見る。
void T_MatgraphZoomStress(ImGuiTestContext* ctx)
{
    EditorContext* ed  = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    const bool  bg = app->IsBackground();
    const float origOverride = dpi::Override();

    // QuantizeText の純関数検査: 段ちょうどで恒等・縮小のみ（r <= 1）・zoom 0.1..4.0 で焼くサイズの種類が段数に収まる。
    {
        using ng::QuantizeText;
        for (float s : ng::kTextZoomSteps)
        {
            const ng::QuantText q = QuantizeText(std::floor(13.0f * s + 0.5f), s);
            IM_CHECK(q.r == 1.0f);   // 段ちょうどは頂点にも触らない
        }
        for (float dpiScale : {1.0f, 1.5f, 2.0f})
            for (float units : {9.0f, 11.0f, 13.0f, 16.0f})
            {
                std::set<int> distinct;
                for (float z = 0.1f; z <= 4.0001f; z += 0.01f)
                {
                    const float px = std::floor(units * z * dpiScale + 0.5f);
                    if (px < 5.0f) continue;
                    const ng::QuantText q = QuantizeText(px, z);
                    IM_CHECK(q.qpx >= px - 0.5f || z < 0.5f);   // 下限 0.5 より小さい zoom だけは拡大側へ丸めない
                    IM_CHECK(q.r <= 1.0f + 1e-4f || z < 0.5f);
                    distinct.insert(static_cast<int>(q.qpx));
                }
                IM_CHECK(static_cast<int>(distinct.size()) <= ng::kTextZoomStepCount * 3);   // px の丸め誤差（±0.5 を step/zoom 倍）で段ごとに高々 3 種。量子化が無ければ 100 種前後
            }
    }

    ed->showMaterialGraph = true;
    ctx->Yield(10);
    MaterialGraphPanel::DebugCall(*ed, "{\"op\":\"new\",\"template\":\"sample\"}");
    ctx->Yield(10);

    // 基準: アトラスの現在のテクスチャ（UniqueID）と SRV ヒープの空き数。
    const int atlasId0 = ImGui::GetIO().Fonts->TexData ? ImGui::GetIO().Fonts->TexData->UniqueID : -1;
    const u32 srvFree0 = app->GetDiagRenderHealth().srvHeapFree;
    bool atlasGrew = false;

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

        // 多サイズのグリフを焼いて、アトラスを育てる（ラテン＋かな。日本語は u8 エスケープ）
        {
            ImFont* font = ImGui::GetFont();
            for (float px = 24.0f; px <= 120.0f; px += 8.0f)
            {
                font->CalcTextSizeA(px, FLT_MAX, 0.0f, "Material Graph ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
                font->CalcTextSizeA(px, FLT_MAX, 0.0f, "\xE3\x83\x9E" "\xE3\x83\x86" "\xE3\x83\xAA" "\xE3\x82\xA2" "\xE3\x83\xAB" "\xE3\x82\xB0" "\xE3\x83\xA9" "\xE3\x83\x95" "\xE3\x80\x80" "\xE3\x81\x82" "\xE3\x81\x84" "\xE3\x81\x86" "\xE3\x81\x88" "\xE3\x81\x8A");
            }
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
        {
            // 焼いた（ImFontBaked）数。GraphView.cpp の行高さ測定が量子化されるまでは増えるので、ログだけ残す。
            ImFontAtlas* atlas = ImGui::GetIO().Fonts;
            ctx->LogInfo("dpi %.2f: baked=%d", sc, atlas->Builder ? atlas->Builder->BakedPool.Size : -1);
        }
        const int atlasId = ImGui::GetIO().Fonts->TexData ? ImGui::GetIO().Fonts->TexData->UniqueID : -1;
        if (atlasId != atlasId0) atlasGrew = true;
        ctx->LogInfo("dpi %.2f: atlas textures=%d uniqueId=%d (base %d)", sc, ImGui::GetPlatformIO().Textures.Size, atlasId, atlasId0);
        IM_CHECK(ImGui::FindWindowByName("###MaterialGraph") != nullptr);
        ImGuiWindow* tb = ImGui::FindWindowByName("##Toolbar");
        ImGuiWindow* hw = ImGui::FindWindowByName("ヒエラルキー");
        IM_CHECK(tb != nullptr && tb->Active && tb->Size.x > 50.0f);   // UI が全部消えていない
        IM_CHECK(tb->DrawList != nullptr && tb->DrawList->VtxBuffer.Size > 0);   // クロームが実際に描かれている
        IM_CHECK(hw != nullptr && hw->Active && hw->Size.x > 50.0f);
    }

    app->SetUiScaleOverride(origOverride);
    ctx->Yield(20);
    // 開いたグラフとズームを残さない（sample を作り直してビューを初期化する。閉じる op は無い）
    MaterialGraphPanel::DebugCall(*ed, "{\"op\":\"new\",\"template\":\"sample\"}");
    ctx->Yield(6);
    ed->showMaterialGraph = false;
    ctx->Yield(6);

    // アトラスが育ち（旧テクスチャの破棄と新規確保が走り）、SRV が漏れていないこと。
    // 返却は NumFramesInFlight フレーム遅れるので、数フレーム回してから数える。
    ctx->Yield(30);
    const u32 srvFree1 = app->GetDiagRenderHealth().srvHeapFree;
    ctx->LogInfo("srv free: %u -> %u, atlas grew=%d", srvFree0, srvFree1, atlasGrew ? 1 : 0);
    IM_CHECK(atlasGrew);
    // 許容 +2: 新しいアトラスのテクスチャ 1 枚と、旧テクスチャの遅延返却がまだ残る分（NumFramesInFlight 遅れ）。
    IM_CHECK(srvFree1 + 2 >= srvFree0);
}

// W1-A10 / BUG-02 / BUG-42: アニメ・シーケンサーの暫定の案内、.dxseq / .uvfx の種別、.dxseq の再生要求。
//   (1) 下部タブ「timeline」: 登録タイトルに「ダミー」が無く、描かれた案内の文面が ws::kTimelineGuidance（W3 と .dxseq のダブルクリックを案内）。
//   (2) .dxseq = 「シーケンス」+ ICON_T_LAYERS + アニメ系の色 / .uvfx = 「VFX」+ ICON_T_PARTICLE + 光り物の色。
//   (3) アセットブラウザの本物のダブルクリック → .dxseq は assets 相対パスで再生が始まる / .uvfx は pendingVfxPlace が立つ（消すのは BD4）。
void T_WsAnimationPlaceholder(ImGuiTestContext* ctx)
{
    EditorContext* ed  = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr);
    namespace fs = std::filesystem;
    const std::string prevWs = ed->currentWorkspace;

    // 後始末（IM_CHECK で途中終了しても必ず走る）: 再生を止める・文書を閉じる・一時ファイルとフォルダを消す。
    struct Cleanup
    {
        Application* app; EditorContext* ed; fs::path seqRoot, workDir; bool madeSeqRoot; std::string docRel; std::string prevWs;
        ~Cleanup()
        {
            AppAccess::SequenceEditorStopForTest(*app);
            if (ed->assetBrowser) ed->assetBrowser->NavigateTo(seqRoot.parent_path());   // 消すフォルダの中に居残らない
            if (!prevWs.empty() && ed->currentWorkspace != prevWs)
            {
                const cmd::Env env{app->GetScene(), PathResolver::AssetsDir()};
                cmd::Execute(*ed, env, "workspace." + prevWs);
            }
            if (!docRel.empty()) AppAccess::SequenceCloseDocForTest(*app, docRel);
            ed->pendingSequencePlay.clear();
            ed->pendingVfxPlace.clear();
            std::error_code ec;
            saferm::RemoveAll(workDir, ec, seqRoot);                      // このテストが作った一時フォルダだけ
            if (madeSeqRoot && fs::is_empty(seqRoot, ec)) fs::remove(seqRoot, ec);
        }
    };

    // (2) 種別の部品（描画に使う表と同じものを引く）
    {
        IM_CHECK(abl::ClassifyExtension(".dxseq") == abl::Kind::Sequence);
        IM_CHECK(abl::ClassifyExtension(".DXSEQ") == abl::Kind::Sequence);
        IM_CHECK(abl::ClassifyExtension(".uvfx") == abl::Kind::Vfx);
        IM_CHECK(std::string(abl::KindLabel(abl::Kind::Sequence)) == "シーケンス");
        IM_CHECK(std::string(abl::KindLabel(abl::Kind::Vfx)) == "VFX");
        IM_CHECK(std::string(AssetBrowserPanel::KindIcon(abl::Kind::Sequence)) == ICON_T_LAYERS);
        IM_CHECK(std::string(AssetBrowserPanel::KindIcon(abl::Kind::Vfx)) == ICON_T_PARTICLE);
        IM_CHECK(std::string(AssetBrowserPanel::KindIcon(abl::Kind::Sequence)) != ICON_FILE);
        IM_CHECK(AssetBrowserPanel::KindTintU32(abl::Kind::Sequence) == ImGui::ColorConvertFloat4ToU32(dx12e::theme::TypeUi));
        IM_CHECK(AssetBrowserPanel::KindTintU32(abl::Kind::Vfx) == ImGui::ColorConvertFloat4ToU32(dx12e::theme::TypeFolder));
    }

    // (1) ワークスペース「アニメ・シーケンサー」→ 下部タブが開き、描かれた案内が定数と同じ文面
    {
        const std::string g = ws::kTimelineGuidance;
        IM_CHECK(g.find("ダミー") == std::string::npos);
        IM_CHECK(g.find("W3") != std::string::npos);
        IM_CHECK(g.find(".dxseq") != std::string::npos);
        IM_CHECK(g.find("ダブルクリック") != std::string::npos);

        const cmd::Env env{app->GetScene(), PathResolver::AssetsDir()};
        cmd::Execute(*ed, env, "workspace.animation");
        ctx->Yield(12);
        IM_CHECK(ed->currentWorkspace == "animation");
        IM_CHECK(bottomdock::IsOpen("timeline"));

        const bottomdock::Tab* tab = bottomdock::Find("timeline");
        IM_CHECK(tab != nullptr);
        IM_CHECK(tab->title.find("ダミー") == std::string::npos);   // タブに出る名前（bottom_dock_slot は終わりに本物を戻す）
        const std::string winName = bottomdock::WindowName(*tab);
        const ImGuiID win = FocusWindow(ctx, winName.c_str());
        IM_CHECK(win != 0);
        ctx->SetRef(win);
        const ImGuiTestItemInfo info = ctx->ItemInfo("##timelineGuidance", ImGuiTestOpFlags_NoError);
        IM_CHECK(info.ID != 0);
        const float want = ImGui::CalcTextSize(ws::kTimelineGuidance).x;   // 描かれた文字の矩形 = 定数の文面の幅
        ctx->LogInfo("案内の幅 %.1f（期待 %.1f）", info.RectFull.GetWidth(), want);
        IM_CHECK_LT(std::fabs(info.RectFull.GetWidth() - want), 2.0f);
    }

    // (3) アセットブラウザの本物のダブルクリック
    AssetBrowserPanel* ab = ed->assetBrowser;
    IM_CHECK(ab != nullptr);
    IM_CHECK(app->GetEngineMode() == Application::EngineMode::Editor);
    const fs::path seqRoot = fs::path(PathResolver::AssetsDir()) / "sequences";
    std::error_code ec;
    Cleanup cl{app, ed, seqRoot, seqRoot / "__a10_ui_dir", !fs::exists(seqRoot, ec), {}, prevWs};
    fs::create_directories(cl.workDir, ec);
    const fs::path seqFile = cl.workDir / "__a10_ui_test.dxseq";
    const fs::path vfxFile = cl.workDir / "__a10_ui_test.uvfx";
    {
        std::ofstream o(seqFile, std::ios::binary);
        o << "{\"format\":\"dxseq\",\"version\":1,\"id\":\"q_a10test\",\"name\":\"__a10_ui_test\",\"ticksPerSecond\":6000,"
             "\"frameRate\":30,\"range\":[0,60000],\"meta\":{},\"cuts\":[],\"markers\":[],\"bindings\":[]}";
    }
    const std::string seqRel = "sequences/__a10_ui_dir/__a10_ui_test.dxseq";
    const std::string vfxRel = "sequences/__a10_ui_dir/__a10_ui_test.uvfx";
    cl.docRel = seqRel;

    // 一覧をこのフォルダへ（「上へ」が 0 番、ファイルが 1 番）
    const ImGuiID assets = FocusWindow(ctx, kWinAssets);
    IM_CHECK(assets != 0);
    ab->NavigateTo(cl.workDir);
    auto waitOne = [&]() {
        for (int i = 0; i < 600; ++i)
        {
            ctx->Yield(1);
            if (ab->CurrentDir() == cl.workDir && !ab->IsScanning() && ab->EntryCount() == 1) { ctx->Yield(3); return true; }
        }
        return false;
    };
    auto itemsWindow = [&]() -> ImGuiID {
        for (ImGuiWindow* w : ctx->UiContext->Windows)
            if (w->WasActive && std::strstr(w->Name, "##Items") != nullptr && std::strstr(w->Name, "アセットブラウザ") != nullptr) return w->ID;
        return 0;
    };
    auto doubleClickFirst = [&]() -> bool {
        const ImGuiID items = itemsWindow();
        if (items == 0) return false;
        ctx->SetRef(items);
        const char* path = ctx->ItemExists("**/$$1/##cell") ? "**/$$1/##cell" : (ctx->ItemExists("**/$$1/##row") ? "**/$$1/##row" : nullptr);
        if (!path) return false;
        ctx->ItemDoubleClick(path);
        return true;
    };

    IM_CHECK(waitOne());
    IM_CHECK(doubleClickFirst());
    std::string playing;
    for (int i = 0; i < 60 && playing.empty(); ++i) { ctx->Yield(1); playing = AppAccess::SequenceEditorPlayingDoc(*app); }
    const std::string loadedRel = AppAccess::SequenceDocRelForTest(*app, seqRel);
    ctx->LogInfo("playing='%s' doc rel='%s' pending='%s'", playing.c_str(), loadedRel.c_str(), ed->pendingSequencePlay.c_str());
    IM_CHECK(ed->pendingSequencePlay.empty());      // Application が消費した
    IM_CHECK(loadedRel == seqRel);                  // パネルが組んだ assets 相対パスがそのまま読み込まれた
    IM_CHECK(!playing.empty());                     // 再生が始まった
    AppAccess::SequenceEditorStopForTest(*app);
    IM_CHECK(AppAccess::SequenceEditorPlayingDoc(*app).empty());

    // .uvfx のダブルクリック: 置く要求が立つ（消費は BD4。A は消さない）
    fs::remove(seqFile, ec);
    {
        std::ofstream o(vfxFile, std::ios::binary);
        o << "{}";
    }
    ab->NavigateTo(cl.workDir);
    ctx->Yield(2);
    IM_CHECK(waitOne());
    ed->pendingVfxPlace.clear();
    IM_CHECK(doubleClickFirst());
    // BD4 が先に統合済みなら、要求は同フレームで消費されて空になる。未統合なら立ったまま。どちらでも可（立っているなら値を検査）。
    ctx->Yield(1);
    ctx->LogInfo("pendingVfxPlace='%s'", ed->pendingVfxPlace.c_str());
    IM_CHECK(ed->pendingVfxPlace.empty() || ed->pendingVfxPlace == vfxRel);
    IM_CHECK(ed->pendingSequencePlay.empty());
}

// W1-A15（BUG-21 の暫定策）: パーティクルエディタの「適用」は、レイヤー 0 に中身があると確認を出す。
//   キャンセルでレイヤーが不変・Undo も積まれない / 「置き換え」で適用され、Ctrl+Z 相当の Undo で元の値に戻る。
//   MCP のセッション（aiSessionEver）かつ仮想入力中はモーダルを出さず、そのまま適用される（AI は押せないため）。
//   実入力で aiSessionEver=true でも確認が出ることは単体テスト（ShouldConfirmReplace）で固定している。
void T_VfxApplyConfirm(ImGuiTestContext* ctx)
{
    EditorContext* ed  = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;
    auto& reg = app->GetScene()->GetRegistry();

    struct Cleanup
    {
        EditorContext* ed; entt::registry* reg; entt::entity e; bool prevShow; bool prevAi;
        ~Cleanup()
        {
            ed->aiSessionEver = prevAi;
            ed->showVfxEditor = prevShow;
            if (reg->valid(e)) { ed->ClearSelection(); reg->destroy(e); }
        }
    } cleanup{ed, &reg, entt::null, ed->showVfxEditor, ed->aiSessionEver};

    LogStep(ctx, "中身のあるレイヤー 0 を持つ放出器を作って選ぶ");
    const entt::entity e = reg.create();
    cleanup.e = e;
    reg.emplace<NameTag>(e, NameTag{"UiTestVfxApply"});
    reg.emplace<Transform>(e);
    ParticleEmitter em;
    em.layers.emplace_back();
    em.layers[0].name = "Torch"; em.layers[0].rate = 11.0f; em.layers[0].kind = 3;
    reg.emplace<ParticleEmitter>(e, em);
    const ParticleEmitter original = reg.get<ParticleEmitter>(e);
    ed->aiSessionEver = false;
    ed->Select(e);
    // 最後の「MCP では出ない」手順は仮想入力が前提。そうでない実行では理由を明示して止める
    IM_CHECK(vinput::Enabled());
    if (!vinput::Enabled()) return;

    const ImGuiID win = OpenToolWindow(ctx, &EditorContext::showVfxEditor, kWinVfx);
    IM_CHECK(win != 0);
    if (win == 0) return;
    ctx->Yield(4);

    const size_t undoBefore = ed->undoSystem.UndoDepth();

    LogStep(ctx, "「選択エンティティへ適用」で確認が出る（まだ適用されない）");
    IM_CHECK(ClickPath(ctx, ChildWindowId(ctx, kWinVfxMain), "選択エンティティへ適用"));
    ctx->Yield(6);
    ImGuiID confirm = FocusWindow(ctx, "//レイヤー 0 を置き換えます###VfxReplaceLayer0Popup");
    IM_CHECK(confirm != 0);
    IM_CHECK(reg.get<ParticleEmitter>(e) == original);

    LogStep(ctx, "キャンセルでレイヤーが不変・Undo も積まれない");
    if (confirm != 0) { ctx->SetRef(confirm); ctx->ItemClick("キャンセル"); }
    ctx->Yield(6);
    IM_CHECK(reg.get<ParticleEmitter>(e) == original);
    IM_CHECK(ed->undoSystem.UndoDepth() == undoBefore);
    IM_CHECK(FocusWindow(ctx, "//レイヤー 0 を置き換えます###VfxReplaceLayer0Popup") == 0);

    LogStep(ctx, "もう一度押して「置き換え」で適用され、Undo で元に戻る");
    IM_CHECK(ClickPath(ctx, ChildWindowId(ctx, kWinVfxMain), "選択エンティティへ適用"));
    ctx->Yield(6);
    confirm = FocusWindow(ctx, "//レイヤー 0 を置き換えます###VfxReplaceLayer0Popup");
    IM_CHECK(confirm != 0);
    if (confirm != 0) { ctx->SetRef(confirm); ctx->ItemClick("置き換え"); }
    ctx->Yield(6);
    IM_CHECK(reg.get<ParticleEmitter>(e) != original);
    IM_CHECK(ed->undoSystem.UndoDepth() == undoBefore + 1);
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);                         // Ctrl+Z の経路
    ctx->Yield(6);
    IM_CHECK(reg.get<ParticleEmitter>(e) == original);

    LogStep(ctx, "MCP のセッションではモーダルを出さずにそのまま適用される");
    ed->aiSessionEver = true;
    IM_CHECK(ClickPath(ctx, ChildWindowId(ctx, kWinVfxMain), "選択エンティティへ適用"));
    ctx->Yield(6);
    IM_CHECK(FocusWindow(ctx, "//レイヤー 0 を置き換えます###VfxReplaceLayer0Popup") == 0);
    IM_CHECK(reg.get<ParticleEmitter>(e) != original);
    ed->undoSystem.Undo();
    IM_CHECK(reg.get<ParticleEmitter>(e) == original);
}


// ===========================================================================
// W1-A16: DG05 の取りこぼし（エディタの操作性）。仮想入力（--background）で動く。
// ===========================================================================
namespace
{
ImGuiWindow* NavRoot()
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    return (g && g->NavWindow) ? g->NavWindow->RootWindow : nullptr;
}
bool NavIs(const char* name)
{
    ImGuiWindow* w = NavRoot();
    return w && std::strcmp(w->Name, name) == 0;
}

// 後始末（シーンの保存先と「保存済み」の印を元へ戻す）
struct SceneStateGuard
{
    EditorContext* ed; Application* app; std::string prevPath;
    ~SceneStateGuard()
    {
        ed->currentScenePath = prevPath;
        if (!prevPath.empty()) ProjectManager::SaveLastOpenedScene(prevPath);
        ed->saveAsOverwrite.open = false;
        ed->saveAsOverwrite.path.clear();
        ed->showNewSceneDialog = false;
        if (app->GetScene()) ed->MarkSceneSaved(SceneSettingsFingerprint(*app->GetScene()));
    }
};
} // namespace

// (a) R1-08 / R3-02: ブロッキングなモーダル（上書き確認）が開いている間、ビューポートのクリックによる選択変更・
//     Ctrl+Z・Ctrl+S はシーンに効かない。閉じれば効く（同じ操作で対照）。
void T_ModalBlocksSceneActions(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;

    const fs::path probe = fs::path(PathResolver::AssetsDir()) / "scenes" / "_a16_modal_probe.json";
    std::error_code ec;
    fs::remove(probe, ec);
    SceneStateGuard guard{ed, app, ed->currentScenePath};
    ed->currentScenePath = probe.string();   // Ctrl+S が保存先の確認ではなく本当に保存へ行く状態にする
    ed->aiSessionEver = false;

    LogStep(ctx, "Box を作る（未保存の人の編集）");
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneDirty());
    const entt::entity sel = ed->selectedEntity;
    IM_CHECK(sel != entt::null && app->GetScene()->GetRegistry().valid(sel));
    const size_t depth = ed->undoSystem.UndoDepth();
    IM_CHECK(ed->viewportW > 100.0f && ed->viewportH > 100.0f);
    // ビューポートの左下（モーダルは画面中央・ビュー操作の帯は上端なので、何も重ならない空所）
    const ImVec2 spot(ed->viewportX + ed->viewportW * 0.08f, ed->viewportY + ed->viewportH * 0.85f);

    LogStep(ctx, "対照: モーダルが無ければ、空所のクリックで選択が外れる");
    ctx->MouseMoveToPos(spot);
    ctx->MouseClick(0);
    ctx->Yield(4);
    IM_CHECK(!ed->HasSelection());
    ed->Select(sel);
    ctx->Yield(2);

    LogStep(ctx, "上書き確認のモーダルを開く");
    ed->saveAsOverwrite.path = (fs::path(PathResolver::AssetsDir()) / "scenes" / "_a16_modal_target.json").string();
    ed->saveAsOverwrite.open = true;
    ctx->Yield(4);
    const ImGuiID modal = FocusWindow(ctx, "//名前を付けて保存###SaveAsOverwritePopup");
    IM_CHECK(modal != 0);

    LogStep(ctx, "空所のクリックで選択が外れない");
    ctx->MouseMoveToPos(spot);
    ctx->MouseClick(0);
    ctx->Yield(4);
    IM_CHECK(ed->HasSelection() && ed->selectedEntity == sel);

    LogStep(ctx, "Ctrl+Z で Box が消えない（Undo されない）");
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->undoSystem.UndoDepth() == depth);
    IM_CHECK(app->GetScene()->GetRegistry().valid(sel));
    ctx->KeyPress(ImGuiKey_Y | ImGuiMod_Ctrl);
    ctx->Yield(4);
    IM_CHECK(ed->undoSystem.RedoDepth() == 0);

    LogStep(ctx, "Ctrl+S でファイルが書かれず、未保存のまま");
    ctx->KeyPress(ImGuiKey_S | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(!fs::exists(probe, ec));
    IM_CHECK(ed->IsSceneDirty());

    LogStep(ctx, "キャンセルで閉じると、同じ Ctrl+Z が効く");
    ctx->SetRef(modal);
    ctx->ItemClick("キャンセル");
    ctx->Yield(6);
    IM_CHECK(FocusWindow(ctx, "//名前を付けて保存###SaveAsOverwritePopup") == 0);
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->undoSystem.UndoDepth() == depth - 1);
    fs::remove(probe, ec);
}

// (b) R2-03: MCP が作った無題のシーン（.dx12/untitled/ へ仮の保存先が付いている）で人が Ctrl+S → 名前を聞く。
void T_SaveUntitledAsksName(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;

    SceneStateGuard guard{ed, app, ed->currentScenePath};
    std::error_code ec;
    const fs::path untitled = fs::path(AppAccess::UntitledDir()) / "_mcp_untitled_a16probe.json";
    fs::remove(untitled, ec);

    LogStep(ctx, "仮の保存先（.dx12/untitled/）が付いたシーンで Ctrl+S");
    ed->currentScenePath = untitled.string();
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_S | ImGuiMod_Ctrl);
    ctx->Yield(8);
    const ImGuiID dlg = FocusWindow(ctx, "//名前を付けて保存###NewScenePopup");
    IM_CHECK(dlg != 0);                      // 名前を聞く
    IM_CHECK(!fs::exists(untitled, ec));     // 隠しフォルダへは書かれない
    if (dlg != 0) { ctx->SetRef(dlg); ctx->ItemClick("キャンセル"); }
    ctx->Yield(6);
    IM_CHECK(FocusWindow(ctx, "//名前を付けて保存###NewScenePopup") == 0);

    LogStep(ctx, "対照: scenes/ の普通のシーンは名前を聞かずに保存される");
    const fs::path normal = fs::path(PathResolver::AssetsDir()) / "scenes" / "_a16_normal_probe.json";
    fs::remove(normal, ec);
    ed->currentScenePath = normal.string();
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_S | ImGuiMod_Ctrl);
    ctx->Yield(8);
    IM_CHECK(fs::exists(normal, ec));
    IM_CHECK(FocusWindow(ctx, "//名前を付けて保存###NewScenePopup") == 0);
    fs::remove(normal, ec);
}

// (b)/(f) 未保存の確認の「保存して続行」も、保存先が MCP の仮の保存先なら隠しフォルダへ書かず、操作を取り消して名前を聞く。
void T_UnsavedConfirmSaveUntitledAsksName(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;

    const fs::path target   = fs::path(PathResolver::AssetsDir()) / "scenes" / "_a16_untitledconfirm_target.json";
    const fs::path untitled = fs::path(AppAccess::UntitledDir()) / "_mcp_untitled_a16confirm.json";
    std::error_code ec;
    fs::remove(target, ec);
    fs::remove(untitled, ec);
    const bool origAi = ed->aiSessionEver;
    SceneStateGuard guard{ed, app, ed->currentScenePath};
    ed->currentScenePath = untitled.string();
    ed->aiSessionEver = true;

    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneHumanDirty());
    const size_t entitiesBefore = app->GetScene()->GetRegistry().storage<entt::entity>().size();

    LogStep(ctx, "Ctrl+N の「作成」→ 未保存の確認 →「保存して続行」");
    ed->newSceneDialogIsCreate = true;
    std::memset(ed->newSceneNameBuf, 0, sizeof(ed->newSceneNameBuf));
    strncpy_s(ed->newSceneNameBuf, "_a16_untitledconfirm_target", _TRUNCATE);
    ed->showNewSceneDialog = true;
    ctx->Yield(4);
    const ImGuiID dlg = FocusWindow(ctx, "//新規シーン###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) { ctx->SetRef(dlg); ctx->ItemClick("作成"); }
    ctx->Yield(10);
    const ImGuiID confirm = FocusWindow(ctx, "//##UnsavedConfirm");
    IM_CHECK(confirm != 0);
    if (confirm != 0) { ctx->SetRef(confirm); ctx->ItemClick("保存して続行"); }
    ctx->Yield(12);

    IM_CHECK(!fs::exists(untitled, ec));      // 隠しフォルダへは書かれない
    IM_CHECK(!fs::exists(target, ec));        // 新しいシーンへも入れ替わっていない
    IM_CHECK(app->GetScene()->GetRegistry().storage<entt::entity>().size() == entitiesBefore);
    const ImGuiID saveAs = FocusWindow(ctx, "//名前を付けて保存###NewScenePopup");
    IM_CHECK(saveAs != 0);                    // 名前を聞く
    if (saveAs != 0) { ctx->SetRef(saveAs); ctx->ItemClick("キャンセル"); }
    ctx->Yield(6);

    ed->aiSessionEver = origAi;
    fs::remove(target, ec);
    fs::remove(untitled, ec);
}

// (c) R1-04: 「エラーで前面」は 1 回のバーストにつき 1 回。エラーが出続けてもフォーカスを奪い続けず、モーダルが開いている間は奪わない。
void T_ConsoleErrorFocusOnce(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    IM_CHECK(ed != nullptr);
    if (!ed) return;
    IM_CHECK(FocusWindow(ctx, kWinConsole) != 0);   // コンソールが出ている前提（無いなら検査にならない）
    IM_CHECK(FocusWindow(ctx, kWinHierarchy) != 0);
    ctx->SleepNoSkip(2.5f, 0.1f);                    // 直前のエラーから間を空けて、新しいバーストにする

    LogStep(ctx, "最初のエラーでコンソールが前面に来る");
    Logger::Error("A16 テスト用のエラー 0（意図したもの）");
    ctx->Yield(4);
    IM_CHECK(NavIs("コンソール"));

    LogStep(ctx, "エラーが出続けても、ヒエラルキーへ戻したフォーカスを奪い返さない");
    IM_CHECK(FocusWindow(ctx, kWinHierarchy) != 0);
    IM_CHECK(NavIs("ヒエラルキー"));
    for (int i = 1; i <= 40; ++i)
    {
        Logger::Error("A16 テスト用のエラー {}（意図したもの）", i);
        ctx->Yield(1);
        IM_CHECK(NavIs("ヒエラルキー"));
    }

    LogStep(ctx, "モーダルが開いている間は、間を空けた新しいバーストでも奪わない");
    ctx->SleepNoSkip(2.5f, 0.1f);
    ed->saveAsOverwrite.path = "_a16_dummy.json";
    ed->saveAsOverwrite.open = true;
    ctx->Yield(4);
    const ImGuiID modal = FocusWindow(ctx, "//名前を付けて保存###SaveAsOverwritePopup");
    IM_CHECK(modal != 0);
    Logger::Error("A16 テスト用のエラー（モーダル中・意図したもの）");
    ctx->Yield(6);
    IM_CHECK(!NavIs("コンソール"));
    IM_CHECK(ImGui::GetTopMostPopupModal() != nullptr);
    ctx->SetRef(modal);
    ctx->ItemClick("キャンセル");
    ctx->Yield(6);
    ed->saveAsOverwrite.path.clear();
}

// (d) R1-05: F2 のあと、拡張子まで打っても二重にならない（box4.txt → box4.txt、box4.txt.txt にならない）。
void T_RenameTypedExtNotDoubled(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    IM_CHECK(ed != nullptr && ed->assetBrowser != nullptr);
    if (!ed || !ed->assetBrowser) return;
    AssetBrowserPanel* ab = ed->assetBrowser;
    const fs::path assets = fs::path(PathResolver::AssetsDir()).lexically_normal();
    const fs::path work = assets / "__a16_rename_ext_test";
    const uint32_t origMask = ab->KindMask();
    std::error_code ec;
    saferm::RemoveAll(work, ec, assets);
    fs::create_directories(work);
    { std::ofstream(work / "x.txt", std::ios::binary) << "a"; }
    { std::ofstream(work / "y.txt", std::ios::binary) << "b"; }

    auto itemsWindow = [&]() -> ImGuiID
    {
        ImGuiID id = ChildWindowId(ctx, "//アセットブラウザ/##FileGrid/##Items");
        if (id != 0) return id;
        for (ImGuiWindow* w : ctx->UiContext->Windows)
            if (w->WasActive && std::strstr(w->Name, "##Items") && std::strstr(w->Name, "アセットブラウザ")) return w->ID;
        return 0;
    };
    auto waitExists = [&](const fs::path& p, bool want)
    {
        for (int i = 0; i < 240; ++i)
        {
            std::error_code e2;
            if (fs::exists(p, e2) == want) { ctx->Yield(3); return true; }
            ctx->Yield(1);
        }
        return false;
    };
    auto beginF2 = [&](const fs::path& target)
    {
        FocusWindow(ctx, kWinAssets);
        ab->SelectOnly(target);
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_F2);
        ImGuiID items = 0;
        for (int i = 0; i < 30 && !(items = itemsWindow(), ItemExistsIn(ctx, items, "**/##Rename")); ++i) ctx->Yield(1);
        ctx->Yield(8);   // 入力欄の準備（3 フレーム）と初期選択の掛け直し（5 フレーム）を越える
        return ItemExistsIn(ctx, items, "**/##Rename");
    };

    LogStep(ctx, "使い捨てフォルダへ移動");
    ab->SetListView(false);
    ab->NavigateTo(work);
    for (int i = 0; i < 600 && (ab->IsScanning() || ab->EntryCount() != 2); ++i) ctx->Yield(1);
    IM_CHECK(ab->EntryCount() == 2);

    LogStep(ctx, "x.txt: F2 → 拡張子まで打つ（box4.txt）→ Enter → box4.txt（box4.txt.txt にならない）");
    IM_CHECK(beginF2(work / "x.txt"));
    ctx->KeyChars("box4.txt");
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(waitExists(work / "box4.txt", true));
    IM_CHECK(!fs::exists(work / "box4.txt.txt", ec));
    IM_CHECK(!fs::exists(work / "x.txt", ec));

    LogStep(ctx, "y.txt: 大文字の拡張子（Box5.TXT）でも二重にならない");
    IM_CHECK(beginF2(work / "y.txt"));
    ctx->KeyChars("Box5.TXT");
    ctx->KeyPress(ImGuiKey_Enter);
    for (int i = 0; i < 240 && fs::exists(work / "y.txt", ec); ++i) ctx->Yield(1);
    ctx->Yield(4);
    int found = 0;
    for (const auto& de : fs::directory_iterator(work, ec))
    {
        std::string n = de.path().filename().string();
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n == "box5.txt") ++found;
        IM_CHECK(n != "box5.txt.txt");
    }
    IM_CHECK(found == 1);

    LogStep(ctx, "後始末");
    ab->SetKindMask(origMask);
    ab->NavigateTo(assets);
    ctx->Yield(10);
    saferm::RemoveAll(work, ec, assets);
}

// (e) R1-09: 人の編集をすべて Undo で戻すと、保存した状態に戻ったことになり「未保存」が解ける（Redo で再び未保存）。
void T_UndoToSavedStateClearsDirty(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;

    const fs::path probe = fs::path(PathResolver::AssetsDir()) / "scenes" / "_a16_savepoint_probe.json";
    std::error_code ec;
    fs::remove(probe, ec);
    SceneStateGuard guard{ed, app, ed->currentScenePath};
    ed->currentScenePath = probe.string();

    LogStep(ctx, "Ctrl+S で保存して「保存済み」にする");
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_S | ImGuiMod_Ctrl);
    ctx->Yield(8);
    IM_CHECK(fs::exists(probe, ec));
    IM_CHECK(!ed->IsSceneDirty() && !ed->IsSceneHumanDirty());

    LogStep(ctx, "Box を 2 つ作る → 未保存");
    AddEntity(ctx, "Box");
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneDirty() && ed->IsSceneHumanDirty());

    LogStep(ctx, "1 つ戻してもまだ未保存、全部戻すと保存した状態 = 未保存でなくなる");
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->IsSceneDirty() && ed->IsSceneHumanDirty());
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(!ed->IsSceneDirty());
    IM_CHECK(!ed->IsSceneHumanDirty());   // 自動保存を止める条件が外れる
    IM_CHECK(!ed->autosaveBlockedByHumanEdits);

    LogStep(ctx, "Redo でまた未保存、もう一度 Undo で解ける");
    ctx->KeyPress(ImGuiKey_Y | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->IsSceneDirty());
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(!ed->IsSceneDirty());

    LogStep(ctx, "設定が保存時と違う状態（Undo を積まない変更）が混ざると解けない");
    AddEntity(ctx, "Box");
    app->GetScene()->GetPostSettings().evComp += 0.5f;   // Undo できない設定の変更（保存時と違う状態）
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(6);
    IM_CHECK(ed->IsSceneDirty());
    app->GetScene()->GetPostSettings().evComp -= 0.5f;
    fs::remove(probe, ec);
}

// (e) R1-09: 保存を押さなくても、シーンを開いた直後・新規シーンを作った直後を基準に、全部 Undo で戻せば未保存が解ける。
void T_UndoToBaselineWithoutSave(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;

    const std::string snapshot = app->SaveSceneSnapshot();
    IM_CHECK(!snapshot.empty());
    if (snapshot.empty()) return;
    SceneStateGuard guard{ed, app, ed->currentScenePath};

    auto waitIdle = [&](bool& flag) { for (int i = 0; i < 300 && flag; ++i) ctx->Yield(1); ctx->Yield(30); };

    LogStep(ctx, "シーンを開く（保存はしない）→ Box を作る → Ctrl+Z で未保存が解ける");
    ed->pendingLoadSkipConfirm = true;
    ed->pendingLoadPath = snapshot;
    ctx->Yield(2);
    for (int i = 0; i < 300 && !ed->pendingLoadPath.empty(); ++i) ctx->Yield(1);
    ctx->Yield(40);
    IM_CHECK(!ed->IsSceneDirty());
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneDirty() && ed->IsSceneHumanDirty());
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(8);
    IM_CHECK(!ed->IsSceneDirty());
    IM_CHECK(!ed->IsSceneHumanDirty());

    LogStep(ctx, "新規シーンを作る（保存はしない）→ Box を作る → Ctrl+Z で未保存が解ける");
    ed->pendingNewSceneSkipConfirm = true;
    ed->pendingNewScene = true;
    waitIdle(ed->pendingNewScene);
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneHumanDirty());
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(8);
    IM_CHECK(!ed->IsSceneDirty());
    IM_CHECK(!ed->IsSceneHumanDirty());

    LogStep(ctx, "後始末（元のシーンへ戻す）");
    ed->pendingLoadSkipConfirm = true;
    ed->pendingLoadPath = snapshot;
    for (int i = 0; i < 300 && !ed->pendingLoadPath.empty(); ++i) ctx->Yield(1);
    ctx->Yield(40);
    std::error_code ec;
    fs::remove(snapshot, ec);
}

// (f) R1-11: 仮想入力でも、未保存の人の編集があるとき Ctrl+N の「作成」で未保存の確認が出る（MCP のセッション中でも）。
void T_NewSceneConfirmUnderVirtualInput(ImGuiTestContext* ctx)
{
    namespace fs = std::filesystem;
    EditorContext* ed = Editor();
    Application*   app = App();
    IM_CHECK(ed != nullptr && app != nullptr && app->GetScene() != nullptr);
    if (!ed || !app || !app->GetScene()) return;
    IM_CHECK(vinput::Enabled());   // このテストは仮想入力下でしか意味が無い

    const fs::path scenes = fs::path(PathResolver::AssetsDir()) / "scenes";
    const fs::path target = scenes / "_a16_newscene_target.json";
    std::error_code ec;
    fs::remove(target, ec);
    const bool origAi = ed->aiSessionEver;
    const std::string snapshot = app->SaveSceneSnapshot();
    SceneStateGuard guard{ed, app, ed->currentScenePath};
    ed->currentScenePath = (scenes / "_a16_newscene_old.json").string();
    ed->aiSessionEver = true;   // MCP が繋がったセッション（以前はここで確認が出ず、黙って退避していた）

    LogStep(ctx, "人の編集（Box）を作る");
    AddEntity(ctx, "Box");
    IM_CHECK(ed->IsSceneHumanDirty());
    const size_t entitiesBefore = app->GetScene()->GetRegistry().storage<entt::entity>().size();

    LogStep(ctx, "Ctrl+N の名前入力で「作成」→ 未保存の確認が出て、まだシーンは入れ替わらない");
    ed->newSceneDialogIsCreate = true;
    std::memset(ed->newSceneNameBuf, 0, sizeof(ed->newSceneNameBuf));
    strncpy_s(ed->newSceneNameBuf, "_a16_newscene_target", _TRUNCATE);
    ed->showNewSceneDialog = true;
    ctx->Yield(4);
    const ImGuiID dlg = FocusWindow(ctx, "//新規シーン###NewScenePopup");
    IM_CHECK(dlg != 0);
    if (dlg != 0) { ctx->SetRef(dlg); ctx->ItemClick("作成"); }
    ctx->Yield(10);
    IM_CHECK(ed->showUnsavedConfirm);
    const ImGuiID confirm = FocusWindow(ctx, "//##UnsavedConfirm");
    IM_CHECK(confirm != 0);
    IM_CHECK(app->GetScene()->GetRegistry().storage<entt::entity>().size() == entitiesBefore);
    IM_CHECK(!fs::exists(target, ec));

    LogStep(ctx, "キャンセルで何も変わらない");
    if (confirm != 0) { ctx->SetRef(confirm); ctx->ItemClick("キャンセル"); }
    for (int i = 0; i < 120 && (ed->showUnsavedConfirm || ed->pendingNewScene); ++i) ctx->Yield(1);
    ctx->Yield(6);
    IM_CHECK(!ed->showUnsavedConfirm && !ed->pendingNewScene);
    IM_CHECK(app->GetScene()->GetRegistry().storage<entt::entity>().size() == entitiesBefore);
    IM_CHECK(ed->IsSceneHumanDirty());
    IM_CHECK(!fs::exists(target, ec));

    LogStep(ctx, "後始末");
    ed->aiSessionEver = origAi;
    if (!snapshot.empty()) { fs::remove(snapshot, ec); }
    fs::remove(target, ec);
}

// (g) R1-10: グローバル game.lua の場所は、開いているプロジェクトのルート（BaseDir）の下で解決される。
void T_GameLuaUnderProjectRoot(ImGuiTestContext* ctx)
{
    (void)ctx;
    const std::string base = PathResolver::BaseDir();
    const std::string path = PathResolver::GameLuaPath();
    IM_CHECK(!base.empty());
    IM_CHECK(path.size() > base.size());
    IM_CHECK(path.compare(0, base.size(), base) == 0);   // <project>/scripts/game.lua か <project>/assets/game.lua
    IM_CHECK(path.find("/scripts/game.lua") != std::string::npos || path.find("/assets/game.lua") != std::string::npos);

    // 起動から今までのログに、game.lua が無いことの警告（レベル 3 以上）が出ていない（任意のファイルなので情報どまり）。
    std::vector<LogEntry> entries;
    Logger::ReadBuffered(0, entries);
    int warnCount = 0;
    for (const LogEntry& e : entries)
        if (e.level >= 3 && e.text.find("ゲームスクリプトが見つかりません") != std::string::npos) ++warnCount;
    IM_CHECK_EQ(warnCount, 0);
}

} // namespace uitest
} // namespace dx12e
