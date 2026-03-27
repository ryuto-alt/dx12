#pragma once

#include <vector>
#include <DirectXMath.h>
#include "core/Types.h"
#include "graphics/Buffer.h"

namespace dx12e
{

struct Vertex
{
    DirectX::XMFLOAT3 position;  // POSITION
    DirectX::XMFLOAT3 normal;    // NORMAL
    DirectX::XMFLOAT4 color;     // COLOR
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

    static const D3D12_INPUT_ELEMENT_DESC* GetInputLayout();
    static u32 GetInputLayoutCount();

private:
    VertexBuffer m_vertexBuffer;
    IndexBuffer  m_indexBuffer;
};

} // namespace dx12e
