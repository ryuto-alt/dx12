#include "editor/UiWidgets.h"
#include "editor/panels/HardwarePanel.h"

#include "editor/EditorContext.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace dx12e
{
namespace HardwarePanel
{
namespace
{

struct Board { const char* label; const char* fqbn; };
constexpr Board kBoards[] = {
    {"ESP32（esp32:esp32:esp32）",            "esp32:esp32:esp32"},
    {"Arduino Uno（arduino:avr:uno）",        "arduino:avr:uno"},
    {"Arduino Nano（旧ブートローダ）",        "arduino:avr:nano:cpu=atmega328old"},
};
constexpr int kBoardCount = static_cast<int>(sizeof(kBoards) / sizeof(kBoards[0]));

const ImVec4 kOk   (0.45f, 0.85f, 0.50f, 1.0f);
const ImVec4 kWarn (1.00f, 0.80f, 0.35f, 1.0f);
const ImVec4 kErr  (1.00f, 0.45f, 0.40f, 1.0f);

bool Contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// 推定ボード名 → 表の添字（分からなければ -1）
int BoardFromGuess(const std::string& g)
{
    if (g.find("ESP32") != std::string::npos || g.find("Espressif") != std::string::npos || g.find("CP210") != std::string::npos) return 0;
    if (g.find("Arduino") != std::string::npos) return 1;
    return -1;
}

// 文字列の候補から選ぶコンボ。選んだら true
bool StrCombo(const char* label, const std::vector<std::string>& items, std::string& sel, const char* emptyLabel)
{
    bool changed = false;
    const char* preview = sel.empty() ? emptyLabel : sel.c_str();
    if (ImGui::BeginCombo(label, preview))
    {
        for (const std::string& it : items)
            if (ImGui::Selectable(it.c_str(), it == sel)) { sel = it; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

const char* StatusLabel(const std::string& s)
{
    if (s == "ready") return "接続中";
    if (s == "opening") return "接続しています";
    if (s == "waiting_hello") return "応答待ち（書き込み済みか確認）";
    if (s == "error") return "エラー";
    return "未接続";
}

ImVec4 StatusColor(const std::string& s)
{
    if (s == "ready") return kOk;
    if (s == "error") return kErr;
    if (s == "opening" || s == "waiting_hello") return kWarn;
    return ImVec4(0.65f, 0.65f, 0.68f, 1.0f);
}

const char* FlashLabel(const std::string& s)
{
    if (s == "installing_core") return "ボードのコアを入れています（初回だけ数分）";
    if (s == "compiling") return "コンパイル中";
    if (s == "uploading") return "ボードへ書き込み中";
    if (s == "reconnecting") return "再接続中";
    if (s == "done") return "完了";
    if (s == "failed") return "失敗";
    return "待機中";
}

void LogBox(const char* id, const std::vector<std::string>& lines, float heightLogical)
{
    if (ImGui::BeginChild(id, ui::Px(0.0f, heightLogical), true))
    {
        for (const std::string& l : lines) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

} // namespace

void Render(EditorContext& ctx)
{
    if (!ctx.showHardware) return;
    hwui::UiState& hu = ctx.hwUi;

    ImGui::SetNextWindowSize(ui::Px(480, 640), ImGuiCond_FirstUseEver);
    // ### 付きの固定 ID（UI 自動テストが窓を引くのに使う）
    if (!ImGui::Begin("ハードウェア###HardwareFloating", &ctx.showHardware))
    {
        ImGui::End();
        return;
    }

    if (!hu.promptMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("%s", hu.promptMessage.c_str());
        ImGui::PopStyleColor();
        if (!hu.promptPort.empty()) ImGui::TextDisabled("ポート %s。下の「ボードに書き込む」で書き込めます（自動では書きません）", hu.promptPort.c_str());
        ImGui::Separator();
    }

    // ---- 1. 書き込み道具 ----
    ImGui::SeparatorText("書き込み道具");
    if (hu.tcReady)
    {
        ImGui::TextColored(kOk, "準備できています");
        if (!hu.tcCli.empty()) ImGui::TextDisabled("%s", hu.tcCli.c_str());
    }
    else if (hu.tcState == "downloading")
    {
        ImGui::TextUnformatted("arduino-cli をダウンロードしています");
        char ov[32];
        std::snprintf(ov, sizeof(ov), "%.0f%%", hu.tcProgress * 100.0f);
        ImGui::ProgressBar(std::clamp(hu.tcProgress, 0.0f, 1.0f), ImVec2(-FLT_MIN, ui::Px(18)), ov);
    }
    else if (hu.tcState == "installing_cores")
    {
        ImGui::TextUnformatted("ボードのコアを入れています（初回だけ数分）");
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, ui::Px(18)));
    }
    else if (hu.tcState == "failed")
    {
        ImGui::TextColored(kErr, "準備できませんでした");
        if (!hu.tcError.empty()) ImGui::TextWrapped("%s", hu.tcError.c_str());
    }
    else
    {
        ImGui::TextColored(kWarn, "まだ準備していません（書き込みの前に 1 回だけ要ります）");
    }
    if (!hu.tcReady && !hu.tcRunning)
    {
        if (ImGui::Button(hu.tcState == "failed" ? "やり直す" : "準備する")) hu.requestToolchainInstall = true;
        ImGui::SameLine();
        ImGui::TextDisabled("arduino-cli と ESP32 / Arduino のコアを自動で入れます");
    }
    if (!hu.tcLog.empty() && ImGui::CollapsingHeader("道具のログ"))
        LogBox("##tcLog", hu.tcLog, 140.0f);

    // ---- 2. デバイス ----
    ImGui::SeparatorText("デバイス");
    if (!hu.projectOpen) ImGui::TextDisabled("プロジェクトを開くと表示されます");
    else if (hu.devices.empty()) ImGui::TextDisabled("このプロジェクトにはハードウェアのデバイスがありません（assets/hardware.json）");
    for (const hwui::DeviceRow& d : hu.devices)
    {
        ImGui::PushID(d.name.c_str());
        ImGui::TextUnformatted(d.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(StatusColor(d.status), "%s", StatusLabel(d.status));
        std::string line;
        if (!d.port.empty()) line += d.port;
        if (!d.board.empty()) line += (line.empty() ? "" : "  /  ") + d.board;
        if (d.isVirtual) line += (line.empty() ? "" : "  ") + std::string("（仮想）");
        if (!line.empty()) ImGui::TextDisabled("%s", line.c_str());
        if (!d.lastError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kErr);
            ImGui::TextWrapped("%s", d.lastError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    // ---- 3. ボードに書き込む ----
    ImGui::SeparatorText("ボードに書き込む");

    // 選択の既定値
    if (hu.selSketch.empty() || !Contains(hu.sketches, hu.selSketch))
        hu.selSketch = hu.sketches.empty() ? std::string() : hu.sketches.front();

    std::vector<std::string> portNames;
    std::vector<std::string> candidates;   // 既知 VID で他のデバイスが使っていないポート
    for (const hwui::PortRow& p : hu.ports)
    {
        portNames.push_back(p.port);
        if (p.knownVid && p.usedBy.empty()) candidates.push_back(p.port);
    }
    if (hu.selPort.empty() || !Contains(portNames, hu.selPort))
        hu.selPort = candidates.size() == 1 ? candidates.front() : std::string();

    std::vector<std::string> serialDevs;
    for (const hwui::DeviceRow& d : hu.devices) if (d.serial) serialDevs.push_back(d.name);
    if (!hu.selDevice.empty() && !Contains(serialDevs, hu.selDevice)) hu.selDevice.clear();
    if (hu.selDevice.empty() && !serialDevs.empty())
    {
        for (const hwui::DeviceRow& d : hu.devices)
            if (d.serial && !hu.selPort.empty() && d.port == hu.selPort) { hu.selDevice = d.name; break; }
        if (hu.selDevice.empty()) hu.selDevice = serialDevs.front();
    }

    if (hu.selPort != hu.lastPortForBoard)
    {
        hu.lastPortForBoard = hu.selPort;
        hu.boardTouched = false;
    }
    if (!hu.boardTouched && !hu.selPort.empty())
    {
        for (const hwui::PortRow& p : hu.ports)
            if (p.port == hu.selPort) { const int b = BoardFromGuess(p.guessedBoard); if (b >= 0) hu.selBoard = b; break; }
    }
    hu.selBoard = std::clamp(hu.selBoard, 0, kBoardCount - 1);

    const float labelW = ui::Px(88);
    const float fieldW = ImGui::GetContentRegionAvail().x - labelW;

    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("スケッチ"); ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fieldW);
    if (hu.sketches.empty()) ImGui::TextDisabled("<プロジェクト>/firmware/ にスケッチがありません");
    else StrCombo("##sketch", hu.sketches, hu.selSketch, "");

    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ボード"); ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::BeginCombo("##board", kBoards[hu.selBoard].label))
    {
        for (int i = 0; i < kBoardCount; ++i)
            if (ImGui::Selectable(kBoards[i].label, i == hu.selBoard)) { hu.selBoard = i; hu.boardTouched = true; }
        ImGui::EndCombo();
    }

    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ポート"); ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fieldW);
    {
        std::string preview = hu.selPort.empty() ? std::string("（選んでください）") : hu.selPort;
        if (ImGui::BeginCombo("##port", preview.c_str()))
        {
            for (const hwui::PortRow& p : hu.ports)
            {
                std::string label = p.port;
                if (!p.friendlyName.empty()) label += "  " + p.friendlyName;
                if (p.knownVid) label += "  ★";
                if (!p.usedBy.empty()) label += "  （" + p.usedBy + " が使用中）";
                if (ImGui::Selectable(label.c_str(), p.port == hu.selPort)) hu.selPort = p.port;
            }
            if (hu.ports.empty()) ImGui::TextDisabled("COM ポートが見つかりません（USB をつないでください）");
            ImGui::EndCombo();
        }
    }

    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("デバイス"); ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fieldW);
    {
        const char* none = "（指定しない：ポートだけ）";
        if (ImGui::BeginCombo("##device", hu.selDevice.empty() ? none : hu.selDevice.c_str()))
        {
            if (ImGui::Selectable(none, hu.selDevice.empty())) hu.selDevice.clear();
            for (const std::string& n : serialDevs)
                if (ImGui::Selectable(n.c_str(), n == hu.selDevice)) hu.selDevice = n;
            ImGui::EndCombo();
        }
    }
    ImGui::TextDisabled("★ = Arduino / ESP32 でよくある USB チップのポート");

    ImGui::Spacing();
    const bool canFlash = hu.tcReady && !hu.flashRunning && !hu.selSketch.empty() && !hu.selPort.empty();
    ImGui::BeginDisabled(!canFlash);
    if (ImGui::Button("ボードに書き込む", ImVec2(-FLT_MIN, ui::Px(40))))
    {
        hu.reqSketch = "firmware/" + hu.selSketch;
        hu.reqDevice = hu.selDevice;
        hu.reqPort = hu.selPort;
        hu.reqFqbn = kBoards[hu.selBoard].fqbn;
        hu.requestFlash = true;
        hu.requestError.clear();
    }
    ImGui::EndDisabled();
    if (!canFlash)
    {
        const char* why = hu.flashRunning ? "書き込み中です" :
                          !hu.tcReady ? "先に「書き込み道具」を準備してください" :
                          hu.selSketch.empty() ? "スケッチがありません" : "ポートを選んでください";
        ImGui::TextDisabled("%s", why);
    }
    if (!hu.requestError.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kErr);
        ImGui::TextWrapped("%s", hu.requestError.c_str());
        ImGui::PopStyleColor();
    }
    if (hu.selBoard == 0) ImGui::TextDisabled("書き込みが始まらないときは基板の BOOT ボタンを押したまま");

    // 書き込みの状態
    if (hu.flashState != "idle")
    {
        ImGui::Spacing();
        const bool failed = hu.flashState == "failed";
        const bool done = hu.flashState == "done";
        ImGui::PushStyleColor(ImGuiCol_Text, failed ? kErr : done ? kOk : kWarn);
        ImGui::Text("%s（%.0f 秒）", FlashLabel(hu.flashState), hu.flashElapsedSec);
        ImGui::PopStyleColor();
        if (hu.flashRunning) ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, ui::Px(14)));
        if (!hu.flashPort.empty()) ImGui::TextDisabled("%s %s", hu.flashPort.c_str(), hu.flashDevice.c_str());
        if (failed && !hu.flashError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kErr);
            ImGui::TextWrapped("%s", hu.flashError.c_str());
            ImGui::PopStyleColor();
        }
        if (!hu.flashLog.empty() && ImGui::CollapsingHeader("書き込みのログ（末尾）"))
            LogBox("##flashLog", hu.flashLog, 180.0f);
    }

    ImGui::End();
}

} // namespace HardwarePanel
} // namespace dx12e
