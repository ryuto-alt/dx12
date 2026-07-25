#include "animation/Skeleton.h"

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

namespace
{
// ASCII 小文字化（ボーン名は ASCII 前提。ロケール非依存にするため自前で持つ）。
std::string ToLowerAscii(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

void Skeleton::AddBone(BoneNode bone)
{
    // localBindPose を TRS へ分解してキャッシュする。
    // AnimPose がトラックの無いボーンをここから埋めるので、
    // 「分解 → S*R*T で再構成」した結果が元の行列と一致する必要がある。
    XMVECTOR s, r, t;
    const XMMATRIX bind = XMLoadFloat4x4(&bone.localBindPose);
    if (XMMatrixDecompose(&s, &r, &t, bind))
    {
        XMStoreFloat3(&bone.bindS, s);
        XMStoreFloat4(&bone.bindR, r);
        XMStoreFloat3(&bone.bindT, t);

        // 再構成して往復誤差を検査する（せん断があると分解は成功しても復元できない）。
        const XMMATRIX rebuilt = XMMatrixScalingFromVector(s) *
                                 XMMatrixRotationQuaternion(r) *
                                 XMMatrixTranslationFromVector(t);
        float maxErr = 0.0f;
        for (int row = 0; row < 4; ++row)
        {
            const XMVECTOR d = XMVectorAbs(XMVectorSubtract(rebuilt.r[row], bind.r[row]));
            maxErr = (std::max)(maxErr, XMVectorGetX(XMVectorMax(
                XMVectorMax(XMVectorSplatX(d), XMVectorSplatY(d)),
                XMVectorMax(XMVectorSplatZ(d), XMVectorSplatW(d)))));
        }
        bone.bindDecomposed = (maxErr < 1e-3f);
    }
    else
    {
        bone.bindT = {0.0f, 0.0f, 0.0f};
        bone.bindR = {0.0f, 0.0f, 0.0f, 1.0f};
        bone.bindS = {1.0f, 1.0f, 1.0f};
        bone.bindDecomposed = false;
    }

    if (!bone.bindDecomposed)
        m_hasUndecomposableBind = true;

    const i32 index = static_cast<i32>(m_bones.size());
    m_boneIndexMap[bone.name] = index;
    m_bones.push_back(std::move(bone));
}

i32 Skeleton::FindBoneIndex(std::string_view name) const
{
    auto it = m_boneIndexMap.find(std::string(name));
    if (it != m_boneIndexMap.end())
    {
        return it->second;
    }
    return -1;
}

bool Skeleton::AreBonesCorrectlyOrdered() const
{
    for (size_t i = 0; i < m_bones.size(); ++i)
    {
        const i32 p = m_bones[i].parentIndex;
        if (p >= 0 && static_cast<size_t>(p) >= i)
            return false;
    }
    return true;
}

i32 Skeleton::FindBoneIndexContaining(std::string_view fragment) const
{
    if (fragment.empty()) return -1;
    const std::string needle = ToLowerAscii(fragment);
    for (size_t i = 0; i < m_bones.size(); ++i)
    {
        if (ToLowerAscii(m_bones[i].name).find(needle) != std::string::npos)
            return static_cast<i32>(i);
    }
    return -1;
}

} // namespace dx12e
