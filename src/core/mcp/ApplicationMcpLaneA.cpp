// ===========================================================================
// レーン A の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）と、
// W05-02（BUG-06）: 人の未保存の変更を自動保存・保存系の経路が本体へ上書きしないための AppAccess の定義。
// Application.h は変えない（W7 まで凍結）。private に触れる処理は core/AppAccess.h のレーン A のブロック。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AppAccess.h"
#include "core/AutosaveGuard.h"
#include "core/SceneBackup.h"
#include "graphics/DeviceCaps.h"
#include "graphics/GraphicsDevice.h"
#include "scene/SceneSettingsHash.h"

#include <algorithm>
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
    using namespace appdetail;
    // W05-04: デバイスの機能（caps）。読み取り専用。値は GraphicsDevice が作成時に 1 回だけ判定したもの。
    McpDefine("get_device_caps", "", DX12E_MCP_HANDLER
        {
            const DeviceCaps* caps = m_graphicsDevice ? m_graphicsDevice->GetCaps() : nullptr;
            if (!caps) throw McpError(McpErr::Internal, "device caps not available", "GraphicsDevice が初期化される前。起動直後なら少し待って呼び直す");
            nlohmann::json c = nlohmann::json::object();
            for (const DeviceCapsField& f : DeviceCapsFields(*caps))
            {
                if (f.kind == DeviceCapsField::Kind::Bool) c[f.name] = (f.value != 0);
                else                                       c[f.name] = f.value;
            }
            const u32 sm = caps->highestShaderModel;
            char smName[16];
            std::snprintf(smName, sizeof(smName), "%u.%u", sm >> 4, sm & 0xF);
            resp["ok"]         = true;
            resp["shaderModelName"] = smName;
            resp["summary"]    = DeviceCapsSummary(*caps);
            resp["caps"]       = std::move(c);
        });
}

const DeviceCaps* AppAccess::GetDeviceCaps(Application& app)
{
    return app.m_graphicsDevice ? app.m_graphicsDevice->GetCaps() : nullptr;
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
    std::string rollingAutoId;                // 自動保存の経路の退避の世代（1 枠。次の退避で置き換える。DF-W05-R2-02）
    std::string rollingAutoScene;             // rollingAutoId を作ったシーンのパス（別シーンの退避は消さない。DF-W05-R3-01）
    std::string untitledStem;                 // 保存先の無い新規シーンの退避の名前（MarkSceneClean まで同じ）
    bool        saveWroteHumanEdits = false;  // 直近の save_scene が人の未保存の変更を含めて本体へ書いた
    // 保存済みの内容の指紋（W1-A16 / R1-09）。保存・読み込み・破棄のたびに取り直し、Undo / Redo で内容がこれに
    // 戻ったら「未保存」を解く。UndoCore.h（凍結）に保存点の印は足せないので、内容そのもので比べる。
    // 指紋は MarkSceneClean の「後」に確定する内容（読み込み完了・新規シーンの既定の配置・エディタ用グリッド）を含めたいので、
    // MarkSceneClean では印を立てるだけにして、次の静かなフレームで取る（needFingerprint）。
    bool        needFingerprint   = false;
    bool        savedContentValid = false;
    u64         savedContentHash  = 0;
    u64         savedSettingsFp   = 0;     // 指紋を取った時点の設定の指紋（Undo できない設定はこれと比べる）
    size_t      savedUndoDepth    = 0;     // 指紋を取った時点の Undo の深さ。Undo / Redo だけで保存した内容へ戻るなら深さも同じ（FU-E-16）
    // 指紋の計算の負荷の計測（FU-E-16）。直列化 1 回ぶんの時間を測り、深さが違うときは計算ごと省く。
    u64         hashRuns     = 0;          // SceneContentHash を実際に走らせた回数
    u64         hashSkipped  = 0;          // 深さが保存時と違うので省いた回数
    double      hashLastMs   = 0.0;
    double      hashMaxMs    = 0.0;
    // 直前のフレームの Undo スタックの様子（人の Undo / Redo を検出する）
    u64         lastEditSeq   = 0;
    u64         lastPushSeq   = 0;
    size_t      lastUndoDepth = 0;
    bool        lastValid     = false;
    // 前回見た保存済みの印。MarkSceneClean を通らない保存（名前を付けて保存）で進んだら指紋を取り直す。
    u64         lastSavedSeq  = 0;
    bool        lastSavedSeqValid = false;
};
LaneAState& S() { static LaneAState s; return s; }

// シーンを保存ファイルと同じ直列化にした文字列の指紋（FNV-1a 64）。保存済みの内容と今の内容が同じかの比較に使う。
u64 SceneContentHash(Scene& scene)
{
    const auto t0 = std::chrono::steady_clock::now();
    const std::string text = SceneSerializer::SaveToStringV2(scene, PathResolver::AssetsDir());
    u64 h = 1469598103934665603ull;
    for (const unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    LaneAState& st = S();
    st.hashLastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    st.hashMaxMs  = (std::max)(st.hashMaxMs, st.hashLastMs);
    ++st.hashRuns;
    constexpr double kSlowHashMs = 20.0;   // 大きいシーンでは Undo / Redo の 1 回ごとに重い。気づけるよう 1 度ずつ知らせる
    static bool warned = false;
    if (st.hashLastMs > kSlowHashMs && !warned)
    {
        warned = true;
        Logger::Info("シーンの内容の指紋に {:.1f} ms かかりました（{} バイト。保存点の判定。Undo の深さが保存時と違うときは計算しません）", st.hashLastMs, text.size());
    }
    return h;
}

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

void AppAccess::SavePointHashStats(u64& runs, u64& skipped, double& lastMs, double& maxMs)
{
    const LaneAState& st = S();
    runs = st.hashRuns; skipped = st.hashSkipped; lastMs = st.hashLastMs; maxMs = st.hashMaxMs;
}

void AppAccess::ResetAutoProtectThrottleForTest()
{
    LaneAState& st = S();
    st.hasProtected = false;
    st.stamp.Reset();
    st.failedStamp.Reset();
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
        if (autosaveguard::CanSkipProtect(false, ctx->autosaveBlockedByHumanEdits && st.stamp.Same(humanSeq, sflag), st.hasProtected, since, (std::max)(kAutoProtectMinIntervalSec, static_cast<double>(scenebackup::GlobalPolicy().minIntervalSec))))
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
        // 新しい世代を作る前に前回の自動の退避を消す（作った後だと Prune が世代数の上限で原本を先に押し出す）。
        //   消すのは理由が自動保存の世代だけ。強制の世代（AI の最初の保存の前の原本など）には触れない。
        if (!force && !st.rollingAutoId.empty())
        {
            if (st.rollingAutoScene == scenePath &&
                scenebackup::ReadReason(projectRoot, st.rollingAutoId) == std::string("protect-human-edits:") + ReasonName(reason))
                scenebackup::Remove(projectRoot, st.rollingAutoId);
            st.rollingAutoId.clear();
        }
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
    // DF-W05-R2-02: 自動保存の経路の退避は 1 枠に畳む（前回の自動の退避は backup 内で先に消してある）。
    if (!force)
    {
        st.rollingAutoId = (scenebackup::ReadReason(projectRoot, r.backupId) == std::string("protect-human-edits:") + ReasonName(reason))
                         ? r.backupId : std::string();
        st.rollingAutoScene = st.rollingAutoId.empty() ? std::string() : scenePath;
    }
    else if (r.backupId == st.rollingAutoId)
    {
        // DF-W05-R3-01: 強制の退避が重複排除で自動の世代へ合流した。その世代は破棄された編集の唯一の写しなので 1 枠から外す。
        st.rollingAutoId.clear();
        st.rollingAutoScene.clear();
    }
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

// 保存済みに揃えたときの後始末（MarkSceneClean と、それを通らない保存の両方から）。
static void ResetAfterSaved(EditorContext& ctx, LaneAState& st)
{
    ctx.humanSettingsDirty = false;
    ctx.autosaveBlockedByHumanEdits = false;
    st.stamp.Reset();
    st.failedStamp.Reset();
    st.hasProtected = false;
    st.untitledStem.clear();
    st.trackValid = false;   // 次の定期の更新で取り直す
    // 保存済みの内容の指紋は取り直す（次の静かなフレームで。TrackSavePoint）。それまでは Undo では解かない。
    st.savedContentValid = false;
    st.needFingerprint   = true;
}

// 保存点の追跡（W1-A16 / R1-09）。UndoCore.h（凍結）に保存点の印は足せないので、保存した内容の指紋で比べる。
//  1. MarkSceneClean の後、読み込み・新規シーンの構築が終わって内容が静かになった最初のフレームで指紋を取る。
//  2. 人（または MCP）の Undo / Redo が実行されたら（EditSeq だけ進み、押された回数 PushSeq は同じで、スタックの深さが変わった）
//     内容と設定が保存した状態へ戻っていないか見て、戻っていれば保存済みの印を今に合わせて未保存を解く。
void AppAccess::TrackSavePoint(Application& app, u64 freshSettings)
{
    EditorContext* ctx = app.m_editorCtx.get();
    LaneAState& st = S();
    if (!ctx || !app.m_scene) return;
    UndoSystem& us = ctx->undoSystem;
    const u64 editSeq = us.EditSeq(), pushSeq = us.PushSeq();
    const size_t depth = us.UndoDepth();
    const bool undoRedoApplied = st.lastValid && editSeq != st.lastEditSeq && pushSeq == st.lastPushSeq && depth != st.lastUndoDepth;
    st.lastEditSeq = editSeq; st.lastPushSeq = pushSeq; st.lastUndoDepth = depth; st.lastValid = true;

    // MarkSceneClean を通らない保存（ツールバーの名前を付けて保存）で保存済みの印が進んだら、保存済みの内容の指紋が古い。
    // 取り直しにして、それまで Undo で解かない。
    if (st.lastSavedSeqValid && ctx->sceneSavedSeq != st.lastSavedSeq)
        ResetAfterSaved(*ctx, st);
    st.lastSavedSeq = ctx->sceneSavedSeq;
    st.lastSavedSeqValid = true;

    const bool quiet = !app.m_loading && !app.m_sceneLoadJob && app.m_engineMode == Application::EngineMode::Editor;
    if (!quiet) return;
    if (st.needFingerprint)
    {
        // 保存した状態から変わっていない間だけ取る（変わっていたら、次の MarkSceneClean まで待つ）。
        if (editSeq == ctx->sceneSavedSeq)
        {
            st.savedContentHash  = SceneContentHash(*app.m_scene);
            st.savedSettingsFp   = freshSettings;
            st.savedUndoDepth    = depth;
            st.savedContentValid = true;
            st.needFingerprint   = false;
        }
        return;
    }
    if (!undoRedoApplied || !st.savedContentValid) return;
    if (!ctx->IsSceneDirty()) return;   // もともと保存済み
    if (freshSettings != st.savedSettingsFp) return;   // Undo を積まない設定（ポスト・空・フォグ…）が保存時と違う
    // Undo / Redo だけで保存した内容へ戻るなら、スタックの深さも保存時と同じ。違うなら直列化（シーン全体の文字列化）を省く。
    // 取りこぼすのは「保存後に A→B→A と編集して、その途中へ Undo / Redo で行き来する」ような稀な形だけ（未保存のまま残る安全側）。
    if (depth != st.savedUndoDepth) { ++st.hashSkipped; return; }
    if (SceneContentHash(*app.m_scene) != st.savedContentHash) return;

    // 内容が保存した状態と同じ: 保存済みの印（EditSeq）を今に合わせ、人の変更の旗と退避の状態を落とす。
    // 自動保存は IsSceneHumanDirty が偽になれば再び本体へ書ける。
    ctx->sceneSavedSeq      = editSeq;
    st.lastSavedSeq         = editSeq;
    ctx->settingsHash       = freshSettings;
    ctx->savedSettingsHash  = freshSettings;
    ctx->humanSettingsDirty = false;
    ctx->autosaveBlockedByHumanEdits = false;
    st.stamp.Reset();
    st.failedStamp.Reset();
    st.hasProtected = false;
    st.trackValid = false;
}

void AppAccess::PollHumanSettings(Application& app, bool active, u64 freshHash)
{
    EditorContext* ctx = app.m_editorCtx.get();
    if (!ctx) return;
    LaneAState& st = S();
    if (!active) { st.trackValid = false; st.lastValid = false; return; }   // Play 中と Stop の復元のフレームは判定しない（Stop の後に取り直す）
    TrackSavePoint(app, freshHash);
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
    LaneAState& st = S();
    ResetAfterSaved(*ctx, st);
    st.lastSavedSeq = ctx->sceneSavedSeq;
    st.lastSavedSeqValid = true;
}
} // namespace dx12e
