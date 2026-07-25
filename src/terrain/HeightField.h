#pragma once

// ハイトフィールド（正方グリッドの高さ配列）。地形の「真実の値」はここだけに置く。
//
// 座標系: ローカル XZ は原点中心。x,z ∈ [-worldSize/2, +worldSize/2]。
//         サンプル (ix, iz) の位置は ( -half + ix*cell, heights[iz*res+ix], -half + iz*cell )、
//         cell = worldSize / (resolution - 1)。
//         描画メッシュ・Jolt の HeightFieldShape・ブラシが全部この同じ規約で読む
//         ＝高さ配列をいじれば見た目もコリジョンも自動的に追従する。
//
// このファイルは純ロジック（GPU も entt も vfs も使わない）。単体テストが
// HeightField.cpp を直接ビルドできるよう、依存は <DirectXMath> と core/Types.h だけに保つこと。

#include <vector>
#include <cstddef>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class HeightField
{
public:
    static constexpr u32 kMinResolution = 16;
    static constexpr u32 kMaxResolution = 512;

    // Jolt の HeightFieldShape は「サンプル数がブロックサイズ（既定 2）の倍数」を要求する。
    // 将来ブロックサイズを 4 に上げても通るよう 4 の倍数へ丸め、範囲もクランプする。
    static u32 NormalizeResolution(u32 res);

    // resolution × resolution の高さ配列を initialHeight で作る。
    void Create(u32 resolution, f32 worldSize, f32 initialHeight = 0.0f);

    bool IsValid() const
    {
        return m_resolution >= kMinResolution
            && m_heights.size() == static_cast<size_t>(m_resolution) * m_resolution;
    }

    u32 Resolution() const { return m_resolution; }
    f32 WorldSize()  const { return m_worldSize; }
    f32 CellSize()   const
    {
        return (m_resolution > 1) ? m_worldSize / static_cast<f32>(m_resolution - 1) : m_worldSize;
    }
    f32 HalfSize() const { return m_worldSize * 0.5f; }

    const std::vector<f32>& Heights() const { return m_heights; }
    std::vector<f32>&       Heights()       { return m_heights; }

    // グリッド座標アクセス。範囲外は端をクランプ（At）／無視（SetAt）。
    f32  At(i32 ix, i32 iz) const;
    void SetAt(i32 ix, i32 iz, f32 v);

    // グリッド座標 → ローカル座標（サンプル位置の XZ）
    f32 GridToLocalX(i32 ix) const { return -HalfSize() + static_cast<f32>(ix) * CellSize(); }
    f32 GridToLocalZ(i32 iz) const { return -HalfSize() + static_cast<f32>(iz) * CellSize(); }

    // ローカル XZ → 高さ（バイリニア補間）。範囲外は端の値。
    f32 SampleHeight(f32 lx, f32 lz) const;
    // ローカル XZ → 単位法線（中央差分）。無効なら (0,1,0)。
    DirectX::XMFLOAT3 SampleNormal(f32 lx, f32 lz) const;

    // ローカル座標系のレイと地形表面の交差。ヒットで true、outT に origin からの距離を返す。
    // dir は正規化されていなくてよい（内部で正規化するので outT は実距離）。
    bool Raycast(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& dir,
                 f32 maxDistance, f32& outT) const;

    void MinMaxHeight(f32& outMin, f32& outMax) const;

    // グリッド座標の閉区間矩形。x1 < x0 または z1 < z0 なら「無効（空）」。
    // ブラシの影響範囲・メッシュ部分更新・Undo のタイル抽出で共有する。
    struct Rect
    {
        i32 x0 = 0, z0 = 0, x1 = -1, z1 = -1;
        bool Valid() const { return x1 >= x0 && z1 >= z0; }
        i32  Width()  const { return Valid() ? (x1 - x0 + 1) : 0; }
        i32  Height() const { return Valid() ? (z1 - z0 + 1) : 0; }
    };

    Rect FullRect() const;
    // ローカル XZ 中心・半径 radius の円を包む最小のグリッド矩形（グリッド内へクランプ済み）。
    Rect RectForCircle(f32 lx, f32 lz, f32 radius) const;
    // グリッド内へクランプ（範囲外なら無効矩形）。
    Rect ClampRect(const Rect& r) const;

    // 2 つの矩形の和（どちらかが無効ならもう一方をそのまま返す）。
    static Rect UnionRect(const Rect& a, const Rect& b);

    // ---- .hf バイナリ（マジック "DXHF" + version + resolution + worldSize + f32 配列）----
    // シーン JSON には入れない（解像度 256 で 6 万要素＝JSON では実用にならない）。
    static constexpr u32 kMagic       = 0x46485844u;  // 'DXHF'（リトルエンディアン）
    static constexpr u32 kVersion     = 1u;
    static constexpr size_t kHeaderSize = 16;         // magic + version + resolution + worldSize

    std::vector<u8> Encode() const;
    bool            Decode(const u8* data, size_t size);
    bool            Decode(const std::vector<u8>& bytes)
    {
        return Decode(bytes.empty() ? nullptr : bytes.data(), bytes.size());
    }

private:
    u32              m_resolution = 0;
    f32              m_worldSize  = 0.0f;
    std::vector<f32> m_heights;
};

} // namespace dx12e
