#pragma once

// 地形レイヤーセット（.terrainlayers）— 4 レイヤーぶんの PBR 素材を Texture2DArray 2 本に焼く。
//
// ★ルートシグネチャを 1 スロットも消費しない設計の中核。
//   通常メッシュが t0/t1/t2 に Texture2D（albedo / normal / metalRoughness）を張るのと
//   まったく同じディスクリプタテーブル（RootSignature::kSlotSRVTable）へ、地形は
//     t0 = Texture2DArray  RGB=albedo(sRGB) / A=height
//     t1 = Texture2DArray  RG=normal.xy / B=roughness / A=AO
//     t2 = Texture2D       RGBA=レイヤー 0..3 の重み（スプラット。地形ごとに異なる）
//   を張る。リソース次元はルートシグネチャに書かれていないので、SRV とシェーダ宣言さえ
//   一致していれば完全に合法（Terrain.hlsl が Texture2DArray と宣言している）。
//
// このクラスが持つのは配列 2 本だけ。t2（スプラット）は地形エンティティごとに違うので、
// 連続 3 ディスクリプタのブロックは Application::EnsureTerrainSrv が組む。

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Texture;
class GraphicsDevice;

// 1 レイヤーぶんの素材。パスは assets 相対（.dxmat と同じ流儀）。
struct TerrainLayerDesc
{
    std::string name;
    std::string albedo;      // ベースカラー（sRGB）
    std::string normal;      // 法線（OpenGL 規約 = nor_gl）
    std::string arm;         // R=AO / G=Roughness / B=Metallic（Poly Haven 互換）
    std::string height;      // 変位（disp）。空ならアルベド輝度から合成する
    f32 tiling      = 0.35f; // 1m あたりのタイル回数（0.35 なら約 2.9m で 1 周）
    f32 heightBias  = 0.0f;  // 高さブレンドのオフセット（-1..1。焼き込み時に height へ加算）
};

struct TerrainLayerSetData
{
    std::string name;
    u32 size = 1024;                        // 配列 1 スライスの解像度（全レイヤー共通へ強制リサイズ）
    std::vector<TerrainLayerDesc> layers;   // 1..4（4 を超えたぶんは捨てて警告）
};

// JSON バイト列 → TerrainLayerSetData。GPU に触らない純関数（テスト可能）。
bool ParseTerrainLayerSet(const std::vector<uint8_t>& jsonBytes, TerrainLayerSetData& out);
// TerrainLayerSetData → .terrainlayers 用 JSON 文字列（整形あり）。
std::string SerializeTerrainLayerSet(const TerrainLayerSetData& data);

// .terrainlayers のロード・配列ビルド・ホットリロード。MaterialAssetManager と同じ流儀。
class TerrainLayerSetManager
{
public:
    struct Entry
    {
        TerrainLayerSetData      data;
        std::unique_ptr<Texture> albedoArray;    // RGB=albedo / A=height
        std::unique_ptr<Texture> surfaceArray;   // RG=normal.xy / B=roughness / A=AO
        u32  layerCount = 0;
        f32  tiling[4]  = {0.35f, 0.35f, 0.35f, 0.35f};
        bool valid      = false;
        bool attempted  = false;                 // 失敗時の毎フレーム再試行を防ぐ
        u32  generation = 0;                     // 再ロードのたびに +1（SRV 張り直しの判定用）
        std::filesystem::file_time_type mtime{};
    };

    void Initialize(GraphicsDevice* device);

    // relPath 例 "terrain/alpine.terrainlayers"（assets 相対）。cmdList はビルド時のみ使用。
    // 失敗時は valid=false の Entry を返す（呼び出し側は従来経路へフォールバックすること）。
    const Entry* GetOrLoad(const std::string& relPath, ID3D12GraphicsCommandList* cmdList);

    void Invalidate(const std::string& relPath);
    // エディタのみ: 0.5 秒間隔で .terrainlayers の mtime を見て自動 Invalidate + 再ロード。
    void PollHotReload(f32 dt, ID3D12GraphicsCommandList* cmdList);

    void Shutdown() { m_cache.clear(); }

private:
    void LoadInto(Entry& entry, const std::string& relPath, ID3D12GraphicsCommandList* cmdList);

    GraphicsDevice* m_device = nullptr;
    std::unordered_map<std::string, Entry> m_cache;   // キー = 正規化済み relPath
    f32 m_pollTimer = 0.0f;
};

} // namespace dx12e
