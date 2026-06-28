#pragma once

namespace dx12e
{

class McpBridge;
class EditorContext;

// MCP / AI ブリッジの状態モニタパネル（エディタ専用・読み取り専用）。
// 待受ポート / クライアント接続状態 / 直近コマンド履歴 / 接続スニペットを表示する。
// パネルは TCP の受け手（ホスト）側なので送信はしない＝純粋なモニタ。
class McpBridgePanel
{
public:
    // ツール窓トグル（ctx.showMcpBridge）が ON のとき毎フレーム呼ぶ。
    // bridge は Application が所有する McpBridge（ゲーム時は呼び出し側で null チェック）。
    static void Render(McpBridge& bridge, EditorContext& ctx);
};

} // namespace dx12e
