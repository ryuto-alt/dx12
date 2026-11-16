#include "editor/UiWidgets.h"
#include "editor/panels/McpBridgePanel.h"
#include "core/mcp/McpBridge.h"
#include "editor/EditorContext.h"
#include "editor/McpInstaller.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <Windows.h>

#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// ビルド時に CMake が焼き込むエンジン repo の絶対パス（最終フォールバック専用）。
// ⚠ これは「ビルドしたマシンの」パス。配布/別PC/フォルダ移動で無効になるため、
//   通常は下の ResolveMcpServerDir() が exe を基準に実行時解決する方を使う。
#ifndef MCP_SERVER_DIR
#define MCP_SERVER_DIR ""   // 未定義ビルドでは空
#endif

namespace dx12e
{
namespace
{
namespace fs = std::filesystem;

// MCP サーバの場所（エンジンが入れたもの / 手で clone したもの / ソースのビルド）。毎フレームのディスク走査を
// 避けるため、導入ボタンの結果が変わった時（Generation）と最初の 1 回だけ引き直す。
std::string ServerDirCached(unsigned generation, bool running)
{
    static std::string dir;
    static unsigned lastGen = ~0u;
    static bool lastRunning = true;
    static bool first = true;
    if (first || lastGen != generation || lastRunning != running)
    {
        dir = mcpinst::ResolveServerDir(MCP_SERVER_DIR);
        first = false;
        lastGen = generation;
        lastRunning = running;
    }
    return dir;
}

const char* StatusMark(mcpinst::ClientResult::Status st)
{
    using S = mcpinst::ClientResult::Status;
    switch (st)
    {
    case S::Written:      return "✓ 登録しました";
    case S::Unchanged:    return "✓ 登録済み";
    case S::NotInstalled: return "- 見つかりません";
    default:              return "✗ 失敗";
    }
}

} // namespace

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
    {
        if (bridge.ClientCount() > 1) ImGui::TextColored(theme::Good, "● 接続中（%d 本）", bridge.ClientCount());
        else                          ImGui::TextColored(theme::Good, "● 接続中");
    }
    else
        ImGui::TextColored(theme::TextDim, "○ 未接続");
    ImGui::SameLine(0, ui::Px(12.0f));
    if (port != 0)
        ImGui::Text("待受 127.0.0.1:%u", static_cast<unsigned>(port));
    else
        ImGui::TextDisabled("待受 (未起動)");

    ImGui::Separator();

    // ---- ワンクリック導入 ----
    // Node.js（無ければ自動で入れる）+ MCP サーバ + 依存を用意し、Codex / Claude Code / Claude Desktop / Cursor の
    // 設定へ登録する。コマンドを貼る必要は無い。
    static mcpinst::McpInstaller installer;
    const mcpinst::McpInstaller::Snapshot snap = installer.Get();
    const std::string serverDir = ServerDirCached(installer.Generation(), snap.running);
    const bool serverFound = !serverDir.empty();

    ImGui::TextWrapped("Codex / Claude Code などの AI から、このエディタを操作してゲームを作れます。"
                       "下のボタン 1 回で、必要なものの用意と AI ツールへの登録が終わります。");
    ImGui::Spacing();

    if (snap.running)
    {
        ImGui::BeginDisabled();
        ImGui::Button("導入中...", ImVec2(-FLT_MIN, 0));
        ImGui::EndDisabled();
        ImGui::TextUnformatted(mcpinst::McpInstaller::StepLabel(snap.step));
        if (snap.progress > 0.0f && snap.progress < 1.0f)
            ImGui::ProgressBar(snap.progress, ImVec2(-FLT_MIN, 0));
    }
    else
    {
        const char* label = serverFound ? "AI ツールへ再登録 / 更新する" : "ワンクリックで MCP をインストール";
        if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)))
            installer.Start(MCP_SERVER_DIR);
        if (snap.step == mcpinst::McpInstaller::Step::Idle)
        {
            if (serverFound) ImGui::TextDisabled("MCP サーバ: %s", serverDir.c_str());
            else ImGui::TextDisabled("未インストールです。ボタンを押すと Node.js と MCP サーバをダウンロードします（約 100MB）。");
        }
    }

    if (snap.step == mcpinst::McpInstaller::Step::Failed)
        ImGui::TextColored(theme::Bad, "失敗: %s", snap.error.c_str());
    else if (snap.step == mcpinst::McpInstaller::Step::Done)
        ImGui::TextColored(theme::Good, "完了。Codex / Claude などを再起動すると dx12_* ツールが使えます。");

    if (!snap.clients.empty())
    {
        for (const auto& r : snap.clients)
        {
            ImGui::BulletText("%s  %s", r.client.c_str(), StatusMark(r.status));
            if (r.status != mcpinst::ClientResult::Status::NotInstalled && !r.path.empty())
                ImGui::TextDisabled("    %s", r.path.c_str());
            if (!r.note.empty()) ImGui::TextDisabled("    %s", r.note.c_str());
        }
        ImGui::TextDisabled("既存の設定は消しません（dx12-engine の項目だけ差し替え、元のファイルは .bak に退避）。");
    }

    if (!snap.log.empty() && ui::CollapsingHeader("導入ログ"))
    {
        std::string joined;
        for (const auto& l : snap.log) { joined += l; joined += '\n'; }
        ImGui::InputTextMultiline("##mcp_install_log", joined.data(), joined.size() + 1,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f),
                                  ImGuiInputTextFlags_ReadOnly);
    }

    if (serverFound && ui::CollapsingHeader("手動で登録する (.mcp.json など)"))
    {
        const std::string index = serverDir + "/index.ts";
        std::string js =
            "{\n  \"mcpServers\": {\n    \"dx12-engine\": {\n      \"command\": \"node\",\n"
            "      \"args\": [\"" + index + "\"]\n    }\n  }\n}\n";
        ImGui::TextDisabled("Codex は ~/.codex/config.toml に [mcp_servers.dx12-engine] で同じ command / args を書く。");
        ImGui::InputTextMultiline("##mcp_json", js.data(), js.size() + 1,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f),
                                  ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(".mcp.json をコピー")) ImGui::SetClipboardText(js.c_str());
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
        ImGui::TableSetupColumn("結果", ImGuiTableColumnFlags_WidthFixed, ui::Px(44.0f));
        ImGui::TableSetupColumn("メソッド", ImGuiTableColumnFlags_WidthFixed, ui::Px(160.0f));
        ImGui::TableSetupColumn("エラー", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // history は古い順に蓄積されるので逆順で「新しい順」に表示する。
        for (auto it = history.rbegin(); it != history.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (it->ok)
                ImGui::TextColored(theme::Good, "✓");
            else
                ImGui::TextColored(theme::Bad, "✗");

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(it->method.empty() ? "(?)" : it->method.c_str());

            ImGui::TableSetColumnIndex(2);
            if (!it->ok && !it->error.empty())
                ImGui::TextColored(theme::Warn, "%s", it->error.c_str());
            else
                ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace dx12e
