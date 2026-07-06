#include "editor/panels/NetworkPanel.h"
#include "network/NetworkSystem.h"
#include "editor/EditorContext.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

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

void NetworkPanel::Render(NetworkSystem& net, entt::registry& reg, EditorContext& ctx)
{
    if (!ImGui::Begin("Network", &ctx.showNetworkStatus))
    {
        ImGui::End();
        return;
    }

    const bool connected = net.IsConnected();
    if (connected)
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "● %s", RoleLabel(net.Role()));
    else
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "○ %s", RoleLabel(net.Role()));

    ImGui::SameLine(0, 12);
    ImGui::Text("tick %u", static_cast<unsigned>(net.CurrentTick()));
    ImGui::SameLine(0, 12);
    ImGui::Text("複製 %u体", static_cast<unsigned>(net.SyncedEntityCount(reg)));

    ImGui::Separator();

    const auto players = net.Players();
    ImGui::Text("接続 (%d)", static_cast<int>(players.size()));

    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##net_players", 4, tflags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("clientId", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("RTT(ms)",  ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("送信",     ImGuiTableColumnFlags_WidthFixed, 90.0f);
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

} // namespace dx12e
