#include "Logger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace dx12e
{
namespace
{
// ---- エディタコンソール用のインメモリ リングバッファ sink ----
// どのスレッドからのログも受ける（Git/Updater 等のワーカー含む）ため mutex で保護。
std::mutex            g_bufMutex;
std::deque<LogEntry>  g_buf;
uint64_t              g_nextId = 1;
constexpr size_t      kBufCap = 4000;

class RingBufferSink : public spdlog::sinks::base_sink<std::mutex>
{
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        // 時刻 "HH:MM:SS"
        const std::time_t t = std::chrono::system_clock::to_time_t(msg.time);
        std::tm tmv{};
        localtime_s(&tmv, &t);
        char tb[16];
        std::snprintf(tb, sizeof(tb), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

        LogEntry e;
        e.level = static_cast<int>(msg.level);
        e.time  = tb;
        e.text.assign(msg.payload.data(), msg.payload.size());

        std::lock_guard<std::mutex> lk(g_bufMutex);
        e.id = g_nextId++;
        g_buf.push_back(std::move(e));
        if (g_buf.size() > kBufCap)
            g_buf.pop_front();
    }
    void flush_() override {}
};
} // namespace

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;

void Logger::Init()
{
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    // GUI アプリはコンソールが無いので、ログをファイルにも残す（CWD 直下に上書き作成）。
    // 例: build/release から起動すると build/release/dx12_engine.log に出る。
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("dx12_engine.log", true));
    } catch (...) { /* ファイルを開けなくてもコンソールのみで続行 */ }

    // エディタのコンソールパネル用（インメモリ・直近 kBufCap 件）
    sinks.push_back(std::make_shared<RingBufferSink>());

    s_logger = std::make_shared<spdlog::logger>("DX12Engine", sinks.begin(), sinks.end());
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    s_logger->set_level(spdlog::level::debug);
    s_logger->flush_on(spdlog::level::debug);  // クラッシュ前でも残るよう即時フラッシュ
    spdlog::register_logger(s_logger);

    s_logger->info("Logger initialized");
}

uint64_t Logger::ReadBuffered(uint64_t sinceId, std::vector<LogEntry>& out)
{
    std::lock_guard<std::mutex> lk(g_bufMutex);
    uint64_t latest = sinceId;
    for (const auto& e : g_buf)
    {
        if (e.id > sinceId)
        {
            out.push_back(e);
            latest = e.id;
        }
    }
    return latest;
}

void Logger::Shutdown()
{
    if (s_logger)
    {
        s_logger->info("Logger shutting down");
        spdlog::drop("DX12Engine");
        s_logger.reset();
    }
}

} // namespace dx12e
