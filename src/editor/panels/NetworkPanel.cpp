#include "editor/UiWidgets.h"
#include "editor/PropertyGrid.h"   // フェーズ 1b: 設定窓を pg:: 2 カラムへ
#include "editor/panels/NetworkPanel.h"
#include "network/NetworkSystem.h"
#include "editor/EditorContext.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdint>
#include <string>

namespace dx12e
{
namespace
{

const char* RoleLabel(NetRole role)
{
    switch (role)
    {
    case NetRole::Host:   return "ホスト(リッスンサーバー)";
    case NetRole::Client: return "クライアント";
    default:              return "オフライン";
    }
}

std::string FormatBytes(uint64_t b)
{
    if (b < 1024) return std::to_string(b) + " B";
    if (b < 1024 * 1024) return std::to_string(b / 1024) + " KB";
    return std::to_string(b / (1024 * 1024)) + " MB";
}

} // namespace

void NetworkPanel::RenderStatus(NetworkSystem& net, entt::registry& reg, EditorContext& ctx)
{
    if (!ImGui::Begin("Network", &ctx.showNetworkStatus))
    {
        ImGui::End();
        return;
    }

    const bool connected = net.IsConnected();
    if (connected)
        ImGui::TextColored(theme::Good, "● %s", RoleLabel(net.Role()));
    else
        ImGui::TextColored(theme::TextDim, "○ %s", RoleLabel(net.Role()));

    ImGui::SameLine(0, ui::Px(12.0f));
    ImGui::Text("tick %u", static_cast<unsigned>(net.CurrentTick()));
    ImGui::SameLine(0, ui::Px(12.0f));
    ImGui::Text("複製 %u体", static_cast<unsigned>(net.SyncedEntityCount(reg)));

    ImGui::Separator();

    const auto players = net.Players();
    ImGui::Text("接続 (%d)", static_cast<int>(players.size()));

    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##net_players", 4, tflags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("clientId", ImGuiTableColumnFlags_WidthFixed, ui::Px(70.0f));
        ImGui::TableSetupColumn("RTT(ms)",  ImGuiTableColumnFlags_WidthFixed, ui::Px(80.0f));
        ImGui::TableSetupColumn("送信",     ImGuiTableColumnFlags_WidthFixed, ui::Px(90.0f));
        ImGui::TableSetupColumn("受信",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& p : players)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", static_cast<unsigned>(p.id));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(p.rttMs));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(FormatBytes(p.bytesSent).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(FormatBytes(p.bytesReceived).c_str());
        }
        ImGui::EndTable();
    }

    if (!connected)
        ImGui::TextDisabled("net:host() / net:join() で接続を開始するとここに情報が出ます。");

    ImGui::End();
}

void NetworkPanel::RenderSettings(NetworkSystem& net, EditorContext& ctx, const std::string& assetsDir)
{
    if (!m_loaded)
    {
        m_staging = net.Config();
        m_loaded = true;
    }

    if (!ImGui::Begin("Network 設定", &ctx.showNetworkSettings))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("保存すると assets/network.json に書き込まれます(次回起動時にも反映)。");
    ImGui::Separator();

    // ★tickRate は保存・表示・dx12_net_status に出るだけで、src/network/ の中に
    //   読み手が 1 つも無い。シム更新は PhysicsSystem.h の kFixedTimeStep = 1/60 で
    //   ハードコード固定。動かせるつまみに見えるのが害なので、serverAuthority と同じく
    //   disabled + 理由のツールチップにする（値そのものは network.json に残す）。
    // ★フェーズ 1b: インスペクタと同じ pg:: の 2 カラム（値の範囲・書き込みの規則は従来どおり）
    if (pg::Begin("##netSettings"))
    {
        ImGui::BeginDisabled(true);
        int tickRate = static_cast<int>(m_staging.tickRate);
        pg::Int("シム更新Hz TickRate", &tickRate, 1.0f, 1, 240, nullptr,
                "未実装。シム更新は 60Hz 固定（PhysicsSystem の kFixedTimeStep）。\n"
                "ここを変えても何も起きないので触れないようにしてある。\n"
                "帯域を変えたいなら下の SnapshotRate。");
        ImGui::EndDisabled();

        int snapshotRate = static_cast<int>(m_staging.snapshotRate);
        if (pg::Int("送信Hz SnapshotRate", &snapshotRate, 1.0f, 1, 120, nullptr,
                    "サーバーが複製Transformを送信する頻度。高いほど滑らかだが帯域を食う"))
            m_staging.snapshotRate = static_cast<u32>(std::max(1, snapshotRate));

        int maxPlayers = static_cast<int>(m_staging.maxPlayers);
        if (pg::Int("最大接続数 MaxPlayers", &maxPlayers, 1.0f, 1, 64))
            m_staging.maxPlayers = static_cast<u32>(std::max(1, maxPlayers));

        int port = static_cast<int>(m_staging.defaultPort);
        if (pg::Int("既定ポート DefaultPort", &port, 1.0f, 1024, 65535, nullptr,
                    "net:host()/net:join() で省略した時に使われるポート番号(MCPの8787番台とは別)"))
            m_staging.defaultPort = static_cast<u16>(std::clamp(port, 1, 65535));
        pg::End();
    }

    ImGui::Separator();
    if (net.IsConnected())
        ImGui::TextColored(theme::Warn,
                           "接続中: 変更は保存しても現在のセッションには反映されません(次回接続から有効)。");

    if (ImGui::Button("保存", ui::Px(120.0f, 0.0f)))
    {
        net.SetConfig(m_staging);
        m_staging.Save(assetsDir + "network.json");
    }
    ImGui::SameLine();
    if (ImGui::Button("既定値に戻す", ui::Px(120.0f, 0.0f)))
        m_staging = NetworkConfig{};

    ImGui::End();
}

} // namespace dx12e
