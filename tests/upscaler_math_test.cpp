// 時間方向アップスケーラ（DLSS）の純関数テスト（renderer/UpscalerMath.h。GPU 不要）。
//
// 守る不変条件:
//   (A) ジッタの位相数は 8 ×（出力/内部）²（DLAA 8 / Quality 18 / Performance 32）で、下限 8・上限 64。
//   (B) テクスチャのミップバイアスは log2(内部/出力)。DLAA / ネイティブは 0（既存の絵を変えない）。
//   (C) NDC のジッタ → 内部解像度のピクセルへの変換が TaaJitter.h の規約（jx = 2*hx/W, jy = -2*hy/H）の逆になっている。
//   (D) 品質モードの公称倍率と文字列の往復。
// 実行: ctest --output-on-failure

#include "renderer/UpscalerMath.h"
#include "renderer/TaaJitter.h"

#include <cmath>
#include <cstdio>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}
bool feq(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }
} // namespace

int main()
{
    using namespace dx12e::upscale;

    // (A) 位相数
    Check(JitterPhaseCount(1920, 1920) == 8,  "DLAA は 8 位相");
    Check(JitterPhaseCount(1280, 1920) == 18, "Quality(1.5x) は 18 位相");
    Check(JitterPhaseCount(960, 1920) == 32,  "Performance(2x) は 32 位相");
    Check(JitterPhaseCount(1114, 1920) >= 23 && JitterPhaseCount(1114, 1920) <= 24, "Balanced(≒1.72x) は 24 前後");
    Check(JitterPhaseCount(400, 1920) == 64,  "上限 64");
    Check(JitterPhaseCount(0, 1920) == 8,     "0 除算しない");

    // (B) ミップバイアス
    Check(TextureMipBias(1920, 1920) == 0.0f, "DLAA は 0");
    Check(TextureMipBias(2000, 1920) == 0.0f, "内部 > 出力でも 0");
    Check(feq(TextureMipBias(960, 1920), -1.0f), "Performance は -1");
    Check(feq(TextureMipBias(1280, 1920), std::log2(2.0f / 3.0f)), "Quality は log2(2/3) ≒ -0.585");

    // (C) ジッタのピクセル変換は ComputeTaaJitterNdc の逆
    for (u32 i = 0; i < 32; ++i)
    {
        const u32 w = 1280, h = 720;
        const TaaJitterNdc j = ComputeTaaJitterNdc(i, w, h, 1.0f);
        const JitterPx p = NdcJitterToPixels(j.x, j.y, w, h);
        const float hx = HaltonRadicalInverse(i + 1, 2) - 0.5f;
        const float hy = HaltonRadicalInverse(i + 1, 3) - 0.5f;
        Check(feq(p.x, hx) && feq(p.y, hy), "NDC → ピクセルはハルトン値（-0.5..0.5）に戻る");
        Check(p.x >= -0.5f && p.x <= 0.5f && p.y >= -0.5f && p.y <= 0.5f, "ジッタは ±0.5px 以内");
    }

    // (D) 品質モード
    u32 rw = 0, rh = 0;
    NominalRenderSize(1920, 1080, DlssQuality::Quality, rw, rh);
    Check(rw == 1280 && rh == 720, "Quality 1080p → 1280x720");
    NominalRenderSize(1920, 1080, DlssQuality::Performance, rw, rh);
    Check(rw == 960 && rh == 540, "Performance 1080p → 960x540");
    NominalRenderSize(1920, 1080, DlssQuality::Dlaa, rw, rh);
    Check(rw == 1920 && rh == 1080, "DLAA は等倍");
    NominalRenderSize(20, 20, DlssQuality::Performance, rw, rh);
    Check(rw == 16 && rh == 16, "最小 16px");
    for (int q = 0; q < 4; ++q)
    {
        DlssQuality back{};
        Check(ParseQuality(QualityName(static_cast<DlssQuality>(q)), back) && static_cast<int>(back) == q, "名前の往復");
    }
    DlssQuality dummy{};
    Check(!ParseQuality("ultra", dummy), "知らない名前は false");
    Check(ClampQuality(9) == DlssQuality::Performance && ClampQuality(-3) == DlssQuality::Dlaa, "範囲外は丸める");

    std::printf("UpscalerMathTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
