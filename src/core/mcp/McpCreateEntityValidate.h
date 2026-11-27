#pragma once
// create_entity の引数検査を McpError にして投げる（W1-FU-E-32 の再検査）。
//   実行側（ApplicationMcpEntity.cpp）と dryRun（ApplicationMcpSafety.inc）が同じ文言・cause・details を返すよう 1 か所にまとめた。
//   純関数の検査本体は McpCreateEntityParams.h（Application に依存しない。ctest 対象）。こちらは McpError に依存する。
#include "core/ApplicationInternal.h"
#include "core/mcp/McpCreateEntityParams.h"

namespace dx12e
{
inline void ThrowIfInvalidCreateEntityParams(const nlohmann::json& params)
{
    using namespace appdetail;
    std::vector<std::string> keys;
    for (auto it = params.begin(); it != params.end(); ++it) keys.push_back(it.key());
    const std::string typeIn = (params.contains("type") && params["type"].is_string()) ? params["type"].get<std::string>() : std::string();
    std::string bad;
    switch (mcpcreate::Validate(keys, typeIn, bad))
    {
    case mcpcreate::Check::UnknownKey:
    {
        McpError err(McpErr::InvalidParam, "unknown parameter '" + bad + "' for create_entity",
            "Accepted: type (required), name, position, parent, parentName, size, subdivisions. "
            "向きや拡大は作成後に dx12_set_transform、色や質感は dx12_set_component / dx12_material_apply で変える");
        err.name  = "E_UNKNOWN_PARAM";
        err.cause = "create_entity が受け取らない引数が渡された（黙って無視すると意図と違うものが作られる）";
        if (const std::string closest = mcpcreate::NearestKey(bad); !closest.empty()) err.didYouMean.push_back(closest);
        throw err;
    }
    case mcpcreate::Check::MissingType:
    {
        McpError err(McpErr::InvalidParam, "create_entity: missing required param 'type'",
            "type は必須: box, sphere, plane, empty, camera, light_directional, light_point, light_spot, "
            "particle_emitter, trigger, decal, water, ui_*。3D の箱なら type:\"box\"、何も描かない親なら type:\"empty\"");
        err.name  = "E_MISSING_PARAM";
        err.cause = "必須の引数 'type' が渡されていない（黙って box にしない）";
        err.details = {{"param", "type"}, {"type", "string"}};
        throw err;
    }
    case mcpcreate::Check::ParentOnCanvas:
        throw McpError(McpErr::InvalidParam, "'" + bad + "' cannot be used with type 'ui_canvas' (a canvas is always a root)",
            "ui_canvas は常にルートに作られる。UI 要素（ui_image / ui_text など）には parent / parentName が使える");
    case mcpcreate::Check::Ok: break;
    }
}
} // namespace dx12e
