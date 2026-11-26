// 自動生成: tools/gen_boot_sound_calibrate.py（W1-BD1）。手で編集しないこと（直すときは生成器を直して再実行する）。
// 出典: 起動音 Calibrate（graphite_1.wav）の実際の配置。assets/editor/sounds/calibrate/events.json と同じ値。
// 時刻は graphite_1.wav の先頭からの ms（make.py の LEAD = 8ms を含む）。sample は 44.1kHz のサンプル位置。
#pragma once

#include <cstddef>
#include <cstdint>

namespace dx12e::splash::calibrate
{

inline constexpr int kSampleRate = 44100;
inline constexpr double kLengthMs = 3750.0;
inline constexpr int kLengthSamples = 165375;
inline constexpr double kLockMs = 1628.0045;      // T_lock（ロック。ラッチ 1 / サブヒット）
inline constexpr int kLockSample = 71795;
inline constexpr double kHaasMs = 0.35;         // パンの片側の遅延の最大（|pan| に比例）
inline constexpr double kSweepStartMs = 8.0045;   // 指針の風切りの層（Reduce Motion では無し）
inline constexpr double kSweepEndMs = 1528.0045;
inline constexpr double kBedLoopStartMs = 1200.0;   // stems/bed.wav のループ区間
inline constexpr double kBedLoopEndMs = 1600.0;
inline constexpr double kBedLoopCrossfadeMs = 40.0;

inline constexpr int kClickCount = 24;
// ラチェット C00〜C23 の時刻（ms）と、そのときの指針の角度（度。15 x (i + 1)）。パンは kEvents の pan
inline constexpr double kClickMs[kClickCount] = {
    268.0045, 358.8209, 448.7982, 536.2585, 619.7052, 697.8231,
    769.5692, 834.263, 891.5873, 941.61, 984.7846, 1021.9274,
    1054.195, 1082.9478, 1109.7959, 1136.4626, 1165.6236, 1199.7732,
    1240.771, 1289.4785, 1345.7143, 1408.2086, 1474.8753, 1542.9932
};
inline constexpr double kClickAngleDeg[kClickCount] = {
    15.0, 30.0, 45.0, 60.0, 75.0, 90.0,
    105.0, 120.0, 135.0, 150.0, 165.0, 180.0,
    195.0, 210.0, 225.0, 240.0, 255.0, 270.0,
    285.0, 300.0, 315.0, 330.0, 345.0, 360.0
};

enum class EventKind : std::uint8_t
{
    Duck, Click, Latch, Tok, Sub, Bell, Ping, End
};

struct Event
{
    double tMs;
    int sample;
    EventKind kind;
    int index;
    double pan;      // -1..1（パン前の値。constant-power の則は events.json の panLaw）
    double gainDb;   // master 処理の前のゲイン（参考値）
    const char* note;
};

// 時刻の昇順（同時刻は Duck, Click, Latch, Tok, Sub, Bell, Ping, End の順）
inline constexpr Event kEvents[] = {
    {268.0045, 11819, EventKind::Click, 0, 0.1553, -15.924, ""},
    {358.8209, 15824, EventKind::Click, 1, 0.3, -15.184, ""},
    {448.7982, 19792, EventKind::Click, 2, 0.4243, -15.134, ""},
    {536.2585, 23649, EventKind::Click, 3, 0.5196, -14.231, ""},
    {619.7052, 27329, EventKind::Click, 4, 0.5796, -13.772, ""},
    {697.8231, 30774, EventKind::Click, 5, 0.6, -13.949, ""},
    {769.5692, 33938, EventKind::Click, 6, 0.5796, -13.438, ""},
    {834.263, 36791, EventKind::Click, 7, 0.5196, -13.68, ""},
    {891.5873, 39319, EventKind::Click, 8, 0.4243, -12.937, ""},
    {941.61, 41525, EventKind::Click, 9, 0.3, -13.389, ""},
    {984.7846, 43429, EventKind::Click, 10, 0.1553, -12.8, ""},
    {1021.9274, 45067, EventKind::Click, 11, 0.0, -12.956, ""},
    {1054.195, 46490, EventKind::Click, 12, -0.1553, -13.489, ""},
    {1082.9478, 47758, EventKind::Click, 13, -0.3, -13.489, ""},
    {1109.7959, 48942, EventKind::Click, 14, -0.4243, -13.402, ""},
    {1136.4626, 50118, EventKind::Click, 15, -0.5196, -13.354, ""},
    {1165.6236, 51404, EventKind::Click, 16, -0.5796, -13.229, ""},
    {1199.7732, 52910, EventKind::Click, 17, -0.6, -14.102, ""},
    {1240.771, 54718, EventKind::Click, 18, -0.5796, -14.16, ""},
    {1289.4785, 56866, EventKind::Click, 19, -0.5196, -14.152, ""},
    {1345.7143, 59346, EventKind::Click, 20, -0.4243, -14.107, ""},
    {1408.2086, 62102, EventKind::Click, 21, -0.3, -15.193, ""},
    {1474.8753, 65042, EventKind::Click, 22, -0.1553, -15.453, ""},
    {1542.9932, 68046, EventKind::Click, 23, -0.0, -15.084, ""},
    {1620.0, 71442, EventKind::Duck, 0, 0.0, -7.959, "air bed x0.4, tau 150ms"},
    {1628.0045, 71795, EventKind::Latch, 0, 0.0, -15.0, ""},
    {1628.0045, 71795, EventKind::Sub, 0, 0.0, -9.5, "sub 115>50Hz"},
    {1628.0045, 71795, EventKind::Sub, 1, 0.0, -17.0, "thump 190Hz"},
    {1630.0, 71883, EventKind::Tok, 0, 0.0, -17.0, ""},
    {1646.0091, 72589, EventKind::Latch, 1, 0.05, -19.0, ""},
    {1648.0045, 72677, EventKind::Bell, 0, -0.28, -14.0, "D4 293.66Hz"},
    {1672.9932, 73779, EventKind::Bell, 1, -0.12, -14.0, "A4 440.0Hz"},
    {1698.0045, 74882, EventKind::Bell, 2, 0.05, -14.0, "E5 659.26Hz"},
    {1722.9932, 75984, EventKind::Bell, 3, 0.18, -14.0, "F#5 739.99Hz"},
    {1748.0045, 77087, EventKind::Bell, 4, 0.32, -18.0, "A5 880.0Hz"},
    {2148.0045, 94727, EventKind::Ping, 0, 0.0, -22.0, "A6 1760Hz"},
    {3750.0, 165375, EventKind::End, 0, 0.0, 0.0, ""},
};
inline constexpr std::size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);

} // namespace dx12e::splash::calibrate
