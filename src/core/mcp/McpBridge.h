#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dx12e {

// エディタ専用の薄い TCP ブリッジ。127.0.0.1:<port> で改行区切りの JSON 行を受け取り、
// メインスレッドの Poll() でハンドラに渡して応答行を返す。ゲーム(封印ランタイム)では起動しない。
// ponytail: 単一クライアント・JSON 非依存(行を運ぶだけ)。同時接続が要るまで複数化しない。
class McpBridge
{
public:
    McpBridge();
    ~McpBridge();
    McpBridge(const McpBridge&) = delete;
    McpBridge& operator=(const McpBridge&) = delete;

    // 失敗(ポート占有など)は false。エディタ起動時に 1 回呼ぶ。
    bool Start(uint16_t port);
    void Stop();

    // メインスレッドから毎フレーム呼ぶ。handler(requestLine) -> responseLine。
    // 溜まったリクエストを順に handler へ渡し、戻り値を同じクライアントへ送り返す。
    void Poll(const std::function<std::string(const std::string&)>& handler);

    // ---- 状態の見える化（MCP / AI Bridge パネル用。すべてメインスレッドから呼ぶ）----
    // 直近コマンド 1 件の記録。method=MCP メソッド名、ok=成否、error=失敗理由(空可)。
    struct CommandLogEntry
    {
        std::string method;
        bool        ok = false;
        std::string error;
    };

    uint16_t Port() const;            // 待受ポート（未起動時は 0）
    bool     IsConnected() const;     // クライアント接続中か（accept/切断は別スレッド＝atomic）
    // HandleMcpCommand 末尾で 1 件記録する。履歴はメインスレッド限定アクセス＝ロック不要。
    void     RecordCommand(const std::string& method, bool ok, const std::string& error = {});
    std::vector<CommandLogEntry> RecentCommands() const;   // 直近リング（古い順）のコピー

private:
    struct Impl;                  // Winsock を winsock2.h 込みで .cpp に隠す(pimpl)
    std::unique_ptr<Impl> m_impl;
};

} // namespace dx12e
