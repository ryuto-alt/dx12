// UI テストのレーン A の本体。宣言は gui/UiTestHarnessShared.h の lane-A:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-A ブロックに足す。
#include <algorithm>
#include <cmath>
#include <cstring>
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/AppAccess.h"
#include "core/DpiScale.h"
#include "core/PathResolver.h"
#include "core/SceneBackup.h"
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
            fs::remove_all(workDir, ec);                                  // このテストが作った一時フォルダだけ
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
    ed->undoSystem.Undo();
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

} // namespace uitest
} // namespace dx12e
