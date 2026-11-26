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
enum class QualityPresetId : u8;   // renderer/QualityPreset.h
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
    // UI テスト用: 自動保存の退避の最短間隔の起点を消す（実時間で 30 秒待たずに次の退避を撮れるように）。
    static void ResetAutoProtectThrottleForTest();
    // W05-04: GraphicsDevice の caps（DeviceCaps.h を include した TU だけが型を使える）。デバイスが無ければ nullptr。
    static const struct DeviceCaps* GetDeviceCaps(Application&);
    // ==== lane-A end ====

    // ==== lane-B begin ====
    // W05-13: モデルのサムネイルのキャッシュ済みハンドル（0 = まだ無い）。UI テスト用。
    static uint64_t ModelThumbHandle(Application& app, const std::string& path);
    // W1-BD4（UnoFX）: 定義は mcp/ApplicationMcpLaneB.cpp。
    //   VfxBindHost … renderer/vfx の Runtime へ「レジストリ・dt・品質・保留中の配置」の口を入れる。
    //                 MCP の登録口と VfxTick が呼ぶ。
    //   VfxPlace    … .uvfx（preset 名 or assets 相対パス）のエフェクトを 1 つ置く（Undo 1 件。呼び出し中の MCP の積み口に載る）。失敗は false と err。
    static void VfxBindHost(Application&);
    //   VfxTick     … ★毎フレーム 1 回、Editor / ゲームの更新（コマンド記録の外）から呼んでほしい（依頼: Application.cpp の持ち主へ）。
    //                 初回に VfxBindHost を呼び（MCP を使わない起動でも VFX が出る）、保留中の配置（pendingVfxPlace）を Editor モードだけで消化する。
    static void VfxTick(Application&);
    static bool VfxPlace(Application&, const std::string& presetOrAsset, const DirectX::XMFLOAT3& worldPos, entt::entity parent,
                         const std::string& name, entt::entity& outEntity, std::string& outAsset, std::string& err);
    // ==== lane-B end ====

    // ==== lane-C begin ====
    // W1-C1: 品質プリセット（定義は core/QualityPresetApply.cpp。自由関数の窓口は core/QualityPresetApply.h）。
    //   QualityPresetId は renderer/QualityPreset.h（使う TU が include する。ここは前方宣言で重くしない）。
    static bool SetQualityPreset(Application&, QualityPresetId, bool recordUndo);
    static QualityPresetId GetQualityPreset(const Application&);
    // ==== lane-C end ====

    // ==== lane-D begin ====
    // ==== lane-D end ====
};
} // namespace dx12e
