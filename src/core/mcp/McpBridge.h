#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

private:
    struct Impl;                  // Winsock を winsock2.h 込みで .cpp に隠す(pimpl)
    std::unique_ptr<Impl> m_impl;
};

} // namespace dx12e
