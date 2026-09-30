#pragma once
// ===========================================================================
// シーンの風（SceneWind）。植生 F1 が読む「シーン全体の風」の設定。シーン JSON の "wind" に保存する（既定と同じなら書かない）。
// ---------------------------------------------------------------------------
// 風は FoliageLayer の頂点アニメ（幹の曲げ / 葉のはばたき）だけに効く。FoliageLayer を使わないシーンでは何も読まれない＝完全に無影響。
// 時間は「ゲームクロックの総時間 + phaseOffset」。決定論キャプチャ（screenshot_final）では総時間が固定値になるので、
// 風を動かした絵を撮りたいときは phaseOffset を変えて撮る（同じ設定なら起動ごとにビット一致する）。
// ===========================================================================
#include <cmath>

#include "core/Types.h"
#include "renderer/foliage/FoliageMath.h"

namespace dx12e::foliage
{

struct SceneWind
{
    bool enabled = true;           // false = 風なし（speed 0 と同じ）
    f32  directionDeg = 45.0f;     // 風が吹いていく向き。XZ 平面で 0° = +X、90° = +Z
    f32  speed = 3.0f;             // 基準の風速 m/s（0 で無風）
    f32  gustStrength = 0.5f;      // 突風の強さ 0..1（場所と時間で風速が ±この割合ゆらぐ）
    f32  gustFrequency = 0.35f;    // 突風が流れる速さ（大きいほど短い周期）
    f32  turbulence = 0.25f;       // 乱れ（高周波の揺らぎ）0..1
    f32  phaseOffset = 0.0f;       // 時間のオフセット（秒）。決定論キャプチャで風の位相を選ぶのに使う

    bool operator==(const SceneWind& o) const
    {
        return enabled == o.enabled && directionDeg == o.directionDeg && speed == o.speed
            && gustStrength == o.gustStrength && gustFrequency == o.gustFrequency
            && turbulence == o.turbulence && phaseOffset == o.phaseOffset;
    }
    bool operator!=(const SceneWind& o) const { return !(*this == o); }
};

// SceneWind → GPU へ渡すフレーム共通の風（FoliageMath.h の WindFrame）。time / prevTime は総時間（phaseOffset は中で足す）。
inline WindFrame MakeWindFrame(const SceneWind& w, f32 time, f32 prevTime)
{
    WindFrame f;
    const f32 rad = w.directionDeg * (kPi / 180.0f);
    f.dirX = std::cos(rad);
    f.dirZ = std::sin(rad);
    f.speed = w.enabled ? std::max(w.speed, 0.0f) : 0.0f;
    f.gustStrength = std::clamp(w.gustStrength, 0.0f, 1.0f);
    f.gustFreq = w.gustFrequency;
    f.turbulence = std::clamp(w.turbulence, 0.0f, 1.0f);
    f.time = time + w.phaseOffset;
    f.prevTime = prevTime + w.phaseOffset;
    return f;
}

} // namespace dx12e::foliage
