#pragma once
// ---------------------------------------------------------------------------
// AnimGraphRuntime — `.animfsm` を 1 体ぶん回す実行器。
//
// ⚠️ ここに entt / GPU / Jolt は入れない。
//    ECS ライブラリが Animation に依存しているので、逆向きの依存は循環になる。
//    エンティティの走査は Scene 側（Scene::Update）が行い、ここへは
//    「そのエンティティのクリップ配列・スケルトン・Animator」を渡すだけにする。
//
// 1 フレームの流れ:
//    パラメータ評価 → PickTransition → ステート時間の進行 → イベント収集
//    → 各レイヤーのポーズ生成 → レイヤー合成 → Animator::SetPoseOverride
// ---------------------------------------------------------------------------
#include <memory>
#include <string>
#include <vector>

#include "core/Types.h"
#include "animation/AnimGraphAsset.h"
#include "animation/AnimPose.h"

namespace dx12e
{

class Skeleton;
class AnimationClip;
class Animator;

// レイヤー 1 枚ぶんの実行状態。
struct AnimLayerRuntime
{
    i32  curState      = -1;
    f32  stateTime     = 0.0f;   // 現ステートの再生位置（秒）
    f32  prevStateTime = 0.0f;
    bool wrapped       = false;  // 今フレームでループを折り返したか

    bool inTransition       = false;
    i32  transTo            = -1;
    f32  transElapsed       = 0.0f;
    f32  transDuration      = 0.0f;
    f32  transTime          = 0.0f;   // 遷移先ステートの再生位置（秒）
    f32  transPrevTime      = 0.0f;
    bool transWrapped       = false;
    // ★遷移先クリップのイベントを拾い始めたか。イベントは「重みが優勢な側」からだけ
    //   拾う設計なので、blend が 0.5 を超えるまでの区間 [0, transPrevTime] が
    //   一度も走査されず、**遷移先クリップの前半のイベントが全部落ちていた**
    //   （Attack の t=0.05 に置いた hitbox_on が duration 0.2 の遷移では常に鳴らない）。
    //   優勢側が切り替わった最初のフレームだけ、頭から拾い直すために使う。
    bool transEventsStarted = false;
    // ★遷移中の合成ポーズのスナップショット（遷移割り込み用）。
    //   割り込みが成立すると transElapsed だけ 0 に戻り curState は遷移元のまま据え置かれるので、
    //   ポーズは「完全な遷移元」から作り直されていた。表示中の合成ポーズ（例 blend=0.7）が
    //   捨てられるため、**割り込んだ瞬間にポーズが 1 フレームだけ遷移元へ巻き戻る**
    //   （移動しながら攻撃、走行中に被弾で毎回カクつく）。
    //   遷移中は毎フレームここへ合成結果を控え、割り込み時はこれを遷移元ポーズとして使う。
    AnimPose blendedSnapshot;
    bool     useSnapshotAsFrom = false;
    bool transInterruptible = true;

    f32  weight = 1.0f;              // 実行時に動かせるレイヤー重み（Lua/MCP から）
    std::vector<f32> maskWeights;    // ボーンごとのマスク（空 = マスク無し。Step 5）
};

// 今フレームに発火したイベント（Scene 側が EventBus へ流す）。
struct AnimFiredEvent
{
    std::string name;
    std::string stringParam;
    f32         floatParam = 0.0f;
    std::string clip;
    f32         time = 0.0f;
    i32         layer = 0;
};

// AnimatorController が unique_ptr で持つ実行状態。
struct AnimGraphRuntimeState
{
    AnimGraphAsset                asset;    // ロード済み（クリップ index 解決済み）
    std::vector<AnimLayerRuntime> layers;
    AnimParamMap                  params;
    bool                          valid = false;

    // マスクで指定されたが見つからなかったボーン名（ロード時に 1 回だけ埋まる。警告用）
    std::vector<std::string> missingMaskBones;

    // 毎フレームの再確保を避けるための一時バッファ
    AnimPose poseA, poseB, layerPose, refPose;
    std::vector<u32> eventHits;
};

// ---------------------------------------------------------------------------
namespace anim_graph
{

// クリップ名 → 添字。見つからなければ -1。
i32 FindClipIndex(const std::vector<std::unique_ptr<AnimationClip>>& clips, const std::string& name);

// asset 内のクリップ名参照（ステート / ブレンドツリー / 加算の参照ポーズ）と
// 遷移の from/to をすべて解決する。解決できなかったものは名前を missing へ積む。
void ResolveAsset(AnimGraphAsset& asset,
                  const std::vector<std::unique_ptr<AnimationClip>>& clips,
                  std::vector<std::string>& missing);

// asset の clipEvents を該当クリップへ流し込む（既存のイベントは置き換える）。
void ApplyClipEvents(const AnimGraphAsset& asset,
                     std::vector<std::unique_ptr<AnimationClip>>& clips);

// 実行状態を asset から初期化する（既定ステートへ入る）。
void InitRuntime(AnimGraphRuntimeState& rt, const Skeleton& skeleton);

// 1 体ぶんを dt 進める。ポーズは animator へ SetPoseOverride で注入する。
// outEvents へは今フレーム発火したイベントを push_back する（呼び出し側が clear）。
void Update(AnimGraphRuntimeState& rt,
            const std::vector<std::unique_ptr<AnimationClip>>& clips,
            const Skeleton& skeleton,
            Animator& animator,
            f32 dt,
            std::vector<AnimFiredEvent>& outEvents);

// 現ステートの正規化時間（0..1）。ステート未設定なら 0。
f32 NormalizedTime(const AnimGraphRuntimeState& rt, u32 layer,
                   const std::vector<std::unique_ptr<AnimationClip>>& clips);

// ステート名で強制遷移（デバッグ / カットシーン用）。成功したら true。
bool PlayState(AnimGraphRuntimeState& rt, u32 layer, const std::string& stateName, f32 blendDuration);

} // namespace anim_graph
} // namespace dx12e
