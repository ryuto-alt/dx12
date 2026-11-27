// 品質プリセットの設定 / 取得（W1-C7 RM-C3）。
//   選択中の id は Application.h が凍結なのでこの TU に持つ（Application は 1 つ）。
//   SetQualityPreset は計画（BuildQualityPlan）をシーンの設定へ当て、Undo は 1 回分（QualityPresetCommand）。
//   id の永続化は settings.json "quality_preset"（0..4）。取得のたびに PersistGet を引くので、プロジェクトを開き直せば
//   そのプロジェクトの値に追従する（キャッシュしない）。--quality は未実装（main.cpp の持ち主へ依頼）。
//   SetRenderScale は High / Epic / Cinematic の適用で呼ばれ、保存済みの render_scale を上書きする（Undo で戻る）。
#include "core/QualityPresetApply.h"
#include "core/AppAccess.h"
#include "core/Logger.h"
#include "core/QualityPresetScene.h"
#include "scene/SceneLookProfile.h"
#include "renderer/UpscalerPass.h"

namespace dx12e
{
namespace
{
// 既定は High（従来の見た目。ゴールデンはビット一致のまま。設定ファイルに値があればそれ）。

// Undo / Redo の 1 回分。前後のシーン設定・id・空間スケールを値で持つ（ポインタは持たない）。
class QualityPresetCommand final : public IUndoCommand
{
public:
    QualityPresetCommand(Application* app, QualityScenePack before, QualityScenePack after,
                         u8 idBefore, u8 idAfter, f32 scaleBefore, f32 scaleAfter)
        : m_app(app), m_b(before), m_a(after), m_idB(idBefore), m_idA(idAfter), m_sB(scaleBefore), m_sA(scaleAfter) {}
    void Undo() override { AppAccess::RestoreQualityState(*m_app, m_b, m_idB, m_sB); }
    void Redo() override { AppAccess::RestoreQualityState(*m_app, m_a, m_idA, m_sA); }
    const char* GetName() const override { return "品質プリセット"; }
private:
    Application* m_app;
    QualityScenePack m_b, m_a;
    u8 m_idB, m_idA;
    f32 m_sB, m_sA;
};
} // namespace

void AppAccess::QueryQualityCaps(const Application& app, bool& dxr, bool& dlss)
{
    dxr  = app.m_dxrEnabled;
    dlss = app.m_upscaler && app.m_upscaler->DlssAvailable();
}

void AppAccess::ReloadQualityPresetFromSettings(Application& app)
{
    (void)app.PersistGet("quality_preset", 0.0);   // 互換のために残す。取得は毎回 PersistGet を引くので読み直しは不要
}

void AppAccess::RestoreQualityState(Application& app, const QualityScenePack& k, u8 id, f32 scale)
{
    if (app.m_scene) RestoreQualityScenePack(*app.m_scene, k);
    app.PersistSet("quality_preset", static_cast<double>(id));
    app.SetRenderScale(scale);
    app.m_renderResFlush = true;
    app.InvalidateTemporalHistory(/*includeDdgi*/ false);
}

bool AppAccess::SetQualityPreset(Application& app, QualityPresetId id, bool recordUndo)
{
    if (static_cast<size_t>(id) >= static_cast<size_t>(QualityPresetId::Count)) return false;
    if (!app.m_scene) return false;
    bool dxr = false, dlss = false;
    QueryQualityCaps(app, dxr, dlss);
    const QualityPlan plan = BuildQualityPlan(id, { dxr, dlss });

    const QualityScenePack before = CaptureQualityScenePack(*app.m_scene);
    const u8  idBefore    = static_cast<u8>(GetQualityPreset(app));
    const f32 scaleBefore = app.m_renderScale;

    ApplyQualityPlanToScene(plan, *app.m_scene);
    if (plan.setRenderScale) app.SetRenderScale(plan.renderScale);
    app.PersistSet("quality_preset", static_cast<double>(static_cast<u8>(id)));
    app.m_renderResFlush = true;
    app.InvalidateTemporalHistory(/*includeDdgi*/ false);   // アップスケーラ・GI・影の構成が変わる。履歴を捨てて収束し直す
    for (u8 i = 0; i < plan.degradeCount; ++i)
        Logger::Info("品質プリセット {}: 縮退 {} … {}", GetPreset(id).mcpName, plan.degrades[i].row, plan.degrades[i].text);

    // Undo: Editor モードのときだけ。MCP の呼び出し中は横取りされて「AI: set_quality_preset」の 1 エントリになる。
    if (recordUndo && app.m_editorCtx && app.m_engineMode == Application::EngineMode::Editor && !app.m_isGameMode)
    {
        app.m_editorCtx->undoSystem.PushCommand(std::make_unique<QualityPresetCommand>(
            &app, before, CaptureQualityScenePack(*app.m_scene), idBefore, static_cast<u8>(id), scaleBefore, app.m_renderScale));
    }
    return true;
}

QualityPresetId AppAccess::GetQualityPreset(const Application& app)
{
    // PersistGet は非 const だが、論理的には読むだけ（未読み込みなら現在のプロジェクトの settings.json を読む）。
    const int v = static_cast<int>(const_cast<Application&>(app).PersistGet("quality_preset", static_cast<double>(static_cast<int>(QualityPresetId::High))));
    return (v >= 0 && v < static_cast<int>(QualityPresetId::Count)) ? static_cast<QualityPresetId>(v) : QualityPresetId::High;
}

bool AppAccess::QualityMatchesScene(const Application& app)
{
    if (!app.m_scene) return false;
    bool dxr = false, dlss = false;
    QueryQualityCaps(app, dxr, dlss);
    return QualityPlanMatchesScene(BuildQualityPlan(GetQualityPreset(app), { dxr, dlss }), *app.m_scene);
}

void AppAccess::ApplyNewSceneLookState(Application& app, const QualityPlan& plan, bool epic, bool giNewApplied)
{
    if (!app.m_scene || !epic) return;   // Classic は何も足さない（従来の新規シーンのまま）
    ApplyEpicSceneLook(*app.m_scene, plan, giNewApplied);
    // ユーザーの全体設定（settings.json の quality_preset / render_scale）は書き換えない。新規シーンに当てるのはシーンの設定だけ
    // （Ctrl+N のたびに保存済みの設定が上書きされ、後で開く既存シーンの描画にまで及ぶのを避ける）。
    // render_scale は変えないので描画リソースの作り直しは要らない（時間履歴の破棄だけ）。
    app.InvalidateTemporalHistory(/*includeDdgi*/ false);
    for (u8 i = 0; i < plan.degradeCount; ++i)
        Logger::Info("新規シーン（Epic）: 縮退 {} … {}", plan.degrades[i].row, plan.degrades[i].text);
}

namespace
{
// 次の新規シーンのプロファイル（Application は 1 つ。1 回で消費）。
NewSceneProfile g_nextNewSceneProfile = NewSceneProfile::Epic;
}

bool IsNewSceneProfileName(const std::string& name)
{
    NewSceneProfile p{};
    return ParseNewSceneProfile(name.c_str(), p);
}

void SetNextNewSceneProfile(const std::string& name)
{
    NewSceneProfile p{};
    g_nextNewSceneProfile = ParseNewSceneProfile(name.c_str(), p) ? p : NewSceneProfile::Epic;
}

std::string ApplyNewSceneProfile(Application& app, bool giNewApplied)
{
    const NewSceneProfile prof = g_nextNewSceneProfile;
    g_nextNewSceneProfile = NewSceneProfile::Epic;
    bool dxr = false, dlss = false;
    AppAccess::QueryQualityCaps(app, dxr, dlss);
    const QualityPlan plan = BuildQualityPlan(QualityPresetId::Epic, { dxr, dlss });
    AppAccess::ApplyNewSceneLookState(app, plan, prof == NewSceneProfile::Epic, giNewApplied);
    return NewSceneProfileName(prof);
}

bool SetQualityPreset(Application& app, QualityPresetId id, bool recordUndo)
{
    return AppAccess::SetQualityPreset(app, id, recordUndo);
}

QualityPresetId GetQualityPreset(const Application& app)
{
    return AppAccess::GetQualityPreset(app);
}

QualityPlan CurrentQualityPlan(const Application& app)
{
    bool dxr = false, dlss = false;
    AppAccess::QueryQualityCaps(app, dxr, dlss);
    return BuildQualityPlan(AppAccess::GetQualityPreset(app), { dxr, dlss });
}

bool QualityMatchesScene(const Application& app)
{
    return AppAccess::QualityMatchesScene(app);
}

} // namespace dx12e
