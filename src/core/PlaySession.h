#pragma once

#include <deque>
#include <string>
#include <vector>

#include "core/Types.h"

// nlohmann::json は前方宣言だけを引く（DeepDiagnostics.h と同じ方針）。
#include <nlohmann/json_fwd.hpp>

namespace dx12e
{

class InputSystem;
class Camera;

// Play 1 回ぶんの記録。
//
// 目的は「AI が Play を押し、人間が遊び、AI が後からその 1 回を読む」こと。
// AI に操作させて挙動を確かめるのは（合成入力なので）人間の遊び方と違う結果になる。
// だから記録側だけを用意して、操作は人間に任せる。
//
// 記録するもの:
//  - 入力のエッジ（キー / パッドの押した・離した）… 何をしたか
//  - 10Hz のサンプル（fps・カメラ位置姿勢・マウス移動量）… どこで何が起きたか
//  - ログの抜粋（warn 以上 + Lua の print/log 行）… ゲーム側が何を言ったか
//
// Stop しても捨てない（遊び終えてから AI が取りに来るため）。次の Play で作り直す。
class PlaySession
{
public:
    // 記録上限。超えたら古いものから捨てる（長時間放置しても青天井にしない）。
    static constexpr size_t kMaxEvents  = 8000;
    static constexpr size_t kMaxSamples = 4000;   // 10Hz → 約 6.6 分
    static constexpr f32    kSampleHz   = 10.0f;

    void Start(f32 nowSec);
    void Stop(f32 nowSec);

    // Play 中に毎フレーム呼ぶ。エッジ検出・サンプリング・ログ取り込みを内部で間引く。
    // cam は null 可（アクティブカメラが無い間はカメラ列だけ欠ける）。
    void Update(f32 nowSec, const InputSystem& input, const Camera* cam, f32 fps);

    bool Started() const { return m_started; }
    bool Recording() const { return m_recording; }

    // maxEvents / maxSamples は返す件数の上限（新しい順に切り詰め、落とした件数を報告する）。
    nlohmann::json ToJson(size_t maxEvents, size_t maxSamples) const;

private:
    struct Event   { f32 t; std::string kind; std::string detail; };
    struct Sample  { f32 t; f32 fps; f32 pos[3]; f32 yaw; f32 pitch; f32 mouse; };

    void Push(f32 t, std::string kind, std::string detail);

    bool   m_started   = false;
    bool   m_recording = false;
    f32    m_startSec  = 0.0f;
    f32    m_endSec    = 0.0f;
    f32    m_nextSampleAt = 0.0f;
    f32    m_mouseAccum   = 0.0f;   // サンプル間のマウス移動量（|dx|+|dy| の積算）
    u64    m_frames       = 0;
    f32    m_fpsMin       = 0.0f;
    u64    m_logCursor    = 0;      // Logger::ReadBuffered のカーソル

    bool   m_prevKeys[256] = {};
    u32    m_prevPad       = 0;     // パッド 0 のボタンビット（前フレーム）

    size_t m_droppedEvents  = 0;
    size_t m_droppedSamples = 0;

    std::deque<Event>  m_events;
    std::deque<Sample> m_samples;
};

} // namespace dx12e
