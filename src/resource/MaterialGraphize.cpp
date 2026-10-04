#include "resource/MaterialGraphize.h"

#include "renderer/matgraph/GraphIO.h"
#include "core/AtomicFile.h"
#include "renderer/matgraph/PbrTemplate.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace dx12e
{
namespace
{

bool ReadFileBytes(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileBytes(const fs::path& p, const std::string& bytes)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return static_cast<bool>(dx12e::atomicfile::WriteFile(p, bytes));
}

matgraph::LegacyPbr ToLegacy(const MaterialAssetData& d)
{
    matgraph::LegacyPbr m;
    m.albedo = d.albedoPath;
    m.normal = d.normalPath;
    m.metalRoughness = d.metalRoughnessPath;
    m.emissive = d.emissivePath;
    m.metallic = d.metallic;
    m.roughness = d.roughness;
    for (int i = 0; i < 3; ++i) m.emissiveColor[i] = d.emissiveColor[i];
    m.emissiveIntensity = d.emissiveIntensity;
    return m;
}

} // namespace

matgraph::ParamOverrides ToParamOverrides(const MaterialAssetData& data)
{
    matgraph::ParamOverrides out;
    for (const auto& [name, p] : data.graphParams)
    {
        matgraph::ParamOverride o;
        switch (p.kind)
        {
        case MaterialAssetData::GraphParam::Kind::Scalar:
            o.v[0] = p.v[0];
            break;
        case MaterialAssetData::GraphParam::Kind::Vector:
            for (int i = 0; i < 4; ++i) o.v[i] = (i < p.n) ? p.v[i] : 0.0f;
            if (p.n == 3) o.v[3] = 1.0f;
            break;
        case MaterialAssetData::GraphParam::Kind::Texture:
            o.isTexture = true;
            o.texture = p.texture;
            break;
        }
        out[name] = std::move(o);
    }
    return out;
}

GraphizeResult GraphizeLegacyMaterial(const MaterialAssetData& legacy, const std::string& graphDirRel)
{
    GraphizeResult r;
    if (legacy.IsGraph())
    {
        r.error = "この材質は既にグラフ材質です（graph: " + legacy.graphPath + "）";
        return r;
    }
    const matgraph::LegacyPbr m = ToLegacy(legacy);

    matgraph::MaterialGraph g;
    matgraph::BuildPbrTemplateGraph(m, g);
    r.templateRel = graphDirRel + matgraph::PbrTemplateName(m) + ".dxmg";
    r.templateText = matgraph::SaveDxmg(g, /*includeView=*/false);

    MaterialAssetData inst;
    inst.name = legacy.name;
    inst.graphPath = r.templateRel;
    inst.uvTilingU = legacy.uvTilingU;
    inst.uvTilingV = legacy.uvTilingV;
    inst.source = legacy.source;
    inst.license = legacy.license;
    inst.metallic = 0.0f;     // 代理値（影・深度・パストレ用）。書き出しには出ない
    inst.roughness = 0.5f;
    for (const matgraph::PbrParam& p : matgraph::PbrTemplateParams(m))
    {
        MaterialAssetData::GraphParam gp;
        switch (p.kind)
        {
        case matgraph::PbrParam::Kind::Scalar:
            gp.kind = MaterialAssetData::GraphParam::Kind::Scalar;
            gp.n = 1;
            gp.v[0] = p.v[0];
            break;
        case matgraph::PbrParam::Kind::Vector:
            gp.kind = MaterialAssetData::GraphParam::Kind::Vector;
            gp.n = p.n;
            for (int i = 0; i < 4; ++i) gp.v[i] = p.v[i];
            break;
        case matgraph::PbrParam::Kind::Texture:
            gp.kind = MaterialAssetData::GraphParam::Kind::Texture;
            gp.texture = p.texture;
            break;
        }
        inst.graphParams[p.name] = std::move(gp);
    }
    r.instanceText = SerializeMaterialAsset(inst);
    r.instance = std::move(inst);
    r.ok = true;
    return r;
}

GraphizeFileResult GraphizeMaterialFile(const std::string& assetsDir, const std::string& dxmatRel, bool dryRun)
{
    GraphizeFileResult out;
    const fs::path dxmatPath = fs::path(assetsDir) / dxmatRel;
    std::string bytes;
    if (!ReadFileBytes(dxmatPath, bytes))
    {
        out.conv.error = "材質ファイルを読めません: " + dxmatRel;
        return out;
    }
    MaterialAssetData legacy;
    if (!ParseMaterialAsset(std::vector<uint8_t>(bytes.begin(), bytes.end()), legacy))
    {
        out.conv.error = "材質ファイルを解釈できません（テクスチャを 1 枚も参照していないか、壊れています）: " + dxmatRel;
        return out;
    }
    out.conv = GraphizeLegacyMaterial(legacy);
    if (!out.conv.ok || dryRun) return out;

    // 1) テンプレート（無い / 内容が違うときだけ書く）
    const fs::path tplPath = fs::path(assetsDir) / out.conv.templateRel;
    std::string existing;
    if (!ReadFileBytes(tplPath, existing) || existing != out.conv.templateText)
    {
        if (!WriteFileBytes(tplPath, out.conv.templateText))
        {
            out.conv.ok = false;
            out.conv.error = "テンプレートグラフを書けません: " + out.conv.templateRel;
            return out;
        }
        out.wroteTemplate = true;
    }
    // 2) 原本の退避（最初の 1 回だけ。既にあれば守る）
    const fs::path bakPath = fs::path(assetsDir) / (dxmatRel + ".bak");
    std::error_code ec;
    if (!fs::exists(bakPath, ec))
    {
        if (!WriteFileBytes(bakPath, bytes))
        {
            out.conv.ok = false;
            out.conv.error = "バックアップ（.bak）を書けません: " + dxmatRel + ".bak";
            return out;
        }
        out.wroteBackup = true;
    }
    // 3) 変換後の .dxmat
    if (!WriteFileBytes(dxmatPath, out.conv.instanceText))
    {
        out.conv.ok = false;
        out.conv.error = "変換後の材質を書けません: " + dxmatRel;
        return out;
    }
    out.wroteInstance = true;
    return out;
}

} // namespace dx12e
