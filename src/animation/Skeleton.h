#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

struct BoneNode
{
    std::string name;
    i32 parentIndex = -1;
    DirectX::XMFLOAT4X4 inverseBindPose;
    DirectX::XMFLOAT4X4 localBindPose;
};

class Skeleton
{
public:
    static constexpr u32 kMaxBones = 128;

    void AddBone(BoneNode bone);
    i32  FindBoneIndex(std::string_view name) const;
    u32  GetBoneCount() const { return static_cast<u32>(m_bones.size()); }

    const BoneNode&              GetBone(u32 index) const { return m_bones[index]; }
    const std::vector<BoneNode>& GetBones() const         { return m_bones; }

private:
    std::vector<BoneNode>                m_bones;
    std::unordered_map<std::string, i32> m_boneIndexMap;
};

} // namespace dx12e
