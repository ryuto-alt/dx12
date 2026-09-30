#pragma once
// ===========================================================================
// MCP の副作用の安全性（M5。docs/MCP.md §13）: 冪等キーのストア / guarded 確認トークン / dryRun プレビュー表
// ---------------------------------------------------------------------------
// ★標準ライブラリだけ（nlohmann にもエンジンにも依存しない）。時計は注入できる。
//   tests/mcp_safety_test.cpp がエンジンをリンクせずに検査する。ディスパッチャ（ApplicationMcp.cpp）が使う。
//
//   IdempotencyStore … idempotency_key → 前回の応答。有限サイズ（256・LRU）・TTL 600 秒。
//                      同じキーの再送は前回の結果を返して再実行しない。method / 引数が違えば衝突エラー、処理中なら InFlight。
//                      遅延応答（フレーム境界で返る method）は (client, requestId) をキーに結び付けておき、
//                      McpBridge::SendToClient の観測点（SetSendObserver）で完了を拾う（CompleteMcp を触らない）。
//   GuardTokens      … guarded な method を実行するための 1 回限り・method 束縛・TTL 60 秒の確認トークン。
//   PreviewTable     … dryRun:true で「実際に何が起こるか」を実行せず返す method 別の関数（実体は ApplicationMcpSafety.inc）。
// ===========================================================================

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/mcp/McpJournal.h"

namespace dx12e
{
namespace mcpsafety
{

using ClockMs = std::function<int64_t()>;

inline int64_t SystemNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// 環境変数を読む（MSVC の C4996 を避けて _dupenv_s）。
inline std::string EnvString(const char* name)
{
#ifdef _WIN32
    char*  buf = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) { out = buf; std::free(buf); }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// ---------------------------------------------------------------------------
// 冪等キーのストア
// ---------------------------------------------------------------------------
class IdempotencyStore
{
public:
    enum class Kind { Miss, Replay, Conflict, InFlight };
    struct Found
    {
        Kind        kind = Kind::Miss;
        std::string resp;            // Replay のとき: 保存済みの応答 JSON（id は前回のもの）
        int64_t     firstAtMs = 0;
        std::string existingMethod;  // Conflict のとき: 最初に使われた method
    };

    explicit IdempotencyStore(size_t capacity = 256, int64_t ttlMs = 600000, int64_t inFlightTtlMs = 120000, ClockMs clock = nullptr)
        : m_capacity(capacity), m_ttlMs(ttlMs), m_inFlightTtlMs(inFlightTtlMs), m_clock(std::move(clock)) {}

    size_t  Capacity() const { return m_capacity; }
    int64_t TtlSec() const { return m_ttlMs / 1000; }
    size_t  Size() const { return m_map.size(); }
    void    SetClock(ClockMs c) { m_clock = std::move(c); }

    Found Find(const std::string& key, const std::string& method, uint64_t hash)
    {
        Found f;
        auto it = m_map.find(key);
        if (it == m_map.end()) return f;
        Entry& e = *it->second;
        const int64_t now = Now();
        const int64_t ttl = e.state == State::InFlight ? m_inFlightTtlMs : m_ttlMs;
        if (now - e.touchedAtMs > ttl) { Erase(it); return f; }   // 期限切れ = 無かったことにする
        f.firstAtMs = e.firstAtMs;
        f.existingMethod = e.method;
        if (e.method != method || e.hash != hash) { f.kind = Kind::Conflict; return f; }
        Touch(it->second);
        if (e.state == State::InFlight) { f.kind = Kind::InFlight; return f; }
        f.kind = Kind::Replay;
        f.resp = e.resp;
        return f;
    }

    // 実行を始める前に積む。既に同じキーがあれば置き換える（呼び出し側が Find で Miss を確認してから呼ぶ）。
    void BeginInFlight(const std::string& key, const std::string& method, uint64_t hash)
    {
        auto it = m_map.find(key);
        if (it != m_map.end()) Erase(it);
        const int64_t now = Now();
        m_lru.push_front(Entry{key, method, hash, State::InFlight, std::string(), now, now});
        m_map[key] = m_lru.begin();
        while (m_map.size() > m_capacity && !m_lru.empty())
        {
            m_map.erase(m_lru.back().key);
            m_lru.pop_back();
        }
    }

    void Complete(const std::string& key, const std::string& respJson)
    {
        auto it = m_map.find(key);
        if (it == m_map.end()) return;
        it->second->state = State::Done;
        it->second->resp = respJson;
        it->second->touchedAtMs = Now();
        Touch(it->second);
    }

    // 失敗したので再送で再実行できるようにする（InFlight のときだけ消す）。
    void Abort(const std::string& key)
    {
        auto it = m_map.find(key);
        if (it != m_map.end() && it->second->state == State::InFlight) Erase(it);
    }

    void Clear() { m_map.clear(); m_lru.clear(); m_byReq.clear(); }

    // ---- 遅延応答 ----
    void BindRequest(uint64_t client, long long reqId, const std::string& key)
    {
        if (m_byReq.size() > m_capacity * 4) m_byReq.clear();   // 取りこぼしの残骸が溜まらないように
        m_byReq[{client, reqId}] = key;
    }
    // 応答が送られたときに呼ぶ。結び付いていれば Done（ok:true）か Abort（失敗）にして true。
    bool ResolveRequest(uint64_t client, long long reqId, bool ok, const std::string& respJson)
    {
        auto it = m_byReq.find({client, reqId});
        if (it == m_byReq.end()) return false;
        const std::string key = it->second;
        m_byReq.erase(it);
        if (ok) Complete(key, respJson); else Abort(key);
        return true;
    }
    size_t PendingRequests() const { return m_byReq.size(); }

private:
    enum class State { InFlight, Done };
    struct Entry
    {
        std::string key, method;
        uint64_t    hash;
        State       state;
        std::string resp;
        int64_t     firstAtMs;
        int64_t     touchedAtMs;
    };
    using List = std::list<Entry>;

    int64_t Now() const { return m_clock ? m_clock() : SystemNowMs(); }
    void Touch(List::iterator it) { m_lru.splice(m_lru.begin(), m_lru, it); }
    void Erase(std::unordered_map<std::string, List::iterator>::iterator it)
    {
        m_lru.erase(it->second);
        m_map.erase(it);
    }

    size_t  m_capacity;
    int64_t m_ttlMs;
    int64_t m_inFlightTtlMs;
    ClockMs m_clock;
    List    m_lru;
    std::unordered_map<std::string, List::iterator> m_map;
    std::map<std::pair<uint64_t, long long>, std::string> m_byReq;
};

// ---------------------------------------------------------------------------
// guarded 確認トークン
// ---------------------------------------------------------------------------
class GuardTokens
{
public:
    enum class Result { Ok, Unknown, WrongMethod, Expired };

    explicit GuardTokens(int64_t ttlMs = 60000, size_t maxTokens = 32, ClockMs clock = nullptr)
        : m_ttlMs(ttlMs), m_max(maxTokens), m_clock(std::move(clock)), m_rng(SeedFromDevice()) {}

    int64_t TtlSec() const { return m_ttlMs / 1000; }
    void    SetTtlMs(int64_t ms) { m_ttlMs = ms; }
    void    SetClock(ClockMs c) { m_clock = std::move(c); }
    size_t  Size() const { return m_tokens.size(); }

    std::string Issue(const std::string& method)
    {
        static const char kHex[] = "0123456789abcdef";
        std::string t;
        for (int w = 0; w < 2; ++w)
        {
            uint64_t v = m_rng();
            for (int k = 0; k < 16; ++k) { t += kHex[v & 0xF]; v >>= 4; }
        }
        m_tokens.push_back(Tok{t, method, Now()});
        while (m_tokens.size() > m_max) m_tokens.erase(m_tokens.begin());   // 古い順に捨てる
        return t;
    }

    // 検査して、見つかれば（成否に関わらず）消費する。別 method のトークンでも消す（使い回しを許さない）。
    Result Consume(const std::string& token, const std::string& method)
    {
        for (auto it = m_tokens.begin(); it != m_tokens.end(); ++it)
        {
            if (it->token != token) continue;
            const Tok tok = *it;
            m_tokens.erase(it);
            if (Now() - tok.issuedAtMs > m_ttlMs) return Result::Expired;
            if (tok.method != method) return Result::WrongMethod;
            return Result::Ok;
        }
        return Result::Unknown;
    }

private:
    struct Tok { std::string token, method; int64_t issuedAtMs; };
    static uint64_t SeedFromDevice()
    {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    }
    int64_t Now() const { return m_clock ? m_clock() : SystemNowMs(); }

    int64_t            m_ttlMs;
    size_t             m_max;
    ClockMs            m_clock;
    std::mt19937_64    m_rng;
    std::vector<Tok>   m_tokens;
};

// ---------------------------------------------------------------------------
// プロセス内シングルトン
// ---------------------------------------------------------------------------
inline IdempotencyStore& Idempotency()
{
    static IdempotencyStore s(256, 600000, 120000);
    return s;
}

inline GuardTokens& Guard()
{
    static GuardTokens g = [] {
        // DX12_MCP_GUARD_TTL_SEC（秒。テスト用に縮められる）。既定 60 秒。
        int64_t ms = 60000;
        const std::string e = EnvString("DX12_MCP_GUARD_TTL_SEC");
        if (!e.empty())
        {
            const double v = std::strtod(e.c_str(), nullptr);
            if (v > 0.0) ms = static_cast<int64_t>(v * 1000.0);
        }
        return GuardTokens(ms, 32);
    }();
    return g;
}

// ---------------------------------------------------------------------------
// dryRun プレビュー表: method 名 → (paramsJson) → previewJson。実体は ApplicationMcpSafety.inc が登録する。
// ---------------------------------------------------------------------------
using PreviewFn = std::function<std::string(const std::string& paramsJson)>;
inline std::map<std::string, PreviewFn>& PreviewTable()
{
    static std::map<std::string, PreviewFn> t;
    return t;
}

// ---------------------------------------------------------------------------
// McpBridge::SendToClient の観測点（遅延応答の完了を冪等ストアへ伝える）。
// ---------------------------------------------------------------------------
using SendObserverFn = std::function<void(uint64_t client, const std::string& jsonLine)>;
inline SendObserverFn& SendObserver()
{
    static SendObserverFn f;
    return f;
}
inline void NotifySend(uint64_t client, const std::string& jsonLine)
{
    if (SendObserver()) SendObserver()(client, jsonLine);
}

// ---------------------------------------------------------------------------
// ハンドラが「書く直前」に呼ぶ口（ジャーナル。スコープが無ければ何もしない）。
// ---------------------------------------------------------------------------
inline void JournalBackup(const std::filesystem::path& absPath) { mcpjournal::Instance().Backup(absPath); }
inline void JournalBackupTree(const std::filesystem::path& absPath) { mcpjournal::Instance().BackupTree(absPath); }
inline void JournalMarkIncomplete(const std::string& reason) { mcpjournal::Instance().MarkIncomplete(reason); }

} // namespace mcpsafety
} // namespace dx12e
