#pragma once
// ===========================================================================
// IblRebakePolicy（W1-C9 / BUG-17）: 物理大気が駆動する IBL を「いつ焼き直すか」「撮影はいつ待つか」の純粋な判定。
// ---------------------------------------------------------------------------
// ヘッダだけで完結（GPU / Application に依存しない）。ApplicationAtmosphere.cpp が毎フレーム呼び、
// tests/ibl_rebake_trigger_test.cpp が決定表を検査する。
//   焼き直しのきっかけ:
//     1. 大気の ON/OFF（driveIBL の切り替えを含む）… Application 側が iblValid を落として全部焼き直す（ここでは扱わない。待ちの判定だけ）
//     2. 設定の鍵（媒質・照度・色・単位）の変化 / 太陽が 5° 以上動いた / 決定論撮影 … 1 フレームで全段（burst）
//     3. 太陽の高度が前回の焼きから 0.5° 以上動いた … 開始（ユーザー設定の閾値が緩くても 0.5° を超えて放置しない）
//     4. 太陽の向きが iblRebakeThresholdDeg を超えた / カメラ高度が動いた … 従来どおり
//   ヒステリシス: 焼き始めの太陽の向きを基準にし直す（基準を動かさないと毎フレーム走る）。開始は iblRebakeMaxHz で間引く。
//   進行中（iblRunning）は新しい開始を判定しない。
// ===========================================================================
#include <algorithm>
#include <cmath>

namespace dx12e::atmosphere
{

inline constexpr double kIblBurstAngleDeg      = 5.0;    // これ以上動いたら 1 フレームで全段
inline constexpr double kIblElevationTriggerDeg = 0.5;   // 太陽の高度の変化がこれ以上なら必ず焼き直す
inline constexpr int    kIblWaitBaseFrames     = 3;      // 撮影を待つフレーム数の下限（焼き直し要求 → 次フレーム冒頭で焼く → 描く）

struct IblRebakeInput
{
    bool   deterministic   = false;   // 決定論撮影中（時間を待たず全段を 1 フレームで）
    bool   keyChanged      = false;   // 媒質・照度・色・単位の鍵が前回の焼きと違う
    double sunAngleDeg     = 0.0;     // 前回の焼きの太陽の向きとの角度（>= 0）
    double sunElevDeltaDeg = 0.0;     // 前回の焼きからの太陽の高度の変化（符号つきでも絶対値を取る）
    double altDeltaM       = 0.0;     // カメラ高度の変化[m]
    double altBaseM        = 0.0;     // 前回の焼きのカメラ高度[m]
    double thresholdDeg    = 0.25;    // AtmosphereSettings::iblRebakeThresholdDeg
    double maxHz           = 2.0;     // AtmosphereSettings::iblRebakeMaxHz
    double sinceStartSec   = 1.0e9;   // 直近の焼き開始からの実時間
};

struct IblRebakeDecision
{
    bool need  = false;   // 焼き直しが要る
    bool start = false;   // 今フレームから焼き始める
    bool burst = false;   // 1 フレームで全段
};

inline IblRebakeDecision DecideIblRebake(const IblRebakeInput& in)
{
    IblRebakeDecision d;
    d.burst = in.keyChanged || in.sunAngleDeg > kIblBurstAngleDeg || in.deterministic;
    const double thr = std::max(in.thresholdDeg, 1.0e-4);
    const bool sunMoved    = in.sunAngleDeg > thr;
    const bool elevMoved   = std::fabs(in.sunElevDeltaDeg) >= kIblElevationTriggerDeg;
    const bool altMoved    = in.altDeltaM > std::max(50.0, 0.1 * in.altBaseM);
    d.need = d.burst || sunMoved || elevMoved || altMoved;
    const double minInterval = 1.0 / std::max(in.maxHz, 0.1);
    d.start = d.need && (d.burst || in.sinceStartSec >= minInterval);
    return d;
}

// 撮影が「焼き直しの完了」を待つかの判定に使う、Application の状態の写し。
struct IblWaitState
{
    bool   sceneEnabled   = false;   // シーンの atmosphere.enabled
    bool   sceneDriveIbl  = false;   // シーンの atmosphere.driveIBL
    bool   hostExists     = false;   // AtmoHost がある（一度でも ON になった）
    bool   enabledPrev    = false;   // ホストが見た前フレームの enabled
    bool   envWantedPrev  = false;   // ホストが見た前フレームの driveIBL
    bool   iblValid       = false;
    bool   envLoaded      = false;   // 大気の環境キューブが IBL として載っている（m_loadedSkyboxPath == 大気）
    bool   iblRunning     = false;   // 増分の再ベイクの途中
    bool   skyboxDirty    = false;   // 次フレーム冒頭の再ベイク要求が立っている
    double sunAngleDeg    = 0.0;     // 今の時刻の太陽の向きと、焼いた太陽の向きの角度
    double sunElevDeltaDeg = 0.0;
    double thresholdDeg   = 0.25;
};

// 焼き直しが終わっていない（または、始まる前で確実に起きる）なら true。
inline bool IblRebakePending(const IblWaitState& s)
{
    if (s.skyboxDirty) return true;
    if (!s.sceneEnabled) return s.hostExists && s.enabledPrev;       // ON → OFF の後始末が未了
    if (!s.hostExists || !s.enabledPrev) return true;                // OFF → ON が未了
    if (s.envWantedPrev != s.sceneDriveIbl) return true;             // driveIBL の切り替えが未了
    if (!s.sceneDriveIbl) return false;                              // 大気が IBL を駆動しないなら焼き直しは無い
    if (!s.iblValid || !s.envLoaded || s.iblRunning) return true;
    return s.sunAngleDeg > std::max(s.thresholdDeg, 1.0e-4) || std::fabs(s.sunElevDeltaDeg) >= kIblElevationTriggerDeg;
}

// 撮影の収束フレームに足す数。待たなくてよいなら 0。
inline int IblRebakeWaitFrames(const IblWaitState& s, int stageCount)
{
    if (!IblRebakePending(s)) return 0;
    return kIblWaitBaseFrames + (s.iblRunning ? std::max(stageCount, 0) : 0);
}

} // namespace dx12e::atmosphere
