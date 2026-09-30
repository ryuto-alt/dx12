#pragma once
// ログ 1 件の型（spdlog 非依存。LogRing / ErrorLog / ctest が共有する）。
#include <cstdint>
#include <string>

namespace dx12e
{

// エディタのコンソールパネル / MCP の log_read が読む、インメモリのログ 1 件。
// id は単調増加（=seq。読み手はカーソルとして使う）。level は spdlog::level::level_enum の整数値
// （0=trace 1=debug 2=info 3=warn 4=err 5=critical）。
// M9: category / frame / epochMs / stack / aux / playing は加算（既存の id/level/time/text は不変）。
struct LogEntry
{
    uint64_t    id    = 0;
    int         level = 2;
    std::string time;          // "HH:MM:SS"（ローカル時刻）
    std::string text;
    // ---- M9（構造化）----
    std::string category;      // lua / d3d12 / shader / asset / physics / mcp / engine（ClassifyLogCategory の推定）
    uint64_t    frame = 0;     // 書いた時点のフレーム番号（Logger::SetFrame）
    int64_t     epochMs = 0;   // 書いた時刻（epoch ms）
    std::string stack;         // Lua の "stack traceback:" 以降（無ければ空）
    bool        aux = false;   // true = リングだけに入る補助エントリ（MCP エラー等）。コンソール / Play サマリには出さない
    bool        playing = false; // Play 中に書かれたか
};

} // namespace dx12e
