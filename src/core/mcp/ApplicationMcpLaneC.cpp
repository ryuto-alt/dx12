// ===========================================================================
// レーン C の MCP 登録口（W05-01b。EnsureMcpMethodTable() が呼ぶ）。
// 中身は各レーンが書く。Application.h は変えない（W7 まで凍結）。
// Application の private に触れる処理は core/AppAccess.h のレーン C のブロックへ宣言する。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/AppAccess.h"
#include "core/QualityPresetApply.h"
#include "renderer/QualityPreset.h"

namespace dx12e
{
namespace
{
nlohmann::json QualityPresetJson(QualityPresetId id)
{
    const QualityPreset& p = GetPreset(id);
    nlohmann::json j = nlohmann::json::object();
    j["preset"]    = p.mcpName;
    j["label"]     = p.label;
    j["hasBudget"] = p.hasBudget;
    if (p.hasBudget)
    {
        j["targetFps"] = p.targetFps;
        j["output"]    = { {"width", p.outW}, {"height", p.outH} };
        j["p95Ms"]     = p.p95Ms;
        j["p99Ms"]     = p.p99Ms;
        j["vfx"]       = { {"emissionMultiplier", p.emissionMultiplier}, {"maxParticles", p.maxParticles} };
    }
    return j;
}
} // namespace

void Application::RegisterMcpLaneCMethods()
{
    using namespace appdetail;
    // W1-C1: 品質プリセットの口。C1 は選択の記録だけ（絵への反映と Undo は C7）。
    McpDefine("get_quality_preset", "", DX12E_MCP_HANDLER
        {
            resp["ok"]     = true;
            resp["result"] = QualityPresetJson(GetQualityPreset(*this));
        });
    McpDefine("set_quality_preset", "preset:string", DX12E_MCP_HANDLER
        {
            const std::string name = params.value("preset", std::string());
            QualityPresetId id{};
            if (!ParseQualityPreset(name.c_str(), id))
                throw McpError(McpErr::InvalidParam, "preset は low | medium | high | epic | cinematic",
                               "例: preset:'epic'");
            if (!SetQualityPreset(*this, id, /*recordUndo*/ true))
                throw McpError(McpErr::Internal, "品質プリセットを設定できなかった", "もう一度試す");
            resp["ok"]     = true;
            resp["result"] = QualityPresetJson(id);
        });
}
} // namespace dx12e
