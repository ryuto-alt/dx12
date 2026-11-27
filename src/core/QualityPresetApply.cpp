// 品質プリセットの設定 / 取得（W1-C7 RM-C3）。
//   選択中の id は Application.h が凍結なのでこの TU に持つ（Application は 1 つ）。
//   SetQualityPreset は計画（BuildQualityPlan）をシーンの設定へ当て、Undo は 1 回分（QualityPresetCommand）。
//   id の永続化は settings.json "quality_preset"（0..4）。--quality / プロジェクトを開いた後の読み直しは
//   ReloadQualityPresetFromSettings を ApplicationProject.cpp の持ち主が呼ぶ（依頼）。
#include "core/QualityPresetApply.h"
#include "core/AppAccess.h"
#include "core/Logger.h"
#include "core/QualityPresetScene.h"
#include "renderer/UpscalerPass.h"

#include <atomic>

namespace dx12e
{
namespace
{
// 既定は High（従来の見た目。ゴールデンはビット一致のまま。設定ファイルに値があればそれ）。
std::atomic<u8>   g_currentPreset{static_cast<u8>(QualityPresetId::High)};
std::atomic<bool> g_loaded{false};

// Undo / Redo の 1 回分。前後のシーン設定・id・空間スケールを値で持つ（ポインタは持たない）。
class QualityPresetCommand final : public IUndoCommand
{
public:
    QualityPresetCommand(Application* app, QualityScenePack before, QualityScenePack after,
                         u8 idBefore, u8 idAfter, f32 scaleBefore, f32 scaleAfter)
        : m_app(app), m_b(before), m_a(after), m_idB(idBefore), m_idA(idAfter), m_sB(scaleBefore), m_sA(scaleAfter) {}
    void Undo() override { AppAccess::RestoreQualityState(*m_app, m_b, m_idB, m_sB); }
    void Redo() override { AppAccess::RestoreQualityState(*m_app, m_a, m_idA, m_sA); }
    const char* GetName() const override { return "Quality Preset"; }
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
    const int v = static_cast<int>(app.PersistGet("quality_preset", static_cast<double>(static_cast<int>(QualityPresetId::High))));
    if (v >= 0 && v < static_cast<int>(QualityPresetId::Count)) g_currentPreset.store(static_cast<u8>(v), std::memory_order_relaxed);
    g_loaded.store(true, std::memory_order_relaxed);
}

void AppAccess::RestoreQualityState(Application& app, const QualityScenePack& k, u8 id, f32 scale)
{
    if (app.m_scene) RestoreQualityScenePack(*app.m_scene, k);
    g_currentPreset.store(id, std::memory_order_relaxed);
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
    g_currentPreset.store(static_cast<u8>(id), std::memory_order_relaxed);
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
    if (!g_loaded.load(std::memory_order_relaxed))
        ReloadQualityPresetFromSettings(const_cast<Application&>(app));   // PersistGet は論理的に読むだけ（初回だけ設定ファイルを読む）
    return static_cast<QualityPresetId>(g_currentPreset.load(std::memory_order_relaxed));
}

bool AppAccess::QualityMatchesScene(const Application& app)
{
    if (!app.m_scene) return false;
    bool dxr = false, dlss = false;
    QueryQualityCaps(app, dxr, dlss);
    return QualityPlanMatchesScene(BuildQualityPlan(GetQualityPreset(app), { dxr, dlss }), *app.m_scene);
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
