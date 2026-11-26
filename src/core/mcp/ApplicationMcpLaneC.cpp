// ===========================================================================
// レーン C の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）。
// 中身は各レーンが書く。Application.h は変えない（W7 まで凍結）。
// Application の private に触れる処理は core/AppAccess.h のレーン C のブロックへ宣言する。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AppAccess.h"
#include "core/FrameStepLogic.h"
#include "core/QualityPresetApply.h"
#include "renderer/QualityPreset.h"

namespace dx12e
{
namespace
{
nlohmann::json QualityPresetJson(QualityPresetId id)
{
    const QualityPreset& p = GetPreset(id);
    nlohmann::json j = nlohmann::json::object();
    j["preset"]    = p.mcpName;
    j["label"]     = p.label;
    j["hasBudget"] = p.hasBudget;
    if (p.hasBudget)
    {
        j["targetFps"] = p.targetFps;
        j["output"]    = { {"width", p.outW}, {"height", p.outH} };
        j["p95Ms"]     = p.p95Ms;
        j["p99Ms"]     = p.p99Ms;
        j["vfx"]       = { {"emissionMultiplier", p.emissionMultiplier}, {"maxParticles", p.maxParticles} };
    }
    return j;
}

// FU-E-24: freeze_time が立てた状態の記録（Application.h は凍結なので、この TU に持つ。エンジンは 1 つ）。
struct FreezeState
{
    bool frozen     = false;   // freeze_time（または frame_step の自動凍結）で止めている
    bool pinRender  = true;    // 描画の時間依存（totalTime・TAA ジッタ・パーティクル等）を固定しているか
};
FreezeState g_freeze;
} // namespace

bool AppAccess::TimeFrozen(const Application& app)
{
    return g_freeze.frozen && app.m_deterministicCapture;
}

bool AppAccess::FreezePinsRender(const Application&)
{
    return g_freeze.frozen && g_freeze.pinRender;
}

void Application::RegisterMcpLaneCMethods()
{
    using namespace appdetail;
    // W1-C1: 品質プリセットの口。C1 は選択の記録だけ（絵への反映と Undo は C7）。
    McpDefine("get_quality_preset", "", DX12E_MCP_HANDLER
        {
            resp["ok"]     = true;
            resp["result"] = QualityPresetJson(GetQualityPreset(*this));
            resp["result"]["applied"] = false;   // C7 までは記録のみ
        });
    McpDefine("set_quality_preset", "preset:string", DX12E_MCP_HANDLER
        {
            const std::string name = params.value("preset", std::string());
            QualityPresetId id{};
            if (!ParseQualityPreset(name.c_str(), id))
                throw McpError(McpErr::InvalidParam, "preset は low | medium | high | epic | cinematic",
                               "例: preset:'epic'");
            if (!SetQualityPreset(*this, id, /*recordUndo*/ true))
                throw McpError(McpErr::Internal, "品質プリセットを設定できなかった", "もう一度試す");
            resp["ok"]     = true;
            resp["result"] = QualityPresetJson(id);
            // C1 は選択の記録だけ。絵・シーン・Undo には何も反映しない（C7 で入る）。
            resp["result"]["applied"] = false;
            resp["result"]["note"]    = "選択を記録しただけで、まだ絵には反映しない（品質プリセットの適用は C7 で入る）";
        });

    // ---- FU-E-24: 時間を止めて 1 フレームずつ進める口（NB3 の「静止した 8 フレーム」を決定論で撮るため）----
    //   step_frames は「N フレーム待つだけ」で、応答を待つ間も時間は流れる（deterministic:true + hold も Play 中のシミュだけ）。
    //   freeze_time   … 時間依存を止める: Play 中のシミュ（EditorContext::paused）と、描画の時間依存
    //                   （決定論キャプチャと同じ固定: totalTime・TAA ジッタ・フォグ/SSGI の位相・パーティクルの dt・大気/雲）。
    //   frame_step    … 凍結したまま N フレーム（固定 dt）だけ進めて、また止める。凍結していなければ自動で凍結する。
    //   ★screenshot_final {deterministic:true} は撮影の後に決定論キャプチャを解除する。解除されたら freeze_time の応答が
    //     frozen:false（renderPinned:false）で知らせる。frame_step は毎回張り直す。
    static const auto freezeInfo = [](Application& app) {
        nlohmann::json j = nlohmann::json::object();
        const bool pinned  = app.m_deterministicCapture;
        const bool simHeld = app.m_editorCtx && app.m_editorCtx->paused && app.m_engineMode == EngineMode::Playing;
        j["frozen"]       = g_freeze.frozen && (pinned || !g_freeze.pinRender);
        j["renderPinned"] = pinned;
        j["simHeld"]      = simHeld;
        j["mode"]         = app.m_engineMode == EngineMode::Playing ? "Playing" : "Editor";
        return j;
    };
    // 凍結する。実行中の撮影と衝突させない。
    static const auto engageFreeze = [](Application& app, bool pinRender) {
        if (app.m_mcpFinalShot.reply.client != 0 || app.m_mcpPerceive != nullptr)
            throw McpError(McpErr::ModeConflict, "撮影（screenshot_final / perceive）の最中は凍結を変えられない",
                           "撮影の応答を待ってから呼ぶ");
        g_freeze.pinRender = pinRender;
        if (pinRender && !app.m_deterministicCapture)
        {
            app.InvalidateTemporalHistory();      // 凍結前の履歴を捨てて、固定ジッタで収束させ直す
            app.m_deterministicCapture = true;
        }
        if (app.m_editorCtx && app.m_engineMode == EngineMode::Playing) app.m_editorCtx->paused = true;
        g_freeze.frozen = true;
    };
    McpDefine("freeze_time", "frozen:bool,pinRender:bool", DX12E_MCP_HANDLER
        {
            // 引数なしは状態の読み取り。frozen:true で止める（既定）/ false で解除。
            if (!params.contains("frozen") && !params.contains("pinRender"))
            {
                resp["ok"]     = true;
                resp["result"] = freezeInfo(*this);
                return;
            }
            if (m_mcpStepReply.client != 0)
                throw McpError(McpErr::ModeConflict, "step_frames / frame_step の応答待ちの間は凍結を変えられない",
                               "応答を待ってから呼ぶ");
            const bool freeze = params.value("frozen", true);
            if (freeze)
            {
                engageFreeze(*this, params.value("pinRender", true));
            }
            else
            {
                if (m_mcpFinalShot.reply.client != 0 || m_mcpPerceive != nullptr)
                    throw McpError(McpErr::ModeConflict, "撮影の最中は凍結を解除できない", "撮影の応答を待ってから呼ぶ");
                if (g_freeze.pinRender) m_deterministicCapture = false;   // 固定を張っていたのは私たち
                if (m_editorCtx && m_engineMode == EngineMode::Playing) m_editorCtx->paused = false;
                g_freeze.frozen = false;
            }
            resp["ok"]     = true;
            resp["result"] = freezeInfo(*this);
            resp["result"]["note"] = freeze
                ? "止めた。frame_step で 1 フレームずつ進める。解除は frozen:false"
                : "解除した。時間は実時間で流れる";
        });
    McpDefine("frame_step", "frames:int,dt:number,pinRender:bool", DX12E_MCP_HANDLER
        {
            if (m_mcpStepReply.client != 0)
                throw McpError(McpErr::ModeConflict, "a step is already pending; retry shortly",
                               "前の step_frames / frame_step の応答を待ってから呼ぶ");
            const FrameStepRequest req = ParseFrameStepRequest(params.value("frames", 1), params.contains("dt"),
                                                               params.value("dt", 1.0 / 60.0));
            if (!req.ok) throw McpError(McpErr::InvalidParam, req.error, "dt は 1/60 = 0.016667 が既定");
            // 凍結していない / 解除されていたら張り直す（途中で screenshot_final が決定論キャプチャを解いた場合など）。
            engageFreeze(*this, params.value("pinRender", g_freeze.frozen ? g_freeze.pinRender : true));
            // ここから N フレームだけ進める（step_frames の deterministic + hold と同じ機構。終わると Application が止め直す）。
            if (m_editorCtx) m_editorCtx->paused = false;
            m_mcpStepHold = true;
            m_gameClock.SetFixedDelta(req.dt);
            m_mcpStepFixedDt         = true;
            m_mcpStepFramesLeft      = req.frames;
            m_mcpStepFramesRequested = req.frames;
            m_mcpStepExtra           = { {"frozen", true}, {"renderPinned", g_freeze.pinRender},
                                         {"api", "frame_step"} };
            m_mcpStepReply = deferred;
            isDeferred     = true;
        });
}
} // namespace dx12e
