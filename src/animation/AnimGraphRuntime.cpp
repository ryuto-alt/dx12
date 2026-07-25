#include "animation/AnimGraphRuntime.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/Skeleton.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
namespace anim_graph
{

namespace
{

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

// ステートの「合成 duration」。Step 3 は単一クリップのみ（ブレンドツリーは Step 4）。
f32 StateDuration(const AnimStateDef& st, const std::vector<std::unique_ptr<AnimationClip>>& clips)
{
    const AnimationClip* c = ClipAt(clips, st._clipIndex);
    return c ? c->GetDuration() : 0.0f;
}

// ステートのポーズを out へ書き、同時に「イベント収集に使う代表クリップ」を返す。
const AnimationClip* SampleStatePose(const AnimStateDef& st, f32 time,
                                     const std::vector<std::unique_ptr<AnimationClip>>& clips,
                                     const Skeleton& skeleton,
                                     const AnimParamMap& /*params*/,
                                     AnimPose& out, AnimPose& /*scratch*/)
{
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
    for (size_t i = 0; i < rt.asset.layers.size(); ++i)
    {
        const AnimLayerDef& def = rt.asset.layers[i];
        rt.layers[i].weight = def.weight;
        EnterState(rt.layers[i], FindAnimState(def, def.defaultState));
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
    const f32 dur = StateDuration(def.states[static_cast<size_t>(lr.curState)], clips);
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

    for (size_t li = 0; li < rt.asset.layers.size(); ++li)
    {
        const AnimLayerDef& def = rt.asset.layers[li];
        AnimLayerRuntime&   lr  = rt.layers[li];

        if (def.states.empty()) continue;
        if (lr.curState < 0) EnterState(lr, FindAnimState(def, def.defaultState));
        if (lr.curState < 0) continue;

        // ---- 遷移の判定 ----------------------------------------------------
        {
            const f32 nt = NormalizedTime(rt, static_cast<u32>(li), clips);
            const i32 pick = PickTransition(def, lr.curState, nt,
                                            lr.inTransition, lr.transInterruptible, rt.params);
            if (pick >= 0)
            {
                const AnimTransitionDef& tr = def.transitions[static_cast<size_t>(pick)];
                ConsumeTriggers(tr, rt.params);
                if (tr.duration <= 1e-6f)
                {
                    // 即時遷移（0 除算・NaN を作らない）
                    lr.inTransition = false;
                    lr.transTo = -1;
                    EnterState(lr, tr._to);
                }
                else
                {
                    lr.inTransition       = true;
                    lr.transTo            = tr._to;
                    lr.transElapsed       = 0.0f;
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
        lr.prevStateTime = lr.stateTime;
        lr.stateTime += dt * cur.speed;
        lr.wrapped = WrapStateTime(lr.stateTime, StateDuration(cur, clips), cur.loop);

        // ---- 遷移先の時間 + ブレンド率 --------------------------------------
        f32 blend = 0.0f;
        const AnimStateDef* next = nullptr;
        if (lr.inTransition && lr.transTo >= 0 && lr.transTo < static_cast<i32>(def.states.size()))
        {
            next = &def.states[static_cast<size_t>(lr.transTo)];
            lr.transPrevTime = lr.transTime;
            lr.transTime += dt * next->speed;
            // 遷移中の先クリップは常にループで畳む（現クリップの loop フラグを流用しない）
            lr.transWrapped = WrapStateTime(lr.transTime, StateDuration(*next, clips), true);

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
        const AnimationClip* eventClip =
            SampleStatePose(active, lr.stateTime, clips, skeleton, rt.params, rt.layerPose, rt.poseB);
        f32 eventPrev = lr.prevStateTime, eventCur = lr.stateTime;
        bool eventWrapped = lr.wrapped;

        if (next && blend > 0.0f)
        {
            SampleStatePose(*next, lr.transTime, clips, skeleton, rt.params, rt.poseB, rt.refPose);
            BlendPoseInPlace(rt.layerPose, rt.poseB, blend);

            // イベントは「重みが優勢な側」からだけ拾う。
            // 両方から拾うとクロスフェード中だけ足音が二重に鳴る。
            if (blend >= 0.5f)
            {
                eventClip    = ClipAt(clips, next->_clipIndex);
                eventPrev    = lr.transPrevTime;
                eventCur     = lr.transTime;
                eventWrapped = lr.transWrapped;
            }
        }

        GatherEvents(eventClip, eventPrev, eventCur, eventWrapped,
                     static_cast<i32>(li), rt.eventHits, outEvents);

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

    if (haveBase) animator.SetPoseOverride(finalPose);
}

} // namespace anim_graph
} // namespace dx12e
