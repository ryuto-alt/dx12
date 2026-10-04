#include "resource/MaterialAssetIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace dx12e
{

using json = nlohmann::json;

namespace
{

// float → JSON の数値。float をそのまま代入すると double へ広がって 0.6f が 0.6000000238418579 になるので、
// float32 の最短ラウンドトリップ表記を経由する（マテリアルグラフの params 専用。従来キーの書き方は変えない）。
double ShortFloat(f32 v)
{
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec != std::errc()) return static_cast<double>(v);
    *r.ptr = '\0';
    return std::strtod(buf, nullptr);
}

void ParseGraphParams(const json& j, MaterialAssetData& data)
{
    if (!j.contains("params") || !j["params"].is_object()) return;
    for (auto it = j["params"].begin(); it != j["params"].end(); ++it)
    {
        MaterialAssetData::GraphParam p;
        const json& v = it.value();
        if (v.is_number())
        {
            p.kind = MaterialAssetData::GraphParam::Kind::Scalar;
            p.n    = 1;
            p.v[0] = v.get<f32>();
        }
        else if (v.is_array() && v.size() >= 2 && v.size() <= 4)
        {
            bool allNum = true;
            for (const auto& e : v) allNum = allNum && e.is_number();
            if (!allNum) continue;
            p.kind = MaterialAssetData::GraphParam::Kind::Vector;
            p.n    = static_cast<int>(v.size());
            for (int i = 0; i < p.n; ++i) p.v[i] = v[static_cast<size_t>(i)].get<f32>();
        }
        else if (v.is_string())
        {
            p.kind    = MaterialAssetData::GraphParam::Kind::Texture;
            p.texture = v.get<std::string>();
        }
        else
            continue;   // 型が合わない値は読み捨てる（ロードを止めない）
        data.graphParams[it.key()] = std::move(p);
    }
}

} // namespace

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
    data.aoStrength          = std::clamp(j.value("aoStrength", 0.0f), 0.0f, 1.0f);
    data.source              = j.value("source", "");
    data.license             = j.value("license", "");

    // ★マテリアルグラフのインスタンス（"graph" キー）。無ければ以下は一切通らない＝従来と同じ動作。
    if (j.contains("graph") && j["graph"].is_string())
        data.graphPath = j["graph"].get<std::string>();
    if (data.IsGraph())
    {
        // 従来の 4 枚 / スカラーは無視する。代理値（影・深度・パストレ用）は既定 0 / 0.5。
        data.albedoPath.clear();
        data.normalPath.clear();
        data.metalRoughnessPath.clear();
        data.emissivePath.clear();
        data.metallic          = j.value("metallic", 0.0f);
        data.roughness         = j.value("roughness", 0.5f);
        data.emissiveIntensity = 0.0f;
        ParseGraphParams(j, data);
    }

    if (j.contains("uvTiling") && j["uvTiling"].is_array() && j["uvTiling"].size() >= 2)
    {
        data.uvTilingU = j["uvTiling"][0].get<f32>();
        data.uvTilingV = j["uvTiling"][1].get<f32>();
    }

    if (!data.IsGraph() && j.contains("emissiveColor") && j["emissiveColor"].is_array() && j["emissiveColor"].size() >= 3)
    {
        for (int i = 0; i < 3; ++i) data.emissiveColor[i] = j["emissiveColor"][i].get<f32>();
    }
    // 色を書かずにテクスチャだけ指定した .dxmat は「素通し」として白 x 1 とみなす
    // (glTF の emissiveFactor 省略時と同じ扱い。ここで 0 のままだと光らない)。
    if (!data.IsGraph() && !data.emissivePath.empty() && data.emissiveColor[0] <= 0.0f
        && data.emissiveColor[1] <= 0.0f && data.emissiveColor[2] <= 0.0f)
    {
        data.emissiveColor[0] = data.emissiveColor[1] = data.emissiveColor[2] = 1.0f;
        if (data.emissiveIntensity <= 0.0f) data.emissiveIntensity = 1.0f;
    }

    // 最低限テクスチャを1枚も参照していないマテリアルは無効(空の .dxmat 等)。グラフ材質は免除（値は params にある）。
    if (!data.IsGraph()
        && data.albedoPath.empty() && data.normalPath.empty() && data.metalRoughnessPath.empty()
        && data.emissivePath.empty())
        return false;

    out = std::move(data);
    return true;
}

std::string SerializeMaterialAsset(const MaterialAssetData& data)
{
    json j;
    // ★グラフ材質（"graph" キーあり）は使っているキーだけを書く（version 2）。従来経路の書き出しは下のまま無改造。
    if (data.IsGraph())
    {
        j["version"] = 2;
        j["name"]    = data.name;
        j["graph"]   = data.graphPath;
        if (!data.graphParams.empty())
        {
            json params = json::object();
            for (const auto& [name, p] : data.graphParams)
            {
                switch (p.kind)
                {
                case MaterialAssetData::GraphParam::Kind::Scalar:
                    params[name] = ShortFloat(p.v[0]);
                    break;
                case MaterialAssetData::GraphParam::Kind::Vector:
                {
                    json arr = json::array();
                    for (int i = 0; i < p.n && i < 4; ++i) arr.push_back(ShortFloat(p.v[i]));
                    params[name] = std::move(arr);
                    break;
                }
                case MaterialAssetData::GraphParam::Kind::Texture:
                    params[name] = p.texture;
                    break;
                }
            }
            j["params"] = std::move(params);
        }
        if (data.uvTilingU != 1.0f || data.uvTilingV != 1.0f)
            j["uvTiling"] = { ShortFloat(data.uvTilingU), ShortFloat(data.uvTilingV) };
        if (!data.source.empty())  j["source"]  = data.source;
        if (!data.license.empty()) j["license"] = data.license;
        return j.dump(2);
    }

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
    // AO も「使っている .dxmat にだけ」書く（既存ファイルの書き戻しでキーが増えない）。
    if (data.aoStrength > 0.0f && !data.metalRoughnessPath.empty())
        j["aoStrength"] = data.aoStrength;
    if (!data.source.empty())  j["source"]  = data.source;
    if (!data.license.empty()) j["license"] = data.license;
    return j.dump(2);
}

} // namespace dx12e
