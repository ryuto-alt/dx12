#include "animation/Animator.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

namespace
{
// 再生位置を [0, duration] に畳む。duration <= 0 は 0 に固定（0 除算・NaN 回避）。
void WrapClipTime(float& time, float duration, bool looping)
{
    if (duration <= 0.0f) { time = 0.0f; return; }
    if (looping)
    {
        time = std::fmod(time, duration);
        if (time < 0.0f) time += duration;
    }
    else
    {
        time = std::clamp(time, 0.0f, duration);
    }
}
} // namespace

// ---------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------
void Animator::Initialize(const Skeleton* skeleton, const AnimationClip* clip)
{
    m_skeleton    = skeleton;
    m_clip        = clip;
    m_currentTime = 0.0f;
    m_nextClip    = nullptr;
    m_blending    = false;
    m_blendFactor = 0.0f;
    m_poseOverridden = false;

    if (!skeleton) return;

    const u32 boneCount = skeleton->GetBoneCount();

    // ポーズはバインドポーズで初期化（GetCurrentPose が最初から有効）。
    MakeBindPose(*skeleton, m_pose);
    m_poseB.assign(boneCount, BoneTRS{});

    // スキニング行列は従来どおり単位行列で初期化する。
    // （単位行列 = メッシュ頂点をそのまま出す = 見た目はバインドポーズ）
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    m_skinningMatrices.assign(boneCount, identity);
    m_globalMatrices.assign(boneCount, identity);
}

// ---------------------------------------------------------------
// SetClip - 即座にクリップを切り替え
// ---------------------------------------------------------------
void Animator::SetClip(const AnimationClip* clip)
{
    m_clip        = clip;
    m_currentTime = 0.0f;
    m_blending    = false;
    m_nextClip    = nullptr;
    m_blendFactor = 0.0f;
}

// ---------------------------------------------------------------
// CrossFadeTo - ブレンドで切り替え
// ---------------------------------------------------------------
void Animator::CrossFadeTo(const AnimationClip* nextClip, float blendDuration)
{
    if (!nextClip || nextClip == m_clip)
    {
        return;
    }

    // ★ブレンド中に同じ行き先をもう一度指定されたら何もしない。
    //   これが無いと「毎フレーム playAnimByName("Walk") を呼ぶ」という
    //   ドキュメント推奨の書き方（docs/SCRIPTING.md / docs/ANIMATION.md の例）で
    //   毎回 m_blendFactor が 0 に巻き戻り、**ブレンドが永久に完了しない**。
    //   完了しない間は m_clip が旧クリップのままなので上のガードも素通りし、
    //   「Walk が少しだけ混ざった Idle」を延々再生する＝アニメが切り替わらない。
    //   （blendDuration=0 だと即時切替パスに落ちるので突然直る、という紛らわしい形になる）
    if (m_blending && nextClip == m_nextClip)
    {
        return;
    }

    m_nextClip      = nextClip;
    m_nextTime      = 0.0f;
    m_blendFactor   = 0.0f;
    m_blendDuration = blendDuration;
    m_blending      = true;

    // blendDuration <= 0 は「即座に切り替え」。ここで畳んでおかないと
    // Update 側で 0 除算 → m_blendFactor が NaN のまま固着する（B2）。
    if (m_blendDuration <= 1e-6f)
    {
        m_clip        = nextClip;
        m_currentTime = 0.0f;
        m_nextClip    = nullptr;
        m_blending    = false;
    }
}

// ---------------------------------------------------------------
// EvaluateClip - クリップ 1 本をサンプルしてポーズ→FK→スキニング
// ---------------------------------------------------------------
void Animator::EvaluateClip(const AnimationClip* clip, float time)
{
    if (!m_skeleton || !clip) return;
    SamplePose(*clip, time, *m_skeleton, m_pose);
    RecomputeFromPose();
}

void Animator::RecomputeFromPose()
{
    if (!m_skeleton) return;
    ComputeGlobalMatrices(*m_skeleton, m_pose, m_globalMatrices);
    GlobalToSkinning(*m_skeleton, m_globalMatrices, m_skinningMatrices);
}

void Animator::RebuildSkinningFromGlobals()
{
    if (!m_skeleton) return;
    GlobalToSkinning(*m_skeleton, m_globalMatrices, m_skinningMatrices);
}

void Animator::SetPoseOverride(const AnimPose& pose)
{
    if (!m_skeleton) return;
    m_pose = pose;
    m_pose.resize(m_skeleton->GetBoneCount());
    RecomputeFromPose();
    m_poseOverridden = true;
}

// ---------------------------------------------------------------
// Update
// ---------------------------------------------------------------
void Animator::Update(float deltaTime)
{
    if (!m_skeleton)
    {
        return;
    }

    // 外部（AnimatorController 等）がポーズを注入したフレームは、
    // 時間の進行もポーズもそちらの管理。ここでは何もしない。
    if (m_poseOverridden)
    {
        m_poseOverridden = false;
        return;
    }

    if (!m_clip)
    {
        return;
    }

    const float duration = m_clip->GetDuration();

    // 再生速度倍率を適用（クロスフェード中の両クリップ・ブレンド進行にも一律適用）
    deltaTime *= m_speed;

    // duration <= 0（1 フレームだけのポーズクリップ）でも、
    // 時刻 0 のポーズを 1 回は評価する（B5。以前は早期 return で無視されていた）。
    if (duration <= 0.0f)
    {
        m_currentTime = 0.0f;
        m_blending = false;
        m_nextClip = nullptr;
        EvaluateClip(m_clip, 0.0f);
        return;
    }

    // 現在のクリップの時間を進める
    m_currentTime += deltaTime * m_clip->GetTicksPerSecond();
    WrapClipTime(m_currentTime, duration, m_looping);

    if (m_blending && m_nextClip)
    {
        // ブレンド先の時間を進める。
        // ⚠️ ここは m_looping（＝現クリップ用のフラグ）ではなく常にループで畳む（B3）。
        //    以前は m_looping が false のとき次クリップの時刻が一切畳まれず、
        //    非ループ → ループの遷移で先クリップが止まって見えていた。
        m_nextTime += deltaTime * m_nextClip->GetTicksPerSecond();
        WrapClipTime(m_nextTime, m_nextClip->GetDuration(), true);

        // ブレンドファクタを進める（m_blendDuration は CrossFadeTo が
        // 1e-6 以下を弾いているので 0 除算は起きない）
        m_blendFactor += deltaTime / m_blendDuration;

        if (m_blendFactor >= 1.0f)
        {
            // ブレンド完了
            m_clip        = m_nextClip;
            m_currentTime = m_nextTime;
            m_nextClip    = nullptr;
            m_blending    = false;
            m_blendFactor = 0.0f;

            EvaluateClip(m_clip, m_currentTime);
        }
        else
        {
            // ★ B1 の修正 ★
            // 両クリップを「ローカル TRS ポーズ」までサンプルしてから
            // 位置=lerp / 回転=slerp / スケール=lerp でブレンドし、そのあと FK する。
            // （以前は最終スキニング行列を要素ごとに XMVectorLerp していたため、
            //   回転行列の線形補間になって遷移中に骨が縮む・シアーしていた）
            SamplePose(*m_clip,     m_currentTime, *m_skeleton, m_pose);
            SamplePose(*m_nextClip, m_nextTime,    *m_skeleton, m_poseB);
            BlendPoseInPlace(m_pose, m_poseB, m_blendFactor);
            RecomputeFromPose();
        }
    }
    else
    {
        // ブレンドなし: 通常の評価
        EvaluateClip(m_clip, m_currentTime);
    }
}

} // namespace dx12e
