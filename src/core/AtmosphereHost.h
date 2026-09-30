#pragma once
// ===========================================================================
// AtmosphereHost — 物理ベース大気(A1)の Application 側の状態。
// ---------------------------------------------------------------------------
// 本体は renderer/atmosphere/(専用ルートシグネチャ・専用 LUT)。ここは
//   ・時刻系の結果(今フレームの太陽の向き・光源の色と照度)
//   ・IBL 増分再ベイクの状態機械(空の変化 → 環境キューブ再生成 → irradiance / prefilter を 1 段ずつ)
//   ・計測用の統計
// を持つ。★既定 OFF: atmosphere.enabled になるまで AtmosphereRenderer も確保しない(遅延確保)。
// 実装は core/ApplicationAtmosphere.cpp。
// ===========================================================================
#include <cstdint>

#include "renderer/atmosphere/AtmosphereMath.h"
#include "renderer/atmosphere/AtmosphereRenderer.h"
#include "renderer/atmosphere/AtmosphereShared.h"

namespace dx12e
{

// m_loadedSkyboxPath に入れる「大気の環境」の目印(実在するパスと衝突しない)
inline constexpr const char* kAtmosphereSkyPath = "__atmosphere__";

struct AtmoHost
{
    AtmosphereRenderer renderer;
    bool initFailed = false;

    // ---- 時刻系(UpdateAtmosphereTime の結果)----
    bool enabledPrev = false;                 // 前フレームの enabled(遷移の検出)
    bool iblEnvWanted = false;                // 前フレームの driveIBL(環境の元が変わったことの検出)
    atmosphere::Vec3d sunDir{0.0, 1.0, 0.0};  // 太陽へ向かう単位ベクトル(ワールド)
    atmosphere::SkyLight light;               // LUT に使う光源(太陽 or 月)と地表の色・照度
    double camAltKm = 0.0;
    double unitScale = 1.0;                   // 物理単位 = 1 / 従来単位 = kClassicUnitScale
    double starAngle = 0.0;
    atmosphere::Vec3d poleAxis{0.0, 1.0, 0.0};
    bool timeValid = false;

    // ---- 主ビューで作った GPU 定数(副ビューが行列だけ差し替えて流用する)----
    atmosphere::AtmosphereGpuParams params{};
    bool paramsValid = false;
    uint64_t recordedFrame = ~0ull;           // 主ビューが LUT を作ったフレーム(m_framesSinceStart)。副ビューはこれが今フレームのときだけ使える
    bool forceLut = true;                     // 次の Update で全 LUT を作り直す

    // ---- IBL 増分再ベイク ----
    bool     iblValid = false;                // 環境キューブ + IBLBaker がこの大気で焼けている
    bool     iblRunning = false;              // 段を進行中
    bool     iblBurst = false;                // 今の再ベイクは 1 フレームで全段を焼く(大きな変化)
    uint32_t iblStage = 0;                    // 次に走らせる段(0..IBLBaker::kRebakeStageCount-1)
    atmosphere::Vec3d iblSunDir{0.0, 1.0, 0.0};   // 最後に焼いた(焼き始めた)ときの太陽の向き
    double   iblAltKm = 0.0;
    uint64_t iblKey = 0;                      // 焼いたときの設定の鍵(媒質・照度・単位)
    double   iblSinceStart = 1.0e9;           // 直近の再ベイク開始からの実時間[s]
    uint32_t iblRebakeCount = 0;              // 開始した再ベイクの回数(統計)
    uint32_t iblBigCount = 0;                 // 1 フレームで全部焼いた回数(統計)
    float    lastSunDeltaDeg = 0.0f;          // 最後に再ベイクを判定したときの太陽の変化角(診断)
};

} // namespace dx12e
