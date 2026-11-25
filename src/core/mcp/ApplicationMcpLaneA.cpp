// ===========================================================================
// レーン A の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）と、
// W05-02（BUG-06）: 人の未保存の変更を自動保存・保存系の経路が本体へ上書きしないための AppAccess の定義。
// Application.h は変えない（W7 まで凍結）。private に触れる処理は core/AppAccess.h のレーン A のブロック。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AppAccess.h"
#include "core/AutosaveGuard.h"
#include "core/SceneBackup.h"
#include "scene/SceneSettingsHash.h"

#include <chrono>
#include <filesystem>

namespace dx12e
{
void Application::RegisterMcpLaneAMethods()
{
}

namespace
{
using Reason = EditorContext::DiscardReason;

const char* ReasonName(Reason r)
{
    switch (r)
    {
    case Reason::Quit:      return "Quit";
    case Reason::OpenScene: return "OpenScene";
    case Reason::NewScene:  return "NewScene";
    case Reason::FleetIdle: return "FleetIdle";
    case Reason::Launcher:  return "Launcher";
    default:                return "Autosave";
    }
}

// 人の変更の追跡と退避の状態（Application は 1 つ。ヘッダは凍結なのでここに持つ）
struct LaneAState
{
    // settings 指紋の追跡（dispatch の前後と定期の更新で共有）
    bool     trackValid = false;
    u64      trackHash  = 0;
    std::chrono::steady_clock::time_point lastMcpWrite{};   // 最後の MCP の書き込み系の終わり
    // 退避
    autosaveguard::ProtectStamp stamp;
    std::string lastBackupId;
};
LaneAState& S() { static LaneAState s; return s; }

constexpr double kMcpDeferredWindowSec = 1.5;   // MCP の書き込みの直後に起きた指紋の変化は MCP 由来とみなす（遅延実行の設定変更）

} // namespace

std::string AppAccess::UntitledDir()
{
    return PathResolver::BaseDir() + ".dx12/untitled/";
}

bool AppAccess::ProtectHumanEdits(Application& app, EditorContext::DiscardReason reason)
{
    namespace fs = std::filesystem;
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return true;
    // Play 中のシーンは実行の状態（Stop で戻る）。それを退避しても意味が無く、本体も書かない。
    if (app.m_engineMode != Application::EngineMode::Editor) return true;
    if (!ctx->IsSceneDirty()) ctx->humanSettingsDirty = false;
    if (!ctx->IsSceneHumanDirty()) return true;   // MCP 由来の変更だけ。退避の必要は無い

    LaneAState& st = S();
    const u64 fp = SceneSettingsFingerprint(*app.m_scene);
    // 同じ状態をもう退避した（自動保存のたびに同じ物を直列化しない）
    if (ctx->autosaveBlockedByHumanEdits && st.stamp.Same(ctx->undoSystem.EditSeq(), fp)) return true;

    autosaveguard::Ops ops;
    ops.saveMain = [] { return false; };   // 人の変更があるときは使わない
    ops.backup = [&](std::string* err) -> std::string {
        std::error_code ec;
        const std::string scenePath = ctx->currentScenePath;
        if (scenePath.empty())
        {
            // 保存先の無い新規シーン: 上書きする本体が無いので、.dx12/untitled/ へ控えを書く（世代は作れない）
            const std::string stage = UntitledDir() + "_protect_untitled.json";
            fs::create_directories(UntitledDir(), ec);
            if (!SceneSerializer::Save(*app.m_scene, stage, PathResolver::AssetsDir()))
            { if (err) *err = "未保存の新規シーンの控えを書けません"; return {}; }
            return "untitled:_protect_untitled.json";
        }
        const std::string stem = fs::path(scenePath).stem().string();
        const std::string staging = PathResolver::BaseDir() + ".dx12/protect/" + stem + ".json";
        fs::create_directories(fs::path(staging).parent_path(), ec);
        if (!SceneSerializer::Save(*app.m_scene, staging, PathResolver::AssetsDir()))
        { if (err) *err = "退避用にシーンを書けません"; return {}; }
        std::string why;
        const std::string id = scenebackup::SnapshotExternal(fs::path(PathResolver::BaseDir()), fs::path(scenePath), fs::path(staging),
                                                             std::string("protect-human-edits:") + ReasonName(reason), &why);
        if (id.empty() && err) *err = why;
        return id;
    };
    const autosaveguard::Result r = autosaveguard::ProtectOrSave(true, ops);
    if (r.outcome != autosaveguard::Outcome::Protected)
    {
        Logger::Error("人の未保存の変更を退避できませんでした（{}）。シーン本体は書きません: {}", ReasonName(reason), r.error);
        ctx->Notify(ui::ToastKind::Error, "未保存の変更を退避できませんでした。シーンは上書きしていません", 8.0f);
        return false;
    }
    const bool changed = !ctx->autosaveBlockedByHumanEdits || r.backupId != st.lastBackupId;
    ctx->autosaveBlockedByHumanEdits = true;
    st.lastBackupId = r.backupId;
    st.stamp.Set(ctx->undoSystem.EditSeq(), fp);
    if (changed)
        Logger::Warn("人の未保存の変更があるので、自動保存はシーンを上書きしません（退避: 以前の版に戻す -> {}）", r.backupId);
    return true;
}

bool AppAccess::ProtectHumanEditsOrSave(Application& app, EditorContext::DiscardReason reason)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return false;
    if (!ctx->IsSceneDirty()) ctx->humanSettingsDirty = false;
    if (ctx->IsSceneHumanDirty()) return ProtectHumanEdits(app, reason);
    ctx->autosaveBlockedByHumanEdits = false;
    return app.SaveSceneForMcp();
}

bool AppAccess::ProtectBeforeSceneReplace(Application& app, EditorContext::DiscardReason reason)
{
    return ProtectHumanEdits(app, reason);
}

void AppAccess::McpWriteBegin(Application& app)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return;
    LaneAState& st = S();
    if (app.m_engineMode != Application::EngineMode::Editor || app.m_showLauncher || app.m_isGameMode)
    { st.trackValid = false; return; }
    const u64 fp = SceneSettingsFingerprint(*app.m_scene);
    // 前回の指紋（MCP の書き込みの終わりに取った値か、定期の更新）から、MCP を通さずに値が変わっていた = 人の編集
    if (st.trackValid && fp != st.trackHash) ctx->humanSettingsDirty = true;
    st.trackHash = fp;
    st.trackValid = true;
}

void AppAccess::McpWriteEnd(Application& app)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return;
    LaneAState& st = S();
    st.lastMcpWrite = std::chrono::steady_clock::now();
    if (app.m_engineMode != Application::EngineMode::Editor || app.m_showLauncher || app.m_isGameMode) { st.trackValid = false; return; }
    st.trackHash  = SceneSettingsFingerprint(*app.m_scene);   // この呼び出しの結果は MCP 由来として受け入れる
    st.trackValid = true;
}

void AppAccess::PollHumanSettings(Application& app, bool active, u64 freshHash)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx) return;
    LaneAState& st = S();
    if (!active) { st.trackValid = false; return; }   // Play 中と Stop の復元のフレームは判定しない（Stop の後に取り直す）
    if (!st.trackValid) { st.trackHash = freshHash; st.trackValid = true; }
    else if (freshHash != st.trackHash)
    {
        const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - st.lastMcpWrite).count();
        if (since > kMcpDeferredWindowSec) ctx->humanSettingsDirty = true;
        st.trackHash = freshHash;
    }
    if (!ctx->IsSceneDirty()) ctx->humanSettingsDirty = false;
    if (!ctx->IsSceneHumanDirty()) ctx->autosaveBlockedByHumanEdits = false;
}

void AppAccess::AnnotateAutosave(Application& app, nlohmann::json& resp, const std::string& method)
{
    if (method != "save_scene" && method != "editor_state" && method != "scene_backups") return;
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !resp.value("ok", false)) return;
    if (!resp.contains("result") || !resp["result"].is_object()) return;
    using json = nlohmann::json;
    json& r = resp["result"];
    const bool blocked = ctx->autosaveBlockedByHumanEdits;
    r["autosave"] = json{{"blockedByHumanEdits", blocked}, {"lastBackupId", S().lastBackupId}};
    if (blocked)
    {
        if (!r.contains("warnings") || !r["warnings"].is_array()) r["warnings"] = json::array();
        r["warnings"].push_back("人の未保存の変更があるため自動保存を止めています。人に保存を依頼してください（guide: autosave）");
    }
}

void AppAccess::OnSceneMarkedClean(Application& app)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx) return;
    ctx->humanSettingsDirty = false;
    ctx->autosaveBlockedByHumanEdits = false;
    LaneAState& st = S();
    st.stamp.Reset();
    st.trackValid = false;   // 次の定期の更新で取り直す
}
} // namespace dx12e
