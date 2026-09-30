#pragma once
// エラー集約（M9）。ログのシンクから流し込み、「同じ原因」を 1 グループにまとめる。エンジン非依存の純ロジック（ctest が単体で建てる）。
//   ・対象: level>=error の全部 / category d3d12 の warn / category mcp（MCP エラー。Logger::PushAux で入る）
//   ・指紋 = カテゴリ + 正規化メッセージ + スクリプト位置(file:line) の 12 桁 hex
//   ・Clear() は集約だけを消して新しいセッションを始める（ログのリングは消さない）。Play が終わっても消えない
#include "core/LogTypes.h"

#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dx12e
{

// ---- 純関数（ログの分類・正規化）----
// カテゴリ推定。優先順: d3d12 / lua / shader / asset / physics / engine。
std::string ClassifyLogCategory(int level, const std::string& text);
// text の "stack traceback:" 以降を stack へ分ける（無ければ stack は空・text は不変）。
void SplitLuaStack(const std::string& full, std::string& text, std::string& stack);
// 0x アドレス→"0x?"、連続数字→"#"、\ → /、連続空白の圧縮、長さ上限(300 バイト)。
std::string NormalizeErrorMessage(const std::string& text);
// 最初の "<path>.lua:<行>" を取り出す。無ければ false。
bool ExtractScriptLocation(const std::string& text, std::string& file, int& line);
// 12 桁 hex。
std::string ErrorFingerprint(const std::string& category, const std::string& normalized, const std::string& scriptFile, int scriptLine);

struct ErrorGroup
{
    std::string fingerprint;
    std::string category;
    int         level = 4;            // 見た中で最大
    std::string message;              // 正規化テンプレート
    std::string sample;               // 初回の生テキスト
    uint64_t    count = 0;
    uint64_t    firstSeq = 0, lastSeq = 0;
    int64_t     firstAt = 0, lastAt = 0;
    uint64_t    firstFrame = 0, lastFrame = 0;
    uint64_t    playCount = 0;        // Play 中に出た回数
    bool        hasScript = false;
    std::string scriptFile;
    int         scriptLine = 0;
    std::string stack;                // 最新の stack
    std::deque<std::string> samples;  // 直近の生テキスト（最大 kMaxSamples）
};

struct ErrorQuery
{
    uint64_t sinceSeq = 0;
    std::vector<std::string> categories;
    uint64_t minCount = 1;
    size_t   limit = 50;
    size_t   samples = 1;
    bool     sortByCount = false;
};

struct ErrorQueryResult
{
    std::vector<ErrorGroup> groups;     // samples は query.samples 件に切り詰め済み
    uint64_t totalGroups = 0;           // セッション全体のグループ数
    uint64_t totalOccurrences = 0;      // セッション全体の出現回数
    uint64_t matchedGroups = 0;         // フィルタに合ったグループ数（limit 前）
    std::map<std::string, uint64_t> byCategory;   // セッション全体のカテゴリ別出現回数
    bool     hasNew = false;
    uint64_t evicted = 0;
    uint64_t sessionId = 0;
    int64_t  clearedAtMs = 0;
    int64_t  startedAtMs = 0;
};

class ErrorLog
{
public:
    static constexpr size_t kMaxGroups = 500;
    static constexpr size_t kMaxSamples = 5;

    explicit ErrorLog(size_t maxGroups = kMaxGroups);
    static ErrorLog& Global();

    // 集約の対象か（level>=error / d3d12 の warn / mcp）
    static bool IsAggregated(const LogEntry& e);
    // e が対象なら取り込む（対象外なら何もしない）。e.id / e.epochMs / e.frame などはシンクが埋めた値を使う。
    void Observe(const LogEntry& e);

    ErrorQueryResult Query(const ErrorQuery& q) const;
    // 集約を空にして新しいセッションを始める。戻り値 = 消したグループ数。
    size_t Clear(int64_t nowMs);

    uint64_t SessionId() const;

private:
    size_t                                      m_maxGroups;
    mutable std::mutex                          m_mutex;
    std::unordered_map<std::string, ErrorGroup> m_groups;
    std::map<std::string, uint64_t>             m_byCategory;
    uint64_t                                    m_total = 0;
    uint64_t                                    m_evicted = 0;
    uint64_t                                    m_session = 1;
    int64_t                                     m_startedAt = 0;
    int64_t                                     m_clearedAt = 0;
};

} // namespace dx12e
