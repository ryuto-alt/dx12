#pragma once

// 地形スカルプトのブラシと生成アルゴリズム。
//
// 方針: 「重み関数(TerrainBrushWeight)」と「ノイズ(TerrainFbm)」は HeightField を一切知らない
//       純関数として切り出してある。将来これを任意メッシュの頂点スカルプトから使い回すときは
//       この 2 つだけ持っていけばよい（それ以上の抽象化は必要になってからで十分）。
//
// このファイルも純ロジック（GPU も entt も vfs も使わない）。単体テストが TerrainBrush.cpp を
// 直接ビルドできるよう、依存は HeightField.h と標準ライブラリだけに保つこと。

#include "core/Types.h"
#include "terrain/HeightField.h"

namespace dx12e
{

enum class TerrainBrushType : int
{
    Raise   = 0,   // 盛る
    Lower   = 1,   // 削る
    Smooth  = 2,   // ならす（3x3 平均へ寄せる）
    Flatten = 3,   // flattenTarget の高さへ寄せる
    Noise   = 4,   // fBm ノイズを足す（岩肌/ざらつき）
    Erode   = 5,   // 熱浸食（安息角を超えた斜面の土砂を隣へ落とす）
    Count   = 6,
};

struct TerrainBrushParams
{
    TerrainBrushType type = TerrainBrushType::Raise;

    f32 radius   = 12.0f;    // ワールド単位の半径
    f32 strength = 10.0f;    // Raise/Lower/Noise = m/秒、Smooth/Flatten = 寄せる速さ
    f32 falloff  = 0.5f;     // 0 = 縁まで一定（円柱）、1 = とろけるように滑らか

    f32 minHeight = -500.0f; // クランプ
    f32 maxHeight =  500.0f;

    f32 flattenTarget = 0.0f;   // Flatten の目標高さ（ストローク開始時のブラシ中心高さを入れる）

    bool mirrorX = false;    // X ミラー（x → -x にも同じ筆を置く）
    bool mirrorZ = false;    // Z ミラー

    // Noise
    f32 noiseFrequency = 0.03f;
    i32 noiseOctaves   = 5;
    f32 noiseRidged    = 0.0f;   // 0 = fBm / 1 = ridged（尾根が立つ）
    u32 noiseSeed      = 1337u;

    // Erode（thermal）
    f32 talusDeg        = 34.0f;  // 安息角。これを超えた斜面から土砂が落ちる
    i32 erodeIterations = 8;
};

// 距離比 d/r (0..1) → 影響量 0..1。falloff=0 で硬い縁、1 で smoothstep の釣鐘。
// w = 1 - smoothstep(0,1,(d/r)) を falloff で「硬い円柱」と混ぜたもの。
f32 TerrainBrushWeight(f32 distRatio, f32 falloff);

// fBm / ridged ノイズ（戻り値は概ね -1..1）。地形の一発生成とノイズブラシで共有する。
f32 TerrainFbm(f32 x, f32 z, f32 frequency, i32 octaves, f32 ridged, u32 seed);

// ブラシを 1 回適用する。dt はストロークの連続適用量（秒）。
// invert=true で Raise↔Lower を入れ替える（Shift ドラッグ用）。
// 戻り値 = 実際に書き換えたグリッド矩形（無効なら何も変えていない）。
HeightField::Rect ApplyTerrainBrush(HeightField& hf, const TerrainBrushParams& p,
                                    f32 lx, f32 lz, f32 dt, bool invert);

// 熱浸食を rect の範囲へ適用する（rect が無効なら全面）。戻り値 = 書き換えた矩形。
HeightField::Rect ApplyThermalErosion(HeightField& hf, f32 talusDeg, i32 iterations,
                                      const HeightField::Rect& rect);

// --- fBm による地形の一発生成（「山を作りたい」への直球）---
struct TerrainGenParams
{
    f32 frequency  = 0.010f;  // 空間周波数（小さいほど大きな起伏）
    i32 octaves    = 5;
    f32 amplitude  = 25.0f;   // 高さの振幅(m)
    f32 ridged     = 0.0f;    // 1 に近いほど鋭い尾根（山脈）
    f32 baseHeight = 0.0f;    // 全体のかさ上げ
    u32 seed       = 1337u;
    // 0 より大きいと外周へ向かって高さを落とす（島にする / 縁が切り立つのを防ぐ）。
    // 1.0 で「中心 1.0 → 外周 0.0」の滑らかな減衰。
    f32 edgeFalloff = 0.0f;
    // 谷を削る量。0 より大きいと低い所をさらに下げて峡谷になる。
    f32 valleyDepth = 0.0f;
};

// プリセット。ユーザーが選ぶだけで「それらしい地形」が出るようにする。
enum class TerrainPreset : int
{
    RollingHills = 0,   // なだらかな丘
    Canyon       = 1,   // 峡谷
    Mountains    = 2,   // 険しい山脈
    Count        = 3,
};

TerrainGenParams MakeTerrainPreset(TerrainPreset preset, f32 worldSize, u32 seed);

// 高さ配列を fBm で丸ごと作り直す（既存の高さは破棄する）。
void GenerateTerrain(HeightField& hf, const TerrainGenParams& p);

} // namespace dx12e
