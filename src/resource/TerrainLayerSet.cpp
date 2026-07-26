#include "resource/TerrainLayerSet.h"

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Texture.h"
#include "resource/TextureLoader.h"

#include <DirectXTex.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dx12e
{

using json = nlohmann::json;

namespace
{

constexpr u32 kMaxLayers = 4;

// MaterialAssetManager::NormalizeKey と同じ方針（大小・区切り文字の揺れ吸収）。
std::string NormalizeKey(const std::string& relPath)
{
    std::string key = relPath;
    for (char& c : key)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

u64 HashBytes64(const uint8_t* data, size_t len)
{
    u64 h = 1469598103934665603ull;   // FNV-1a
    for (size_t i = 0; i < len; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

u64 HashStr64(const std::string& s)
{
    return HashBytes64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// assets 相対パスの画像を RGBA8 の size×size へ揃えて読む。
// 読めなければ false（呼び出し側が既定色で埋める）。
bool LoadRgbaResized(const std::string& relPath, u32 size, std::vector<u8>& out, u64& outHash)
{
    out.clear();
    outHash = 0;
    if (relPath.empty()) return false;

    const std::vector<uint8_t> bytes = vfs::ReadAsset(relPath);
    if (bytes.empty())
    {
        Logger::Warn("地形レイヤーのテクスチャが見つかりません: {}", relPath);
        return false;
    }
    outHash = HashBytes64(bytes.data(), bytes.size());

    using namespace DirectX;
    ScratchImage img;
    const bool ddsMagic = (bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D'
                        && bytes[2] == 'S' && bytes[3] == ' ');
    HRESULT hr = ddsMagic
        ? LoadFromDDSMemory(bytes.data(), bytes.size(), DDS_FLAGS_NONE, nullptr, img)
        : LoadFromWICMemory(bytes.data(), bytes.size(), WIC_FLAGS_NONE, nullptr, img);
    if (FAILED(hr))
    {
        Logger::Warn("地形レイヤーのテクスチャを読めません: {}", relPath);
        return false;
    }

    // BC 等の圧縮入力は伸長してから扱う（ソースに .dds を指定されても通す）
    if (IsCompressed(img.GetMetadata().format))
    {
        ScratchImage dec;
        if (FAILED(Decompress(img.GetImages(), img.GetImageCount(), img.GetMetadata(),
                              DXGI_FORMAT_R8G8B8A8_UNORM, dec)))
            return false;
        img = std::move(dec);
    }
    if (img.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        ScratchImage conv;
        if (FAILED(Convert(*img.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
                           TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, conv)))
            return false;
        img = std::move(conv);
    }
    if (img.GetMetadata().width != size || img.GetMetadata().height != size)
    {
        ScratchImage rs;
        if (FAILED(Resize(*img.GetImage(0, 0, 0), size, size, TEX_FILTER_DEFAULT, rs)))
            return false;
        img = std::move(rs);
    }

    const Image* src = img.GetImage(0, 0, 0);
    if (!src) return false;
    out.resize(static_cast<size_t>(size) * size * 4);
    const size_t rowBytes = static_cast<size_t>(size) * 4;
    for (u32 y = 0; y < size; ++y)
        std::memcpy(out.data() + static_cast<size_t>(y) * rowBytes,
                    src->pixels + static_cast<size_t>(y) * src->rowPitch, rowBytes);
    return true;
}

void FillConst(std::vector<u8>& buf, u32 size, u8 r, u8 g, u8 b, u8 a)
{
    buf.assign(static_cast<size_t>(size) * size * 4, 0);
    for (size_t i = 0; i < buf.size(); i += 4)
    {
        buf[i] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = a;
    }
}

} // namespace

// ============================================================================
//  .terrainlayers JSON
// ============================================================================
bool ParseTerrainLayerSet(const std::vector<uint8_t>& jsonBytes, TerrainLayerSetData& out)
{
    if (jsonBytes.empty()) return false;

    json j;
    try { j = json::parse(jsonBytes.begin(), jsonBytes.end()); }
    catch (const json::exception&) { return false; }
    if (!j.is_object()) return false;

    TerrainLayerSetData data;
    data.name = j.value("name", "");
    data.size = j.value("size", 1024u);
    // 配列は全スライス同一サイズ・同一フォーマットが必須。2 のべき乗へ丸めて 256..2048 に収める。
    {
        u32 p = 256;
        while (p < data.size && p < 2048) p <<= 1;
        data.size = std::clamp(p, 256u, 2048u);
    }

    if (!j.contains("layers") || !j["layers"].is_array()) return false;
    for (const auto& lj : j["layers"])
    {
        if (!lj.is_object()) continue;
        if (data.layers.size() >= kMaxLayers)
        {
            Logger::Warn("地形レイヤーセットのレイヤーが 4 を超えています（超過分は無視します）");
            break;
        }
        TerrainLayerDesc d;
        d.name       = lj.value("name", "");
        d.albedo     = lj.value("albedo", "");
        d.normal     = lj.value("normal", "");
        d.arm        = lj.value("arm", "");
        d.height     = lj.value("height", "");
        d.tiling     = lj.value("tiling", 0.35f);
        d.heightBias = lj.value("heightBias", 0.0f);
        if (!(d.tiling > 0.0f)) d.tiling = 0.35f;
        data.layers.push_back(std::move(d));
    }
    if (data.layers.empty()) return false;

    out = std::move(data);
    return true;
}

std::string SerializeTerrainLayerSet(const TerrainLayerSetData& data)
{
    json j;
    j["version"] = 1;
    j["name"] = data.name;
    j["size"] = data.size;
    j["layers"] = json::array();
    for (const auto& l : data.layers)
    {
        json lj;
        lj["name"]   = l.name;
        lj["albedo"] = l.albedo;
        lj["normal"] = l.normal;
        lj["arm"]    = l.arm;
        if (!l.height.empty()) lj["height"] = l.height;
        lj["tiling"]     = l.tiling;
        lj["heightBias"] = l.heightBias;
        j["layers"].push_back(std::move(lj));
    }
    return j.dump(2);
}

// ============================================================================
//  マネージャ
// ============================================================================
void TerrainLayerSetManager::Initialize(GraphicsDevice* device)
{
    m_device = device;
}

void TerrainLayerSetManager::LoadInto(Entry& entry, const std::string& relPath,
                                      ID3D12GraphicsCommandList* cmdList)
{
    entry.valid = false;
    entry.attempted = true;
    entry.layerCount = 0;

    // mtime は成否に関わらず先に刻む（壊れた .terrainlayers を毎フレーム叩かない）。
    if (!vfs::InGameMode())
    {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(PathResolver::AssetsDir() + relPath, ec);
        if (!ec) entry.mtime = t;
    }

    if (!m_device || !cmdList) return;

    const std::vector<uint8_t> bytes = vfs::ReadAsset(relPath);
    TerrainLayerSetData data;
    if (!ParseTerrainLayerSet(bytes, data))
    {
        Logger::Warn("地形レイヤーセットの読み込みに失敗しました: {}", relPath);
        return;
    }
    entry.data = data;

    const u32 size  = data.size;
    const u32 count = static_cast<u32>(data.layers.size());
    const size_t sliceBytes = static_cast<size_t>(size) * size * 4;

    // レイヤーごとにチャンネルを組み替えて 2 本の配列スライスを作る。
    //   albedoSlice : RGB=albedo / A=height
    //   surfaceSlice: R=normal.x / G=normal.y / B=roughness / A=AO
    std::vector<std::vector<u8>> albedoSlices(count), surfaceSlices(count);
    u64 contentHash = HashStr64(relPath) ^ (static_cast<u64>(size) << 32);

    for (u32 i = 0; i < count; ++i)
    {
        const TerrainLayerDesc& L = data.layers[i];

        std::vector<u8> alb, nor, arm, hgt;
        u64 h0 = 0, h1 = 0, h2 = 0, h3 = 0;
        if (!LoadRgbaResized(L.albedo, size, alb, h0)) FillConst(alb, size, 160, 160, 160, 255);
        if (!LoadRgbaResized(L.normal, size, nor, h1)) FillConst(nor, size, 128, 128, 255, 255);
        if (!LoadRgbaResized(L.arm,    size, arm, h2)) FillConst(arm, size, 255, 200, 0,   255);
        const bool hasHeight = LoadRgbaResized(L.height, size, hgt, h3);

        contentHash ^= (h0 * 31) ^ (h1 * 131) ^ (h2 * 1031) ^ (h3 * 10007);
        contentHash = contentHash * 1099511628211ull + i;

        albedoSlices[i].resize(sliceBytes);
        surfaceSlices[i].resize(sliceBytes);

        const f32 bias = std::clamp(L.heightBias, -1.0f, 1.0f);
        for (size_t p = 0; p < sliceBytes; p += 4)
        {
            // --- t0: RGB=albedo / A=height ---
            albedoSlices[i][p + 0] = alb[p + 0];
            albedoSlices[i][p + 1] = alb[p + 1];
            albedoSlices[i][p + 2] = alb[p + 2];

            f32 hv;
            if (hasHeight)
            {
                hv = static_cast<f32>(hgt[p]) / 255.0f;   // disp は R に入っている前提
            }
            else
            {
                // 変位テクスチャが無いときはアルベド輝度で代用する（定番の代用。品質は落ちる）。
                // Poly Haven のダウンロードは diff / nor_gl / arm の 3 枚だけで disp を落としていない。
                const f32 lum = (0.2126f * static_cast<f32>(alb[p + 0])
                               + 0.7152f * static_cast<f32>(alb[p + 1])
                               + 0.0722f * static_cast<f32>(alb[p + 2])) / 255.0f;
                hv = lum;
            }
            hv = std::clamp(hv + bias, 0.0f, 1.0f);
            albedoSlices[i][p + 3] = static_cast<u8>(hv * 255.0f + 0.5f);

            // --- t1: RG=normal.xy / B=roughness / A=AO ---
            surfaceSlices[i][p + 0] = nor[p + 0];
            surfaceSlices[i][p + 1] = nor[p + 1];
            surfaceSlices[i][p + 2] = arm[p + 1];   // ARM の G = Roughness
            surfaceSlices[i][p + 3] = arm[p + 0];   // ARM の R = AO
        }

        entry.tiling[i] = L.tiling;
        if (!hasHeight && !L.albedo.empty() && L.height.empty())
        {
            // 1 レイヤーにつき 1 回だけの情報ログ（毎フレームは出ない：ロード時のみ）
            Logger::Info("地形レイヤー '{}' に変位(height)が無いのでアルベド輝度から合成しました",
                         L.name.empty() ? L.albedo : L.name);
        }
    }
    for (u32 i = count; i < kMaxLayers; ++i) entry.tiling[i] = 0.35f;

    std::vector<const uint8_t*> albPtr(count), srfPtr(count);
    for (u32 i = 0; i < count; ++i)
    {
        albPtr[i] = albedoSlices[i].data();
        srfPtr[i] = surfaceSlices[i].data();
    }

    // ★配列は「全スライス同一サイズ・同一フォーマット・同一ミップ数」が D3D12 の要求。
    //   上でリサイズを強制しているので必ず揃う。
    auto albedoTex = TextureLoader::CreateArrayFromRGBA(
        *m_device, cmdList, albPtr, size, size, /*srgb=*/true,
        TextureUsage::BaseColor, "terrainlayers_albedo|" + relPath, contentHash);
    auto surfaceTex = TextureLoader::CreateArrayFromRGBA(
        *m_device, cmdList, srfPtr, size, size, /*srgb=*/false,
        TextureUsage::NonColor, "terrainlayers_surface|" + relPath, contentHash);

    if (!albedoTex || !surfaceTex)
    {
        Logger::Error("地形レイヤー配列の作成に失敗しました: {}", relPath);
        return;
    }

    entry.albedoArray  = std::move(albedoTex);
    entry.surfaceArray = std::move(surfaceTex);
    entry.layerCount   = count;
    entry.valid        = true;
    ++entry.generation;

    Logger::Info("地形レイヤーセットを読み込みました: {} ({} 層 / {}px)", relPath, count, size);
}

const TerrainLayerSetManager::Entry* TerrainLayerSetManager::GetOrLoad(
    const std::string& relPath, ID3D12GraphicsCommandList* cmdList)
{
    if (relPath.empty()) return nullptr;

    const std::string key = NormalizeKey(relPath);
    auto it = m_cache.find(key);
    if (it != m_cache.end() && (it->second.valid || it->second.attempted))
        return &it->second;

    Entry& entry = m_cache[key];
    LoadInto(entry, relPath, cmdList);
    return &entry;
}

void TerrainLayerSetManager::Invalidate(const std::string& relPath)
{
    auto it = m_cache.find(NormalizeKey(relPath));
    if (it == m_cache.end()) return;
    it->second.valid = false;
    it->second.attempted = false;
}

void TerrainLayerSetManager::PollHotReload(f32 dt, ID3D12GraphicsCommandList* cmdList)
{
    if (vfs::InGameMode()) return;

    m_pollTimer += dt;
    if (m_pollTimer < 0.5f) return;
    m_pollTimer = 0.0f;

    for (auto& [key, entry] : m_cache)
    {
        if (!entry.attempted) continue;
        std::error_code ec;
        auto t = std::filesystem::last_write_time(PathResolver::AssetsDir() + key, ec);
        if (ec || t == entry.mtime) continue;
        LoadInto(entry, key, cmdList);
    }
}

} // namespace dx12e
