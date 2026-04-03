#pragma once

#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "graphics/Buffer.h"

namespace dx12e
{

struct Material;

struct Vertex
{
    DirectX::XMFLOAT3 position;                        // POSITION
    DirectX::XMFLOAT3 normal;                          // NORMAL
    DirectX::XMFLOAT4 color;                           // COLOR
    DirectX::XMFLOAT2 texCoord;                        // TEXCOORD
    DirectX::XMFLOAT4 tangent  = {0, 0, 0, 1};        // TANGENT (xyz=tangent, w=handedness)
    DirectX::XMUINT4  boneIndices = {0, 0, 0, 0};     // BLENDINDICES
    DirectX::XMFLOAT4 boneWeights = {0, 0, 0, 0};     // BLENDWEIGHT
};

class Mesh
{
public:
    void Initialize(GraphicsDevice& device,
                    const std::vector<Vertex>& vertices,
                    const std::vector<u32>& indices);

    void InitializeAsBox(GraphicsDevice& device);
    void InitializeAsPlane(GraphicsDevice& device, f32 size = 50.0f, u32 subdivisions = 1);
    void InitializeAsSphere(GraphicsDevice& device, f32 radius = 0.5f, u32 slices = 16, u32 stacks = 16);

    void FinishUpload();

    const VertexBuffer& GetVertexBuffer() const { return m_vertexBuffer; }
    const IndexBuffer&  GetIndexBuffer()  const { return m_indexBuffer; }
    u32 GetIndexCount() const { return m_indexBuffer.GetIndexCount(); }

    void SetMaterial(Material* mat) { m_material = mat; }
    const Material* GetMaterial() const { return m_material; }

    DirectX::XMFLOAT3 GetAABBMin() const { return m_aabbMin; }
    DirectX::XMFLOAT3 GetAABBMax() const { return m_aabbMax; }
    const std::vector<DirectX::XMFLOAT3>& GetPositions() const { return m_positions; }

    // UV スケール適用（頂点の texCoord を乗算して VertexBuffer を再作成）
    void ApplyUVScale(GraphicsDevice& device, float scaleU, float scaleV);

    static const D3D12_INPUT_ELEMENT_DESC* GetInputLayout();
    static u32 GetInputLayoutCount();

private:
    VertexBuffer m_vertexBuffer;
    IndexBuffer  m_indexBuffer;
    Material*    m_material = nullptr;
    DirectX::XMFLOAT3 m_aabbMin = { 0, 0, 0 };
    DirectX::XMFLOAT3 m_aabbMax = { 0, 0, 0 };
    std::vector<DirectX::XMFLOAT3> m_positions; // Convex Hull 用の頂点座標キャッシュ
    std::vector<Vertex> m_verticesCache;         // UV スケール用の頂点データキャッシュ
};

} // namespace dx12e
