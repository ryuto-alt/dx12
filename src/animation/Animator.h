#pragma once
#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "animation/AnimPose.h"

namespace dx12e
{

class Skeleton;
class AnimationClip;
template<typename T> struct Keyframe;

// ---------------------------------------------------------------------------
// Animator — 単一クリップ + 1 本のクロスフェードの再生器。
//
// 内部は「クリップ → ローカル TRS ポーズ（AnimPose）→ ブレンド → FK →
// スキニング行列」の順で動く。クロスフェードは**ポーズ段階**で
// 位置=lerp / 回転=slerp / スケール=lerp するので、遷移中に骨が縮まない。
// （2026-07-26 以前は最終スキニング行列を要素ごとに XMVectorLerp していた＝B1）
//
// 公開 API は 1 つも消していない。既存の Scene / ScriptEngine / MCP /
// InspectorPanel はそのまま動く（決定 3: 後方互換）。
// ---------------------------------------------------------------------------
class Animator
{
public:
    void Initialize(const Skeleton* skeleton, const AnimationClip* clip);
    void Update(float deltaTime);

    const std::vector<DirectX::XMFLOAT4X4>& GetSkinningMatrices() const
    {
        return m_skinningMatrices;
    }

    void SetLooping(bool loop) { m_looping = loop; }
    bool GetLooping() const    { return m_looping; }

    // 再生速度倍率（既定 1.0。2.0 で2倍速、0 で一時停止）。
    // Update の deltaTime に乗算され、クロスフェード中は両クリップに適用される。
    void  SetSpeed(float speed) { m_speed = speed; }
    float GetSpeed() const      { return m_speed; }

    // 現在クリップの再生位置（ticks。NormalizeToSeconds 済みクリップなら秒）。
    // ※ GetCurrentTime は windows.h のマクロと衝突するためこの名前
    float GetClipTime() const { return m_currentTime; }
    void  SetClipTime(float t) { m_currentTime = t; }

    void CrossFadeTo(const AnimationClip* nextClip, float blendDuration = 0.3f);
    void SetClip(const AnimationClip* clip);

    // ---- 状態の読み出し（AnimatorController / MCP / Inspector 用）----
    const Skeleton*      GetSkeleton() const { return m_skeleton; }
    const AnimationClip* GetClip() const     { return m_clip; }
    const AnimationClip* GetNextClip() const { return m_nextClip; }
    bool  IsBlending() const                 { return m_blending; }
    float GetBlendFactor() const             { return m_blendFactor; }

    // ---- ポーズ空間の口（AnimGraphRuntime / FootIK が使う）----

    // 外部が計算したローカル TRS ポーズを注入する。
    // 呼んだ時点で FK + スキニング行列まで再計算し、
    // **その後の Update(dt) はクリップのサンプリングを丸ごとスキップする**
    // （時間の進行はポーズを作った側が管理しているため）。
    void SetPoseOverride(const AnimPose& pose);

    // 注入を解除して次フレームから通常の再生に戻る。
    void ClearPoseOverride() { m_poseOverridden = false; }
    bool HasPoseOverride() const { return m_poseOverridden; }

    // FK 前のローカルポーズ。
    const AnimPose& GetCurrentPose() const { return m_pose; }
    // IK がローカル回転を直接書き換えるための口。書き換えたら RecomputeFromPose()。
    AnimPose&       GetMutablePose()       { return m_pose; }

    // FK 済みのグローバル行列（IK が読む / 書く）。
    const std::vector<DirectX::XMFLOAT4X4>& GetGlobalMatrices() const { return m_globalMatrices; }
    std::vector<DirectX::XMFLOAT4X4>&       GetMutableGlobalMatrices() { return m_globalMatrices; }

    // ポーズ → FK → スキニング行列（ポーズを直接いじった後に呼ぶ）。
    void RecomputeFromPose();
    // グローバル行列 → スキニング行列（IK でグローバルを直接いじった後に呼ぶ）。
    void RebuildSkinningFromGlobals();

private:
    // clip を time でサンプルして m_pose に置き、FK + スキニングまで通す。
    void EvaluateClip(const AnimationClip* clip, float time);

    const Skeleton*      m_skeleton = nullptr;
    const AnimationClip* m_clip     = nullptr;
    float                m_currentTime = 0.0f;
    bool                 m_looping     = true;
    float                m_speed       = 1.0f;

    const AnimationClip* m_nextClip      = nullptr;
    float                m_nextTime      = 0.0f;
    float                m_blendFactor   = 0.0f;
    float                m_blendDuration = 0.3f;
    bool                 m_blending      = false;

    bool                 m_poseOverridden = false;

    AnimPose                         m_pose;       // 今フレームのローカル TRS ポーズ
    AnimPose                         m_poseB;      // クロスフェード先の一時バッファ
    std::vector<DirectX::XMFLOAT4X4> m_globalMatrices;
    std::vector<DirectX::XMFLOAT4X4> m_skinningMatrices;
};

} // namespace dx12e
