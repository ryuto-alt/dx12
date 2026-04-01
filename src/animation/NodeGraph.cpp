#include "animation/NodeGraph.h"

namespace dx12e
{

void NodeGraph::AddNode(SceneNode node)
{
    i32 index = static_cast<i32>(m_nodes.size());
    m_nodeIndexMap[node.name] = index;
    m_nodes.push_back(std::move(node));
}

i32 NodeGraph::FindNodeIndex(std::string_view name) const
{
    auto it = m_nodeIndexMap.find(std::string(name));
    if (it != m_nodeIndexMap.end())
    {
        return it->second;
    }
    return -1;
}

i32 NodeGraph::FindNodeForMesh(u32 meshIndex) const
{
    for (u32 i = 0; i < static_cast<u32>(m_nodes.size()); ++i)
    {
        for (u32 mi : m_nodes[i].meshIndices)
        {
            if (mi == meshIndex)
            {
                return static_cast<i32>(i);
            }
        }
    }
    return -1;
}

void NodeGraph::ComputeInverseDefaultGlobals()
{
    u32 count = static_cast<u32>(m_nodes.size());
    std::vector<DirectX::XMMATRIX> globals(count);

    for (u32 i = 0; i < count; ++i)
    {
        DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(&m_nodes[i].localDefault);

        if (m_nodes[i].parentIndex >= 0)
        {
            globals[i] = local * globals[static_cast<u32>(m_nodes[i].parentIndex)];
        }
        else
        {
            globals[i] = local;
        }

        // 逆行列を計算して保存
        DirectX::XMVECTOR det;
        DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, globals[i]);
        DirectX::XMStoreFloat4x4(&m_nodes[i].inverseDefaultGlobal, inv);
    }
}

} // namespace dx12e
