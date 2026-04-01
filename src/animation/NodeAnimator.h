#pragma once
#include <vector>
#include <string>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class NodeGraph;
class NodeAnimationClip;
template<typename T> struct Keyframe;

class NodeAnimator
{
public:
    // restClip = デフォルト姿勢のクリップ（"static" 等）、playClip = 再生するクリップ
    void Initialize(const NodeGraph* graph,
                    const NodeAnimationClip* playClip,
                    const NodeAnimationClip* restClip);
    void Update(float deltaTime);

    const std::vector<DirectX::XMFLOAT4X4>& GetNodeGlobalMatrices() const
    {
        return m_nodeGlobalMatrices;
    }

    void SetLooping(bool loop) { m_looping = loop; }

    void CrossFadeTo(const NodeAnimationClip* nextClip, float blendDuration = 0.3f);
    void SetClip(const NodeAnimationClip* clip);

    const NodeAnimationClip* GetCurrentClip() const { return m_clip; }

private:
    // 素のグローバル行列を計算（inverseDefault 適用なし）
    void ComputeRawNodeMatrices(const NodeAnimationClip* clip, float time,
                                std::vector<DirectX::XMFLOAT4X4>& outMatrices) const;

    DirectX::XMFLOAT3 InterpolatePosition(
        const std::vector<Keyframe<DirectX::XMFLOAT3>>& keys, float time) const;
    DirectX::XMFLOAT4 InterpolateRotation(
        const std::vector<Keyframe<DirectX::XMFLOAT4>>& keys, float time) const;
    DirectX::XMFLOAT3 InterpolateScale(
        const std::vector<Keyframe<DirectX::XMFLOAT3>>& keys, float time) const;

    const NodeGraph*          m_graph = nullptr;
    const NodeAnimationClip*  m_clip  = nullptr;
    float                     m_currentTime = 0.0f;
    bool                      m_looping     = true;

    // CrossFade
    const NodeAnimationClip*  m_nextClip      = nullptr;
    float                     m_nextTime      = 0.0f;
    float                     m_blendFactor   = 0.0f;
    float                     m_blendDuration = 0.3f;
    bool                      m_blending      = false;

    std::vector<DirectX::XMFLOAT4X4> m_nodeGlobalMatrices;

    // rest pose のグローバル逆行列（ベイク行列と同じ計算で得たもの）
    std::vector<DirectX::XMFLOAT4X4> m_inverseRestGlobalMatrices;
};

} // namespace dx12e
