#pragma once

#include <cstdint>
#include <map>
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
    std::string emissivePath;        // 自己発光（sRGB。看板・照明パネル・非常口サイン）

    f32 metallic  = 1.0f;  // スケーリングファクター（glTF 意味論: テクスチャ値 * この係数）
    f32 roughness = 1.0f;

    // 自己発光。実効値 = emissiveColor * emissiveIntensity * (emissivePath があればその値)。
    // 既定は黒 x 0 = 無発光なので、既存の .dxmat は 1 ピクセルも変わらない。
    f32 emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    f32 emissiveIntensity = 0.0f;

    // マテリアル AO。> 0 のとき metalRoughnessPath（ORM / PolyHaven ARM）の R を AO として読む（強さ 0..1）。
    // 既定 0 = 読まない＝既存の .dxmat は ARM を指していても 1 ピクセルも変わらない。
    // 間接光（IBL・アンビエント・GI）だけに掛かり、直接光には掛からない。
    f32 aoStrength = 0.0f;

    f32 uvTilingU = 1.0f;
    f32 uvTilingV = 1.0f;

    std::string source;   // 例 "Poly Haven"（未指定可）
    std::string license;  // 例 "CC0"（未指定可）

    // ---- マテリアルグラフのインスタンス（"graph" キー。docs/MATGRAPH_FORMAT.md §3）-------------------------
    // ★graphPath が空 = 従来の .dxmat（このブロックは一切読み書きしない＝既存ファイルは 1 バイトも変わらない）。
    // graphPath がある（.dxmg の assets 相対パス）= グラフ材質。albedo / normal / metalRoughness / emissive の 4 枚と
    //   metallic / roughness / emissive* は無視され（書き出しもしない）、値は params が持つ。
    //   このとき metallic / roughness は「影・深度・パストレの近似に使う代理値」で、既定は 0 / 0.5（従来の 1 / 1 ではない）。
    std::string graphPath;
    struct GraphParam
    {
        enum class Kind : uint8_t { Scalar, Vector, Texture };
        Kind        kind = Kind::Scalar;
        f32         v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        int         n    = 1;      // Scalar = 1 / Vector = 2..4（書き戻しで長さを保つ）
        std::string texture;       // Texture: assets 相対パス
        bool operator==(const GraphParam& o) const
        {
            return kind == o.kind && n == o.n && texture == o.texture
                && v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2] && v[3] == o.v[3];
        }
    };
    std::map<std::string, GraphParam> graphParams;   // パラメータ名 → 上書き値（数値 = Scalar / 配列 = Vector / 文字列 = Texture）

    bool IsGraph() const { return !graphPath.empty(); }
};

// JSON バイト列から MaterialAssetData を読む。パース失敗/必須フィールド欠落なら false。
bool ParseMaterialAsset(const std::vector<uint8_t>& jsonBytes, MaterialAssetData& out);

// MaterialAssetData を .dxmat 用 JSON 文字列へ（保存/テスト用、整形あり）。
std::string SerializeMaterialAsset(const MaterialAssetData& data);

} // namespace dx12e
