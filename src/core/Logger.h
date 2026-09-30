#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "core/LogTypes.h"
#include "core/LogRing.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dx12e
{

// LogEntry（エディタのコンソールパネル / MCP の log_read が読むインメモリのログ 1 件）は core/LogTypes.h。
// id は単調増加（読み手はカーソルとして使う）。level は spdlog::level::level_enum の整数値
// （0=trace 1=debug 2=info 3=warn 4=err 5=critical）。

class Logger
{
public:
    static void Init();
    static void Shutdown();

    // sinceId より新しいバッファ内エントリを out へ追記し、最後のエントリ id を返す
    // （新着なしなら sinceId をそのまま返す）。スレッド安全。容量 4096 でリング。
    // ★M9: aux（PushAux の補助エントリ）は返さない（コンソール / Play サマリの挙動を変えない）。
    static uint64_t ReadBuffered(uint64_t sinceId, std::vector<LogEntry>& out);

    // ---- M9: 構造化ログ（MCP の log_read / errors）----
    // 毎フレーム 1 回呼ぶ。以後に書かれるエントリの frame / playing になる。
    static void SetFrame(uint64_t frame, bool playing);
    // sinceSeq 以降の条件に合うエントリを取り出す（取りこぼし 0 のカーソル付き）。aux も対象（filter.includeAux / categories）。
    static LogQueryResult ReadStructured(const LogFilter& f);
    // リングだけに入る補助エントリ（ファイル・コンソールには出さない）。MCP エラーの集約用。
    // エラー集約（ErrorLog）には category "mcp" などとして入る。戻り値 = 採番された seq。
    static uint64_t PushAux(int level, const std::string& category, const std::string& msg);
    static uint64_t LatestSeq();
    static uint64_t SessionId();     // プロセス起動（リング構築）の epoch ms。変われば再起動
    static size_t   RingCapacity();

    template<typename... Args>
    static void Info(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        if (s_logger) s_logger->info(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void Warn(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        if (s_logger) s_logger->warn(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void Error(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        if (s_logger) s_logger->error(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void Debug(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        if (s_logger) s_logger->debug(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void Critical(spdlog::format_string_t<Args...> fmt, Args&&... args)
    {
        if (s_logger) s_logger->critical(fmt, std::forward<Args>(args)...);
    }

private:
    static std::shared_ptr<spdlog::logger> s_logger;
};

} // namespace dx12e
