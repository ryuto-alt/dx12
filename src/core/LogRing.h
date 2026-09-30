#pragma once
// ログのリングバッファ（M9）。Logger の内部部品だが、spdlog に依存しないので ctest が単体で建てられる。
//   ・seq(=LogEntry::id) は 1 から単調増加。リングが溢れても番号は戻らない
//   ・Query は「since より後の条件に合う先頭 limit 件」を返し、取りこぼし 0 のカーソル(nextCursor)を返す
#include "core/LogTypes.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace dx12e
{

struct LogFilter
{
    uint64_t sinceSeq = 0;
    size_t   limit = 200;
    bool     tail = false;          // true = 条件に合う最新 limit 件（差分取得ではなく末尾を見る用）
    bool     includeAux = false;    // aux（MCP エラー等）を含める。categories に aux のカテゴリを名指ししても含める
    uint32_t levelMask = 0;         // ビット i = level i を通す。0 = 制限なし
    int      minLevel = -1;         // >=0 ならこの level 以上
    std::vector<std::string> categories;   // 空 = 全カテゴリ
    std::string grep;               // ECMAScript 正規表現（大文字小文字を無視）。空 = なし
};

struct LogQueryResult
{
    std::vector<LogEntry> entries;
    uint64_t nextCursor = 0;   // 次回の since_seq。打ち切ったら最後に返した seq、打ち切らなければ latestSeq
    uint64_t latestSeq = 0;
    uint64_t oldestSeq = 0;    // リングに残っている最古の seq（空なら latestSeq+1）
    uint64_t dropped = 0;      // since より後なのに追い出されて読めなくなった件数
    uint64_t omitted = 0;      // tail のとき、上限のために返さなかった古い一致件数
    uint64_t filteredOut = 0;  // 走査して条件に合わなかった件数
    bool     hasMore = false;
    size_t   capacity = 0;
    std::string error;         // 正規表現が不正なときの理由（entries は空）
};

// "warn" / "warning" / "error" / "err" / "critical" / "info" / "debug" / "trace" -> 0..5。不明は -1。
int ParseLogLevelName(const std::string& name);
const char* LogLevelName(int level);   // 0..5 -> "trace".."critical"

class LogRing
{
public:
    explicit LogRing(size_t capacity = 4096);

    // id を採番して積む。time / epochMs が空ならここで埋める。積んだ（採番済みの）コピーを返す。スレッド安全。
    LogEntry Push(LogEntry e);

    // 旧 Logger::ReadBuffered の互換: since より新しい **aux でない** エントリを out へ追記し、走査した最後の id（無ければ since）を返す。
    uint64_t ReadLegacy(uint64_t since, std::vector<LogEntry>& out) const;

    LogQueryResult Query(const LogFilter& f) const;

    uint64_t LatestSeq() const;
    size_t   Size() const;
    size_t   Capacity() const { return m_cap; }
    uint64_t SessionId() const { return m_sessionId; }   // 構築時の epoch ms（エンジン再起動の検知に使う）

private:
    size_t                m_cap;
    uint64_t              m_sessionId;
    mutable std::mutex    m_mutex;
    std::deque<LogEntry>  m_buf;
    uint64_t              m_nextId = 1;
};

int64_t NowEpochMs();
std::string FormatHms(int64_t epochMs);

} // namespace dx12e
