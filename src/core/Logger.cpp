#include "Logger.h"
#include "core/CrashReport.h"
#include "core/ErrorLog.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/base_sink.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

namespace dx12e
{
namespace
{
// ---- エディタコンソール / MCP log_read 用のインメモリ リングバッファ ----
// どのスレッドからのログも受ける（Git/Updater 等のワーカー含む）ため LogRing 内部の mutex で保護。
constexpr size_t kBufCap = 4096;

LogRing& Ring()
{
    static LogRing r(kBufCap);
    return r;
}

std::atomic<uint64_t> g_frame{0};
std::atomic<bool>     g_playing{false};

// 1 件をリングへ積み、エラー集約へ流す。
void Record(LogEntry e)
{
    const LogEntry pushed = Ring().Push(std::move(e));
    ErrorLog::Global().Observe(pushed);
}

class RingBufferSink : public spdlog::sinks::base_sink<std::mutex>
{
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        LogEntry e;
        e.level   = static_cast<int>(msg.level);
        e.epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(msg.time.time_since_epoch()).count();
        const std::string full(msg.payload.data(), msg.payload.size());
        e.category = ClassifyLogCategory(e.level, full);
        SplitLuaStack(full, e.text, e.stack);
        e.frame   = g_frame.load(std::memory_order_relaxed);
        e.playing = g_playing.load(std::memory_order_relaxed);
        Record(std::move(e));
    }
    void flush_() override {}
};
} // namespace

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;

void Logger::Init()
{
    // ★前回クラッシュしていたら、dx12_engine.log を truncate する前に報告として保管する（M9）。
    //   （クラッシュ経路には手を入れず、次回起動時に拾う方式。crash_report / doctor が読む）
    std::string crashId;
    try { CrashReport::CaptureIfPresent(std::filesystem::current_path(), CrashReport::DefaultRoot(), &crashId); }
    catch (...) {}

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    // GUI アプリはコンソールが無いので、ログをファイルにも残す（CWD 直下に上書き作成）。
    // 例: build/release から起動すると build/release/dx12_engine.log に出る。
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("dx12_engine.log", true));
    } catch (...) { /* ファイルを開けなくてもコンソールのみで続行 */ }

    // エディタのコンソールパネル / log_read 用（インメモリ・直近 kBufCap 件）
    sinks.push_back(std::make_shared<RingBufferSink>());

    s_logger = std::make_shared<spdlog::logger>("DX12Engine", sinks.begin(), sinks.end());
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    s_logger->set_level(spdlog::level::debug);
    s_logger->flush_on(spdlog::level::debug);  // クラッシュ前でも残るよう即時フラッシュ
    spdlog::register_logger(s_logger);

    s_logger->info("Logger initialized");
    if (!crashId.empty())
        s_logger->warn("前回の実行はクラッシュで終了していました（報告 {}。MCP の crash_report / dx12_doctor で読めます）", crashId);
}

uint64_t Logger::ReadBuffered(uint64_t sinceId, std::vector<LogEntry>& out)
{
    return Ring().ReadLegacy(sinceId, out);
}

void Logger::SetFrame(uint64_t frame, bool playing)
{
    g_frame.store(frame, std::memory_order_relaxed);
    g_playing.store(playing, std::memory_order_relaxed);
}

LogQueryResult Logger::ReadStructured(const LogFilter& f)
{
    return Ring().Query(f);
}

uint64_t Logger::PushAux(int level, const std::string& category, const std::string& msg)
{
    LogEntry e;
    e.level    = level;
    e.category = category;
    e.text     = msg;
    e.aux      = true;
    e.frame    = g_frame.load(std::memory_order_relaxed);
    e.playing  = g_playing.load(std::memory_order_relaxed);
    const LogEntry pushed = Ring().Push(std::move(e));
    ErrorLog::Global().Observe(pushed);
    return pushed.id;
}

uint64_t Logger::LatestSeq()    { return Ring().LatestSeq(); }
uint64_t Logger::SessionId()    { return Ring().SessionId(); }
size_t   Logger::RingCapacity() { return Ring().Capacity(); }

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
