#pragma once
#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class Skeleton;
class AnimationClip;
template<typename T> struct Keyframe;

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

    // 再生速度倍率（既定 1.0。2.0 で2倍速、0 で一時停止）。
    // Update の deltaTime に乗算され、クロスフェード中は両クリップに適用される。
    void  SetSpeed(float speed) { m_speed = speed; }
    float GetSpeed() const      { return m_speed; }

    // 現在クリップの再生位置（ticks。NormalizeToSeconds 済みクリップなら秒）。
    // ※ GetCurrentTime は windows.h のマクロと衝突するためこの名前
    float GetClipTime() const { return m_currentTime; }

    void CrossFadeTo(const AnimationClip* nextClip, float blendDuration = 0.3f);
    void SetClip(const AnimationClip* clip);

private:
    void ComputeBoneMatrices(const AnimationClip* clip, float time,
                             std::vector<DirectX::XMFLOAT4X4>& outMatrices) const;
    DirectX::XMFLOAT3 InterpolatePosition(
        const std::vector<Keyframe<DirectX::XMFLOAT3>>& keys, float time) const;

    DirectX::XMFLOAT4 InterpolateRotation(
        const std::vector<Keyframe<DirectX::XMFLOAT4>>& keys, float time) const;

    DirectX::XMFLOAT3 InterpolateScale(
        const std::vector<Keyframe<DirectX::XMFLOAT3>>& keys, float time) const;

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

    std::vector<DirectX::XMFLOAT4X4> m_skinningMatrices;
};

} // namespace dx12e
