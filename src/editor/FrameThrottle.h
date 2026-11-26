#pragma once
// ===== 裏に回ったときのフレーム間引き（W1-A11 / MOD-ED-11 / AUD-14）の純ロジック =====
// Application のメインループが毎フレーム Evaluate() を呼び、返った上限 fps でフレームを歩調取りする。
// 窓・ImGui・時計に触らない（状態遷移を tests/frame_throttle_test.cpp で表にして検査する）。
//   tier 0 = 制限なし
//   tier 1 = 窓がアクティブだが idleSec 秒（既定 5）入力・再生・MCP の要求・仮想入力・ジョブが無い → 30fps
//   tier 2 = 窓が非アクティブ（--background / --headless は常にこちら）で idleSec 秒活動が無い → 10fps
// 「活動」= busy（再生・MCP の要求と応答待ち・仮想入力の待ち・ジョブ）または、アクティブな窓での実入力。
// 活動があると lastActivitySec を更新し、その後 idleSec 秒は tier 0 に戻る（MCP の要求の直後の数フレームを
// 遅らせない＝step_frames と撮影の所要時間が変わらない）。「常にリアルタイム」の設定、ゲーム、UI テストでは無効。
#include <algorithm>

namespace dx12e::throttle
{

struct Config
{
    double idleSec = 5.0;    // 入力・活動が無いとみなすまでの秒数
    float  idleFps = 30.0f;  // tier 1
    float  bgFps   = 10.0f;  // tier 2
};

struct Inputs
{
    double nowSec         = 0.0;    // 単調時計の秒
    bool   allowed        = true;   // false = 間引かない（ゲーム・UI テスト・エディタ無し）
    bool   alwaysRealtime = false;  // 「常にリアルタイム」の設定
    bool   windowActive   = true;   // 窓（または同じプロセスの窓）が前面にあり最小化されていない
    bool   busy           = false;  // 再生・MCP の要求の処理中・仮想入力の待ち・ジョブ
    bool   input          = false;  // このフレームに実入力（マウス・キー・文字）があった
};

// S は EditorContext::FrameThrottleState（tier / lastActivitySec / windowActive / alwaysRealtime を持つ型）。
template <class S>
inline int Evaluate(S& st, const Inputs& in, const Config& cfg = {})
{
    st.windowActive   = in.windowActive;
    st.alwaysRealtime = in.alwaysRealtime;
    // 起動直後（初回）は活動として数える＝読み込みと初期描画を遅らせない
    if (st.lastActivitySec <= 0.0) st.lastActivitySec = in.nowSec > 0.0 ? in.nowSec : 1e-9;
    const bool active = in.busy || (in.windowActive && in.input);
    if (active) st.lastActivitySec = (std::max)(st.lastActivitySec, in.nowSec);

    int tier = 0;
    if (in.allowed && !in.alwaysRealtime && (in.nowSec - st.lastActivitySec) >= cfg.idleSec)
        tier = in.windowActive ? 1 : 2;
    st.tier = tier;
    return tier;
}

// tier の上限 fps（0 = 制限なし）。
inline float TierFps(int tier, const Config& cfg = {})
{
    return tier == 1 ? cfg.idleFps : tier == 2 ? cfg.bgFps : 0.0f;
}

// 既存の fps 上限（0 = 無制限）と tier の上限を合わせた、実際の歩調の上限（0 = 制限なし）。
inline float CombineFps(float baseLimit, float tierFps)
{
    if (tierFps <= 0.0f) return baseLimit;
    if (baseLimit <= 0.0f) return tierFps;
    return (std::min)(baseLimit, tierFps);
}

} // namespace dx12e::throttle
