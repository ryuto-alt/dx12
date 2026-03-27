#include "animation/Skeleton.h"

namespace dx12e
{

void Skeleton::AddBone(BoneNode bone)
{
    i32 index = static_cast<i32>(m_bones.size());
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

} // namespace dx12e
