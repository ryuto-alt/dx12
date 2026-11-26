#pragma once
// ===========================================================================
// UnoFX（W1-BD4）: GPU とやりとりする型と定数。shaders/vfx/VfxCommon.hlsli と一致させること
//   （サイズとオフセットは下の static_assert で固定。HLSL 側を変えたらここも変える）。
//
// 設計の要点（MOD-VFX-03 / 04 / 05 / 10 / 11）:
//   * 粒子は emitter インスタンスごとの「リングバッファ」に置く。粒子 id（= そのインスタンスの通し番号）が
//     slot = id % cap を決める。生成にアトミックも dead リストも使わないので、生成順・描画順・結果が決定論になる。
//   * 放出は 1 ステップにつき 1 dispatch（モジュールのスタックを GPU が順に実行する）。
//   * シミュレーションは固定ステップ（seek）または実時間 1 ステップ（再生）。粒子ごとの乱数は (seed, id) のハッシュだけ。
//   * 「生きている粒子の数」だけは wave 単位にまとめたアトミック 1 回（WaveActiveCountBits）で数える（統計用。描画順には関与しない）。
// ===========================================================================
#include <cstddef>

#include "core/Types.h"

namespace dx12e::vfx
{

// ---- 粒子 1 個（64B 以下。MOD-VFX-04）----
struct GPart
{
    float pos[3];  float age;      // 16
    float vel[3];  float life;     // 32（life == 0 は死んでいる）
    u32   id;                      // 36 そのインスタンスの通し番号（乱数のシード）
    u32   col0[2];                 // 44 開始色 half4（r|g<<16, b|a<<16）
    u32   col1[2];                 // 52 終了色 half4
    u32   size;                    // 56 half2（開始サイズ, 終了サイズ）
    u32   rot;                     // 60 half2（角度 rad, 角速度 rad/s）
    u32   flags;                   // 64 予約（フリップブックの開始フレーム等。BD7）
};
static_assert(sizeof(GPart) <= 64, "UnoFX: GPart は 64B 以下（MOD-VFX-04）");
static_assert(sizeof(GPart) == 64, "UnoFX: GPart のレイアウトが HLSL と一致していること");
static_assert(offsetof(GPart, life) == 28 && offsetof(GPart, id) == 32 && offsetof(GPart, flags) == 60, "GPart のオフセット");

// ---- モジュールの命令 1 個（GPU のスタックが順に実行する）----
enum OpCode : u32
{
    // 放出の時に 1 回（init スタック）
    kOpShape    = 1,   // a0 = 種類(0 point,1 sphere,2 box,3 cone,4 disc), a1 = 表面のみ(0/1), a2 = 円錐の半角(度)
                       // b = (半径 or 半寸法 x, y, z, 円錐の長さ), c.x = 内半径(disc / sphere の殻)
    kOpVelocity = 2,   // a0 = モード(0 direction,1 radial,2 shape), a1 = 広がり(度), a2 = 放出体の速度の継承率
                       // b.xyz = 方向(放出体のローカル), c.xy = 速さの範囲
    kOpLifetime = 3,   // c.xy = 寿命の範囲
    kOpSize     = 4,   // b = (開始の最小, 最大, 終了の最小, 最大)
    kOpColor    = 5,   // b = 開始 rgba, c = 終了 rgba, a0 = 明るさのばらつき(0..1)
    kOpRotation = 6,   // b = (角度の最小, 最大 [deg], 角速度の最小, 最大 [deg/s])
    // 毎ステップ（update スタック）
    kOpGravity  = 16,  // a = 加速度
    kOpDrag     = 17,  // a0 = 係数 k（v *= exp(-k dt)）
    kOpCurl     = 18,  // a0 = 強さ(加速度), a1 = スケール(1/m 相当), a2 = 時間の流れの速さ
    kOpVortex   = 19,  // a = 軸, b = (中心 xyz, 強さ), c.x = 半径
    kOpAttract  = 20,  // b = (位置 xyz, 強さ), c = (半径, 柔らかさ, 0, 0)
    kOpFloor    = 21,  // a0 = 高さ, a1 = 反発, a2 = 摩擦, b.x = 当たったら消す(0/1)
};

struct GOp
{
    u32   op;
    float a[3];
    float b[4];
    float c[4];
};
static_assert(sizeof(GOp) == 48, "UnoFX: GOp は 48B");

// ---- 定数 ----
inline constexpr u32   kPoolCapacity      = 262144;   // 全インスタンスの粒子の合計（16MB）
inline constexpr u32   kMaxOpsTotal       = 16384;    // 全アセットの命令の合計
inline constexpr u32   kMaxOpsPerStage    = 32;
inline constexpr u32   kMaxStatSlots      = 256;
inline constexpr u32   kMaxEmittersPerAsset = 8;
inline constexpr float kSeekStepDt        = 1.0f / 60.0f;   // seek の固定ステップ
inline constexpr u32   kMaxSeekSteps      = 3600;           // 60 秒ぶん
inline constexpr float kMaxLiveDt         = 0.1f;           // 再生 1 ステップの上限（ヒッチで暴れない）
inline constexpr u32   kMaxEmitPerStep    = 16384;          // 1 ステップの放出の上限（超えた分は持ち越す）
inline constexpr u32   kCurlRes           = 64;             // カール場（焼き込み）の一辺
inline constexpr float kCurlWorldSize     = 8.0f;           // scale=1 のとき場が 1 周する長さ（m）
inline constexpr u32   kSimThreads        = 64;

// 描画のブレンド（premultiplied: additive は alpha 0 で出す）
enum class Blend : u8 { Additive, Alpha };
enum class RenderKind : u8 { Sprite, Flipbook };
enum class Fade : u8 { None, Linear, Quad, InOut };

} // namespace dx12e::vfx
