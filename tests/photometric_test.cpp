// renderer/PhotometricMath.h（光の物理単位・露出 EV100・トーンマップ）の単体テスト。
// ヘッダオンリー・標準ライブラリのみ。実行: ctest -R PhotometricMathTests
//
// 守りたいこと:
//   ・EV100 ↔ 露出係数の往復、補正 +1 EV = 2 倍、自動露出が 18% グレーを目標へ写す
//   ・lm ↔ cd の往復、閉形式の照度(逆二乗)と放射輝度(ランバート)
//   ・物理モードの減衰 = 逆二乗 × 窓(範囲が十分大きければ純粋な 1/d²)、従来の減衰との差
//   ・トーンマップの参照値(tools/parity/parity/tonemap.py の numpy 実装が出した値。float64 → float32 で 1e-4 以内)
//     と不変条件(0 → 0・単調・18% 灰 → 18%・白クリップ)
#include "renderer/PhotometricMath.h"

#include <cmath>
#include <cstdio>

using namespace dx12e::photo;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{
bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }
bool NearRel(float a, float b, float rel) { return std::fabs(a - b) <= rel * (std::fabs(b) + 1e-12f); }

struct Golden { Rgb in; Rgb out; };

// numpy 実装(float64)の出力。UE Filmic はリニア sRGB の値（sRGB OETF の前）。
const Golden kUe[] = {
    { { 0.180000f, 0.180000f, 0.180000f }, { 0.18000000f, 0.18000000f, 0.18000000f } },
    { { 1.000000f, 1.000000f, 1.000000f }, { 0.72335946f, 0.72335946f, 0.72335946f } },
    { { 0.010000f, 0.010000f, 0.010000f }, { 0.00165657f, 0.00165657f, 0.00165657f } },
    { { 10.000000f, 10.000000f, 10.000000f }, { 0.99947556f, 0.99947556f, 0.99947556f } },
    { { 0.500000f, 0.200000f, 0.100000f }, { 0.51030361f, 0.22057018f, 0.09567351f } },
    { { 0.050000f, 0.400000f, 0.050000f }, { 0.05312902f, 0.43595007f, 0.04732919f } },
    { { 2.000000f, 0.500000f, 0.100000f }, { 0.92656162f, 0.56681640f, 0.18665671f } },
    { { 0.020000f, 0.030000f, 0.900000f }, { 0.00473850f, 0.01339858f, 0.69738913f } },
    { { 4.000000f, 1.000000f, 0.200000f }, { 0.99667002f, 0.75935392f, 0.42599547f } },
    { { 0.300000f, 0.300000f, 0.050000f }, { 0.33411254f, 0.33287692f, 0.04485773f } },
    { { 0.001000f, 0.002000f, 0.003000f }, { 0.00003193f, 0.00010623f, 0.00019962f } },
    { { 50.000000f, 20.000000f, 5.000000f }, { 1.03476059f, 1.02054789f, 0.98915312f } },
};
const Golden kNeutral[] = {
    { { 0.180000f, 0.180000f, 0.180000f }, { 0.14000000f, 0.14000000f, 0.14000000f } },
    { { 1.000000f, 1.000000f, 1.000000f }, { 0.86909091f, 0.86909091f, 0.86909091f } },
    { { 0.010000f, 0.010000f, 0.010000f }, { 0.00062500f, 0.00062500f, 0.00062500f } },
    { { 10.000000f, 10.000000f, 10.000000f }, { 0.99389831f, 0.99389831f, 0.99389831f } },
    { { 0.500000f, 0.200000f, 0.100000f }, { 0.46000000f, 0.16000000f, 0.06000000f } },
    { { 0.050000f, 0.400000f, 0.050000f }, { 0.01562500f, 0.36562500f, 0.01562500f } },
    { { 2.000000f, 0.500000f, 0.100000f }, { 0.96000000f, 0.32113576f, 0.15077196f } },
    { { 0.020000f, 0.030000f, 0.900000f }, { 0.00755861f, 0.01703071f, 0.84110345f } },
    { { 4.000000f, 1.000000f, 0.200000f }, { 0.98325581f, 0.46829917f, 0.33097740f } },
    { { 0.300000f, 0.300000f, 0.050000f }, { 0.26562500f, 0.26562500f, 0.01562500f } },
    { { 0.001000f, 0.002000f, 0.003000f }, { 0.00000625f, 0.00100625f, 0.00200625f } },
    { { 50.000000f, 20.000000f, 5.000000f }, { 0.99883495f, 0.92695477f, 0.89101468f } },
};
}

int main()
{
    // ---------------------------------------------------------------
    // 1) 露出（EV100）
    // ---------------------------------------------------------------
    {
        // 定義: F = 1/(1.2·2^EV)
        CHECK(NearRel(ExposureScaleFromEv100(0.0f), 1.0f / 1.2f, 1e-6f));
        CHECK(NearRel(ExposureScaleFromEv100(15.0f), 1.0f / (1.2f * 32768.0f), 1e-6f));
        // 1 EV 上げると係数は半分
        CHECK(NearRel(ExposureScaleFromEv100(10.0f) * 0.5f, ExposureScaleFromEv100(11.0f), 1e-6f));
        // 往復
        for (float ev = -8.0f; ev <= 22.0f; ev += 1.75f)
            CHECK(Near(Ev100FromExposureScale(ExposureScaleFromEv100(ev)), ev, 1e-4f));
        // 補正 +1EV = 係数 2 倍（明るく）。EV100 を 1 下げるのと同じ
        CHECK(NearRel(ManualExposureScale(12.0f, 1.0f), 2.0f * ManualExposureScale(12.0f, 0.0f), 1e-6f));
        CHECK(NearRel(ManualExposureScale(12.0f, 1.0f), ManualExposureScale(11.0f, 0.0f), 1e-6f));
        CHECK(NearRel(ManualExposureScale(12.0f, -2.0f), 0.25f * ManualExposureScale(12.0f, 0.0f), 1e-6f));
        // 既定 EV100=15: 太陽 10 万 lux の下の白いランバート面(垂直入射)= 3.18e4 nit → 露出後 0.81
        {
            const float lum = LambertRadiance(1.0f, 100000.0f);
            CHECK(Near(lum * ExposureScaleFromEv100(kDefaultEv100), 0.8089f, 2e-3f));
            const float gray = LambertRadiance(0.18f, 100000.0f) * ExposureScaleFromEv100(kDefaultEv100);
            CHECK(Near(gray, 0.1456f, 1e-3f));
        }
        // 自動露出: 平均輝度を 18% グレーへ写す EV100 で露出すると、平均 → 0.18
        for (float avg : { 0.05f, 3.0f, 250.0f, 8000.0f, 60000.0f })
        {
            const float ev = AutoEv100FromLuminance(avg, 0.0f);
            CHECK(Near(avg * ExposureScaleFromEv100(ev), kMidGray, 1e-4f * kMidGray + 1e-7f));
            // 補正 +1 EV で 2 倍明るい目標
            const float ev1 = AutoEv100FromLuminance(avg, 1.0f);
            CHECK(Near(avg * ExposureScaleFromEv100(ev1), 2.0f * kMidGray, 1e-3f));
        }
        // ヒストグラム範囲の変換は往復し、EV100 と log2 輝度の差は定数
        for (float ev : { -10.0f, 0.0f, 9.7f, 20.0f })
        {
            CHECK(Near(Ev100FromLog2Luminance(Log2LuminanceFromEv100(ev)), ev, 1e-5f));
            CHECK(Near(Log2LuminanceFromEv100(ev) - ev, std::log2(1.2f * 0.18f), 1e-5f));
        }
        // 「その EV100 で 18% グレーが写る輝度」= 1.2·2^EV·0.18 の 1 EV 刻み
        CHECK(Near(std::exp2(Log2LuminanceFromEv100(15.0f)), 1.2f * 0.18f * 32768.0f, 1.0f));
    }

    // ---------------------------------------------------------------
    // 2) 光の単位
    // ---------------------------------------------------------------
    {
        // 点光源: 4π sr。1000 lm = 79.577 cd
        CHECK(Near(LumensToCandelaPoint(1000.0f), 79.5775f, 1e-3f));
        CHECK(Near(CandelaToLumensPoint(LumensToCandelaPoint(1234.5f)), 1234.5f, 1e-2f));
        // スポット: 半頂角 30° の立体角 = 2π(1−cos30°) = 0.8417 sr
        CHECK(Near(SpotSolidAngle(30.0f), 0.84178f, 1e-4f));
        CHECK(Near(CandelaToLumensSpot(LumensToCandelaSpot(800.0f, 25.0f), 25.0f), 800.0f, 1e-2f));
        // 半頂角 90° 近傍（半球）は 2π、極端に大きい値でも有限
        CHECK(Near(SpotSolidAngle(89.999f), 2.0f * kPi, 2e-3f));
        CHECK(std::isfinite(LumensToCandelaSpot(100.0f, 0.0f)));

        // 閉形式: 100 cd の点光源の 2 m 直下 → 25 lux。白いランバート面なら 25/π nit
        CHECK(Near(PointIlluminance(100.0f, 2.0f, 1.0f), 25.0f, 1e-4f));
        CHECK(Near(LambertRadiance(1.0f, PointIlluminance(100.0f, 2.0f, 1.0f)), 25.0f / kPi, 1e-4f));
        // 斜め入射は cosθ
        CHECK(Near(PointIlluminance(100.0f, 2.0f, 0.5f), 12.5f, 1e-4f));
        CHECK(Near(PointIlluminance(100.0f, 2.0f, -0.3f), 0.0f, 1e-6f));   // 裏面は 0
        // 逆二乗: 距離 2 倍で 1/4
        CHECK(NearRel(PointIlluminance(50.0f, 6.0f, 1.0f) * 4.0f, PointIlluminance(50.0f, 3.0f, 1.0f), 1e-5f));

        // 物理モードの減衰: 範囲が十分大きければ純粋な 1/d²（PT の lightFalloff:"physical" と同じ）
        for (float d : { 0.5f, 1.0f, 3.0f, 7.5f, 20.0f })
            CHECK(NearRel(PhysicalFalloff(d, 100000.0f, 0.0f), 1.0f / (d * d), 1e-4f));
        // 窓: range で 0、近傍ではほぼ 1
        CHECK(Near(PhysicalFalloff(10.0f, 10.0f, 0.0f), 0.0f, 1e-9f));
        CHECK(Near(PhysicalFalloff(12.0f, 10.0f, 0.0f), 0.0f, 1e-9f));
        CHECK(NearRel(PhysicalFalloff(1.0f, 10.0f, 0.0f), 1.0f * (1.0f - 1e-4f) * (1.0f - 1e-4f), 1e-6f));
        // 光源半径: d < r では 1/r² で頭打ち（特異点を作らない）
        CHECK(NearRel(PhysicalFalloff(0.0f, 100.0f, 0.5f), 4.0f, 1e-5f));
        CHECK(NearRel(PhysicalFalloff(0.2f, 100.0f, 0.5f), 4.0f, 1e-4f));
        CHECK(PhysicalFalloff(0.0f, 100.0f, 0.0f) < 1.1e4f);   // 半径 0 でも下限（1 cm）で有限
        // 従来の減衰との差（設計書 §3.5: 従来は逆二乗ではない）: range 10 の 3 m 地点
        CHECK(Near(LegacyFalloff(3.0f, 10.0f), 0.49f, 1e-6f));
        CHECK(Near(PhysicalFalloff(3.0f, 10.0f, 0.0f), (1.0f - 0.0081f) * (1.0f - 0.0081f) / 9.0f, 1e-6f));
        CHECK(Near(LegacyFalloff(0.0f, 10.0f), 1.0f, 1e-6f));   // 従来は 0 m で 1（逆二乗は発散する）
        CHECK(Near(LegacyFalloff(12.0f, 10.0f), 0.0f, 1e-9f));
    }

    // ---------------------------------------------------------------
    // 3) トーンマップ
    // ---------------------------------------------------------------
    {
        // 従来 ACES（PostProcess.hlsl の ACESFilm と同じ）
        CHECK(Near(AcesNarkowicz(0.0f), 0.0f, 1e-6f));
        CHECK(Near(AcesNarkowicz(0.18f), 0.26691f, 2e-4f));
        CHECK(Near(AcesNarkowicz(100.0f), 1.0f, 1e-4f));

        // sRGB OETF
        CHECK(Near(LinearToSrgb(0.0f), 0.0f, 1e-7f));
        CHECK(Near(LinearToSrgb(1.0f), 1.0f, 1e-6f));
        CHECK(Near(LinearToSrgb(0.18f), 0.4614f, 5e-4f));
        CHECK(Near(LinearToSrgb(0.002f), 12.92f * 0.002f, 1e-7f));   // 線形区間
        CHECK(Near(LinearToSrgb(2.0f), 1.0f, 1e-6f));                 // クリップ
        CHECK(Near(LinearToSrgb(-1.0f), 0.0f, 1e-7f));

        // 参照値（numpy 実装との突き合わせ）
        for (const Golden& g : kUe)
        {
            const Rgb o = UeFilmicLinear(g.in);
            CHECK(Near(o.r, g.out.r, 2e-4f * (1.0f + g.out.r)));
            CHECK(Near(o.g, g.out.g, 2e-4f * (1.0f + g.out.g)));
            CHECK(Near(o.b, g.out.b, 2e-4f * (1.0f + g.out.b)));
        }
        for (const Golden& g : kNeutral)
        {
            const Rgb o = PbrNeutralLinear(g.in);
            CHECK(Near(o.r, g.out.r, 1e-5f * (1.0f + g.out.r)));
            CHECK(Near(o.g, g.out.g, 1e-5f * (1.0f + g.out.g)));
            CHECK(Near(o.b, g.out.b, 1e-5f * (1.0f + g.out.b)));
        }

        // UE Filmic の不変条件
        {
            const Rgb z = UeFilmicLinear({ 0.0f, 0.0f, 0.0f });
            CHECK(Near(z.r, 0.0f, 1e-6f) && Near(z.g, 0.0f, 1e-6f) && Near(z.b, 0.0f, 1e-6f));
            const Rgb g18 = UeFilmicLinear({ 0.18f, 0.18f, 0.18f });
            CHECK(Near(g18.r, 0.18f, 1e-4f) && Near(g18.g, 0.18f, 1e-4f) && Near(g18.b, 0.18f, 1e-4f));
            // 灰は灰のまま（彩度が生まれない）
            for (float x : { 0.001f, 0.02f, 0.18f, 1.0f, 8.0f })
            {
                const Rgb o = UeFilmicLinear({ x, x, x });
                CHECK(Near(o.r, o.g, 2e-5f * (1.0f + o.r)) && Near(o.g, o.b, 2e-5f * (1.0f + o.g)));
            }
            // 単調増加（灰の対数掃引）
            float prev = -1.0f;
            for (int i = 0; i <= 60; ++i)
            {
                const float x = std::pow(10.0f, -4.0f + 0.1f * static_cast<float>(i));
                const float y = UeFilmicLinear({ x, x, x }).g;
                CHECK(y >= prev - 1e-7f);
                prev = y;
            }
            // 大きな入力は 1 + WhiteClip 付近へ（既定 1.04）
            CHECK(Near(UeFilmicLinear({ 1000.0f, 1000.0f, 1000.0f }).g, 1.04f, 0.01f));
            // ホワイトクリップを 0 にすると 1 に漸近
            FilmParams p; p.whiteClip = 0.0f; p.shoulder = 0.26f;
            CHECK(Near(UeFilmicLinear({ 1000.0f, 1000.0f, 1000.0f }, p).g, 1.0f, 0.01f));
            // ブラッククリップを掛けると暗部が持ち上がらず 0 未満へ潜れる（黒が締まる）
            FilmParams q; q.blackClip = 0.1f;
            CHECK(UeFilmicLinear({ 0.0001f, 0.0001f, 0.0001f }, q).g < UeFilmicLinear({ 0.0001f, 0.0001f, 0.0001f }).g + 1e-9f);
            // 傾き(slope)を上げるとコントラストが上がる（0.18 は固定点のまま）
            FilmParams hi; hi.slope = 1.2f;
            CHECK(Near(UeFilmicLinear({ 0.18f, 0.18f, 0.18f }, hi).g, 0.18f, 1e-3f));
            CHECK(UeFilmicLinear({ 0.5f, 0.5f, 0.5f }, hi).g > UeFilmicLinear({ 0.5f, 0.5f, 0.5f }).g);
        }

        // PBR Neutral の不変条件: 0.8 - 0.04 未満は「オフセットを引くだけ」（暗部はほぼ恒等）
        {
            const Rgb o = PbrNeutralLinear({ 0.5f, 0.5f, 0.5f });
            CHECK(Near(o.r, 0.46f, 1e-6f));
            float prev = -1.0f;
            for (int i = 0; i <= 60; ++i)
            {
                const float x = std::pow(10.0f, -3.0f + 0.1f * static_cast<float>(i));
                const float y = PbrNeutralLinear({ x, x, x }).g;
                CHECK(y >= prev - 1e-7f);
                CHECK(y < 1.0f);
                prev = y;
            }
        }

        // 表示値（sRGB エンコード）: 灰 0.18 → UE は 0.4614、線形クリップは 1 で頭打ち
        CHECK(Near(UeFilmicDisplay({ 0.18f, 0.18f, 0.18f }).g, LinearToSrgb(0.18f), 1e-4f));
        CHECK(Near(LinearClipDisplay({ 0.18f, 5.0f, -1.0f }).r, LinearToSrgb(0.18f), 1e-6f));
        CHECK(Near(LinearClipDisplay({ 0.18f, 5.0f, -1.0f }).g, 1.0f, 1e-6f));
        CHECK(Near(LinearClipDisplay({ 0.18f, 5.0f, -1.0f }).b, 0.0f, 1e-6f));
        // 全モードで 0..1 に収まる
        for (int tm = kTmUeFilmic; tm < kTmCount; ++tm)
        {
            CHECK(IsNewToneMapper(tm));
            for (float x : { 0.0f, 0.001f, 0.18f, 1.0f, 30.0f, 1.0e5f })
            {
                const Rgb o = ToneMapNewDisplay(tm, { x, 0.5f * x, 0.1f * x });
                CHECK(o.r >= 0.0f && o.r <= 1.0f && o.g >= 0.0f && o.g <= 1.0f && o.b >= 0.0f && o.b <= 1.0f);
            }
        }
        CHECK(!IsNewToneMapper(kTmAces) && !IsNewToneMapper(kTmAgx) && !IsNewToneMapper(kTmNone));
        CHECK(!IsNewToneMapper(-1) && !IsNewToneMapper(kTmCount));
    }

    std::printf("PhotometricMathTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
