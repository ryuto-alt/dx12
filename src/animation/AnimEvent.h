#pragma once
// ---------------------------------------------------------------------------
// AnimEvent — スケルタルアニメのクリップに埋めるイベントマーカー（足音・SE 等）。
//
// glTF / FBX にはイベントを埋める手段が無いので、実体は `.animfsm` の
// "clipEvents" に書き、ロード時に該当クリップへ流し込む。
// 発火先は既存の EventBus（UI アニメの UiAnimEvent と同じ流儀）。
//
// 収集の境界は「開いた左・閉じた右」= (prevTime, curTime] で統一する。
// ループでラップしたフレームは (prev, end] + [0, cur] の 2 区間を見る。
// これで同じイベントが 1 周につきちょうど 1 回だけ出る。
// （実装は animation/UiAnimAsset.h の CollectUiAnimEvents をスケルタル用に移植したもの）
//
// 依存は STL だけ。tests/ から直接ビルドして検証できる。
// ---------------------------------------------------------------------------
#include <string>
#include <vector>
#include "core/Types.h"

namespace dx12e
{

struct AnimEvent
{
    f32         time = 0.0f;   // クリップ内の時刻（秒。NormalizeToSeconds 済み前提）
    std::string name;          // EventBus のイベント名（例 "footstep"）
    std::string stringParam;   // 任意の文字列パラメータ（例 "left"）
    f32         floatParam = 0.0f;
};

// (prevTime, curTime] に入るイベントの添字を out へ積む。
//   wrapped（curTime < prevTime）= ループで折り返したフレーム
//     → (prev, duration] と [0, cur] の 2 区間を見る
//   prevTime == curTime（時間が進んでいない）→ 0 件
//   逆再生（cur < prev だがラップではない）は呼び出し側が wrapped=false で渡すこと
//
// duration はラップ判定にのみ使う（events は [0, duration] に入っている前提）。
// out は push_back で積む（呼び出し側が clear する）。時刻順にはならないので、
// 順序が要るなら SortAnimEvents 済みの列を渡すこと（添字順＝時刻順になる）。
inline void CollectAnimEvents(const std::vector<AnimEvent>& events,
                              f32 prevTime, f32 curTime, bool wrapped,
                              std::vector<u32>& out)
{
    if (events.empty()) return;
    for (u32 i = 0; i < static_cast<u32>(events.size()); ++i)
    {
        const f32 t = events[i].time;
        const bool hit = wrapped ? (t > prevTime || t <= curTime)
                                 : (t > prevTime && t <= curTime);
        if (hit) out.push_back(i);
    }
}

// ラップの有無を時刻の大小から推定する簡便版（前進再生を仮定）。
inline void CollectAnimEvents(const std::vector<AnimEvent>& events,
                              f32 prevTime, f32 curTime, std::vector<u32>& out)
{
    CollectAnimEvents(events, prevTime, curTime, curTime < prevTime, out);
}

} // namespace dx12e
