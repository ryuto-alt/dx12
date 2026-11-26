#include "renderer/vfx/VfxRuntime.h"

namespace dx12e::vfx
{
Runtime& Runtime::Get()
{
    static Runtime r;
    return r;
}

void Runtime::ReportGpuFailure(const std::string& reason)
{
    m_stats = Stats{};   // 失敗前の古い値（instances / poolUsed / list）を残さない
    m_stats.gpuFailed = true;
    m_stats.lastError = reason;
}

std::string Runtime::DescribeGpuFailure() const
{
    if (!m_stats.gpuFailed) return {};
    return m_stats.lastError.empty() ? std::string("UnoFX: GPU の初期化に失敗した（理由は不明。ログを確認）") : m_stats.lastError;
}
} // namespace dx12e::vfx
