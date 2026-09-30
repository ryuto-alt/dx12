// ===========================================================================
// Application × シーケンサー(S1b): ホストの生成・フレーム更新・アクティブカメラの選択
// ---------------------------------------------------------------------------
//   実体は core/SequencerHost.{h,cpp}(Application 非依存。tests/sequencer_bind_test.cpp が単体検査する)。
//   ここは「エンジンの各部へ結ぶ」糊だけ。設計: docs/SEQUENCER_DESIGN.md §3 / §7、仕様: docs/DXSEQ_FORMAT.md §18〜§20。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/SequencerHost.h"

namespace dx12e
{
using namespace appdetail;

void Application::InitSequencer()
{
    if (m_sequencer) return;
    m_sequencer = std::make_unique<seqhost::SequencerHost>();

    seqhost::HostCallbacks cb;
    cb.emit = [this](const EngineEvent& e) { m_eventBus.Emit(e); };
    cb.callLua = [this](const std::string& fn, const std::vector<EngineEvent::Value>& args, std::string& err) -> bool
    {
        if (!m_scriptEngine) { err = "ScriptEngine が無い"; return false; }
        return m_scriptEngine->CallGlobalFunction(fn, args, err);
    };
    cb.loadScene = [this](const std::string& rel)
    {
        if (m_editorCtx) m_editorCtx->pendingGameLoadPath = rel;   // Lua の loadScene と同じ経路(フレーム境界で処理される)
    };
    cb.setTimeScale = [this](float s) { if (m_scriptEngine) m_scriptEngine->SetTimeScale(s); };
    cb.getTimeScale = [this]() -> float { return m_scriptEngine ? m_scriptEngine->GetTimeScale() : 1.0f; };
    cb.subscribe = [this](const std::string& name, std::function<void(const EngineEvent&)> fn)
    {
        m_eventBus.On(name, std::move(fn));
    };
    m_sequencer->SetCallbacks(std::move(cb));
    m_sequencer->InstallSaveHook();   // あらゆる保存(Save / SaveToString / SerializeEntity / SerializeSubtree)の直前に元値へ戻す
    if (m_scriptEngine) m_scriptEngine->SetSequenceApi(m_sequencer.get());
}

void Application::UpdateSequencers(f32 dt, bool paused)
{
    if (!m_sequencer || !m_scene) return;
    m_sequencer->SetScene(m_scene.get());
    const seqhost::HostMode mode = (m_engineMode == EngineMode::Playing) ? seqhost::HostMode::Playing : seqhost::HostMode::Editor;
    // シーンが作り直されると変わる値(open_scene / Stop / ランタイムの loadScene)。entity id は全部入れ替わる。
    const std::uint64_t token = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(m_sceneGeneration)) << 32) ^
                                static_cast<std::uint64_t>(std::hash<std::string>{}(m_currentSceneRel) & 0xFFFFFFFFu);
    m_sequencer->Update(dt, mode, paused, token);
}

entt::entity Application::FindActiveCameraEntity()
{
    if (!m_scene) return entt::null;
    auto& reg = m_scene->GetRegistry();
    // カットが選んだカメラを優先する(isActive は書き換えない = シーンを汚さず、終了時に戻す処理も要らない)
    if (m_sequencer)
    {
        const entt::entity cut = m_sequencer->CutCameraEntity();
        if (cut != entt::null && reg.valid(cut) && reg.all_of<CameraComponent>(cut)) return cut;
    }
    for (auto [e, cam] : reg.view<const CameraComponent>().each())
        if (cam.isActive) return e;
    return entt::null;
}

} // namespace dx12e
