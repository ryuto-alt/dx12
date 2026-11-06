// ===========================================================================
// MCP: 反射プローブ（ReflectionProbe）
//   reflection_capture      … プローブを焼き直す（entity / name で 1 個、省略で全部）。次のフレームで撮影される
//   reflection_probe_stats  … 数・解像度・VRAM と、プローブごとの焼き済み / 焼き待ち / 最終キャプチャからの秒数
// 作成・個別パラメータは既存の経路（create_entity + add_component / set_component component:"reflectionProbe"）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)

void Application::RegisterMcpReflectionProbeMethods()
{
    using nlohmann::json;
    {
        McpMeta m;
        m.summary  = "反射プローブ（ReflectionProbe）を焼き直す。entity か name で 1 個、どちらも省略すると全部。"
                     "実際の撮影は次のフレーム（プローブの位置から 6 面を描いて GGX プリフィルタ）。"
                     "進み具合は reflection_probe_stats の captured / ageSec で見る。プローブ自身は写らない";
        m.keywords = "reflection probe capture bake 反射 プローブ 焼く キャプチャ 室内";
        m.category = "scene"; m.group = "reflection"; m.target = "probe";
        m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 10000; m.idempotent = true;
        m.aliases = {"dx12_reflection_capture"};
        m.params = {
            P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "対象の entity id（ReflectionProbe を持つもの）"),
            P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "対象の名前。entity と排他"),
        };
        m.examples = {{"{}", "シーンの全プローブを焼き直す"}, {"{\"name\":\"LobbyProbe\"}", "1 個だけ焼き直す"}};
        m.next = {{"reflection_probe_stats", "焼き済みかどうか・最終キャプチャ時刻を見る"}};
        McpDefine("reflection_capture", McpMeta(m), DX12E_MCP_HANDLER
            {
                if (!m_reflProbes || !m_reflProbes->IsReady())
                    throw McpError(McpErr::ModeConflict, "反射プローブが初期化されていません", "クラスタードライティングが使えない環境です");
                auto& reg = m_scene->GetRegistry();
                entt::entity e = entt::null;
                if (params.contains("entity") && params["entity"].is_number())
                {
                    e = static_cast<entt::entity>(params["entity"].get<u32>());
                    if (!reg.valid(e) || !reg.all_of<ReflectionProbe>(e))
                        throw McpError(McpErr::NotFound, "ReflectionProbe を持つエンティティではありません", "reflection_probe_stats で一覧を取り直してください");
                }
                else if (params.contains("name") && params["name"].is_string())
                {
                    const auto ent = m_scene->FindEntity(params["name"].get<std::string>());
                    if (!ent.IsValid() || !reg.all_of<ReflectionProbe>(ent.GetHandle()))
                        throw McpError(McpErr::NotFound, "その名前の ReflectionProbe がありません", "reflection_probe_stats で一覧を取り直してください");
                    e = ent.GetHandle();
                }
                const int n = RequestReflectionProbeCapture(e);
                resp["ok"] = true;
                resp["result"] = {{"queued", n}, {"note", "次のフレームで撮影されます。reflection_probe_stats で captured / ageSec を確認"}};
            });
    }
    {
        McpMeta m;
        m.summary  = "反射プローブの統計。数 / 上限 / 撮影解像度 / 畳み込み解像度 / VRAM と、プローブごとの"
                     "焼き済み・焼き待ち・最終キャプチャからの秒数・箱の設定を返す（読み取り専用）";
        m.keywords = "reflection probe stats 反射 プローブ 統計 一覧";
        m.category = "scene"; m.group = "reflection"; m.target = "probe";
        m.effect = McpEffect::Read; m.mode = "any"; m.timeoutMs = 10000; m.idempotent = true;
        m.aliases = {"dx12_reflection_probe_stats"};
        m.params = {};
        m.next = {{"reflection_capture", "焼き直す"}};
        McpDefine("reflection_probe_stats", McpMeta(m), DX12E_MCP_HANDLER
            {
                resp["ok"] = true;
                resp["result"] = ReflectionProbeStatsJson();
            });
    }
}

} // namespace dx12e
