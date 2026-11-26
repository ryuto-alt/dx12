#pragma once
// ===========================================================================
// UnoFX のカール場（64^3、W1-BD4。MOD-VFX-05）。
//   ベクトルポテンシャル ψ（周期的な値ノイズの 3 オクターブ）の rot（中心差分）を 64^3 に焼いた場。発散はほぼ 0（離散）。
//   GPU は同じ配列を half4 のテクスチャに上げ、ラップ + 三重線形で引く。CPU の参照 SampleCurl() が同じ式（誤差 1e-3 以下）。
//   NB2 が焼く curl64.dds を使えるようになるまで、実行時はこの生成器で作る（決定論。乱数は固定ハッシュ）。
// ===========================================================================
#include <vector>

#include "core/Types.h"
#include "renderer/vfx/VfxTypes.h"

namespace dx12e::vfx
{

// 長さ kCurlRes^3 * 4 の float（xyz = rot ψ を最大長 1 へ正規化、w = 0）。x が最速、次に y、z。
std::vector<float> GenerateCurlField();
// half に丸めた RGBA16F（GPU へ上げる中身。丸めた値が CPU 参照の「正」）。長さ kCurlRes^3 * 4 の u16。
std::vector<u16> GenerateCurlFieldHalf();

// 参照のサンプラ。uvw は 0..1 でラップ（周期）。テクセル中心は GPU と同じ (i + 0.5) / N。field は float RGBA の配列。
void SampleCurl(const std::vector<float>& field, float u, float v, float w, float out[3]);

// 離散の発散（検査用）。内点の中心差分の最大絶対値。
float MaxDivergence(const std::vector<float>& field);

} // namespace dx12e::vfx
