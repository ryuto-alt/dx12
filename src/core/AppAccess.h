#pragma once
// ===========================================================================
// AppAccess（W05-01b）: Application の private に触れる関数の入口。
//   Application.h は W7 まで凍結なので、private への出入り口はここへ static で足す。
//   例: static void SetQualityPreset(Application&, QualityPreset);
//   include するのは使う TU だけ（Application.h からは include しない）。
//   各レーンは自分のブロックだけに追記し、定義は自分の .cpp に書く。
// ===========================================================================
#include "core/Application.h"
#include "editor/EditorContext.h"

namespace dx12e
{
struct AppAccess
{
    // ==== lane-A begin ====
    // W05-02: 人の未保存の変更を自動保存が上書きしない（定義は mcp/ApplicationMcpLaneA.cpp）。
    //   ProtectHumanEdits       … 人の未保存の変更があれば、本体に書かずに SceneBackup へ退避する。退避できた / 不要なら true。
    //   ProtectHumanEditsOrSave … 人の変更が無ければ本体へ保存（MCP 由来の変更だけ）。有れば ProtectHumanEdits。
    // force=true: 間引き・同じ状態の省略をせず必ず今の状態を新しい世代へ残す（閉じる・開く・置き換えの全経路）。
    // includeMcp=true: 人の変更が無くても（MCP 由来だけでも）退避する（Play 中で本体へ書けないとき）。
    static bool ProtectHumanEdits(Application&, EditorContext::DiscardReason, bool force = false, bool includeMcp = false);
    static bool ProtectHumanEditsOrSave(Application&, EditorContext::DiscardReason);
    // MCP の dispatch の前後（書き込み系だけ。読み取り専用は呼ばない）。Pre は人の settings 変更を拾う。
    static void McpWriteBegin(Application&, const std::string& method);
    static void McpWriteEnd(Application&);
    // シーンを置き換える MCP（open_scene / new_scene / scene_backups restore）の前。人の変更を退避する。失敗なら false（置き換えない）。
    static bool ProtectBeforeSceneReplace(Application&, EditorContext::DiscardReason);
    // 定期の settings 指紋の更新（active=false は Play 中などで判定しない）。
    static void PollHumanSettings(Application&, bool active, u64 freshHash);
    // MarkSceneClean の後（保存済みに揃えた）。
    static void OnSceneMarkedClean(Application&);
    // 保存系と editor_state の MCP 結果に autosave: { blockedByHumanEdits, lastBackupId } を足す（真なら warnings[] にも）。
    static void AnnotateAutosave(Application&, nlohmann::json& resp, const std::string& method);
    // 未保存の新規シーン（_mcp_untitled_*）の置き場: <project>/.dx12/untitled/
    static std::string UntitledDir();
    // ==== lane-A end ====

    // ==== lane-B begin ====
    // W05-13: モデルのサムネイルのキャッシュ済みハンドル（0 = まだ無い）。UI テスト用。
    static uint64_t ModelThumbHandle(Application& app, const std::string& path);
    // ==== lane-B end ====

    // ==== lane-C begin ====
    // ==== lane-C end ====

    // ==== lane-D begin ====
    // ==== lane-D end ====
};
} // namespace dx12e
