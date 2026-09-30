// ===========================================================================
// Q2（UE 同等化の校正）: ライティング単位の切替 / 任意解像度のオフスクリーン撮影 / 線形 HDR スクリーンショットの進行管理
// ---------------------------------------------------------------------------
// 描画側の小さな差し込み（Application::PrepareFrame / RenderPostChain / FinishFinalScreenshot）は別の TU にあり、
// ここは「状態の進行」と「入力の検証」だけを持つ（共有ファイルの編集を最小にするため新しい TU に切り出した）。
// 純ロジック（サイズの解釈・上限・VRAM の見積）は core/OffscreenShot.h（tests/offscreen_shot_test.cpp）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/OffscreenShot.h"
#include "core/mcp/FleetGuard.h"
#include "renderer/LinearCapturePass.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace dx12e
{
using namespace appdetail;

// ---------------------------------------------------------------------------
// ライティング単位（0=従来 / 1=物理）。フォワード系のピクセルシェーダを *Phys_PS.cso へ差し替える。
// Run ループの先頭（フレーム外）で呼ぶ。切替は GPU 待ちを 1 回挟む（頻繁に切り替える用途ではない）。
// ---------------------------------------------------------------------------
void Application::UpdateLightingUnits()
{
    if (!m_scene || !m_commandQueue || !m_graphicsDevice || !m_rootSignature) return;
    const int want = (m_scene->GetPostSettings().lightingUnits == 1) ? 1 : 0;
    if (want == m_lightingUnitsApplied) return;
    m_commandQueue->WaitIdle();
    m_lightingUnitsApplied = want;
    RecreateForwardPsos();
    RecreateSkinnedPsos();
    RecreateTerrainPsos();
    Logger::Info("ライティング単位: {}（フォワード系 PSO を差し替えました）", want == 1 ? "物理" : "従来");
}

// ---------------------------------------------------------------------------
// screenshot_final の format / formats / width / height を解釈して m_mcpFinalShot へ入れる（検証つき）。
// 既定（何も指定しない）は従来どおり: PNG のみ・ビューポート矩形。--size があれば width/height の既定になる。
// ---------------------------------------------------------------------------
bool Application::SetupFinalShotOptions(const nlohmann::json& params, std::string& err)
{
    auto& fs = m_mcpFinalShot;

    // ---- 出力形式 ----
    std::set<std::string> fmts;
    auto addFmt = [&](const nlohmann::json& v) -> bool
    {
        if (!v.is_string()) { err = "format must be a string (png | pfm | exr)"; return false; }
        std::string s = v.get<std::string>();
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "png" || s == "pfm" || s == "exr") { fmts.insert(s); return true; }
        err = "unknown format: " + s + " (png | pfm | exr)";
        return false;
    };
    if (params.contains("formats"))
    {
        const auto& v = params["formats"];
        if (v.is_array()) { for (const auto& e : v) if (!addFmt(e)) return false; }
        else if (!addFmt(v)) return false;
    }
    if (params.contains("format") && !addFmt(params["format"])) return false;
    if (fmts.empty()) fmts.insert("png");
    fs.wantPng = fmts.count("png") > 0;
    fs.wantPfm = fmts.count("pfm") > 0;
    fs.wantExr = fmts.count("exr") > 0;
    fs.offFormats.clear();
    for (const auto& f : fmts) { if (!fs.offFormats.empty()) fs.offFormats += ","; fs.offFormats += f; }

    // ---- 解像度 ----
    uint32_t w = 0, h = 0;
    if (params.contains("width"))  w = static_cast<uint32_t>((std::max)(0, params.value("width", 0)));
    if (params.contains("height")) h = static_cast<uint32_t>((std::max)(0, params.value("height", 0)));
    if ((w == 0) != (h == 0))
    {
        err = "width and height must be given together";
        return false;
    }
    if (w == 0 && offshot::Cli().w > 0) { w = offshot::Cli().w; h = offshot::Cli().h; }   // 起動引数 --size

    if (w > 0)
    {
        const bool lin = fs.WantLinear();
        offshot::Verdict v = offshot::Validate(w, h, lin);
        if (v == offshot::Verdict::Ok && m_graphicsDevice)
        {
            double usedMB = 0.0, budgetMB = 0.0;
            uint64_t avail = 0;
            if (fleet::QueryVideoMemoryMB(m_graphicsDevice->GetDevice(), usedMB, budgetMB) && budgetMB > usedMB)
                avail = static_cast<uint64_t>((budgetMB - usedMB) * 1024.0 * 1024.0);
            v = offshot::CheckVram(w, h, lin, avail);
        }
        if (v != offshot::Verdict::Ok) { err = offshot::Describe(v, w, h); return false; }
        fs.offW = w;
        fs.offH = h;
    }
    else if (fs.WantLinear())
    {
        // 解像度の指定なし: 線形 float はシーン RT（レンダー解像度）そのまま。大きすぎる場合だけ弾く。
        const uint32_t rw = m_renderW > 0 ? m_renderW : 1u, rh = m_renderH > 0 ? m_renderH : 1u;
        const offshot::Verdict v = offshot::Validate((std::max)(rw, offshot::kMinSide), (std::max)(rh, offshot::kMinSide), true);
        if (v != offshot::Verdict::Ok && v != offshot::Verdict::TooSmall) { err = offshot::Describe(v, rw, rh); return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// オフスクリーン撮影の進行。Run ループで UpdateRenderResolution() の直後に毎フレーム呼ぶ。
//   ① 解像度の切替を待つ（UpdateRenderResolution が offW x offH へ即時に切り替える）
//   ② 切り替わったら履歴を捨てて収束フレームを数え直す（決定論: 「同じ初期状態 + 同じフレーム数」）
//   ③ 数え終えたら Render の中で撮る（pending）
// 終了は m_mcpFinalShot.reply の消去で自動（OffscreenActive() が false → UpdateRenderResolution が元の解像度へ戻す）。
// ---------------------------------------------------------------------------
void Application::ServiceOffscreenShot()
{
    auto& fs = m_mcpFinalShot;
    if (!m_offscreenWasActive && !fs.OffscreenActive()) return;

    // 終了後の後始末: 出力先 RT を最小へ縮める（大きな RT を持ち続けない）
    if (!fs.OffscreenActive())
    {
        if (m_offscreenOutRT && m_offscreenOutRT->GetWidth() > 16)
        {
            m_commandQueue->WaitIdle();
            m_offscreenOutRT->Resize(*m_graphicsDevice, 16, 16);
        }
        return;   // m_offscreenWasActive は UpdateRenderResolution が落とす
    }

    if (!fs.offApplied)
    {
        if (m_renderW != fs.offW || m_renderH != fs.offH) return;   // まだ切り替わっていない

        // 出力先（LDR。バックバッファと同じ形式）を用意する
        try
        {
            m_commandQueue->WaitIdle();
            if (!m_offscreenOutRT)
            {
                const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                m_offscreenOutRT = std::make_unique<RenderTarget>();
                m_offscreenOutRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                             fs.offW, fs.offH, m_swapChain->GetFormat(), clear);
            }
            else if (m_offscreenOutRT->GetWidth() != fs.offW || m_offscreenOutRT->GetHeight() != fs.offH)
            {
                m_offscreenOutRT->Resize(*m_graphicsDevice, fs.offW, fs.offH);
            }
        }
        catch (const std::exception& e)
        {
            AbortOffscreenShot(std::string("offscreen output allocation failed: ") + e.what());
            return;
        }
        fs.offApplied = true;
        if (fs.deterministic)
        {
            InvalidateTemporalHistory();
            m_deterministicFramesLeft = fs.offSettle;
            m_deterministicCapture    = true;
        }
        else
        {
            fs.offFramesLeft = 2;   // 切替直後の 1 枚は履歴が無いので、2 フレーム描いてから撮る
        }
        return;
    }

    if (!fs.deterministic && fs.offFramesLeft > 0 && --fs.offFramesLeft == 0)
        fs.pending = true;
}

// オフスクリーン撮影を中止する（メモリ不足など）。応答はエラー、状態は元へ（解像度は次フレームに戻る）。
void Application::AbortOffscreenShot(const std::string& why)
{
    Logger::Warn("オフスクリーン撮影を中止しました: {}", why);
    if (m_mcpFinalShot.reply.client != 0)
        FailMcp(m_mcpBridge.get(), m_mcpFinalShot.reply, McpErr::Internal, why);
    m_mcpFinalShot = {};
    m_deterministicCapture    = false;
    m_deterministicFramesLeft = 0;
}

// ---------------------------------------------------------------------------
// 線形 HDR の記録（RenderPostChain の自動露出と同じ位置から。要求のあるフレームだけ）。
// 失敗しても撮影全体は止めない（PNG は返る）。理由は Finish が応答に載せる。
// ---------------------------------------------------------------------------
bool Application::RecordLinearCapture(ID3D12GraphicsCommandList* cmd, D3D12_GPU_DESCRIPTOR_HANDLE srcSrv, u32 w, u32 h)
{
    auto& fs = m_mcpFinalShot;
    fs.linearRecorded = false;
    fs.linearErr.clear();
    try
    {
        if (!m_linearCapture)
        {
            m_linearCapture = std::make_unique<LinearCapturePass>();
            m_linearCapture->Initialize(*m_graphicsDevice, PathResolver::ShaderDirW());
        }
        std::string err;
        if (!m_linearCapture->Record(*m_graphicsDevice, cmd, srcSrv, w, h, err))
        {
            fs.linearErr = err;
            return false;
        }
    }
    catch (const std::exception& e)
    {
        fs.linearErr = e.what();
        return false;
    }
    fs.linearRecorded = true;
    return true;
}

} // namespace dx12e
