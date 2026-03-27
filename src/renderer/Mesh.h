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

    void FinishUpload();

    const VertexBuffer& GetVertexBuffer() const { return m_vertexBuffer; }
    const IndexBuffer&  GetIndexBuffer()  const { return m_indexBuffer; }
    u32 GetIndexCount() const { return m_indexBuffer.GetIndexCount(); }

    void SetMaterial(Material* mat) { m_material = mat; }
    const Material* GetMaterial() const { return m_material; }

    static const D3D12_INPUT_ELEMENT_DESC* GetInputLayout();
    static u32 GetInputLayoutCount();

private:
    VertexBuffer m_vertexBuffer;
    IndexBuffer  m_indexBuffer;
    Material*    m_material = nullptr;
};

} // namespace dx12e
