#include "resource/VgeoProxyLoader.h"

#include "core/Logger.h"
#include "core/vfs/Vfs.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Texture.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"
#include "renderer/vg/VgeoFormat.h"
#include "resource/ResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <Windows.h>

namespace dx12e
{
namespace
{
// ProxyVertex（.vgeo）と Mesh.h の Vertex は同一レイアウト。片方が変わったらここで落ちる。
static_assert(sizeof(Vertex) == vg::kProxyVertexSize, "Vertex must stay 96 B (.vgeo PROXY layout)");
static_assert(sizeof(vg::ProxyVertex) == sizeof(Vertex), "ProxyVertex / Vertex size mismatch");
static_assert(offsetof(Vertex, position)    == offsetof(vg::ProxyVertex, position),    "position offset");
static_assert(offsetof(Vertex, normal)      == offsetof(vg::ProxyVertex, normal),      "normal offset");
static_assert(offsetof(Vertex, color)       == offsetof(vg::ProxyVertex, color),       "color offset");
static_assert(offsetof(Vertex, texCoord)    == offsetof(vg::ProxyVertex, texCoord),    "texCoord offset");
static_assert(offsetof(Vertex, tangent)     == offsetof(vg::ProxyVertex, tangent),     "tangent offset");
static_assert(offsetof(Vertex, boneIndices) == offsetof(vg::ProxyVertex, boneIndices), "boneIndices offset");
static_assert(offsetof(Vertex, boneWeights) == offsetof(vg::ProxyVertex, boneWeights), "boneWeights offset");

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// .vgeo のあるフォルダ基準の相対パス → 実在するテクスチャの絶対パス。無い / 危ないパスは空。
std::filesystem::path ResolveVgeoTexture(const std::filesystem::path& vgeoDir, const char* rel)
{
    if (!rel || !rel[0]) return {};
    // 形式（C24）が保証しているが、平置きの未署名ファイルなので念のためここでも検査する。
    if (!vg::detail::IsSafeRelativePath(rel))
    {
        Logger::Warn(".vgeo のテクスチャパスが安全ではないため無視しました: {}", rel);
        return {};
    }
    std::filesystem::path p = (vgeoDir / std::filesystem::path(Utf8ToWide(rel))).lexically_normal();
    p = std::filesystem::path(p.generic_wstring());
    std::error_code ec;
    if (vfs::ExistsAbs(p.wstring()) || std::filesystem::exists(p, ec)) return p;
    Logger::Warn(".vgeo のテクスチャが見つかりません: {}", rel);
    return {};
}

// VG P4: プロキシの頂点接線を作る（.vgeo v1 のプロキシは接線を持たない = cooker が (0,0,0,1) を書く）。
//   ★接線 0 のまま法線マップ付き材質を描くと UnoNormalFromTangentSpace の normalize(0) が NaN になり、
//     直接光が全部消えて IBL だけの青白く平たい絵になっていた（VG OFF = プロキシだけが元の glb と違う絵）。
//   式と符号は ModelLoader（assimp aiProcess_CalcTangentSpace + 従接線からの符号）と同じ:
//     面ごとに UV の勾配から T / B（dirCorrection つき）→ 頂点へ積算 → 法線に直交化 → w = sign(dot(cross(N, T), B))。
//   VG の resolve（三角形ごとの UV 勾配の接線）とも同じ式。UV が縮退した面は assimp と同じく (1,0) / (0,1) を仮定する。
void GenerateProxyTangents(std::vector<Vertex>& vtx, const std::vector<u32>& idx)
{
    const size_t nv = vtx.size();
    if (nv == 0) return;
    std::vector<float> acc(nv * 6, 0.0f);   // T.xyz / B.xyz
    for (size_t f = 0; f + 2 < idx.size(); f += 3)
    {
        const u32 i0 = idx[f], i1 = idx[f + 1], i2 = idx[f + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
        const Vertex& a = vtx[i0];
        const Vertex& b = vtx[i1];
        const Vertex& c = vtx[i2];
        const float v[3] = {b.position.x - a.position.x, b.position.y - a.position.y, b.position.z - a.position.z};
        const float w[3] = {c.position.x - a.position.x, c.position.y - a.position.y, c.position.z - a.position.z};
        float sx = b.texCoord.x - a.texCoord.x, sy = b.texCoord.y - a.texCoord.y;
        float tx = c.texCoord.x - a.texCoord.x, ty = c.texCoord.y - a.texCoord.y;
        const float dirCorrection = (tx * sy - ty * sx) < 0.0f ? -1.0f : 1.0f;
        if (sx * ty == sy * tx) { sx = 0.0f; sy = 1.0f; tx = 1.0f; ty = 0.0f; }
        float T[3], B[3];
        for (int k = 0; k < 3; ++k)
        {
            T[k] = (w[k] * sy - v[k] * ty) * dirCorrection;
            B[k] = (w[k] * sx - v[k] * tx) * dirCorrection;
        }
        for (const u32 vi : {i0, i1, i2})
            for (int k = 0; k < 3; ++k) { acc[vi * 6 + k] += T[k]; acc[vi * 6 + 3 + k] += B[k]; }
    }
    for (size_t i = 0; i < nv; ++i)
    {
        Vertex& x = vtx[i];
        const float N[3] = {x.normal.x, x.normal.y, x.normal.z};
        float T[3] = {acc[i * 6], acc[i * 6 + 1], acc[i * 6 + 2]};
        const float B[3] = {acc[i * 6 + 3], acc[i * 6 + 4], acc[i * 6 + 5]};
        const float nd = T[0] * N[0] + T[1] * N[1] + T[2] * N[2];
        for (int k = 0; k < 3; ++k) T[k] -= N[k] * nd;                       // 法線に直交化
        float len = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
        if (!(len > 1e-12f))
        {
            // 積算が消えた（UV が無い / 打ち消し合い）: 法線に直交する任意の向き（NaN を出さない）
            const float ax[3] = {std::fabs(N[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(N[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            T[0] = ax[1] * N[2] - ax[2] * N[1];
            T[1] = ax[2] * N[0] - ax[0] * N[2];
            T[2] = ax[0] * N[1] - ax[1] * N[0];
            len = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
            if (!(len > 1e-12f)) { T[0] = 1.0f; T[1] = T[2] = 0.0f; len = 1.0f; }
        }
        x.tangent.x = T[0] / len;
        x.tangent.y = T[1] / len;
        x.tangent.z = T[2] / len;
        const float cNT[3] = {N[1] * T[2] - N[2] * T[1], N[2] * T[0] - N[0] * T[2], N[0] * T[1] - N[1] * T[0]};
        x.tangent.w = (cNT[0] * B[0] + cNT[1] * B[1] + cNT[2] * B[2]) < 0.0f ? -1.0f : 1.0f;
    }
}
} // namespace

bool IsVgeoPath(const std::filesystem::path& filePath)
{
    std::string ext = filePath.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".vgeo";
}

ModelData LoadVgeoProxy(GraphicsDevice& device,
                        ID3D12GraphicsCommandList* cmdList,
                        const std::filesystem::path& filePath,
                        ResourceManager& resourceManager)
{
    ModelData result;

    vg::FileSource src;
    if (!src.Open(filePath.string()))
    {
        Logger::Error(".vgeo を開けません: {}", filePath.string());
        return {};
    }

    // 目次だけ読む（ページ本体は読まない）。CRC は検証する（プロキシは小さいので安い）。
    vg::ReadOptions opt;
    opt.verifyCrc = true;
    vg::VgeoMeta meta;
    if (const vg::VgeoError e = vg::LoadMeta(src, meta, opt); !e.ok())
    {
        Logger::Error(".vgeo の検証に失敗しました（{}）: {}", filePath.string(), e.ToString());
        return {};
    }

    // PROXY が空で NONVG だけある場合は NONVG を代わりに使う。
    std::vector<vg::ProxySectionData> sections;
    u32 which = vg::kSecProxy;
    if (const vg::VgeoError e = vg::ReadProxySections(src, meta, vg::kSecProxy, sections, opt); !e.ok())
    {
        Logger::Error(".vgeo のプロキシを読めません（{}）: {}", filePath.string(), e.ToString());
        return {};
    }
    if (sections.empty())
    {
        which = vg::kSecNonVg;
        if (const vg::VgeoError e = vg::ReadProxySections(src, meta, vg::kSecNonVg, sections, opt); !e.ok())
        {
            Logger::Error(".vgeo の NONVG を読めません（{}）: {}", filePath.string(), e.ToString());
            return {};
        }
    }
    if (sections.empty())
    {
        Logger::Error(".vgeo にプロキシ（PROXY / NONVG）がありません。cook 時にプロキシを出力してください: {}",
                      filePath.string());
        return {};
    }

    const std::filesystem::path dir = filePath.parent_path();
    auto* srvHeap = resourceManager.GetSrvHeap();
    GraphicsDevice& dev = *resourceManager.GetDevice();

    for (const vg::ProxySectionData& sec : sections)
    {
        if (sec.materialIndex >= meta.materials.size())
        {
            Logger::Error(".vgeo のプロキシが範囲外の材質を指しています（{}）", filePath.string());
            return {};
        }
        const vg::MaterialRecord& mr = meta.materials[sec.materialIndex];

        std::vector<Vertex> vertices(sec.vertices.size());
        if (!vertices.empty())
            std::memcpy(vertices.data(), sec.vertices.data(), vertices.size() * sizeof(Vertex));
        GenerateProxyTangents(vertices, sec.indices);   // VG P4: v1 のプロキシは接線を持たない（上のコメント）

        auto mesh = std::make_unique<Mesh>();
        mesh->Initialize(device, vertices, sec.indices, cmdList);   // DEFAULT ヒープ(VRAM)常駐

        auto material = std::make_unique<Material>();
        material->defaultMetallic   = mr.metallic;
        material->defaultRoughness  = mr.roughness;
        material->alphaCutoff       = mr.alphaCutoff;
        material->baseColorAlpha    = std::clamp(mr.baseColorAlpha, 0.0f, 1.0f);
        if (mr.flags & vg::kMatBlend)          material->alphaMode = AlphaMode::Blend;
        else if (mr.flags & vg::kMatAlphaTest) material->alphaMode = AlphaMode::Mask;

        auto loadTex = [&](u32 off, bool srgb, TextureUsage usage) -> Texture* {
            if (off == vg::kNone) return nullptr;
            const auto p = ResolveVgeoTexture(dir, meta.String(off));
            if (p.empty()) return nullptr;
            return resourceManager.GetOrLoadTexture(p.wstring(), cmdList, srgb, usage);
        };
        material->albedoTexture         = loadTex(mr.albedoPathOff,     true,  TextureUsage::BaseColor);
        material->normalMapTexture      = loadTex(mr.normalPathOff,     false, TextureUsage::Normal);
        material->metalRoughnessTexture = loadTex(mr.metalRoughPathOff, false, TextureUsage::NonColor);
        material->emissiveTexture       = loadTex(mr.emissivePathOff,   true,  TextureUsage::BaseColor);

        material->emissiveColor     = { mr.emissiveColor[0], mr.emissiveColor[1], mr.emissiveColor[2] };
        material->emissiveIntensity = mr.emissiveIntensity;
        if (material->emissiveTexture && material->emissiveIntensity <= 0.0f)
        {
            // テクスチャだけの指定＝素通し（ModelLoader と同じ扱い）
            material->emissiveColor     = { 1.0f, 1.0f, 1.0f };
            material->emissiveIntensity = 1.0f;
        }

        // PBR SRV ブロック（albedo / normal / metalRoughness / emissive の 4 連続。ModelLoader と同じ）
        {
            Texture* albedo = material->albedoTexture ? material->albedoTexture : resourceManager.GetDefaultWhiteTexture();
            Texture* normal = material->normalMapTexture ? material->normalMapTexture : resourceManager.GetDefaultNormalTexture();
            Texture* mrTex  = material->metalRoughnessTexture ? material->metalRoughnessTexture : resourceManager.GetDefaultMetalRoughnessTexture();
            Texture* emis   = material->emissiveTexture ? material->emissiveTexture : resourceManager.GetDefaultBlackTexture();
            const u32 blockStart = srvHeap->AllocateBlock(kMaterialSrvBlockSize);
            albedo->CreateSRV(dev, srvHeap->GetCpuHandle(blockStart));
            normal->CreateSRV(dev, srvHeap->GetCpuHandle(blockStart + 1));
            mrTex->CreateSRV(dev, srvHeap->GetCpuHandle(blockStart + 2));
            emis->CreateSRV(dev, srvHeap->GetCpuHandle(blockStart + 3));
            material->srvBlockIndex = blockStart;
        }

        mesh->SetMaterial(material.get());
        mesh->SetMaterialName(meta.String(mr.nameOff));
        mesh->SetName(meta.String(mr.nameOff));

        result.meshes.push_back(std::move(mesh));
        result.materials.push_back(std::move(material));
    }

    Logger::Info(".vgeo プロキシを読み込みました: {} （{} セクション, {}）", filePath.string(),
                 static_cast<u32>(result.meshes.size()), which == vg::kSecProxy ? "PROXY" : "NONVG");
    return result;
}

} // namespace dx12e
