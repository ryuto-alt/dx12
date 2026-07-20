#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Types.h"

namespace dx12e
{

// .dxmat（マテリアルアセット）の中身。パス類は assets 相対（例 "textures/x/x_diff_2k.jpg"）。
// GPU/SRVには一切触れない純データ構造体なので、MaterialAssetManager を介さず単体テスト可能。
struct MaterialAssetData
{
    std::string name;
    std::string albedoPath;
    std::string normalPath;
    std::string metalRoughnessPath;  // Poly Haven の ARM（R=AO 未使用/G=Roughness/B=Metallic）互換

    f32 metallic  = 1.0f;  // スケーリングファクター（glTF 意味論: テクスチャ値 * この係数）
    f32 roughness = 1.0f;

    f32 uvTilingU = 1.0f;
    f32 uvTilingV = 1.0f;

    std::string source;   // 例 "Poly Haven"（未指定可）
    std::string license;  // 例 "CC0"（未指定可）
};

// JSON バイト列から MaterialAssetData を読む。パース失敗/必須フィールド欠落なら false。
bool ParseMaterialAsset(const std::vector<uint8_t>& jsonBytes, MaterialAssetData& out);

// MaterialAssetData を .dxmat 用 JSON 文字列へ（保存/テスト用、整形あり）。
std::string SerializeMaterialAsset(const MaterialAssetData& data);

} // namespace dx12e
