#include "Logger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace dx12e
{

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

    s_logger = std::make_shared<spdlog::logger>("DX12Engine", sinks.begin(), sinks.end());
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    s_logger->set_level(spdlog::level::debug);
    s_logger->flush_on(spdlog::level::debug);  // クラッシュ前でも残るよう即時フラッシュ
    spdlog::register_logger(s_logger);

    s_logger->info("Logger initialized");
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
