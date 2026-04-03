#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

struct SceneNode
{
    std::string name;
    i32  parentIndex = -1;
    DirectX::XMFLOAT4X4 localDefault;
    DirectX::XMFLOAT4X4 inverseDefaultGlobal;  // 初期グローバル行列の逆行列（inverseBindPose相当）
    std::vector<u32>     meshIndices;  // aiScene::mMeshes へのインデックス
};

class NodeGraph
{
public:
    void  AddNode(SceneNode node);
    i32   FindNodeIndex(std::string_view name) const;
    u32   GetNodeCount() const { return static_cast<u32>(m_nodes.size()); }

    const SceneNode& GetNode(u32 i) const { return m_nodes[i]; }
    const std::vector<SceneNode>& GetNodes() const { return m_nodes; }

    // メッシュインデックス → ノードインデックス の逆引き
    i32 FindNodeForMesh(u32 meshIndex) const;

    // 全ノードの初期グローバル行列の逆行列を計算して保存
    void ComputeInverseDefaultGlobals();

private:
    std::vector<SceneNode>                m_nodes;
    std::unordered_map<std::string, i32>  m_nodeIndexMap;
};

} // namespace dx12e
