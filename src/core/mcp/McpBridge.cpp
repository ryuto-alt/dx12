// winsock2.h は windows.h より前に include する必要がある。
#include <winsock2.h>
#include "core/CrashHandler.h"
#include <ws2tcpip.h>

#include "core/mcp/McpBridge.h"
#include "core/mcp/McpSafety.h"   // M5: 遅延応答の観測点
#include "core/Logger.h"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>   // GetTempPathA（ポートファイルの出力先）/ Sleep

namespace dx12e {

namespace {
// 同時に繋げるクライアントの上限。MCP サーバ 1 本 + AI が書いたスクリプト(コードモード)+ 診断で数本あれば足りる。
// ★旧実装は「1 本だけ」で、2 本目は TCP の接続には成功するのに一切返事が来ず、呼び出し側がタイムアウト(8〜10 秒)まで
//   固まっていた(2026-10-04 に実測)。上限を超えたら黙らせずに 1 行のエラーを返して閉じる。
constexpr size_t kMaxClients = 8;
} // namespace

struct McpBridge::Impl
{
    SOCKET listenSock = INVALID_SOCKET;
    std::thread acceptThread;
    std::atomic<bool> running{ false };
    bool wsaUp = false;

    // ---- 接続中のクライアント ----
    // トークン(uint64)は接続ごとに 1 から増える番号。SOCKET の値をそのまま使うと、閉じたソケットの値を OS が
    // 次の接続へ再利用したとき、前のクライアント宛ての遅延応答が新しいクライアントへ誤配される。
    // 0 は「MCP 由来でない」の意味で使われている(McpDeferred.h)ので発行しない。
    std::mutex clientsMtx;                              // clients と、ソケットへの send / close を守る
    std::unordered_map<uint64_t, SOCKET> clients;
    std::atomic<uint64_t> nextClientId{ 1 };
    std::atomic<int> clientCount{ 0 };
    std::atomic<int> activeReaders{ 0 };                // Stop が読み取りスレッドの終わりを待つため

    std::mutex mtx;
    std::vector<std::pair<uint64_t, std::string>> pending;  // (client, requestLine)

    // ---- 状態の見える化（MCP / AI Bridge パネル用）----
    uint16_t port = 0;                       // Start で一度だけ書く（以後は読み取り専用）
    std::deque<CommandLogEntry> history;     // 直近 64 件（メインスレッド限定＝ロック不要）

    // 登録を外してソケットを閉じる。先に外した側だけが閉じる(Stop と読み取りスレッドの二重 close を防ぐ)。
    void DropClient(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(clientsMtx);
        auto it = clients.find(id);
        if (it == clients.end()) return;
        ::closesocket(it->second);
        clients.erase(it);
        clientCount.store(static_cast<int>(clients.size()));
    }

    void ReadLoop(uint64_t id, SOCKET sock)
    {
        std::string buf;
        char tmp[4096];
        bool firstLine = true;
        bool drop = false;
        while (running.load() && !drop)
        {
            int n = ::recv(sock, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (n <= 0) break;   // 切断/エラー/Stop による close
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
                pending.emplace_back(id, std::move(line));
            }
        }
        DropClient(id);
        Logger::Info("MCP bridge: client #{} disconnected ({} connected)", id, clientCount.load());
    }

    void AcceptLoop()
    {
        while (running.load())
        {
            SOCKET sock = ::accept(listenSock, nullptr, nullptr);
            if (sock == INVALID_SOCKET)
            {
                if (!running.load()) break;   // Stop() が listenSock を閉じた
                continue;
            }
            const uint64_t id = nextClientId.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(clientsMtx);
                if (clients.size() >= kMaxClients)
                {
                    static const char kBusy[] =
                        "{\"ok\":false,\"error_code\":9,\"error_name\":\"E_ENGINE_BUSY\","
                        "\"error\":\"too many MCP clients connected\","
                        "\"error_hint\":\"使っていない接続(古い MCP サーバ・止め忘れたスクリプト)を閉じてから繋ぎ直す\"}\n";
                    ::send(sock, kBusy, static_cast<int>(sizeof(kBusy) - 1), 0);
                    ::closesocket(sock);
                    Logger::Warn("MCP bridge: refused a client (limit {} reached)", kMaxClients);
                    continue;
                }
                clients.emplace(id, sock);
                clientCount.store(static_cast<int>(clients.size()));
            }
            Logger::Info("MCP bridge: client #{} connected ({} connected)", id, clientCount.load());
            activeReaders.fetch_add(1);
            // 読み取りは接続ごとのスレッド。終わったら自分で抜ける(detach)。Stop は activeReaders が 0 になるのを待つ。
            std::thread([this, id, sock] {
                CrashHandler::PrepareThread();
                ReadLoop(id, sock);
                activeReaders.fetch_sub(1);
            }).detach();
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

bool McpBridge::Start(uint16_t preferredPort, bool writePortFile)
{
    auto& s = *m_impl;
    if (s.running.load()) return true;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Logger::Error("MCPブリッジ: WSAStartup に失敗しました");
        return false;
    }
    s.wsaUp = true;

    s.listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s.listenSock == INVALID_SOCKET)
    {
        Logger::Error("MCPブリッジ: socket() に失敗しました");
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

    // backlog は上限と同じだけ取る(旧実装の 1 だと、短時間の連続接続が ECONNREFUSED になっていた)。
    if (chosen == 0 || ::listen(s.listenSock, static_cast<int>(kMaxClients)) == SOCKET_ERROR)
    {
        Logger::Error("MCPブリッジ: ポート {}..{} で bind/listen に失敗しました（すべて使用中？）",
                      preferredPort, static_cast<int>(preferredPort) + 10);
        Stop();
        return false;
    }

    s.port = chosen;          // 待受ポートを記録（パネル表示用）
    // ★ポートを明示指定された場合はファイルを書かない。複数インスタンスを並べたとき、
    //   後から起動した方がファイルを奪って「どのエンジンに繋がるか分からない」状態になる。
    //   明示した側は自分でポートを知っているので、書く必要が無い。
    if (writePortFile) WritePortFile(chosen);   // Node 側の自動検出用
    s.running.store(true);
    s.acceptThread = std::thread([&s] { CrashHandler::PrepareThread(); s.AcceptLoop(); });
    Logger::Info("MCP bridge listening on 127.0.0.1:{} (up to {} clients)", chosen, kMaxClients);
    return true;
}

void McpBridge::Stop()
{
    auto& s = *m_impl;
    if (!s.running.exchange(false))
    {
        // 起動失敗の後始末でも WSA だけは畳む。
        if (s.listenSock != INVALID_SOCKET) { ::closesocket(s.listenSock); s.listenSock = INVALID_SOCKET; }
        if (s.wsaUp) { ::WSACleanup(); s.wsaUp = false; }
        return;
    }

    // accept を叩き起こすため待受ソケットを閉じ、各クライアントのソケットも閉じて recv を起こす。
    if (s.listenSock != INVALID_SOCKET) { ::closesocket(s.listenSock); s.listenSock = INVALID_SOCKET; }
    if (s.acceptThread.joinable()) s.acceptThread.join();
    {
        std::lock_guard<std::mutex> lock(s.clientsMtx);
        for (auto& [id, sock] : s.clients) ::closesocket(sock);
        s.clients.clear();
        s.clientCount.store(0);
    }
    // 読み取りスレッドは detach 済み。Impl を参照しているので、抜けきるまで待つ(recv は close で即座に戻る)。
    for (int i = 0; i < 2000 && s.activeReaders.load() > 0; ++i) ::Sleep(1);
    if (s.activeReaders.load() > 0)
        Logger::Warn("MCP bridge: {} reader thread(s) did not stop in time", s.activeReaders.load());
    if (s.wsaUp) { ::WSACleanup(); s.wsaUp = false; }
}

void McpBridge::Poll(const std::function<std::string(uint64_t, const std::string&)>& handler)
{
    auto& s = *m_impl;
    std::vector<std::pair<uint64_t, std::string>> work;
    {
        std::lock_guard<std::mutex> lock(s.mtx);
        work.swap(s.pending);
    }
    for (auto& [client, line] : work)
    {
        std::string resp;
        // ハンドラ例外がここを抜けると Run→main まで伝播してエディタが落ちるので握り潰す。
        try { resp = handler(client, line); }
        catch (...) { resp = "{\"ok\":false,\"error\":\"internal error\"}"; }
        if (resp.empty()) continue;   // 遅延応答: フレーム境界で結果確定後に SendToClient が送る
        resp.push_back('\n');
        std::lock_guard<std::mutex> lock(s.clientsMtx);
        auto it = s.clients.find(client);
        if (it != s.clients.end())
            ::send(it->second, resp.data(), static_cast<int>(resp.size()), 0);
    }
}

void McpBridge::SendToClient(uint64_t client, const std::string& jsonLine)
{
    auto& s = *m_impl;
    // M5: 遅延応答の完了を冪等ストアへ伝える観測点（接続が切れていても完了は記録する）。
    mcpsafety::NotifySend(client, jsonLine);
    if (client == 0) return;
    std::string out = jsonLine;
    out.push_back('\n');
    // 受信したクライアントが既に切断していたら捨てる(番号は使い回さないので別クライアントへ誤配しない)。
    std::lock_guard<std::mutex> lock(s.clientsMtx);
    auto it = s.clients.find(client);
    if (it == s.clients.end()) return;
    ::send(it->second, out.data(), static_cast<int>(out.size()), 0);
}

uint16_t McpBridge::Port() const { return m_impl->port; }

bool McpBridge::IsConnected() const { return m_impl->clientCount.load() > 0; }

int McpBridge::ClientCount() const { return m_impl->clientCount.load(); }

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
