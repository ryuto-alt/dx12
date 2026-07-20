#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dx12e
{

// エディタのコンソールパネル用に、直近のログをインメモリで保持するエントリ。
// id は単調増加（読み手はカーソルとして使う）。level は spdlog::level::level_enum の整数値
// （0=trace 1=debug 2=info 3=warn 4=err 5=critical）。
struct LogEntry
{
    uint64_t    id    = 0;
    int         level = 2;
    std::string time;    // "HH:MM:SS"
    std::string text;
};

class Logger
{
public:
    static void Init();
    static void Shutdown();

    // sinceId より新しいバッファ内エントリを out へ追記し、最後のエントリ id を返す
    // （新着なしなら sinceId をそのまま返す）。スレッド安全。容量約4000でリング。
    static uint64_t ReadBuffered(uint64_t sinceId, std::vector<LogEntry>& out);

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
