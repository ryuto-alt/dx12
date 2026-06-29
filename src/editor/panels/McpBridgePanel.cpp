#include "editor/panels/McpBridgePanel.h"
#include "core/mcp/McpBridge.h"
#include "editor/EditorContext.h"

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

// tools/mcp-server を「実行時に」探して返す（末尾区切り無し・'/' 区切り）。見つからなければ空。
//
// 絶対パスをビルド時に焼き込むと、別PCやフォルダ移動・配布物では壊れる（= 今回の不具合）。
// そこで実行ファイルの実体位置を起点に、上位ディレクトリを辿って tools/mcp-server/index.ts を探す。
// dev ビルドの exe は build/<cfg>/DX12Engine.exe なので 2 階層上が repo ルート。
// 配布で exe 隣に tools/ を同梱した場合も 0 階層目（exe 隣）で当たる。
// これでビルドした本人かどうか・どのフォルダに置いたかに関係なく、その PC 上の実体を指す。
std::string ResolveMcpServerDir()
{
    auto hasIndex = [](const fs::path& dir) {
        std::error_code ec;
        return fs::exists(dir / "tools" / "mcp-server" / "index.ts", ec);
    };

    // 1) 実行時解決: exe の場所から上位へ最大 8 階層辿る。
    wchar_t buf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0)
    {
        fs::path dir = fs::path(buf).parent_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i)
        {
            if (hasIndex(dir))
                return (dir / "tools" / "mcp-server").generic_string();
            fs::path parent = dir.parent_path();
            if (parent == dir) break;  // ルート到達
            dir = parent;
        }
    }

    // 2) フォールバック: ビルド時マクロ（このマシンでビルドし、repo を動かしてない場合のみ有効）。
    if (MCP_SERVER_DIR[0] != '\0')
    {
        std::error_code ec;
        if (fs::exists(fs::path(MCP_SERVER_DIR) / "index.ts", ec))
            return fs::path(MCP_SERVER_DIR).generic_string();
    }

    return std::string();  // どこにも無い（配布物に tools/ 未同梱など）
}

// 解決結果を 1 度だけ計算してキャッシュ（毎フレームのディスク走査を避ける）。
const std::string& McpServerDir()
{
    static const std::string dir = ResolveMcpServerDir();
    return dir;
}

// index.ts の絶対パス。実行時に解決するので別 PC でもそのマシンの実体を指す。
// 解決できなければ空文字（呼び出し側で「未検出」表示にフォールバック）。
std::string IndexPath()
{
    const std::string& dir = McpServerDir();
    return dir.empty() ? std::string() : dir + "/index.ts";
}

// Claude Code 用ワンライナー。これをターミナルに貼るだけで繋がる（パス手入力ゼロ）。
std::string ClaudeCommand()
{
    return "claude mcp add dx12-engine -- node \"" + IndexPath() + "\"";
}

// Codex / 手動用 .mcp.json。args に実行時解決した絶対パスを埋める。
std::string McpJson()
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
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.35f, 1.0f), "● 接続中");
    else
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "○ 未接続");
    ImGui::SameLine(0, 12);
    if (port != 0)
        ImGui::Text("待受 127.0.0.1:%u", static_cast<unsigned>(port));
    else
        ImGui::TextDisabled("待受 (未起動)");

    ImGui::Separator();

    // tools/mcp-server を実行時に探して解決（別 PC でもそのマシンの実体を指す）。
    const std::string serverDir = McpServerDir();
    const bool serverFound = !serverDir.empty();

    // ---- かんたん接続（Claude Code）: コマンド1本コピー → 貼る → 再起動 ----
    ImGui::TextWrapped("AI(Claude Code)から、このエディタを操作してゲームを作れます。");
    ImGui::Spacing();

    if (!serverFound)
    {
        // tools/ が exe 近傍に無い（= ソース無しの配布物など）。手動で場所を指定してもらう。
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.5f, 1.0f),
                           "MCP サーバ(tools/mcp-server)が見つかりません。");
        ImGui::TextWrapped("エンジンのソース一式（tools/mcp-server を含む）を用意し、"
                           "tools/mcp-server/index.ts の場所を指定してな。");
        ImGui::Spacing();
        ImGui::TextDisabled("登録コマンド（<PATH> を index.ts の絶対パスに置換）:");
        static std::string tmpl =
            "claude mcp add dx12-engine -- node \"<PATH>/tools/mcp-server/index.ts\"";
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##mcp_cmd_tmpl", tmpl.data(), tmpl.size() + 1, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button("テンプレをコピー", ImVec2(-FLT_MIN, 0)))
            ImGui::SetClipboardText(tmpl.c_str());
    }
    else
    {
        ImGui::TextDisabled("① 下の「コマンドをコピー」を押す");
        ImGui::TextDisabled("② ターミナル(PowerShell 等)に貼って実行");
        ImGui::TextDisabled("③ Claude Code を再起動 → 繋がります");
        ImGui::Spacing();

        // コピーされる中身を見せておく（読み取り専用・選択可）。
        // 実行時解決した値を毎回反映できるよう static にしない（パスはプロセス内で一定なので再計算は軽い）。
        std::string cmd = ClaudeCommand();
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
            std::string js = McpJson();
            ImGui::InputTextMultiline("##mcp_json", js.data(), js.size() + 1,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8.0f),
                                      ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button(".mcp.json をコピー"))
                ImGui::SetClipboardText(js.c_str());
        }
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
