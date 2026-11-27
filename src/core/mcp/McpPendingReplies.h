#pragma once
// ===== 未応答の MCP 遅延応答の数え上げ（W1 FU-E-15 / A11 の間引きの busy 判定用）=====
// ディスパッチャが遅延応答（isDeferred）を受理したとき Add し、McpBridge::SendToClient の観測点で Resolve する。
// どの method の遅延応答でも数えるので、個別の待ちリストを Application に足さなくても「応答待ちがある間は間引かない」が言える。
// 取りこぼし（応答が一度も送られない不具合）で永久に busy にならないよう、maxAge 秒を超えた項目は数えない。
// 純ロジック（時計は引数）。tests/mcp_pending_replies_test.cpp で検査する。
#include <cstdint>
#include <mutex>
#include <set>
#include <utility>
#include <map>

namespace dx12e
{

// editor_command_run の待ち（ApplicationMcpEditorUi.cpp。最長 6 秒・180 フレーム）が残っているか。
bool HasPendingEditorRunWaits();

namespace mcppending
{

class PendingReplies
{
public:
    void Add(uint64_t client, long long requestId, double nowSec)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_items[{client, requestId}] = nowSec;
    }
    // 応答が送られた。登録が無ければ何もしない（遅延でない応答・通知）。
    void Resolve(uint64_t client, long long requestId)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_items.erase({client, requestId});
    }
    // nowSec - 登録時刻 <= maxAgeSec の項目の数。古すぎる項目は掃除する。
    size_t Count(double nowSec, double maxAgeSec = 30.0)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_items.begin(); it != m_items.end();)
            it = (nowSec - it->second > maxAgeSec) ? m_items.erase(it) : std::next(it);
        return m_items.size();
    }
    void Clear()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_items.clear();
    }

private:
    std::mutex m_mtx;
    std::map<std::pair<uint64_t, long long>, double> m_items;
};

inline PendingReplies& Replies()
{
    static PendingReplies r;
    return r;
}

} // namespace mcppending
} // namespace dx12e
