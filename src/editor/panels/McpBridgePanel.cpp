#include "editor/panels/McpBridgePanel.h"
#include "core/mcp/McpBridge.h"
#include "editor/EditorContext.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

// エンジン repo の tools/mcp-server の絶対パス（CMake が定義）。
// game プロジェクトへ再ポイントされる AssetsDir と違い、常にエンジン本体を指す。
#ifndef MCP_SERVER_DIR
#define MCP_SERVER_DIR ""   // 未定義ビルドでは空（その場合は手動設定にフォールバック）
#endif

namespace dx12e
{

// index.ts の絶対パス。CMake のパスは '/' 区切りなので Windows でも Node がそのまま解釈する。
static std::string IndexPath()
{
    return std::string(MCP_SERVER_DIR) + "/index.ts";
}

// Claude Code 用ワンライナー。これをターミナルに貼るだけで繋がる（パス手入力ゼロ）。
static std::string ClaudeCommand()
{
    return "claude mcp add dx12-engine -- node \"" + IndexPath() + "\"";
}

// Codex / 手動用 .mcp.json。args に絶対パスを埋める（${PROJECT_ROOT} の置換は不要）。
static std::string McpJson()
{
    return
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"dx12-engine\": {\n"
        "      \"command\": \"node\",\n"
        "      \"args\": [\"" + IndexPath() + "\"]\n"
        "    }\n"
        "  }\n"
        "}\n";
}

void McpBridgePanel::Render(McpBridge& bridge, EditorContext& ctx)
{
    // タイトルバーの×で showMcpBridge を倒せるよう p_open を渡す（他ツール窓と同じトグル）。
    if (!ImGui::Begin("MCP / AI Bridge", &ctx.showMcpBridge))
    {
        ImGui::End();
        return;
    }

    // ---- 接続インジケータ ----
    const uint16_t port = bridge.Port();
    if (bridge.IsConnected())
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "● 接続中");
    else
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "○ 未接続");
    ImGui::SameLine(0, 12);
    if (port != 0)
        ImGui::Text("待受 127.0.0.1:%u", static_cast<unsigned>(port));
    else
        ImGui::TextDisabled("待受 (未起動)");

    ImGui::Separator();

    // ---- かんたん接続（Claude Code）: コマンド1本コピー → 貼る → 再起動 ----
    ImGui::TextWrapped("AI(Claude Code)から、このエディタを操作してゲームを作れます。");
    ImGui::Spacing();
    ImGui::TextDisabled("① 下の「コマンドをコピー」を押す");
    ImGui::TextDisabled("② ターミナル(PowerShell 等)に貼って実行");
    ImGui::TextDisabled("③ Claude Code を再起動 → 繋がります");
    ImGui::Spacing();

    // コピーされる中身を見せておく（読み取り専用・選択可）。InputText は可変バッファが要るので static。
    static std::string cmd = ClaudeCommand();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##mcp_cmd", cmd.data(), cmd.size() + 1, ImGuiInputTextFlags_ReadOnly);

    // 主役ボタン: ワンライナーを丸ごとクリップボードへ。
    if (ImGui::Button("コマンドをコピー", ImVec2(-FLT_MIN, 0)))
        ImGui::SetClipboardText(cmd.c_str());

    ImGui::Spacing();

    // ---- 別の接続方法（Codex / 手動）。普段は折りたたみ ----
    if (ImGui::CollapsingHeader("別の接続方法 (Codex / .mcp.json)"))
    {
        ImGui::TextDisabled(".mcp.json に貼るか、Codex の設定に同じ command/args を書く。");
        static std::string js = McpJson();
        ImGui::InputTextMultiline("##mcp_json", js.data(), js.size() + 1,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f),
                                  ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(".mcp.json をコピー"))
            ImGui::SetClipboardText(js.c_str());
    }

    ImGui::Separator();

    // ---- 直近コマンド履歴（新しい順・最大 64 件）----
    const std::vector<McpBridge::CommandLogEntry> history = bridge.RecentCommands();
    ImGui::Text("直近コマンド (%d)", static_cast<int>(history.size()));

    const ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    // ScrollY を効かせるため残り高さを明示的に与える（窓のリサイズに追従）。
    if (ImGui::BeginTable("##mcp_history", 3, tflags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("結果", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("メソッド", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("エラー", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // history は古い順に蓄積されるので逆順で「新しい順」に表示する。
        for (auto it = history.rbegin(); it != history.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (it->ok)
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "✓");
            else
                ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f), "✗");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(it->method.empty() ? "(?)" : it->method.c_str());

            ImGui::TableSetColumnIndex(2);
            if (!it->ok && !it->error.empty())
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.5f, 1.0f), "%s", it->error.c_str());
            else
                ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace dx12e
