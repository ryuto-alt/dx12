#pragma once
// ===========================================================================
// AtmosphereSettings — 物理ベース大気(A1 / Hillaire 2020)のシーン単位設定
// ---------------------------------------------------------------------------
// シーン JSON の "atmosphere" に保存する(既定と同じなら書かない)。既定 OFF(enabled=false)＝従来の空・従来の時刻曲線・従来の IBL で
// 絵はビット一致。ヘッダオンリー・標準ライブラリのみ(GPU にも DirectXMath にも依存しない。単体テストがそのまま使う)。
//
// 単位
//   ・距離はパラメータ内では km(惑星半径・大気厚・散乱係数の 1/km)。ワールドは m(1 単位 = 1 m)、y が上。
//   ・時刻 timeOfDay は「現地太陽時」0..24(正午 = 太陽が南中。日の出/日の入りは春分・秋分で 6 時/18 時)。
//   ・方位はエンジン規約(+Z = 0°、+X = 90°、時計回り)。既定では +Z を北とする(northYawDeg でずらす)。
//   ・sunIlluminance は「大気の上端での太陽の照度 [lux]」。地表ではオゾン・レイリー・ミー透過を受けて約 8〜10 万 lux になる。
//     物理ライティング単位(lightingUnits)のときは太陽ライトの lux・空の nit がそのまま出る。従来単位のときは
//     kClassicUnitScale を掛けて従来の任意単位(太陽 intensity ≈ 3)へ写す(atmosphere::ClassicUnitScale)。
// ===========================================================================
#include <algorithm>
#include <cstdint>
#include <string>

namespace dx12e
{

struct AtmosphereSettings
{
    // ---- 切替 ----
    bool  enabled = false;                 // false = 従来の空(既定)/ true = 物理大気

    // ---- 時刻系 ----
    float timeOfDay   = 12.0f;             // 0..24(現地太陽時)
    float timeSpeed   = 0.0f;              // Play 中にゲーム内時間(時間)を進める速さ [時間/実秒]。0 = 止める(例 0.1 → 1 日 = 240 秒)
    float latitudeDeg = 35.0f;             // 緯度(北が正)。-90..90
    int   dayOfYear   = 81;                // 1..365(81 = 春分。日の出/日の入り = 6 時/18 時)
    float northYawDeg = 0.0f;              // ワールド +Z が北から時計回りに何度ずれているか(方位の回転)
    int   sunMode     = 0;                 // 0 = 時刻から太陽の向きを決める / 1 = 太陽 DirectionalLight の向きを直接使う
    bool  driveSun    = true;              // 太陽ライトの向き(時刻モードのみ)・色・強度を大気(透過率)から決める
    bool  driveIBL    = true;              // 空を環境マップ(IBL/DDGI の空項)へ反映する(OFF なら環境光は従来のまま)
    bool  drawStars   = true;              // 夜の星
    bool  drawMoon    = true;              // 夜の月(円盤 + 光源)

    // ---- 物理パラメータ(既定 = 地球。値の出典は sebh/UnrealEngineSkyAtmosphere(MIT)の既定値)----
    float planetRadiusKm      = 6360.0f;
    float atmosphereHeightKm  = 100.0f;
    float rayleighScattering[3] = {0.005802f, 0.013558f, 0.033100f};   // 1/km(海面)
    float rayleighScaleHeightKm = 8.0f;
    float mieScattering[3]    = {0.003996f, 0.003996f, 0.003996f};     // 1/km(海面)
    float mieAbsorption[3]    = {0.000444f, 0.000444f, 0.000444f};     // 1/km(海面。消散 = 散乱 + 吸収)
    float mieScaleHeightKm    = 1.2f;
    float mieG                = 0.8f;                                  // Henyey-Greenstein の非対称度
    float ozoneAbsorption[3]  = {0.000650f, 0.001881f, 0.000085f};     // 1/km(層の中心での値)
    float ozoneCenterKm       = 25.0f;                                 // オゾン層の中心高度(2 層のテント。幅 = 中心の値の 1 倍)
    float ozoneWidthKm        = 15.0f;                                 // 中心から片側の幅(0 でオゾン無し)
    float groundAlbedo[3]     = {0.3f, 0.3f, 0.3f};                    // 地表アルベド(地平線下の色・多重散乱の地面反射)
    float sunAngularRadius    = 0.004675f;                             // 太陽の視半径 [rad](約 0.268°)
    float sunIlluminance      = 128000.0f;                             // 大気上端の太陽照度 [lux]
    float sunTint[3]          = {1.0f, 1.0f, 1.0f};                    // 太陽色の補正(白 = 上端では無彩色)
    float moonIlluminance     = 0.25f;                                 // 満月の照度 [lux](夜の光源)
    float moonTint[3]         = {0.62f, 0.78f, 1.0f};                  // 月光の色(青白い)
    float multiScatteringFactor = 1.0f;                                // 多重散乱の強さ(0 = 単散乱のみ。テスト用にも使う)
    float seaLevelY           = 0.0f;                                  // ワールド y のうち標高 0 に当たる高さ [m]
    float nightSkyNits        = 0.0015f;                               // 夜空の下限輝度(星明かり・大気光)[nit]。青みは内部で付ける
    float skyLuminanceScale   = 1.0f;                                  // 空(スカイパス)の明るさ倍率(IBL には掛けない。演出用)

    // ---- エアリアルパースペクティブ(遠景の霞)----
    bool  aerialPerspective    = true;                                 // false で遠景の霞(AP)を描かない
    float apStartDepth         = 100.0f;                               // [m] これより手前は AP を掛けない(UE の既定と同じ)
    float apMaxDistanceKm      = 64.0f;                                // AP の froxel が届く最大距離(それより遠くは端の値)
    float apStrength           = 1.0f;                                 // AP の濃さ倍率(1 = 物理どおり。演出用)

    // ---- IBL の再ベイク(増分)----
    float iblRebakeThresholdDeg = 0.25f;                               // 太陽の向きがこれだけ動くまで再ベイクしない
    float iblRebakeMaxHz        = 2.0f;                                // 再ベイクを始める頻度の上限
    // 大きな変化(シーン読込・パラメータ変更・>5°)は 1 フレームで全部焼く。小さな変化は数フレームに分けて焼く。

    bool operator==(const AtmosphereSettings& o) const;
    bool operator!=(const AtmosphereSettings& o) const { return !(*this == o); }
};

inline bool AtmosphereSettings::operator==(const AtmosphereSettings& o) const
{
    auto eq3 = [](const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; };
    return enabled == o.enabled && timeOfDay == o.timeOfDay && timeSpeed == o.timeSpeed
        && latitudeDeg == o.latitudeDeg && dayOfYear == o.dayOfYear && northYawDeg == o.northYawDeg
        && sunMode == o.sunMode && driveSun == o.driveSun && driveIBL == o.driveIBL
        && drawStars == o.drawStars && drawMoon == o.drawMoon
        && planetRadiusKm == o.planetRadiusKm && atmosphereHeightKm == o.atmosphereHeightKm
        && eq3(rayleighScattering, o.rayleighScattering) && rayleighScaleHeightKm == o.rayleighScaleHeightKm
        && eq3(mieScattering, o.mieScattering) && eq3(mieAbsorption, o.mieAbsorption)
        && mieScaleHeightKm == o.mieScaleHeightKm && mieG == o.mieG
        && eq3(ozoneAbsorption, o.ozoneAbsorption) && ozoneCenterKm == o.ozoneCenterKm && ozoneWidthKm == o.ozoneWidthKm
        && eq3(groundAlbedo, o.groundAlbedo) && sunAngularRadius == o.sunAngularRadius
        && sunIlluminance == o.sunIlluminance && eq3(sunTint, o.sunTint)
        && moonIlluminance == o.moonIlluminance && eq3(moonTint, o.moonTint)
        && multiScatteringFactor == o.multiScatteringFactor && seaLevelY == o.seaLevelY
        && nightSkyNits == o.nightSkyNits && skyLuminanceScale == o.skyLuminanceScale
        && aerialPerspective == o.aerialPerspective && apStartDepth == o.apStartDepth
        && apMaxDistanceKm == o.apMaxDistanceKm && apStrength == o.apStrength
        && iblRebakeThresholdDeg == o.iblRebakeThresholdDeg && iblRebakeMaxHz == o.iblRebakeMaxHz;
}

// ---- プリセット(パラメータの一括設定。enabled / 時刻系 / driveSun 等の「使い方」は保つ)----
enum class AtmospherePreset : int
{
    Earth = 0,      // 地球(晴天。既定値)
    Mars,           // 火星風(薄い大気・塵の赤い空・夕焼けが青い)
    Haze,           // 霞(ミー散乱を強く。遠景が白く煙る)
    Twilight,       // 薄明(日の入り直後の時刻 + 少し霞)
    Count
};

inline const char* AtmospherePresetId(AtmospherePreset p)
{
    switch (p)
    {
        case AtmospherePreset::Earth:    return "earth";
        case AtmospherePreset::Mars:     return "mars";
        case AtmospherePreset::Haze:     return "haze";
        case AtmospherePreset::Twilight: return "twilight";
        default: return "earth";
    }
}

inline bool AtmospherePresetFromId(const std::string& id, AtmospherePreset& out)
{
    for (int i = 0; i < static_cast<int>(AtmospherePreset::Count); ++i)
        if (id == AtmospherePresetId(static_cast<AtmospherePreset>(i))) { out = static_cast<AtmospherePreset>(i); return true; }
    return false;
}

// 物理パラメータだけを既定(地球)へ戻したうえでプリセットの値を書く。enabled / driveSun / driveIBL / 時刻系(Twilight の時刻を除く)は触らない。
inline void ApplyAtmospherePreset(AtmosphereSettings& s, AtmospherePreset p)
{
    const AtmosphereSettings d;   // 地球の既定
    auto copy3 = [](float* dst, const float* src) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; };
    s.planetRadiusKm = d.planetRadiusKm;  s.atmosphereHeightKm = d.atmosphereHeightKm;
    copy3(s.rayleighScattering, d.rayleighScattering);  s.rayleighScaleHeightKm = d.rayleighScaleHeightKm;
    copy3(s.mieScattering, d.mieScattering);  copy3(s.mieAbsorption, d.mieAbsorption);
    s.mieScaleHeightKm = d.mieScaleHeightKm;  s.mieG = d.mieG;
    copy3(s.ozoneAbsorption, d.ozoneAbsorption);  s.ozoneCenterKm = d.ozoneCenterKm;  s.ozoneWidthKm = d.ozoneWidthKm;
    copy3(s.groundAlbedo, d.groundAlbedo);  s.sunAngularRadius = d.sunAngularRadius;
    s.sunIlluminance = d.sunIlluminance;  copy3(s.sunTint, d.sunTint);
    s.multiScatteringFactor = d.multiScatteringFactor;
    switch (p)
    {
        case AtmospherePreset::Earth:
            break;
        case AtmospherePreset::Mars:
        {
            // 薄い CO2 の大気 + 赤い塵。昼は黄土色、日没は太陽の周りが青くなる(前方散乱の強い細かい塵)。
            s.planetRadiusKm = 3390.0f;  s.atmosphereHeightKm = 60.0f;
            s.rayleighScattering[0] = 0.0000090f; s.rayleighScattering[1] = 0.0000170f; s.rayleighScattering[2] = 0.0000420f;
            s.rayleighScaleHeightKm = 11.0f;
            s.mieScattering[0] = 0.0045f; s.mieScattering[1] = 0.0030f; s.mieScattering[2] = 0.0016f;
            s.mieAbsorption[0] = 0.0003f; s.mieAbsorption[1] = 0.0007f; s.mieAbsorption[2] = 0.0016f;
            s.mieScaleHeightKm = 11.0f;  s.mieG = 0.65f;
            s.ozoneAbsorption[0] = s.ozoneAbsorption[1] = s.ozoneAbsorption[2] = 0.0f;
            s.groundAlbedo[0] = 0.34f; s.groundAlbedo[1] = 0.22f; s.groundAlbedo[2] = 0.14f;
            s.sunIlluminance = 128000.0f * 0.43f;   // 太陽までの距離が 1.52 倍 → 照度は約 0.43 倍
            s.sunAngularRadius = 0.003070f;
            break;
        }
        case AtmospherePreset::Haze:
        {
            for (int i = 0; i < 3; ++i) { s.mieScattering[i] *= 6.0f; s.mieAbsorption[i] *= 6.0f; }
            s.mieScaleHeightKm = 1.8f;  s.mieG = 0.72f;
            break;
        }
        case AtmospherePreset::Twilight:
        {
            for (int i = 0; i < 3; ++i) { s.mieScattering[i] *= 2.0f; s.mieAbsorption[i] *= 2.0f; }
            s.mieG = 0.78f;
            s.timeOfDay = 18.35f;    // 日没の直後(太陽は地平線の少し下。空の高い所が赤〜青紫)
            break;
        }
        default: break;
    }
}

namespace atmosphere
{
// 従来単位(lightingUnits = 0)のとき、物理量(lux / nit)へ掛けて従来の任意単位へ写す係数。
// 太陽が地表で約 10 万 lux のとき、従来の太陽 intensity 3.0 と同じ明るさになる。
inline constexpr float kClassicUnitScale = 3.0f / 100000.0f;

// スカイパスのピクセルへ書く値の上限(fp16 の 65504 を超えると inf がポスト・TAA へ漏れるため)。
inline constexpr float kMaxSkyRadiance = 6.0e4f;
} // namespace atmosphere

} // namespace dx12e
