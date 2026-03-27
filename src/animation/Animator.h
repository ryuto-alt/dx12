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

private:
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

    std::vector<DirectX::XMFLOAT4X4> m_skinningMatrices;
};

} // namespace dx12e
