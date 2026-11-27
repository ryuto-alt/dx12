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
struct QualityScenePack;          // core/QualityPresetScene.h
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
    // 保存点の追跡（毎フレーム、PollHumanSettings から）。保存した内容の指紋を取り、Undo / Redo で内容が保存した状態へ
    // 戻ったら「未保存」を解く（W1-A16 / R1-09）。
    static void TrackSavePoint(Application&, u64 freshSettingsHash);
    // 保存系と editor_state の MCP 結果に autosave: { blockedByHumanEdits, lastBackupId } を足す（真なら warnings[] にも）。
    static void AnnotateAutosave(Application&, nlohmann::json& resp, const std::string& method);
    // 未保存の新規シーン（_mcp_untitled_*）の置き場: <project>/.dx12/untitled/
    static std::string UntitledDir();
    // UI テスト用: 自動保存の退避の最短間隔の起点を消す（実時間で 30 秒待たずに次の退避を撮れるように）。
    static void ResetAutoProtectThrottleForTest();
    // FU-E-16: 保存点の判定（SceneContentHash）の負荷。実行回数・Undo の深さが違って省いた回数・直近と最大の時間（ms）。UI テスト用。
    static void SavePointHashStats(u64& runs, u64& skipped, double& lastMs, double& maxMs);
    // W05-04: GraphicsDevice の caps（DeviceCaps.h を include した TU だけが型を使える）。デバイスが無ければ nullptr。
    static const struct DeviceCaps* GetDeviceCaps(Application&);
    // W1-A10: エディタのプレビュー再生中のシーケンス名（無ければ空。定義は core/ApplicationSequencer.cpp）。UI テスト用。
    static std::string SequenceEditorPlayingDoc(Application&);
    // UI テスト用: エディタのシーケンス編集セッションを終える（元の値へ戻す）。
    static void SequenceEditorStopForTest(Application&);
    // UI テスト用: 読み込んだ .dxseq の文書を閉じる / その assets 相対パス（テストが一覧に文書を残さないため）。
    static void SequenceCloseDocForTest(Application&, const std::string& nameOrRel);
    static std::string SequenceDocRelForTest(Application&, const std::string& nameOrRel);   // 開いていなければ空
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
    // W1-C7: 品質プリセットの本体（定義は QualityPresetApply.cpp）。caps（DXR / DLSS）の問い合わせ、現在のシーンが選択中のプリセットと
    //   一致しているか、Undo / Redo の復元。
    //   ReloadQualityPresetFromSettings は互換のために残した空の関数（GetQualityPreset が毎回 settings.json を引くので呼ぶ必要は無い）。
    static void QueryQualityCaps(const Application&, bool& dxr, bool& dlss);
    static bool QualityMatchesScene(const Application&);
    static void ReloadQualityPresetFromSettings(Application&);
    static void RestoreQualityState(Application&, const QualityScenePack&, u8 id, f32 renderScale);
    // W1-C3: shadingRevision（1 / 2）を PSO とグラフ材質へ反映する（定義は ApplicationPipeline.cpp。Run ループの先頭 = フレーム外で呼ぶ）。
    static void UpdateShadingRevision(Application&);
    // W1-C6: Epic の動的 CSM（2048 D16 x3 + pancaking）の切り替え。UpdateShadingRevision の先頭から呼ぶ（フレーム外。定義は ApplicationPipeline.cpp）。
    static void UpdateEpicCsm(Application&);
    // W1-C3: 今 PSO に反映済みの revision と、V2 の .cso から作れた PSO の数（フォワード / スキンド / 地形）。MCP の get_shading 用。
    //   FU-E-25: 水 / 植生 / 仮想ジオメトリの V2 PSO の本数も返す（実体が無い / V1 のときは 0）。
    static void QueryShadingPsos(const Application&, int& appliedRevision, int& v2Forward, int& v2Skinned, int& v2Terrain,
                                 int& v2Water, int& v2Foliage, int& v2Vg);
    // W1-C9: 撮影が IBL の焼き直しを待つために足すフレーム数（0 = 待たない。定義は core/ApplicationAtmosphere.cpp）。
    static int IblRebakeWaitFrames(Application&);
    // FU-E-24: freeze_time で描画の時間依存を固定している最中か（screenshot_final が「決定論キャプチャ中」と取り違えないため。定義は mcp/ApplicationMcpLaneC.cpp）。
    static bool TimeFrozen(const Application&);
    //   FreezePinsRender … freeze_time が描画の時間依存を固定する約束の最中か（撮影の後始末が決定論キャプチャを戻す先）。
    static bool FreezePinsRender(const Application&);
    //   FreezeApply … 凍結の約束（FreezeModel）に m_deterministicCapture / paused を合わせる。MCP クライアントの切断（5 秒）で凍結を解く。
    //   FreezeTick  … 毎フレーム 1 回（UpdateShadingRevision の先頭から呼ぶ）。撮影の中断・Play/Stop・シーン切替の後始末を約束へ戻す。
    static void FreezeApply(Application&, bool restartHistory, bool releasedNow);
    static void FreezeTick(Application&);
    // W1-C8: 新規シーンのルック（定義は QualityPresetApply.cpp。自由関数の窓口は core/QualityPresetApply.h）。
    //   プロファイルの Scene への書き込み + 時間履歴の破棄。settings.json（品質プリセット id・render_scale）は書かない。Undo は積まない。
    static void ApplyNewSceneLookState(Application&, const struct QualityPlan& plan, bool epic, bool giNewApplied);
    // ==== lane-C end ====

    // ==== lane-D begin ====
    // ==== lane-D end ====
};
} // namespace dx12e
