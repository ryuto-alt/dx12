#include "editor/panels/AudioMixerPanel.h"

#include "audio/AudioSystem.h"
#include "editor/EditorContext.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dx12e
{

namespace
{
constexpr float kMeterFloorDb = -60.0f;   // メーターの下端（これ以下は空）

float DbToMeter01(float db)
{
    return std::clamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
}

ImU32 MeterColor(float db)
{
    if (db > -3.0f)  return IM_COL32(235, 80, 70, 255);    // 赤: 割れる手前
    if (db > -12.0f) return IM_COL32(235, 200, 70, 255);   // 黄
    return IM_COL32(90, 200, 120, 255);                    // 緑
}

// 縦メーター 1 本（RMS の帯 + ピークの線 + -12/-24/-48 の目盛り）
void DrawMeter(const char* id, float rmsDb, float peakDb, ImVec2 size)
{
    ImGui::InvisibleButton(id, size);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(24, 26, 30, 255), 2.0f);
    const float h = b.y - a.y;
    const float rmsY = b.y - h * DbToMeter01(rmsDb);
    if (rmsY < b.y - 0.5f)
        dl->AddRectFilled(ImVec2(a.x + 2, rmsY), ImVec2(b.x - 2, b.y - 1), MeterColor(rmsDb), 1.0f);
    if (peakDb > kMeterFloorDb)
    {
        const float py = b.y - h * DbToMeter01(peakDb);
        dl->AddLine(ImVec2(a.x + 1, py), ImVec2(b.x - 1, py), MeterColor(peakDb), 2.0f);
    }
    for (float t : {-12.0f, -24.0f, -48.0f})
    {
        const float ty = b.y - h * DbToMeter01(t);
        dl->AddLine(ImVec2(a.x, ty), ImVec2(a.x + 4, ty), IM_COL32(255, 255, 255, 60));
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 30), 2.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("RMS %.1f dBFS / ピーク %.1f dBFS（-120 = 無音）", rmsDb, peakDb);
}

void RenderBusStrips(AudioSystem& a)
{
    const auto buses = a.GetBuses();
    const float stripW = 64.0f;
    const float meterH = 150.0f;
    ImGui::BeginChild("##strips", ImVec2(0, meterH + 118.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (size_t i = 0; i < buses.size(); ++i)
    {
        const auto& b = buses[i];
        if (i > 0) ImGui::SameLine(0.0f, 6.0f);
        ImGui::BeginGroup();
        ImGui::PushID(b.name.c_str());

        // 名前（子バスは親を小さく添える）
        ImGui::TextUnformatted(b.name.c_str());
        if (!b.parent.empty() && b.parent != "master")
            ImGui::TextDisabled("< %s", b.parent.c_str());
        else
            ImGui::TextDisabled("%s", b.reverbReturn ? "(戻り)" : " ");

        // メーターとフェーダーを横に並べる
        DrawMeter("##meter", b.rmsDb, b.peakDb, ImVec2(16.0f, meterH));
        ImGui::SameLine(0.0f, 4.0f);
        float vol = b.volume;
        if (ImGui::VSliderFloat("##fader", ImVec2(22.0f, meterH), &vol, 0.0f, 2.0f, ""))
            a.SetBusVolume(b.name, vol);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("音量 %.2f（%.1f dB）\nスナップショット補正 ×%.2f / 実効 ×%.2f",
                              b.volume, audio::LinearToDb(b.volume), b.snapshotGain, b.effectiveGain);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) a.SetBusVolume(b.name, 1.0f);   // 右クリックで 0dB

        ImGui::Text("%+.1f", std::max(audio::LinearToDb(b.effectiveGain), -99.9f));

        // ミュート
        bool muted = b.muted;
        if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.2f, 1.0f));
        if (ImGui::Button("M", ImVec2(stripW * 0.45f, 0))) a.SetBusMute(b.name, !muted);
        if (muted) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ミュート");

        // ローパス（0 = 無し）
        float lp = b.lowpassHz;
        ImGui::SetNextItemWidth(stripW);
        if (ImGui::DragFloat("##lp", &lp, 20.0f, 0.0f, 20000.0f, lp > 0.0f ? "%.0fHz" : "LP 無し"))
            a.SetBusLowpass(b.name, lp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ローパス（Hz、0 = 無し）。スナップショットの分: %.0f Hz\n"
                              "出力のサンプルレート/6（48kHz なら 8kHz）より上は掛からない",
                              b.snapshotLowpass);
        ImGui::TextDisabled("%u/%u", b.realVoices, b.realVoices + b.virtualVoices);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("鳴っている実ボイス / 論理ボイス（仮想含む）。上限 %s",
                              b.voiceLimit ? std::to_string(b.voiceLimit).c_str() : "なし");

        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::EndChild();
}

void RenderSnapshots(AudioSystem& a)
{
    static int   s_sel = 0;
    static float s_sec = 0.5f;
    const auto st = a.GetSnapshotState();
    if (s_sel >= static_cast<int>(st.defined.size())) s_sel = 0;
    std::vector<const char*> names;
    for (const auto& n : st.defined) names.push_back(n.c_str());
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("##snap", &s_sel, names.data(), static_cast<int>(names.size()));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("秒", &s_sec, 0.05f, 0.0f, 10.0f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("切り替え") && !names.empty()) a.SetSnapshot(names[static_cast<size_t>(s_sel)], s_sec);
    ImGui::Text("現在: %s → %s", st.current.c_str(), st.target.c_str());
    ImGui::SameLine();
    ImGui::ProgressBar(st.progress, ImVec2(120.0f, 0.0f));
    if (st.defined.size() <= 1)
        ImGui::TextDisabled("スナップショットは Lua の audio:defineSnapshot で定義する（Play 中に出る）");
}

void RenderReverb(AudioSystem& a)
{
    const auto rv = a.GetReverbState();
    if (!rv.available)
    {
        ImGui::TextDisabled("リバーブ無し（音声デバイスが無い / 作成失敗）");
        return;
    }
    ImGui::Text("ゾーン: %s", rv.dominant.empty() ? "（外 = 既定）" : rv.dominant.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("既定 %s / wet %.2f", rv.defaultPreset.c_str(), rv.defaultWet);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "wet %.2f → %.2f", rv.currentWet, rv.targetWet);
    ImGui::ProgressBar(rv.currentWet, ImVec2(200.0f, 0.0f), buf);
    ImGui::SameLine();
    ImGui::Text("残響 %.2f 秒", rv.current.decayTime);
    for (const auto& z : rv.zones)
        ImGui::BulletText("%s  %s  重み %.2f  wet %.2f  優先度 %d", z.name.c_str(), z.preset.c_str(),
                          z.weight, z.wet, z.priority);
}

void RenderVoices(AudioSystem& a)
{
    const auto voices = a.GetVoices();
    if (voices.empty())
    {
        ImGui::TextDisabled("鳴っている音はありません");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##voices", 8, flags, ImVec2(0, 220.0f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("音", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("バス");
    ImGui::TableSetupColumn("優先");
    ImGui::TableSetupColumn("dB");
    ImGui::TableSetupColumn("距離");
    ImGui::TableSetupColumn("遮蔽");
    ImGui::TableSetupColumn("状態");
    ImGui::TableSetupColumn("位置");
    ImGui::TableHeadersRow();
    for (const auto& v : voices)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const size_t slash = v.path.find_last_of("/\\");
        ImGui::TextUnformatted(slash == std::string::npos ? v.path.c_str() : v.path.c_str() + slash + 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s（id %d）", v.path.c_str(), v.id);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(v.bus.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", v.priority);
        ImGui::TableNextColumn(); ImGui::Text("%.1f", std::max(audio::LinearToDb(v.audibility), -99.9f));
        ImGui::TableNextColumn();
        if (v.spatial) ImGui::Text("%.1fm", v.distance); else ImGui::TextDisabled("2D");
        ImGui::TableNextColumn();
        if (v.spatial) ImGui::Text("%.2f", v.occlusion); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (v.isVirtual)
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1.0f), "仮想(%s)", v.virtualReason.c_str());
        else
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s%s%s", v.bgm ? "BGM " : "",
                               v.stream ? "ストリーム" : "実", v.paused ? " 停止中" : "");
        ImGui::TableNextColumn();
        ImGui::Text("%.1f/%.1f%s", v.positionSec, v.lengthSec, v.loop ? " ∞" : "");
    }
    ImGui::EndTable();
}
} // namespace

void RenderAudioMixerPanel(AudioSystem* audio, EditorContext& ctx)
{
    if (!ctx.showAudioMixer) return;

    ImGui::SetNextWindowSize(ImVec2(620.0f, 720.0f), ImGuiCond_FirstUseEver);
    // ### 付きの固定 ID（UI 自動テストが窓を引くのに使う。表示名を変えても壊れない）
    if (!ImGui::Begin("オーディオミキサー###AudioMixerFloating", &ctx.showAudioMixer,
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    if (!audio)
    {
        ImGui::TextDisabled("AudioSystem がありません");
        ImGui::End();
        return;
    }
    AudioSystem& a = *audio;

    u32 real = 0, virt = 0;
    a.GetVoiceCounts(real, virt);
    if (a.IsDeviceReady())
        ImGui::Text("出力 %u ch / %u Hz", a.GetOutputChannels(), a.GetOutputSampleRate());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "音声デバイス無し（%s）。状態だけ追っています",
                           a.GetDeviceStatus().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  ボイス 実 %u / 仮想 %u（上限 %u）", real, virt, a.GetMaxVoices());
    const auto ss = a.GetStreamStats();
    if (ss.count > 0)
        ImGui::TextDisabled("ストリーム %u 本 / %.1f MB / 途切れ %llu / デコード %.2f ms/秒",
                            ss.count, static_cast<double>(ss.memoryBytes) / (1024.0 * 1024.0),
                            static_cast<unsigned long long>(ss.underruns),
                            ss.audioSecondsDecoded > 0.0 ? ss.decodeMs / ss.audioSecondsDecoded : 0.0);
    ImGui::TextDisabled("フェーダーの操作はシーンに保存されません（Play 中は Lua の setBusVolume と同じ値）");

    if (ImGui::CollapsingHeader("バス", ImGuiTreeNodeFlags_DefaultOpen)) RenderBusStrips(a);
    if (ImGui::CollapsingHeader("スナップショット", ImGuiTreeNodeFlags_DefaultOpen)) RenderSnapshots(a);
    if (ImGui::CollapsingHeader("リバーブ", ImGuiTreeNodeFlags_DefaultOpen)) RenderReverb(a);
    if (ImGui::CollapsingHeader("鳴っている音", ImGuiTreeNodeFlags_DefaultOpen)) RenderVoices(a);

    ImGui::End();
}

} // namespace dx12e
