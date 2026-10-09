#pragma once
// ===========================================================================
// ScriptUdp — Lua の net.udpOpen が返すループバック専用 UDP ソケット。
//   127.0.0.1 にだけ bind / 送信する（0.0.0.0・外部 IP には一切触れない）。ノンブロッキング。
//   ファイル経由の IPC（Defender / OneDrive が毎回走査する）の代わり。
//   Winsock は WSAStartup の参照カウントで保持し、ソケットを閉じれば解放する。
// ===========================================================================
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dx12e
{

class ScriptUdpSocket
{
public:
    static constexpr size_t kMaxRecv = 64 * 1024;   // 受信 1 通の上限
    static constexpr size_t kMaxSend = 60 * 1024;   // 送信 1 通の上限

    // 127.0.0.1:port に bind。失敗なら nullptr と err。port は 1024..65535。
    static std::shared_ptr<ScriptUdpSocket> Open(int port, std::string& err);

    ~ScriptUdpSocket();
    ScriptUdpSocket(const ScriptUdpSocket&) = delete;
    ScriptUdpSocket& operator=(const ScriptUdpSocket&) = delete;

    // 次の 1 通。無ければ false。ブロックしない。
    bool Recv(std::string& out);
    // 溜まっている分を全部読み、最後の 1 通だけ返す。無ければ false。
    bool RecvLatest(std::string& out);
    // 127.0.0.1:port へ送る。port は 1..65535、text は kMaxSend 以下。
    bool Send(int port, const std::string& text);
    void Close();   // 何度呼んでもよい
    bool IsOpen() const { return m_sock != kInvalid; }
    int  Port() const { return m_port; }

private:
    ScriptUdpSocket() = default;
    static constexpr std::uintptr_t kInvalid = ~static_cast<std::uintptr_t>(0);
    std::uintptr_t m_sock = kInvalid;   // SOCKET
    int            m_port = 0;
};

} // namespace dx12e
