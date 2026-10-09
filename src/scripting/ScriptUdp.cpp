#include "scripting/ScriptUdp.h"

#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace dx12e
{

namespace
{
sockaddr_in LoopbackAddr(int port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 固定
    return a;
}
} // namespace

std::shared_ptr<ScriptUdpSocket> ScriptUdpSocket::Open(int port, std::string& err)
{
    if (port < 1024 || port > 65535) { err = "port must be 1024..65535"; return nullptr; }
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup failed"; return nullptr; }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        err = "socket() failed: " + std::to_string(::WSAGetLastError());
        ::WSACleanup();
        return nullptr;
    }
    u_long nb = 1;
    const sockaddr_in a = LoopbackAddr(port);
    if (::ioctlsocket(s, FIONBIO, &nb) != 0 ||
        ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0)
    {
        err = "bind 127.0.0.1:" + std::to_string(port) + " failed: " + std::to_string(::WSAGetLastError());
        ::closesocket(s);
        ::WSACleanup();
        return nullptr;
    }
    // 宛先不在で届いた ICMP がソケットを「接続切れ」扱いにしないよう無効化（Windows の WSAECONNRESET 対策）
    DWORD ret = 0; BOOL off = FALSE;
    ::WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);

    std::shared_ptr<ScriptUdpSocket> p(new ScriptUdpSocket());
    p->m_sock = static_cast<std::uintptr_t>(s);
    p->m_port = port;
    return p;
}

ScriptUdpSocket::~ScriptUdpSocket() { Close(); }

void ScriptUdpSocket::Close()
{
    if (m_sock == kInvalid) return;
    ::closesocket(static_cast<SOCKET>(m_sock));
    m_sock = kInvalid;
    ::WSACleanup();
}

bool ScriptUdpSocket::Recv(std::string& out)
{
    if (m_sock == kInvalid) return false;
    static thread_local std::vector<char> buf(kMaxRecv);
    for (;;)
    {
        const int n = ::recvfrom(static_cast<SOCKET>(m_sock), buf.data(), static_cast<int>(buf.size()),
                                 0, nullptr, nullptr);
        if (n >= 0) { out.assign(buf.data(), static_cast<size_t>(n)); return true; }
        const int e = ::WSAGetLastError();
        if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;   // 読み捨てて次へ
        return false;   // WSAEWOULDBLOCK など
    }
}

bool ScriptUdpSocket::RecvLatest(std::string& out)
{
    std::string cur;
    bool got = false;
    while (Recv(cur)) { out.swap(cur); got = true; }
    return got;
}

bool ScriptUdpSocket::Send(int port, const std::string& text)
{
    if (m_sock == kInvalid || port < 1 || port > 65535 || text.size() > kMaxSend) return false;
    const sockaddr_in a = LoopbackAddr(port);
    const int n = ::sendto(static_cast<SOCKET>(m_sock), text.data(), static_cast<int>(text.size()), 0,
                           reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    return n == static_cast<int>(text.size());
}

} // namespace dx12e
