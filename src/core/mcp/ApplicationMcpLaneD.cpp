// ===========================================================================
// レーン D の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）。
// 中身は各レーンが書く。Application.h は変えない（W7 まで凍結）。
// Application の private に触れる処理は core/AppAccess.h のレーン D のブロックへ宣言する。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
void Application::RegisterMcpLaneDMethods()
{
}
} // namespace dx12e
