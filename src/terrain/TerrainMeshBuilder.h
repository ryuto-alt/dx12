#pragma once

// ハイトフィールド → 通常の Mesh 用の頂点/インデックス。
//
// 地形を「専用の描画パス」ではなく「普通の Mesh を持つ普通のエンティティ」にするための橋渡し。
// これで描画・フラスタムカリング・影・ピッキング・マテリアル割当が全部そのまま乗る
// （CDLOD のような地形専用 LOD は必要になってから）。
//
// 頂点はグリッド順（index = z * resolution + x）で並べる。この並びを崩さないために
// Mesh::Initialize（meshoptimizer が頂点を並べ替える）ではなく Mesh::InitializeDynamic を使うこと。

#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "renderer/Mesh.h"
#include "terrain/HeightField.h"

namespace dx12e
{

class TerrainMeshBuilder
{
public:
    // 全面ビルド。uvTiles = 地形全体で UV が何回繰り返すか。
    static void Build(const HeightField& hf, f32 uvTiles, const DirectX::XMFLOAT4& color,
                      std::vector<Vertex>& outVertices, std::vector<u32>& outIndices);

    // dirty 矩形ぶんの頂点だけ書き換える（法線は隣接セルを見るので内部で 1 セル広げる）。
    // vertices は Build 済みで頂点数が一致していること（不一致なら何もしない＝呼び出し側が
    // Build し直す）。トポロジは変わらないのでインデックスは触らない。
    static void UpdateVertices(const HeightField& hf, f32 uvTiles, const DirectX::XMFLOAT4& color,
                               const HeightField::Rect& dirty, std::vector<Vertex>& vertices);
};

} // namespace dx12e
