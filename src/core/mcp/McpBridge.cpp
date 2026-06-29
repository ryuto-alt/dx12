// winsock2.h は windows.h より前に include する必要がある。
#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/mcp/McpBridge.h"
#include "core/Logger.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>   // GetTempPathA（ポートファイルの出力先）

namespace dx12e {

struct McpBridge::Impl
{
    SOCKET listenSock = INVALID_SOCKET;
    std::atomic<SOCKET> clientSock{ INVALID_SOCKET };
    std::thread worker;
    std::atomic<bool> running{ false };
    bool wsaUp = false;

    std::mutex mtx;
    std::vector<std::pair<SOCKET, std::string>> pending;  // (client, requestLine)

    // ---- 状態の見える化（MCP / AI Bridge パネル用）----
    uint16_t port = 0;                       // Start で一度だけ書く（以後は読み取り専用）
    std::atomic<bool> connected{ false };    // accept/切断は worker スレッド＝atomic
    std::deque<CommandLogEntry> history;     // 直近 64 件（メインスレッド限定＝ロック不要）

    void AcceptLoop()
    {
        while (running.load())
        {
            SOCKET client = ::accept(listenSock, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                if (!running.load()) break;   // Stop() が listenSock を閉じた
                continue;
            }
            clientSock.store(client);
            connected.store(true);
            Logger::Info("MCP bridge: client connected");

            std::string buf;
            char tmp[4096];
            bool firstLine = true;
            bool drop = false;
            while (running.load() && !drop)
            {
                int n = ::recv(client, tmp, static_cast<int>(sizeof(tmp)), 0);
                if (n <= 0) break;   // 切断/エラー
                buf.append(tmp, static_cast<size_t>(n));

                size_t pos;
                while ((pos = buf.find('\n')) != std::string::npos)
                {
                    std::string line = buf.substr(0, pos);
                    buf.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    // 正規クライアントの行は必ず JSON オブジェクト。最初の行が '{' で始まらなければ
                    // ブラウザの HTTP/WebSocket ドライブバイ等とみなし接続を切る(無認証ポートの最低防御)。
                    if (firstLine) { firstLine = false; if (line[0] != '{') { drop = true; break; } }
                    std::lock_guard<std::mutex> lock(mtx);
                    pending.emplace_back(client, std::move(line));
                }
            }

            // close 所有を一本化(Stop() も同じ exchange を使う)。先に valid を取った側だけが閉じる。
            {
                SOCKET c = clientSock.exchange(INVALID_SOCKET);
                if (c != INVALID_SOCKET) ::closesocket(c);
            }
            connected.store(false);
            Logger::Info("MCP bridge: client disconnected");
        }
    }
};

McpBridge::McpBridge() : m_impl(std::make_unique<Impl>()) {}

McpBridge::~McpBridge() { Stop(); }

namespace {
// 確定ポートを %TEMP%/dx12_mcp.port に書く。Node 側 engineClient が自動検出に使う
// (DX12_MCP_PORT が無いとき os.tmpdir()/dx12_mcp.port を読む)。失敗しても致命ではない。
void WritePortFile(uint16_t port)
{
    char tmp[MAX_PATH];
    DWORD n = ::GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n > MAX_PATH) return;
    std::string path = std::string(tmp) + "dx12_mcp.port";
    FILE* f = nullptr;
    if (::fopen_s(&f, path.c_str(), "wb") == 0 && f)
    {
        char buf[16];
        int len = std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(port));
        if (len > 0) std::fwrite(buf, 1, static_cast<size_t>(len), f);
        std::fclose(f);
    }
}
} // namespace

bool McpBridge::Start(uint16_t preferredPort)
{
    auto& s = *m_impl;
    if (s.running.load()) return true;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Logger::Error("MCP bridge: WSAStartup failed");
        return false;
    }
    s.wsaUp = true;

    s.listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s.listenSock == INVALID_SOCKET)
    {
        Logger::Error("MCP bridge: socket() failed");
        Stop();
        return false;
    }

    // preferredPort から +10 まで順に bind を試す。Windows は EADDRINUSE 後の同一ソケット
    // 再 bind を許すので、socket を作り直さず次のポートへ進める。
    uint16_t chosen = 0;
    for (uint16_t i = 0; i < 11; ++i)
    {
        const uint16_t tryPort = static_cast<uint16_t>(preferredPort + i);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(tryPort);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);   // ローカルのみ。外部から触れない。
        if (::bind(s.listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            chosen = tryPort;
            break;
        }
    }

    if (chosen == 0 || ::listen(s.listenSock, 1) == SOCKET_ERROR)
    {
        Logger::Error("MCP bridge: bind/listen failed on ports {}..{} (all in use?)",
                      preferredPort, static_cast<int>(preferredPort) + 10);
        Stop();
        return false;
    }

    s.port = chosen;          // 待受ポートを記録（パネル表示用）
    WritePortFile(chosen);    // Node 側の自動検出用
    s.running.store(true);
    s.worker = std::thread([&s] { s.AcceptLoop(); });
    Logger::Info("MCP bridge listening on 127.0.0.1:{}", chosen);
    return true;
}

void McpBridge::Stop()
{
    auto& s = *m_impl;
    if (!s.running.exchange(false))
    {
        // 起動失敗の後始末でも WSA だけは畳む。
        if (s.wsaUp) { ::WSACleanup(); s.wsaUp = false; }
        if (s.listenSock != INVALID_SOCKET) { ::closesocket(s.listenSock); s.listenSock = INVALID_SOCKET; }
        return;
    }

    // accept/recv を叩き起こすためソケットを閉じる。
    SOCKET client = s.clientSock.exchange(INVALID_SOCKET);
    if (client != INVALID_SOCKET) ::closesocket(client);
    if (s.listenSock != INVALID_SOCKET) { ::closesocket(s.listenSock); s.listenSock = INVALID_SOCKET; }

    if (s.worker.joinable()) s.worker.join();
    s.connected.store(false);
    if (s.wsaUp) { ::WSACleanup(); s.wsaUp = false; }
}

void McpBridge::Poll(const std::function<std::string(uint64_t, const std::string&)>& handler)
{
    auto& s = *m_impl;
    std::vector<std::pair<SOCKET, std::string>> work;
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        work.swap(s.pending);
    }
    for (auto& [sock, line] : work)
    {
        std::string resp;
        // ハンドラ例外がここを抜けると Run→main まで伝播してエディタが落ちるので握り潰す。
        try { resp = handler(static_cast<uint64_t>(sock), line); }
        catch (...) { resp = "{\"ok\":false,\"error\":\"internal error\"}"; }
        if (resp.empty()) continue;   // 遅延応答: フレーム境界で結果確定後に SendToClient が送る
        resp.push_back('\n');
        // メインスレッドからの send と worker の recv は同一ソケットで並行可(Winsock)。
        ::send(sock, resp.data(), static_cast<int>(resp.size()), 0);
    }
}

void McpBridge::SendToClient(uint64_t client, const std::string& jsonLine)
{
    auto& s = *m_impl;
    const SOCKET sock = static_cast<SOCKET>(client);
    if (sock == INVALID_SOCKET) return;
    // 受信時のクライアントが既に切断/別クライアントに置き換わっていたら捨てる。
    if (s.clientSock.load() != sock) return;
    std::string out = jsonLine;
    out.push_back('\n');
    ::send(sock, out.data(), static_cast<int>(out.size()), 0);
}

uint16_t McpBridge::Port() const { return m_impl->port; }

bool McpBridge::IsConnected() const { return m_impl->connected.load(); }

void McpBridge::RecordCommand(const std::string& method, bool ok, const std::string& error)
{
    auto& h = m_impl->history;
    h.push_back({ method, ok, error });   // CommandLogEntry{method, ok, error}
    while (h.size() > 64) h.pop_front();   // 直近 64 件だけ保持（古いものから捨てる）
}

std::vector<McpBridge::CommandLogEntry> McpBridge::RecentCommands() const
{
    return { m_impl->history.begin(), m_impl->history.end() };   // 古い順のコピー
}

} // namespace dx12e
