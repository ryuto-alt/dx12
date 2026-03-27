#include "Logger.h"

namespace dx12e
{

std::shared_ptr<spdlog::logger> Logger::s_logger = nullptr;

void Logger::Init()
{
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    s_logger = std::make_shared<spdlog::logger>("DX12Engine", consoleSink);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    s_logger->set_level(spdlog::level::debug);
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
