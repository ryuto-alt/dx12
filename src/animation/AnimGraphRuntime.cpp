#include "animation/AnimGraphRuntime.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/BlendTree.h"
#include "animation/BoneMask.h"
#include "animation/Skeleton.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace anim_graph
{

namespace
{

// ブレンドツリー 1 個あたりのサンプル数の上限（毎フレームのヒープ確保を避けるため固定長）。
constexpr size_t kMaxBlendSamples = 16;

// 再生位置を [0, duration] に畳む。戻り値 = 折り返したか。
bool WrapStateTime(f32& time, f32 duration, bool loop)
{
    if (duration <= 0.0f) { time = 0.0f; return false; }
    if (!loop)
    {
        if (time >= duration) { time = duration; return false; }
        if (time < 0.0f) time = 0.0f;
        return false;
    }
    if (time < duration && time >= 0.0f) return false;
    time = std::fmod(time, duration);
    if (time < 0.0f) time += duration;
    return true;
}

const AnimationClip* ClipAt(const std::vector<std::unique_ptr<AnimationClip>>& clips, i32 index)
{
    if (index < 0 || index >= static_cast<i32>(clips.size())) return nullptr;
    return clips[static_cast<size_t>(index)].get();
}

// ブレンドツリーの入力パラメータ値。
f32 BlendTreeInput(const AnimBlendTree& bt, const AnimParamMap& params)
{
    auto it = params.find(bt.param);
    return (it == params.end()) ? 0.0f : it->second.f;
}

// ブレンドツリーの重みを計算して out（要素数 kMaxBlendSamples）へ書く。
// 返り値 = 実際に使ったサンプル数。ヒープを触らないよう固定長で持つ。
size_t EvalBlendTreeWeights(const AnimBlendTree& bt, const AnimParamMap& params,
                            f32 (&out)[kMaxBlendSamples])
{
    const size_t n = (std::min)(bt.samples.size(), kMaxBlendSamples);
    for (size_t i = 0; i < kMaxBlendSamples; ++i) out[i] = 0.0f;
    if (n == 0) return 0;

    if (bt.type == AnimBlendTreeType::TwoD || bt.type == AnimBlendTreeType::TwoDPolar)
    {
        // Gradient Band Interpolation（BlendTree.h。式は Johansen の論文で裏取り済み）。
        BlendPoint2D pts[kMaxBlendSamples];
        for (size_t i = 0; i < n; ++i) pts[i] = { bt.samples[i].value, bt.samples[i].valueY };

        auto ity = params.find(bt.paramY);
        const BlendPoint2D p{ BlendTreeInput(bt, params),
                              (ity == params.end()) ? 0.0f : ity->second.f };

        if (bt.type == AnimBlendTreeType::TwoDPolar)
            GradientBandPolar(pts, n, p, bt.polarAlpha, out);
        else
            GradientBandCartesian(pts, n, p, out);
        return n;
    }

    f32 axis[kMaxBlendSamples];
    for (size_t i = 0; i < n; ++i) axis[i] = bt.samples[i].value;

    Eval1DBlend(axis, n, BlendTreeInput(bt, params), out);
    return n;
}

// ステートの「合成 duration」。
// ブレンドツリーでは、重み付き平均した duration を「合成クリップの長さ」として扱う。
// 位相同期はこの合成 duration に対する正規化時間で行う。
f32 StateDuration(const AnimStateDef& st, const std::vector<std::unique_ptr<AnimationClip>>& clips,
                  const AnimParamMap& params)
{
    if (st.blendTree.type != AnimBlendTreeType::None)
    {
        f32 w[kMaxBlendSamples];
        const size_t n = EvalBlendTreeWeights(st.blendTree, params, w);
        f32 dur = 0.0f, wsum = 0.0f;
        for (size_t i = 0; i < n; ++i)
        {
            if (w[i] <= 0.0f) continue;
            const AnimationClip* c = ClipAt(clips, st.blendTree.samples[i]._clipIndex);
            if (!c || c->GetDuration() <= 0.0f) continue;
            dur  += w[i] * c->GetDuration();
            wsum += w[i];
        }
        return (wsum > 1e-5f) ? (dur / wsum) : 0.0f;
    }
    const AnimationClip* c = ClipAt(clips, st._clipIndex);
    return c ? c->GetDuration() : 0.0f;
}

// ステートの再生レート倍率。
// speedMatch: 実パラメータ値 ÷ 補間された基準速度。
//   サンプルの value を「そのクリップ本来の移動速度(m/s)」にしてあれば、
//   **サンプル範囲の内側では Σw·value == x になるので倍率は 1** になる
//   （ブレンド自体が既に速度を合わせているため）。効くのは範囲外へ出たときで、
//   「一番速い走りより速く動く」ときに脚の回転が追従する。
f32 StateRate(const AnimStateDef& st, const AnimParamMap& params,
              const f32 (&weights)[kMaxBlendSamples], size_t n)
{
    if (st.blendTree.type != AnimBlendTreeType::OneD || !st.blendTree.speedMatch) return 1.0f;

    f32 base = 0.0f;
    for (size_t i = 0; i < n; ++i)
        base += weights[i] * st.blendTree.samples[i].value;

    if (base <= 1e-4f) return 1.0f;
    const f32 x = BlendTreeInput(st.blendTree, params);
    if (x <= 0.0f) return 1.0f;
    return std::clamp(x / base, 0.1f, 4.0f);
}

// ステートのポーズを out へ書き、同時に「イベント収集に使う代表クリップ」を返す。
// ブレンドツリーの場合は最大重みのサンプルが代表になる。
//
// ★位相同期（syncPhase）★
//   全クリップを「合成 duration に対する正規化時間」で同じ位相にしてサンプルする。
//   これをやらないと walk と run の接地タイミングがずれ、足が 2 本に見える
//   （ブレンドツリーで最も出やすい見た目の破綻）。
// eventTimeScale には「ステートの時間軸 → 代表クリップの時間軸」の倍率を返す
// （イベント収集の (prev, cur] を代表クリップの時刻に直すため）。
const AnimationClip* SampleStatePose(const AnimStateDef& st, f32 time,
                                     const std::vector<std::unique_ptr<AnimationClip>>& clips,
                                     const Skeleton& skeleton,
                                     const AnimParamMap& params,
                                     f32 synthDuration,
                                     AnimPose& out, AnimPose& scratch,
                                     f32& eventTimeScale)
{
    eventTimeScale = 1.0f;

    if (st.blendTree.type != AnimBlendTreeType::None)
    {
        f32 w[kMaxBlendSamples];
        const size_t n = EvalBlendTreeWeights(st.blendTree, params, w);
        const f32 phase = (synthDuration > 0.0f) ? (time / synthDuration) : 0.0f;

        const AnimationClip* dominant = nullptr;
        f32 dominantW = 0.0f;
        f32 accum = 0.0f;
        bool first = true;

        for (size_t i = 0; i < n; ++i)
        {
            if (w[i] <= 1e-4f) continue;
            const AnimationClip* c = ClipAt(clips, st.blendTree.samples[i]._clipIndex);
            if (!c) continue;

            // ★位相同期★ 合成 duration に対する正規化時間で全クリップを揃える
            const f32 t = st.blendTree.syncPhase ? (phase * c->GetDuration()) : time;

            if (first)
            {
                SamplePose(*c, t, skeleton, out);
                accum = w[i];
                first = false;
            }
            else
            {
                SamplePose(*c, t, skeleton, scratch);
                accum += w[i];
                // 逐次ブレンド: acc = lerp(acc, p_i, w_i / Σ_{k<=i} w_k)
                BlendPoseAccum(out, scratch, w[i] / accum);
            }

            if (w[i] > dominantW) { dominantW = w[i]; dominant = c; }
        }

        if (first) { MakeBindPose(skeleton, out); return nullptr; }

        if (dominant && st.blendTree.syncPhase && synthDuration > 0.0f)
            eventTimeScale = dominant->GetDuration() / synthDuration;
        return dominant;
    }

    const AnimationClip* c = ClipAt(clips, st._clipIndex);
    if (!c)
    {
        MakeBindPose(skeleton, out);
        return nullptr;
    }
    SamplePose(*c, time, skeleton, out);
    return c;
}

// (prev, cur] のイベントを積む。
void GatherEvents(const AnimationClip* clip, f32 prevTime, f32 curTime, bool wrapped,
                  i32 layerIndex, std::vector<u32>& scratch, std::vector<AnimFiredEvent>& out)
{
    if (!clip || clip->GetEvents().empty()) return;
    scratch.clear();
    CollectAnimEvents(clip->GetEvents(), prevTime, curTime, wrapped, scratch);
    for (u32 i : scratch)
    {
        const AnimEvent& ev = clip->GetEvents()[i];
        AnimFiredEvent fe;
        fe.name        = ev.name;
        fe.stringParam = ev.stringParam;
        fe.floatParam  = ev.floatParam;
        fe.clip        = clip->GetName();
        fe.time        = ev.time;
        fe.layer       = layerIndex;
        out.push_back(std::move(fe));
    }
}

// ステートへ入る（時刻を 0 に戻す）。
void EnterState(AnimLayerRuntime& lr, i32 state)
{
    lr.curState      = state;
    lr.stateTime     = 0.0f;
    lr.prevStateTime = 0.0f;
    lr.wrapped       = false;
}

} // namespace

// ---------------------------------------------------------------------------
i32 FindClipIndex(const std::vector<std::unique_ptr<AnimationClip>>& clips, const std::string& name)
{
    if (name.empty()) return -1;
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i] && clips[i]->GetName() == name) return static_cast<i32>(i);
    return -1;
}

void ResolveAsset(AnimGraphAsset& asset,
                  const std::vector<std::unique_ptr<AnimationClip>>& clips,
                  std::vector<std::string>& missing)
{
    for (AnimLayerDef& layer : asset.layers)
    {
        for (AnimStateDef& st : layer.states)
        {
            if (st.blendTree.type != AnimBlendTreeType::None)
            {
                for (AnimBlendSample1D& s : st.blendTree.samples)
                {
                    s._clipIndex = FindClipIndex(clips, s.clip);
                    if (s._clipIndex < 0) missing.push_back(s.clip);
                }
                // ブレンドツリーの代表クリップ（Step 4 まではこれで再生する）
                if (!st.blendTree.samples.empty())
                    st._clipIndex = st.blendTree.samples.front()._clipIndex;
            }
            else
            {
                st._clipIndex = FindClipIndex(clips, st.clip);
                if (st._clipIndex < 0 && !st.clip.empty()) missing.push_back(st.clip);
            }
        }

        layer._referenceClipIndex = FindClipIndex(clips, layer.referenceClip);
        if (layer._referenceClipIndex < 0 && !layer.referenceClip.empty())
            missing.push_back(layer.referenceClip);

        for (AnimTransitionDef& tr : layer.transitions)
        {
            tr._fromAny = (tr.from == "*");
            tr._from = tr._fromAny ? -1 : FindAnimState(layer, tr.from);
            tr._to   = FindAnimState(layer, tr.to);
            if (tr._to < 0) missing.push_back("state:" + tr.to);
            if (!tr._fromAny && tr._from < 0) missing.push_back("state:" + tr.from);
        }
    }
}

void ApplyClipEvents(const AnimGraphAsset& asset,
                     std::vector<std::unique_ptr<AnimationClip>>& clips)
{
    for (const AnimGraphClipEvents& ce : asset.clipEvents)
    {
        const i32 idx = FindClipIndex(clips, ce.clip);
        if (idx < 0) continue;
        AnimationClip& clip = *clips[static_cast<size_t>(idx)];
        clip.ClearEvents();
        for (const AnimEvent& ev : ce.events) clip.AddEvent(ev);
    }
}

void InitRuntime(AnimGraphRuntimeState& rt, const Skeleton& skeleton)
{
    InitAnimParams(rt.asset, rt.params);
    rt.layers.assign(rt.asset.layers.size(), AnimLayerRuntime{});
    rt.missingMaskBones.clear();
    for (size_t i = 0; i < rt.asset.layers.size(); ++i)
    {
        const AnimLayerDef& def = rt.asset.layers[i];
        rt.layers[i].weight = def.weight;
        EnterState(rt.layers[i], FindAnimState(def, def.defaultState));

        // ボーンマスクをここで 1 回だけ解決する（毎フレーム名前を引かない）。
        // レイヤー 0 のマスクは意味が薄い（下に敷くものが無い）が、
        // 「一部のボーンだけアニメさせる」用途で使えるので禁止はしない。
        if (def.hasMask)
        {
            BuildBoneMaskFromNames(skeleton, def.mask.include, def.mask.includeChildren,
                                   def.mask.weight, rt.layers[i].maskWeights,
                                   &rt.missingMaskBones);
        }
    }
    MakeBindPose(skeleton, rt.poseA);
    rt.poseB     = rt.poseA;
    rt.layerPose = rt.poseA;
    rt.refPose   = rt.poseA;
    rt.valid = true;
}

f32 NormalizedTime(const AnimGraphRuntimeState& rt, u32 layer,
                   const std::vector<std::unique_ptr<AnimationClip>>& clips)
{
    if (layer >= rt.layers.size()) return 0.0f;
    const AnimLayerRuntime& lr = rt.layers[layer];
    const AnimLayerDef& def = rt.asset.layers[layer];
    if (lr.curState < 0 || lr.curState >= static_cast<i32>(def.states.size())) return 0.0f;
    const f32 dur = StateDuration(def.states[static_cast<size_t>(lr.curState)], clips, rt.params);
    return (dur > 0.0f) ? std::clamp(lr.stateTime / dur, 0.0f, 1.0f) : 0.0f;
}

bool PlayState(AnimGraphRuntimeState& rt, u32 layer, const std::string& stateName, f32 blendDuration)
{
    if (layer >= rt.layers.size()) return false;
    const AnimLayerDef& def = rt.asset.layers[layer];
    const i32 target = FindAnimState(def, stateName);
    if (target < 0) return false;

    AnimLayerRuntime& lr = rt.layers[layer];
    if (blendDuration <= 1e-6f || lr.curState < 0)
    {
        lr.inTransition = false;
        lr.transTo = -1;
        EnterState(lr, target);
        return true;
    }
    lr.inTransition       = true;
    lr.transTo            = target;
    lr.transElapsed       = 0.0f;
    lr.transDuration      = blendDuration;
    lr.transTime          = 0.0f;
    lr.transPrevTime      = 0.0f;
    lr.transWrapped       = false;
    lr.transInterruptible = true;
    return true;
}

// ---------------------------------------------------------------------------
void Update(AnimGraphRuntimeState& rt,
            const std::vector<std::unique_ptr<AnimationClip>>& clips,
            const Skeleton& skeleton,
            Animator& animator,
            f32 dt,
            std::vector<AnimFiredEvent>& outEvents)
{
    if (!rt.valid || rt.asset.layers.empty()) return;
    if (rt.layers.size() != rt.asset.layers.size()) return;

    AnimPose& finalPose = rt.poseA;   // 合成結果
    bool haveBase = false;

    // このフレームに発火した遷移。トリガの消費は全レイヤーを評価し終えてから行う（[4]）。
    std::vector<const AnimTransitionDef*> firedTransitions;

    for (size_t li = 0; li < rt.asset.layers.size(); ++li)
    {
        const AnimLayerDef& def = rt.asset.layers[li];
        AnimLayerRuntime&   lr  = rt.layers[li];

        if (def.states.empty()) continue;
        if (lr.curState < 0) EnterState(lr, FindAnimState(def, def.defaultState));
        if (lr.curState < 0) continue;

        // ---- 遷移の判定 ----------------------------------------------------
        {
            AnimTransitionQuery q;
            q.curState             = lr.curState;
            q.normalizedTime       = NormalizedTime(rt, static_cast<u32>(li), clips);
            q.inTransition         = lr.inTransition;
            q.currentInterruptible = lr.transInterruptible;
            q.curTransitionTo      = lr.transTo;
            q.loopedThisFrame      = lr.wrapped;
            const i32 pick = PickTransition(def, q, rt.params);
            if (pick >= 0)
            {
                const AnimTransitionDef& tr = def.transitions[static_cast<size_t>(pick)];
                // ★トリガの消費はレイヤーループの**後**でまとめて行う。
                //   ここで消すと、rt.params が全レイヤー共有なので先に評価された
                //   レイヤーがトリガを食ってしまい、同じトリガで動くはずの上半身レイヤーが
                //   一生反応しない（「マスクが壊れている」と誤診しやすい）。
                firedTransitions.push_back(&tr);
                if (tr.duration <= 1e-6f)
                {
                    // 即時遷移（0 除算・NaN を作らない）
                    lr.inTransition = false;
                    lr.transTo = -1;
                    EnterState(lr, tr._to);
                }
                else
                {
                    // ★割り込み（既に遷移中）なら、表示中の合成ポーズを遷移元として使う。
                    //   そうしないと完全な遷移元へ 1 フレーム巻き戻ってから混ぜ直しになる。
                    lr.useSnapshotAsFrom = lr.inTransition && !lr.blendedSnapshot.empty();

                    lr.inTransition       = true;
                    lr.transTo            = tr._to;
                    lr.transElapsed       = 0.0f;
                    lr.transEventsStarted = false;   // 遷移先の頭からイベントを拾い直す
                    lr.transDuration      = tr.duration;
                    lr.transTime          = 0.0f;
                    lr.transPrevTime      = 0.0f;
                    lr.transWrapped       = false;
                    lr.transInterruptible = tr.interruptible;
                }
            }
        }

        const AnimStateDef& cur = def.states[static_cast<size_t>(lr.curState)];

        // ---- 現ステートの時間を進める --------------------------------------
        // ブレンドツリーでは「重み付き平均した duration」を合成クリップの長さとして扱い、
        // speedMatch が有効なら実パラメータ値に合わせて再生レートを補正する。
        f32 curW[kMaxBlendSamples];
        const size_t curN = EvalBlendTreeWeights(cur.blendTree, rt.params, curW);
        const f32 curDur  = StateDuration(cur, clips, rt.params);
        const f32 curRate = StateRate(cur, rt.params, curW, curN);

        lr.prevStateTime = lr.stateTime;
        lr.stateTime += dt * cur.speed * curRate;
        lr.wrapped = WrapStateTime(lr.stateTime, curDur, cur.loop);

        // ---- 遷移先の時間 + ブレンド率 --------------------------------------
        f32 blend = 0.0f;
        const AnimStateDef* next = nullptr;
        f32 nextDur = 0.0f;
        if (lr.inTransition && lr.transTo >= 0 && lr.transTo < static_cast<i32>(def.states.size()))
        {
            next = &def.states[static_cast<size_t>(lr.transTo)];
            f32 nW[kMaxBlendSamples];
            const size_t nN = EvalBlendTreeWeights(next->blendTree, rt.params, nW);
            nextDur = StateDuration(*next, clips, rt.params);
            const f32 nextRate = StateRate(*next, rt.params, nW, nN);

            lr.transPrevTime = lr.transTime;
            lr.transTime += dt * next->speed * nextRate;
            // 遷移中の先クリップは常にループで畳む（現クリップの loop フラグを流用しない）
            lr.transWrapped = WrapStateTime(lr.transTime, nextDur, true);

            lr.transElapsed += dt;
            blend = std::clamp(lr.transElapsed / lr.transDuration, 0.0f, 1.0f);

            if (lr.transElapsed >= lr.transDuration)
            {
                // 遷移完了: 先ステートを現ステートにして時刻を引き継ぐ
                lr.curState      = lr.transTo;
                lr.stateTime     = lr.transTime;
                lr.prevStateTime = lr.transPrevTime;
                lr.wrapped       = lr.transWrapped;
                lr.inTransition  = false;
                lr.transTo       = -1;
                next  = nullptr;
                blend = 0.0f;
            }
        }

        // ---- ポーズ生成 ------------------------------------------------------
        const AnimStateDef& active = def.states[static_cast<size_t>(lr.curState)];
        const f32 activeDur = next ? curDur : StateDuration(active, clips, rt.params);
        f32 evScale = 1.0f;
        const AnimationClip* eventClip = nullptr;
        f32 eventPrev = 0.0f, eventCur = 0.0f;
        bool eventWrapped = false;

        if (lr.useSnapshotAsFrom && lr.blendedSnapshot.size() == skeleton.GetBoneCount())
        {
            // 割り込み前の合成ポーズをそのまま遷移元にする（凍結）。
            // ★イベントはこちら側から拾わない。ポーズが進んでいないのに足音が鳴るのは嘘なので。
            rt.layerPose = lr.blendedSnapshot;
        }
        else
        {
            lr.useSnapshotAsFrom = false;
            eventClip = SampleStatePose(active, lr.stateTime, clips, skeleton, rt.params, activeDur,
                                        rt.layerPose, rt.poseB, evScale);
            eventPrev    = lr.prevStateTime * evScale;
            eventCur     = lr.stateTime * evScale;
            eventWrapped = lr.wrapped;
        }

        if (next && blend > 0.0f)
        {
            f32 nextEvScale = 1.0f;
            const AnimationClip* nextClip =
                SampleStatePose(*next, lr.transTime, clips, skeleton, rt.params, nextDur,
                                rt.poseB, rt.refPose, nextEvScale);
            BlendPoseInPlace(rt.layerPose, rt.poseB, blend);

            // イベントは「重みが優勢な側」からだけ拾う。
            // 両方から拾うとクロスフェード中だけ足音が二重に鳴る。
            if (blend >= 0.5f)
            {
                eventClip = nextClip;
                // 優勢側が切り替わった最初のフレームだけ、クリップの頭から拾い直す。
                // そうしないと [0, transPrevTime] が永久に未走査のまま捨てられる。
                eventPrev    = lr.transEventsStarted ? (lr.transPrevTime * nextEvScale) : 0.0f;
                eventCur     = lr.transTime * nextEvScale;
                eventWrapped = lr.transEventsStarted ? lr.transWrapped : false;
                lr.transEventsStarted = true;
            }
        }

        GatherEvents(eventClip, eventPrev, eventCur, eventWrapped,
                     static_cast<i32>(li), rt.eventHits, outEvents);

        // 遷移中は合成結果を控えておく（次フレームで割り込まれたときの遷移元になる）。
        // 遷移していないときは凍結を解いて捨てる（メモリを抱え続けない）。
        if (lr.inTransition)
        {
            lr.blendedSnapshot = rt.layerPose;
        }
        else
        {
            lr.useSnapshotAsFrom = false;
            lr.blendedSnapshot.clear();
        }

        // ---- レイヤー合成 ----------------------------------------------------
        const f32 layerWeight = std::clamp(lr.weight, 0.0f, 1.0f);
        const f32* mask = lr.maskWeights.empty() ? nullptr : lr.maskWeights.data();

        if (!haveBase)
        {
            // 最下層は素通し（重み 1 未満ならバインドポーズと混ぜる）
            if (layerWeight >= 1.0f && !mask)
            {
                finalPose = rt.layerPose;
            }
            else
            {
                MakeBindPose(skeleton, finalPose);
                OverrideBlendPose(finalPose, rt.layerPose, layerWeight, mask);
            }
            haveBase = true;
        }
        else if (def.blend == AnimLayerBlend::Additive)
        {
            const AnimationClip* refClip = ClipAt(clips, def._referenceClipIndex);
            if (refClip) SamplePose(*refClip, 0.0f, skeleton, rt.refPose);
            else         MakeBindPose(skeleton, rt.refPose);
            AdditiveBlendPose(finalPose, rt.layerPose, rt.refPose, layerWeight, mask);
        }
        else
        {
            OverrideBlendPose(finalPose, rt.layerPose, layerWeight, mask);
        }
    }

    // ★全レイヤーを評価し終えてからトリガを消す（[4]）。ループ内で消すと、
    //   rt.params が全レイヤー共有なので先に評価されたレイヤーがトリガを食い、
    //   同じトリガで動くはずの上半身レイヤーが一生反応しなかった。
    for (const AnimTransitionDef* tr : firedTransitions)
        ConsumeTriggers(*tr, rt.params);

    if (haveBase) animator.SetPoseOverride(finalPose);
}

} // namespace anim_graph
} // namespace dx12e
