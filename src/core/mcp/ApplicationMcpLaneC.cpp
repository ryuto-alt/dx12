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
#include "renderer/QualityPresetPlan.h"

#include <chrono>

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

const char* UpscalerName(QualityUpscaler u)
{
    switch (u) { case QualityUpscaler::Off: return "off"; case QualityUpscaler::Taa: return "taa"; case QualityUpscaler::Dlss: return "dlss"; }
    return "off";
}
const char* ColumnName(QualityColumn c)
{
    switch (c) { case QualityColumn::Dlss: return "dlss"; case QualityColumn::Taau: return "taau"; case QualityColumn::TaaNative: return "taaNative"; default: break; }
    return "";
}

// 今の caps で入る構成（縮退つき）。get / set の両方が返す。
nlohmann::json QualityPlanJson(const QualityPlan& p, bool matchesScene)
{
    nlohmann::json j = nlohmann::json::object();
    j["caps"]        = { {"dxr", p.caps.dxr}, {"dlss", p.caps.dlss} };
    j["upscaler"]    = UpscalerName(p.upscaler);
    if (p.upscaler == QualityUpscaler::Dlss) j["dlssQuality"] = upscale::QualityName(upscale::ClampQuality(p.dlssQuality));
    j["shadingRevision"] = p.shadingRevision;
    j["shadows"]     = { {"pcss", p.pcss}, {"dynamicCsm", p.epicDynamicCsm}, {"rtSun", p.rtShadow}, {"contact", p.contactShadow} };
    j["gi"]          = { {"ddgi", p.ddgi}, {"ssgi", p.ssgi}, {"ssao", p.ssao} };
    j["reflections"] = { {"ssr", p.ssr}, {"rt", p.rtReflect}, {"rtHalfRes", p.rtReflectHalfRes} };
    if (p.setRenderScale) j["renderScale"] = p.renderScale;
    const QualityPreset& pre = GetPreset(p.id);
    if (pre.hasBudget)
    {
        // 合否の列: DLSS があれば dlss（W1 の合否）。無ければ taaNative（記録のみ）。
        j["budgetColumn"] = ColumnName(p.column);
        j["budgetGated"]  = qualityplan::IsGated(pre, p.column);
        j["budgetTotalMs"] = pre.Column(p.column).totalMs;
        j["vram"] = { {"capGb", pre.hasVramCap ? nlohmann::json(pre.vramCapGb) : nlohmann::json(nullptr)},
                      {"marginGb", pre.vramMarginGb}, {"precheckGb", pre.vramPrecheckGb} };
    }
    nlohmann::json deg = nlohmann::json::array();
    for (u8 i = 0; i < p.degradeCount; ++i) deg.push_back({ {"row", p.degrades[i].row}, {"text", p.degrades[i].text} });
    j["degrades"]     = deg;
    j["matchesScene"] = matchesScene;   // false = 選んだ後にシーンの設定を手で変えた（または別のシーンを開いた）
    return j;
}

// FU-E-24: freeze_time が立てた状態の記録（Application.h は凍結なので、この TU に持つ。エンジンは 1 つ）。
// 状態の持ち主はここ 1 か所（FrameStepLogic.h の FreezeModel）。m_deterministicCapture / paused は
// FreezeApply が毎フレームこの約束に合わせる（撮影の中断・Play/Stop・MCP 切断の後始末もここへ集約）。
FreezeModel g_freeze;
std::chrono::steady_clock::time_point g_lastConnected{};   // 値初期化（動的初期化を増やさない）。freeze_time / frame_step の呼び出しと接続中に更新する
} // namespace

bool AppAccess::TimeFrozen(const Application& app)
{
    return g_freeze.WantPin() && app.m_deterministicCapture;
}

bool AppAccess::FreezePinsRender(const Application&)
{
    return g_freeze.WantPin();
}

void AppAccess::FreezeApply(Application& app, bool restartHistory, bool releasedNow)
{
    if (!g_freeze.frozen && !g_freeze.ownsCapture && !releasedNow) return;   // 何も張っていない（通常はここで抜ける）
    const auto now = std::chrono::steady_clock::now();
    const bool connected = !app.m_mcpBridge || app.m_mcpBridge->ClientCount() > 0;
    if (connected) g_lastConnected = now;

    FreezeView v;
    v.captureOn       = app.m_deterministicCapture;
    v.shotInFlight    = app.m_mcpFinalShot.reply.client != 0 || app.m_mcpFinalShot.pending || app.m_mcpFinalShot.captured   // ★deterministic / wantSceneRt は撮影後も残る印なので見ない（reply.client が撮影中の印）
                        || app.m_mcpPerceive != nullptr || app.m_deterministicFramesLeft > 0;
    v.playing         = app.m_engineMode == Application::EngineMode::Playing && !app.m_isGameMode;
    v.paused          = app.m_editorCtx && app.m_editorCtx->paused;
    v.stepping        = app.m_mcpStepFramesLeft > 0 || app.m_mcpStepReply.client != 0;
    v.disconnectedSec = std::chrono::duration<double>(now - g_lastConnected).count();

    const FreezePlan p = PlanFreeze(g_freeze, v, restartHistory, releasedNow);
    if (p.releaseFreeze)
    {
        g_freeze.frozen = false;
        Logger::Info("freeze_time: MCP が {} 秒切れていたので凍結を解いた", kFreezeLeaseSec);
    }
    if (p.invalidateHistory) app.InvalidateTemporalHistory();   // 固定を張る前に履歴を捨て、固定ジッタで収束させ直す
    if (p.setCapture)
    {
        app.m_deterministicCapture = p.captureValue;
        g_freeze.ownsCapture       = p.captureValue;
    }
    if (p.setPaused && app.m_editorCtx) app.m_editorCtx->paused = p.pausedValue;
}

void AppAccess::FreezeTick(Application& app)
{
    FreezeApply(app, false, false);
}

void Application::RegisterMcpLaneCMethods()
{
    using namespace appdetail;
    // W1-C7: 品質プリセット。set は計画をシーンの設定へ書く（Undo は呼び出し 1 回分）。get は今の構成と縮退を返す。
    McpDefine("get_quality_preset", "", DX12E_MCP_HANDLER
        {
            resp["ok"]     = true;
            resp["result"] = QualityPresetJson(GetQualityPreset(*this));
            resp["result"]["plan"] = QualityPlanJson(CurrentQualityPlan(*this), QualityMatchesScene(*this));
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
            resp["result"]["applied"] = true;
            resp["result"]["plan"]    = QualityPlanJson(CurrentQualityPlan(*this), QualityMatchesScene(*this));
            resp["result"]["note"]    = "シーンの設定へ反映した（Undo 1 回分）。アップスケーラ・解像度は次のフレームから";
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
        const bool simHeld = app.m_editorCtx && app.m_editorCtx->paused && app.m_engineMode == EngineMode::Playing
                             && !app.m_isGameMode;
        j["frozen"]       = g_freeze.frozen;
        j["renderPinned"] = app.m_deterministicCapture && g_freeze.WantPin();
        j["pinRender"]    = g_freeze.pinRender;
        j["simHeld"]      = simHeld;
        j["mode"]         = app.m_engineMode == EngineMode::Playing ? "Playing" : "Editor";
        return j;
    };
    // 凍結する（張り直す）。実行中の撮影と衝突させない。
    static const auto engageFreeze = [](Application& app, bool pinRender) {
        if (app.m_mcpFinalShot.reply.client != 0 || app.m_mcpPerceive != nullptr)
            throw McpError(McpErr::ModeConflict, "撮影（screenshot_final / perceive）の最中は凍結を変えられない",
                           "撮影の応答を待ってから呼ぶ");
        g_freeze.frozen    = true;
        g_freeze.pinRender = pinRender;
        AppAccess::FreezeApply(app, /*restartHistory*/ true, /*releasedNow*/ false);
    };
    McpDefine("freeze_time", "frozen:bool,pinRender:bool", DX12E_MCP_HANDLER
        {
            g_lastConnected = std::chrono::steady_clock::now();   // 最後の呼び出しからリースを数える（接続を毎回切る fleet でも保つ）
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
                g_freeze.frozen = false;
                AppAccess::FreezeApply(*this, /*restartHistory*/ false, /*releasedNow*/ true);   // 固定と paused を戻す
            }
            resp["ok"]     = true;
            resp["result"] = freezeInfo(*this);
            resp["result"]["note"] = freeze
                ? "止めた。frame_step で 1 フレームずつ進める（描画を固定している間の撮影は常に決定論経路）。解除は frozen:false"
                : "解除した。時間は実時間で流れる";
        });
    McpDefine("frame_step", "frames:int,dt:number,pinRender:bool", DX12E_MCP_HANDLER
        {
            g_lastConnected = std::chrono::steady_clock::now();
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
            m_mcpStepExtra           = { {"frozen", true}, {"renderPinned", m_deterministicCapture && g_freeze.WantPin()},
                                         {"api", "frame_step"},
                                         {"note", g_freeze.pinRender
                                             ? "描画の時間は固定（Editor では絵は変わらない）。dt が効くのは Play 中のシミュだけ"
                                             : "描画の時間は固定していない"} };
            m_mcpStepReply = deferred;
            isDeferred     = true;
        });
}
} // namespace dx12e
