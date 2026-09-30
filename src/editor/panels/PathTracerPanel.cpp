#include "editor/UiWidgets.h"
#include "editor/panels/PathTracerPanel.h"

#include "editor/EditorContext.h"
#include "editor/PropertyGrid.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace dx12e
{
namespace PathTracerPanel
{
namespace
{

const char* PhaseLabel(int phase)
{
    switch (phase)
    {
    case 1: return "準備中(シーンの BLAS / TLAS を構築)";
    case 2: return "レンダー中";
    case 3: return "結果を書き出し中";
    case 4: return "完了";
    case 5: return "失敗";
    case 6: return "中止";
    default: return "待機中";
    }
}

std::string FormatTime(float sec)
{
    if (sec < 0.0f) return "-";
    const int s = static_cast<int>(sec + 0.5f);
    char buf[32];
    if (s >= 3600) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else           std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

} // namespace

void Render(EditorContext& ctx)
{
    if (!ctx.showPathTracer) return;
    pt::UiState& ui = ctx.ptUi;

    ImGui::SetNextWindowSize(ui::Px(440, 760), ImGuiCond_FirstUseEver);
    // ### 付きの固定 ID（UI 自動テストが窓を引くのに使う。表示名を変えても壊れない）
    if (!ImGui::Begin("リファレンスレンダー###PathTracerFloating", &ctx.showPathTracer))
    {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "DXR のパストレーサー(地上真値)でシーンを描き、線形 HDR(PFM)+ プレビュー PNG を書き出す。"
        "フォワード + DDGI/SSGI/SSR との比較の基準画像になる。エンジンを固めないよう、1 フレームに使う GPU 時間は"
        "「フレーム予算」までに絞ってある。");
    ImGui::Separator();

    const bool busy = (ui.phase >= 1 && ui.phase <= 3);

    // 開始 / キャンセルと進捗は最上段(窓が短くても必ず見える)
    const float w = ImGui::GetContentRegionAvail().x;
    if (!busy)
    {
        if (ImGui::Button("レンダー開始", ImVec2(w, ui::Px(32)))) ui.requestStart = true;
    }
    else
    {
        if (ImGui::Button("キャンセル", ImVec2(w, ui::Px(32)))) ui.requestCancel = true;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(PhaseLabel(ui.phase));
    if (busy || ui.phase == 4)
    {
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%d / %d spp  (%.0f%%)", ui.samplesDone, ui.samplesTarget, ui.progress * 100.0f);
        ImGui::ProgressBar(std::clamp(ui.progress, 0.0f, 1.0f), ImVec2(-FLT_MIN, ui::Px(20)), overlay);
        if (ui.phase == 2)
            ImGui::Text("経過 %s  残り %s  (%.2f ms/spp)", FormatTime(ui.elapsedSec).c_str(), FormatTime(ui.etaSec).c_str(), ui.msPerSpp);
    }
    if (ui.message[0])
    {
        if (ui.phase == 5) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", ui.message);
        if (ui.phase == 5) ImGui::PopStyleColor();
    }
    if (ui.lastOutput[0] && (ui.phase == 4 || ui.phase == 6))
    {
        ImGui::Spacing();
        ImGui::TextDisabled("出力");
        ImGui::TextWrapped("%s.*", ui.lastOutput);
    }
    ImGui::Separator();

    ImGui::BeginDisabled(busy);

    if (pg::Begin("##ptOut"))
    {
        pg::Group("出力");
        pg::Int("幅 (px)", &ui.width, 8.0f, 16, 16384, nullptr, "出力解像度。ビューポートの大きさには依存しない");
        pg::Int("高さ (px)", &ui.height, 8.0f, 16, 16384);
        pg::Text("画素数", "%.2f Mpx", static_cast<double>(ui.width) * ui.height / 1.0e6);
        pg::InputText("保存先", ui.outputBase, sizeof(ui.outputBase), 0, nullptr,
                      "拡張子なしの基準パス。空 = <プロジェクト>/.dx12/pt/render_<日時>");
        pg::Checkbox("PFM(線形 HDR)", &ui.writePfm, "比較ツール(FLIP 等)の入力。トーンマップ前の値");
        pg::Checkbox("EXR(線形 HDR)", &ui.writeExr, "無圧縮 float32 の EXR");
        pg::Checkbox("PNG(プレビュー)", &ui.writePng, "簡易 ACES + sRGB。比較には使わない");
        pg::SliderFloat("プレビュー露出", &ui.exposure, 0.05f, 16.0f, "%.2f", nullptr, "PNG にだけ掛かる倍率(HDR には掛けない)");
        pg::End();
    }

    if (pg::Begin("##ptQuality"))
    {
        pg::Group("品質");
        pg::Int("サンプル数 (spp)", &ui.spp, 16.0f, 1, 1048576, nullptr,
                "画素あたりの目標サンプル数。誤差は 1/√N で減る。数千 spp は数分かかる");
        pg::SliderInt("バウンス", &ui.bounces, 1, 32, nullptr, "1 = 直接光のみ / N = 最大 N-1 回の間接バウンス");
        pg::Int("シード", &ui.seed, 1.0f, 0, 2147483647, nullptr, "同じシード + 同じ設定 = 同じ結果(決定論)");
        pg::Float("放射輝度クランプ", &ui.maxRadiance, 1.0f, 0.0f, 1.0e6f, "%.1f", nullptr,
                  "ファイアフライ抑制。0 = クランプ無し(地上真値の既定)");
        pg::End();
    }

    if (pg::Begin("##ptCamera"))
    {
        pg::Group("カメラ");
        pg::Checkbox("今の描画カメラ", &ui.useEditorCamera, "OFF にすると下の位置 / 注視点 / FOV で撮る");
        if (!ui.useEditorCamera)
        {
            pg::Float3("位置", ui.cameraPos, 0.05f);
            pg::Float3("注視点", ui.cameraTarget, 0.05f);
            pg::SliderFloat("垂直 FOV (度)", &ui.fovDeg, 5.0f, 120.0f, "%.1f");
        }
        pg::End();
    }

    if (pg::Begin("##ptBudget"))
    {
        pg::Group("負荷");
        pg::SliderFloat("フレーム予算 (ms)", &ui.frameBudgetMs, 1.0f, 60.0f, "%.0f", nullptr,
                        "1 フレームに PT へ使う GPU 時間の上限。小さいほどエディタが軽く、完了は遅い");
        pg::End();
    }
    ImGui::EndDisabled();

    ImGui::End();
}

} // namespace PathTracerPanel
} // namespace dx12e
