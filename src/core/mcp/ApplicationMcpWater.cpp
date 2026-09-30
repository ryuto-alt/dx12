// ===========================================================================
// MCP: 水面 W1（WaterBody + 専用パス）
//   water_apply_preset … WaterBody へプリセット（湖 / 海 / 川 / プール / 沼）を適用する。無ければ作る（name 指定 + create:true）
// ---------------------------------------------------------------------------
// 作成・個別パラメータの設定は既存の経路（create_entity type:"water" / set_component component:"waterBody" data:{...}）で行う。
// 統計は perf_stats の water ブロック（bodies / rings / triangles / gpuMs の内訳 / VRAM）。
// ★undo: WaterBody を値で追跡する（McpUndo().TrackByJsonKey）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "core/mcp/McpManifestBuild.h"
#include "renderer/water/WaterMath.h"

namespace dx12e
{
using namespace appdetail;
using namespace mcpdata;   // P(...)

void Application::RegisterMcpWaterMethods()
{
    using nlohmann::json;
    McpMeta m;
    m.summary  = "水面（WaterBody）へプリセットを適用する。lake=湖 / ocean=海（無限平面・波高め）/ river=川（流れあり）/ pool=プール（透明）/ swamp=沼。"
                 "形（shape/size/polygon）・高さ・種・時間は変えず、波・色・吸収・泡・岸のパラメータだけを差し替える。"
                 "name か entity で指定。create:true と name を渡すと、無ければ水面エンティティを新しく作る（position の y が水面の高さ）。Undo できる";
    m.keywords = "water preset lake ocean river pool swamp 水 水面 湖 海 川 プール 沼 プリセット waterbody";
    m.category = "scene"; m.group = "water"; m.target = "preset";
    m.effect = McpEffect::WriteScene; m.mode = "editor"; m.timeoutMs = 10000; m.idempotent = true;
    m.aliases = {"dx12_water_apply_preset"};
    m.params = {
        P("preset", "string", true, nullptr, nullptr, nullptr, nullptr, "lake / ocean / river / pool / swamp（日本語の 湖 / 海 / 川 / プール / 沼 も可）"),
        P("name", "string", false, nullptr, nullptr, nullptr, nullptr, "対象の名前（WaterBody を持つエンティティ）"),
        P("entity", "int", false, nullptr, nullptr, nullptr, nullptr, "対象の entity id"),
        P("create", "bool", false, nullptr, nullptr, nullptr, "false", "true で、対象が無いとき name の水面エンティティを作る"),
        P("position", "vec3", false, nullptr, nullptr, nullptr, nullptr, "create:true のときの位置（y = 水面の高さ）"),
        P("size", "any", false, nullptr, nullptr, nullptr, nullptr, "create:true のときの矩形の全幅 x,z（ocean は無限平面なので無視）"),
    };
    m.examples = {{"{\"preset\":\"lake\",\"name\":\"Lake\",\"create\":true,\"position\":[0,1,0],\"size\":[120,80]}", "湖を作る"},
                  {"{\"preset\":\"ocean\",\"name\":\"Lake\"}", "既存の水面を海にする"}};
    m.next = {{"set_component", "component:\"waterBody\" で個別のパラメータを変える"}, {"perf_stats", "water ブロックで GPU 時間を見る"}};
    McpDefine("water_apply_preset", McpMeta(m), DX12E_MCP_HANDLER
        {
            const std::string preset = params.value("preset", std::string());
            if (preset.empty()) throw McpError(McpErr::InvalidParam, "preset が要ります", "lake / ocean / river / pool / swamp のどれか");
            auto& reg = m_scene->GetRegistry();
            const bool create = params.value("create", false);
            entt::entity e = entt::null;
            bool created = false;
            if (params.contains("entity") && params["entity"].is_number())
            {
                e = static_cast<entt::entity>(params["entity"].get<u32>());
                if (!reg.valid(e)) throw McpError(McpErr::NotFound, "invalid entity id", "dx12_list_entities で取り直してください");
            }
            else if (params.contains("name") && params["name"].is_string())
            {
                const auto ent = m_scene->FindEntity(params["name"].get<std::string>());
                if (ent.IsValid()) e = ent.GetHandle();
            }
            if (e == entt::null)
            {
                if (!create) throw McpError(McpErr::NotFound, "対象の水面がありません", "name / entity で指定するか、create:true と name で作ってください");
                if (busyPlaying) throw McpError(McpErr::ModeConflict, "Play 中は作れません", "先に dx12_stop してください");
                e = reg.create();
                reg.emplace<NameTag>(e, NameTag{params.value("name", std::string("Water"))});
                Transform t{};
                if (params.contains("position") && params["position"].is_array() && params["position"].size() == 3)
                    t.position = McpF3(params["position"]);
                reg.emplace<Transform>(e, t);
                WaterBody wb;
                if (params.contains("size") && params["size"].is_array() && params["size"].size() >= 2)
                {
                    wb.size.x = params["size"][0].get<float>();
                    wb.size.y = params["size"][1].get<float>();
                }
                reg.emplace<WaterBody>(e, wb);
                created = true;
            }
            else if (!reg.all_of<WaterBody>(e))
            {
                if (!create) throw McpError(McpErr::InvalidParam, "このエンティティは WaterBody を持っていません", "create:true で付けるか、set_component / add_component で付けてください");
                reg.emplace<WaterBody>(e);
                created = true;
            }
            if (!created) McpUndo().TrackByJsonKey(e, "waterBody");
            auto& wb = reg.get<WaterBody>(e);
            if (!water::ApplyPreset(wb, preset))
                throw McpError(McpErr::InvalidParam, "未知のプリセット: " + preset, "lake / ocean / river / pool / swamp（湖 / 海 / 川 / プール / 沼）");
            resp["ok"] = true;
            resp["result"] = {{"entityId", static_cast<u32>(e)}, {"preset", water::NormalizePresetName(preset)}, {"created", created},
                              {"shape", wb.shape}, {"waveAmplitude", wb.waveAmplitude}, {"wavelength", wb.wavelength}};
        });
}

} // namespace dx12e
