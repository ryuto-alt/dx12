#include "resource/MaterialAssetIO.h"

#include <nlohmann/json.hpp>

namespace dx12e
{

using json = nlohmann::json;

bool ParseMaterialAsset(const std::vector<uint8_t>& jsonBytes, MaterialAssetData& out)
{
    if (jsonBytes.empty())
        return false;

    json j;
    try
    {
        j = json::parse(jsonBytes.begin(), jsonBytes.end());
    }
    catch (const json::exception&)
    {
        return false;
    }
    if (!j.is_object())
        return false;

    MaterialAssetData data;
    data.name               = j.value("name", "");
    data.albedoPath          = j.value("albedo", "");
    data.normalPath          = j.value("normal", "");
    data.metalRoughnessPath  = j.value("metalRoughness", "");
    data.emissivePath        = j.value("emissive", "");
    data.metallic            = j.value("metallic", 1.0f);
    data.roughness           = j.value("roughness", 1.0f);
    data.emissiveIntensity   = j.value("emissiveIntensity", 0.0f);
    data.source              = j.value("source", "");
    data.license             = j.value("license", "");

    if (j.contains("uvTiling") && j["uvTiling"].is_array() && j["uvTiling"].size() >= 2)
    {
        data.uvTilingU = j["uvTiling"][0].get<f32>();
        data.uvTilingV = j["uvTiling"][1].get<f32>();
    }

    if (j.contains("emissiveColor") && j["emissiveColor"].is_array() && j["emissiveColor"].size() >= 3)
    {
        for (int i = 0; i < 3; ++i) data.emissiveColor[i] = j["emissiveColor"][i].get<f32>();
    }
    // 色を書かずにテクスチャだけ指定した .dxmat は「素通し」として白 x 1 とみなす
    // (glTF の emissiveFactor 省略時と同じ扱い。ここで 0 のままだと光らない)。
    if (!data.emissivePath.empty() && data.emissiveColor[0] <= 0.0f
        && data.emissiveColor[1] <= 0.0f && data.emissiveColor[2] <= 0.0f)
    {
        data.emissiveColor[0] = data.emissiveColor[1] = data.emissiveColor[2] = 1.0f;
        if (data.emissiveIntensity <= 0.0f) data.emissiveIntensity = 1.0f;
    }

    // 最低限テクスチャを1枚も参照していないマテリアルは無効(空の .dxmat 等)。
    if (data.albedoPath.empty() && data.normalPath.empty() && data.metalRoughnessPath.empty()
        && data.emissivePath.empty())
        return false;

    out = std::move(data);
    return true;
}

std::string SerializeMaterialAsset(const MaterialAssetData& data)
{
    json j;
    j["version"] = 1;
    j["name"] = data.name;
    if (!data.albedoPath.empty())         j["albedo"] = data.albedoPath;
    if (!data.normalPath.empty())         j["normal"] = data.normalPath;
    if (!data.metalRoughnessPath.empty()) j["metalRoughness"] = data.metalRoughnessPath;
    if (!data.emissivePath.empty())       j["emissive"] = data.emissivePath;
    j["metallic"]  = data.metallic;
    j["roughness"] = data.roughness;
    j["uvTiling"]  = { data.uvTilingU, data.uvTilingV };
    // 自己発光は「使っている .dxmat にだけ」書く = 既存ファイルを読んで書き戻しても
    // キーが 1 つも増えない(差分が出ない)。
    if (data.emissiveIntensity > 0.0f)
    {
        j["emissiveColor"]     = { data.emissiveColor[0], data.emissiveColor[1], data.emissiveColor[2] };
        j["emissiveIntensity"] = data.emissiveIntensity;
    }
    if (!data.source.empty())  j["source"]  = data.source;
    if (!data.license.empty()) j["license"] = data.license;
    return j.dump(2);
}

} // namespace dx12e
