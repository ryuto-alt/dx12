#pragma once

// ============================================================================
// 知覚の判定（純ロジック）。視線の遮蔽（物理レイ）だけは呼び出し側（AiSystem）が渡す。
// ============================================================================
//   視覚: 距離（水平）→ 視野角 → 遮蔽。近く（nearSense 以内）は視野角を問わない（背後の気配）
//         見えている間 awareness が 0→1 へ上がり（confirmTime 秒で 1）、1 になったら「見つけた」
//   聴覚: 音の半径 × 聴力 ×（遮蔽されていれば occlusion）の内側なら聞こえる。
//         聞こえた強さ（loudness）= 1 - 距離 / 届く半径
//   記憶: 最後に見た / 聞いた位置と時刻。memoryTime 秒、刺激が無ければ忘れる
// 向きの約束: Transform.rotation.y（度）の前方 = (sin(yaw), 0, cos(yaw))
// ============================================================================

#include <string>

#include "core/Types.h"

namespace dx12e
{
namespace ai
{

struct ViewCheck
{
    f32  dist = 0.0f;        // 水平距離
    f32  angleDeg = 0.0f;    // 前方からの角度（0..180）
    bool inRange = false;    // sightRange 以内
    bool inFov = false;      // 視野角の内側
    bool isNear = false;     // nearSense 以内（視野角を問わない）。★near は windows.h のマクロなので使えない
    bool Candidate() const { return isNear || (inRange && inFov); }   // 遮蔽を調べる価値がある
};

ViewCheck CheckView(const f32 eye[3], f32 yawDeg, const f32 target[3],
                    f32 range, f32 fovDeg, f32 nearRange);

// 音が届く半径
f32  HearingReach(f32 soundRadius, f32 hearingScale, bool occluded, f32 occlusionFactor);
// 距離 dist で聞こえるか。loudness（任意）に 0..1
bool Hears(f32 dist, f32 reach, f32* loudness);

// awareness を 1 ステップ進める。見えていれば confirmTime 秒で 0→1、見えなければ decay/秒で下がる
f32  StepAwareness(f32 awareness, bool visible, f32 dt, f32 confirmTime, f32 decayPerSec);

// 1 つの対象についての記憶
struct TargetMemory
{
    u32  entity = 0xffffffffu;
    bool visible = false;      // 今、視線が通っている
    bool seen = false;         // 見つけている（visible かつ awareness = 1）
    f32  awareness = 0.0f;
    f32  dist = 0.0f;          // 今の実距離
    bool known = false;        // 位置を覚えている
    f32  knownPos[3]{ 0, 0, 0 };
    f64  lastSeen = -1e9;      // 最後に「見つけていた」時刻
    f64  lastHeard = -1e9;
    f64  lastStimulus = -1e9;  // 見た / 聞いた の新しい方
    f32  lastSeenPos[3]{ 0, 0, 0 };
};

// 見つけている間 / 聞いた時に呼ぶ（位置と時刻を覚える）
void RememberSeen(TargetMemory& m, const f32 pos[3], f64 now);
void RememberHeard(TargetMemory& m, const f32 pos[3], f64 now);
// 刺激が memoryTime 秒無ければ位置を忘れる。忘れたら true
bool ForgetIfStale(TargetMemory& m, f64 now, f32 memoryTime);

// 聞いた音（デバッグ表示と黒板用）
struct HeardSound
{
    f32 pos[3]{ 0, 0, 0 };
    f32 radius = 0.0f;
    f32 loudness = 0.0f;
    f64 time = 0.0;
    u32 source = 0xffffffffu;
    bool occluded = false;
    std::string tag;
};

} // namespace ai
} // namespace dx12e
