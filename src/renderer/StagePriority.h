#pragma once
// ===========================================================================
// ステージ内の実行順序の帯（W05-25 / PROGRAM_PLAN「ステージ内の順序」）。
//   値が小さいほど先に走る。帯は 100 刻みで、レーンと機能ごとに予約する。
//   新しい機能は、帯の中の番号をこのファイルに書いてから使う。この表を変えてよいのは各 Wave の IF チャンクだけ。
//   同じ優先度は登録順（FeatureRegistry が安定ソートする）。
// ===========================================================================
#include "core/Types.h"

namespace dx12e
{
namespace stage_priority
{

struct Band
{
    i32 lo;   // 含む
    i32 hi;   // 含む
    constexpr bool Contains(i32 p) const { return p >= lo && p <= hi; }
};

// ---- AfterDepthPrepass ----
inline constexpr Band kAfterDepthPrepass_C      {   0,  99 };   // C: 深度に依る前処理
inline constexpr Band kAfterDepthPrepass_VfxSim { 100, 199 };   // BD-VFX: VfxSim（シミュレーションを描画から切り離す。副ビューでも使える）
inline constexpr Band kAfterDepthPrepass_BdB    { 200, 299 };   // BD-B

// ---- AfterTranslucent ----
inline constexpr Band kAfterTranslucent_C       {   0,  99 };   // C: MH-7 のガラス
inline constexpr Band kAfterTranslucent_Vfx     { 100, 199 };   // BD-VFX: 描画 -> VFX-07 の合成 -> VFX-10 の屈折（VfxDraw は帯の先頭）
inline constexpr Band kAfterTranslucent_BdB     { 200, 299 };   // BD-B: PI-03 の後ろのコピー

// ---- 他のステージ（既定の帯。必要になった Wave の IF チャンクで細かく割り当てる）----
inline constexpr Band kDefault_C   {   0,  99 };
inline constexpr Band kDefault_Vfx { 100, 199 };
inline constexpr Band kDefault_BdB { 200, 299 };

// 既定の優先度（帯の先頭）。
inline constexpr i32 kFirstC   = kDefault_C.lo;
inline constexpr i32 kFirstVfx = kDefault_Vfx.lo;
inline constexpr i32 kFirstBdB = kDefault_BdB.lo;

} // namespace stage_priority
} // namespace dx12e
