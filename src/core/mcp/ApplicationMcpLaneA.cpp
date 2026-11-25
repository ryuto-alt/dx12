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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <vector>
#include "core/SafeRemove.h"

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
    // settings 指紋の追跡（dispatch の前後と毎フレームの更新で共有）
    bool     trackValid = false;
    u64      trackHash  = 0;
    int      framesSinceMcpWrite = 1000000;   // 最後の MCP の書き込み系の終わりからのフレーム数（Poll が進める）
    // 退避
    autosaveguard::ProtectStamp stamp;        // 退避済みの状態（LastHumanEditSeq, 人の settings 変更の有無）
    autosaveguard::ProtectStamp failedStamp;  // 退避に失敗した状態（同じ状態では再試行・再通知しない）
    std::chrono::steady_clock::time_point lastProtect{};
    bool        hasProtected = false;
    std::string lastBackupId;
    std::string untitledStem;                 // 保存先の無い新規シーンの退避の名前（MarkSceneClean まで同じ）
    bool        saveWroteHumanEdits = false;  // 直近の save_scene が人の未保存の変更を含めて本体へ書いた
};
LaneAState& S() { static LaneAState s; return s; }

// MCP の書き込みの後に、遅延実行（後のフレームで走る生成・読み込み）の設定変更が着地するまでのフレーム数。
// 時間（秒）ではなくフレームで数える: 人の変更をこの間だけ見落とす（約 50ms）代わりに、MCP の書き込みが続いても人の変更を拾える。
constexpr int kMcpDeferredFrames = 3;
constexpr double kAutoProtectMinIntervalSec = 30.0;   // 自動保存の経路の退避の最短間隔（閉じる・開く・終了は無視する）

std::string UntitledStem()
{
    LaneAState& st = S();
    if (st.untitledStem.empty())
    {
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tmv{};
        char buf[32] = {};
        if (localtime_s(&tmv, &t) == 0) std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
        st.untitledStem = std::string("_protect_untitled_") + buf;
    }
    return st.untitledStem;
}

// .dx12/protect/<stem>.* の退避用の一時ファイルを消す（残った .parts / .nav が次の世代へ混ざらないように）
void CleanStaging(const std::filesystem::path& dir, const std::string& stem)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<fs::path> victims;
    for (const auto& e : fs::directory_iterator(dir, ec))
    {
        const std::string n = e.path().filename().string();
        if (n.size() > stem.size() && n.compare(0, stem.size(), stem) == 0 && n[stem.size()] == '.') victims.push_back(e.path());
    }
    for (const fs::path& v : victims) saferm::RemoveAll(v, ec, dir);
}

} // namespace

std::string AppAccess::UntitledDir()
{
    return PathResolver::BaseDir() + ".dx12/untitled/";
}

bool AppAccess::ProtectHumanEdits(Application& app, EditorContext::DiscardReason reason, bool forceArg, bool includeMcp)
{
    namespace fs = std::filesystem;
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return true;
    const bool playing = app.m_engineMode != Application::EngineMode::Editor;
    if (!ctx->IsSceneDirty()) ctx->humanSettingsDirty = false;
    if (!includeMcp && !ctx->IsSceneHumanDirty()) return true;   // MCP 由来の変更だけ。退避の必要は無い
    if (includeMcp && !ctx->IsSceneDirty()) return true;

    LaneAState& st = S();
    // Play 中のシーンは実行の状態。Play の開始時のスナップショット（Stop で戻る先）から退避する。無ければ「退避していない」として false。
    if (playing && app.m_playSceneJson.empty()) return false;
    const bool force = forceArg || includeMcp || reason != Reason::None;   // 閉じる・開く・置き換えは必ず今の状態を残す。UpdateMcpAutoSave の経路だけ間引く
    const unsigned long long humanSeq = ctx->undoSystem.LastHumanEditSeq();
    const unsigned long long sflag = ctx->humanSettingsDirty ? 1ull : 0ull;
    if (!force)
    {
        if (st.failedStamp.Same(humanSeq, sflag)) return false;   // 同じ状態で失敗済み（毎回直列化・通知しない）
        const double since = st.hasProtected ? std::chrono::duration<double>(std::chrono::steady_clock::now() - st.lastProtect).count() : 0.0;
        if (autosaveguard::CanSkipProtect(false, ctx->autosaveBlockedByHumanEdits && st.stamp.Same(humanSeq, sflag), st.hasProtected, since, kAutoProtectMinIntervalSec))
            return true;
    }

    const fs::path projectRoot(PathResolver::BaseDir());
    const bool untitled = ctx->currentScenePath.empty();
    const std::string scenePath = untitled ? UntitledDir() + UntitledStem() + ".json" : ctx->currentScenePath;
    const std::string stem = fs::path(scenePath).stem().string();
    const fs::path stageDir = projectRoot / ".dx12" / "protect";
    const std::string staging = (stageDir / (stem + ".json")).string();

    autosaveguard::Ops ops;
    ops.saveMain = [] { return false; };   // 人の変更があるときは使わない
    ops.backup = [&](std::string* err) -> std::string {
        std::error_code ec;
        fs::create_directories(stageDir, ec);
        CleanStaging(stageDir, stem);
        if (playing)
        {
            std::ofstream f(fs::path(staging), std::ios::binary | std::ios::trunc);
            f.write(app.m_playSceneJson.data(), static_cast<std::streamsize>(app.m_playSceneJson.size()));
            if (!f.good()) { if (err) *err = "退避用にシーンを書けません"; return {}; }
        }
        else if (!SceneSerializer::Save(*app.m_scene, staging, PathResolver::AssetsDir()))
        { if (err) *err = "退避用にシーンを書けません"; return {}; }
        std::string why;
        std::string id = scenebackup::SnapshotExternal(projectRoot, fs::path(scenePath), fs::path(staging),
                                                       std::string("protect-human-edits:") + ReasonName(reason), &why);
        CleanStaging(stageDir, stem);
        if (id.empty() && err) *err = why;
        return id;
    };
    const autosaveguard::Result r = autosaveguard::ProtectOrSave(true, ops);
    const bool humanBlock = ctx->IsSceneHumanDirty();
    st.lastProtect = std::chrono::steady_clock::now();
    st.hasProtected = true;
    if (r.outcome != autosaveguard::Outcome::Protected)
    {
        st.failedStamp.Set(humanSeq, sflag);
        Logger::Error("人の未保存の変更を退避できませんでした（{}）。シーン本体は書きません: {}", ReasonName(reason), r.error);
        ctx->Notify(ui::ToastKind::Error, "未保存の変更を退避できませんでした。シーンは上書きしていません", 8.0f);
        return false;
    }
    st.failedStamp.Reset();
    if (!humanBlock) { st.lastBackupId = r.backupId; return true; }   // MCP 由来だけの退避（Play 中）。自動保存の停止表示にはしない
    const bool changed = !ctx->autosaveBlockedByHumanEdits || r.backupId != st.lastBackupId;
    ctx->autosaveBlockedByHumanEdits = true;
    st.lastBackupId = r.backupId;
    st.stamp.Set(humanSeq, sflag);
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
    if (app.m_engineMode != Application::EngineMode::Editor)
    {
        // Play 中のシーンは実行の状態なので本体へ書かない。Play 前の状態（MCP の未保存の編集を含む）を退避へ残す。
        if (app.m_playSceneJson.empty()) return false;
        return ProtectHumanEdits(app, reason, true, true);
    }
    return app.SaveSceneForMcp();
}

bool AppAccess::ProtectBeforeSceneReplace(Application& app, EditorContext::DiscardReason reason)
{
    return ProtectHumanEdits(app, reason, true);
}

void AppAccess::McpWriteBegin(Application& app, const std::string& method)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return;
    LaneAState& st = S();
    if (app.m_engineMode != Application::EngineMode::Editor || app.m_showLauncher || app.m_isGameMode)
    { st.trackValid = false; return; }
    const u64 fp = SceneSettingsFingerprint(*app.m_scene);
    // 前回の指紋（毎フレームの更新か、MCP の書き込みの終わりに取った値）から、MCP を通さずに値が変わっていた = 人の編集。
    // ただし直前の MCP の書き込みの遅延実行が着地した直後は MCP 由来として受け入れる。
    if (st.trackValid && fp != st.trackHash && st.framesSinceMcpWrite > kMcpDeferredFrames) ctx->humanSettingsDirty = true;
    st.trackHash = fp;
    st.trackValid = true;
    if (method == "save_scene") st.saveWroteHumanEdits = ctx->IsSceneHumanDirty();   // 人の変更を含めて本体へ書く（警告する。止めはしない）
}

void AppAccess::McpWriteEnd(Application& app)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx || !app.m_scene) return;
    LaneAState& st = S();
    st.framesSinceMcpWrite = 0;
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
    if (st.framesSinceMcpWrite < 1000000) ++st.framesSinceMcpWrite;
    if (!st.trackValid) { st.trackHash = freshHash; st.trackValid = true; }
    else if (freshHash != st.trackHash)
    {
        // MCP の書き込みの直後の数フレームは、後のフレームで走る MCP の作業（生成・読み込み）の結果とみなす
        if (st.framesSinceMcpWrite > kMcpDeferredFrames) ctx->humanSettingsDirty = true;
        st.trackHash = freshHash;
    }
    // 汚れていないか（鮮度の高い指紋で判定する。ctx->settingsHash は 0.5 秒ごとの更新で遅れる）
    const bool dirty = ctx->undoSystem.EditSeq() != ctx->sceneSavedSeq || freshHash != ctx->savedSettingsHash;
    if (!dirty) ctx->humanSettingsDirty = false;
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
    if (method == "save_scene" && S().saveWroteHumanEdits)
    {
        S().saveWroteHumanEdits = false;
        r["autosave"]["wroteHumanEdits"] = true;
        if (!r.contains("warnings") || !r["warnings"].is_array()) r["warnings"] = json::array();
        r["warnings"].push_back("人の未保存の変更を含めてシーン本体へ保存しました（save_scene は明示の保存なので止めません）。直前の状態は「以前の版に戻す」で戻せます");
    }
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
    st.failedStamp.Reset();
    st.hasProtected = false;
    st.untitledStem.clear();
    st.trackValid = false;   // 次の定期の更新で取り直す
}
} // namespace dx12e
