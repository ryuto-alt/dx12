#pragma once

// 地形のスプラット重みマップ（4 レイヤー = RGBA 各 1 チャンネル）。
//
// 「どのレイヤーをどこにどれだけ出すか」の真実の値はここだけに置く。描画は
// これを R8G8B8A8_UNORM の Texture2D として t2 に張り、Terrain.hlsl が 1 タップで読む。
//
// ★.hf には 1 バイトも足さない。HeightField::Decode は version != kVersion を即拒否し、
//   DeepDiagnostics もヘッダ 16 バイト前提で検査しているため、.hf を拡張すると
//   「新エンジンが書いた .hf を旧エンジンが読めない」形の非互換になる。
//   スプラットは独立した assets/terrain/<name>.splat に置く。
//
// 座標系は HeightField と同じ（ローカル XZ 原点中心、x,z ∈ [-worldSize/2, +worldSize/2]）。
// ただし解像度は地形の resolution とは独立（既定 512）。
//
// このファイルは純ロジック（GPU も entt も vfs も使わない）。単体テストから直接ビルドできる。

#include <vector>
#include <cstddef>
#include "core/Types.h"
#include "terrain/HeightField.h"

namespace dx12e
{

// 自動ペイント（傾斜と標高から 4 層の重みを焼く）のパラメータ。
// 既定値は「草 / 土 / 岩 / 雪」の並びを想定している。
struct TerrainAutoPaintParams
{
    // 傾斜（0 = 平ら, 1 = 垂直）のしきい値。
    f32 dirtSlopeStart = 0.10f;   // ここから土が混ざり始める
    f32 dirtSlopeEnd   = 0.26f;   // ここで土が最大
    f32 rockSlopeStart = 0.30f;   // ここから岩
    f32 rockSlopeEnd   = 0.55f;   // ここで岩が総取り
    // 標高（ワールド Y、メートル）。既定は Apply 時に地形の最大高さから自動で決める。
    f32 snowHeightStart = 0.0f;
    f32 snowHeightEnd   = 0.0f;
    bool autoSnowHeight = true;   // true なら高さレンジの 55%〜80% を雪帯にする
    f32 snowSlopeLimit  = 0.45f;  // これより急な斜面には雪を積もらせない（岩が出る）
    f32 noiseStrength   = 0.12f;  // 境界を自然に散らすノイズの強さ（0 = 直線的な帯）
    f32 noiseScale      = 0.08f;  // ノイズの周期（1/m）
};

class TerrainSplatMap
{
public:
    static constexpr u32    kMagic      = 0x50535844u;  // 'DXSP'（リトルエンディアン）
    static constexpr u32    kVersion    = 1u;
    static constexpr size_t kHeaderSize = 32;           // magic/ver/width/height/layers + 予約 3
    static constexpr u32    kLayers     = 4;            // RGBA 1 枚 = 4 層（8 層化は version 2 で）
    static constexpr u32    kMinSize    = 32;
    static constexpr u32    kMaxSize    = 2048;
    static constexpr u32    kDefaultSize = 512;

    static u32 NormalizeSize(u32 size);

    // size × size の RGBA8 を作る。全テクセル「レイヤー 0 が 100%」。
    void Create(u32 size);

    bool IsValid() const
    {
        return m_size >= kMinSize
            && m_pixels.size() == static_cast<size_t>(m_size) * m_size * 4;
    }
    u32 Size() const { return m_size; }

    const std::vector<u8>& Pixels() const { return m_pixels; }
    std::vector<u8>&       Pixels()       { return m_pixels; }

    // GPU アップロードの要否判定用。中身を書き換えたら必ず Touch() する。
    u32  Version() const { return m_version; }
    void Touch()          { ++m_version; }

    // ミップチェーン（box フィルタ）。戻り値[0] が mip0（＝Pixels() のコピー）。
    // スプラットは低周波なのでミップが無いと遠景でジャギる。GPU 側で作る手段が無いので CPU で焼く。
    std::vector<std::vector<u8>> BuildMipChain() const;
    static u32 MipCount(u32 size);

    // ローカル XZ → テクセル座標（範囲外はクランプ）。
    i32 LocalToTexelX(f32 lx, f32 worldSize) const;
    i32 LocalToTexelZ(f32 lz, f32 worldSize) const;

    // 円形ブラシ。layer の重みを strength ぶん上げ、残りを比例で下げる（合計 255 を保つ）。
    // radius/lx/lz はローカル座標（m）。戻り値は触ったテクセル矩形。
    HeightField::Rect PaintCircle(f32 worldSize, f32 lx, f32 lz, f32 radius,
                                  u32 layer, f32 strength, f32 falloff);

    // 傾斜と標高から 4 層の重みを焼く。手で塗らなくても「草 / 土 / 岩 / 雪」が出る。
    void AutoPaintFromHeightField(const HeightField& hf, const TerrainAutoPaintParams& params);

    // ---- .splat バイナリ ----
    std::vector<u8> Encode() const;
    bool            Decode(const u8* data, size_t size);
    bool            Decode(const std::vector<u8>& bytes)
    {
        return Decode(bytes.empty() ? nullptr : bytes.data(), bytes.size());
    }

private:
    u32             m_size    = 0;
    u32             m_version = 1;
    std::vector<u8> m_pixels;   // RGBA8。R=layer0 G=layer1 B=layer2 A=layer3
};

} // namespace dx12e
