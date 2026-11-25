// ===========================================================================
// レーン B の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）。
// 中身は各レーンが書く。Application.h は変えない（W7 まで凍結）。
// Application の private に触れる処理は core/AppAccess.h のレーン B のブロックへ宣言する。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AppAccess.h"

namespace dx12e
{
void Application::RegisterMcpLaneBMethods()
{
}

uint64_t AppAccess::ModelThumbHandle(Application& app, const std::string& path)
{
    return app.m_thumbRenderer ? app.m_thumbRenderer->GetCachedHandle(path) : 0;
}
} // namespace dx12e
