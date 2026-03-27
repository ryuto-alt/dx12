#include "renderer/Mesh.h"

using namespace DirectX;

namespace dx12e
{

static const D3D12_INPUT_ELEMENT_DESC kInputLayout[] =
{
    { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

void Mesh::Initialize(GraphicsDevice& device,
                      const std::vector<Vertex>& vertices,
                      const std::vector<u32>& indices)
{
    m_vertexBuffer.Initialize(device,
                              vertices.data(),
                              static_cast<u32>(vertices.size() * sizeof(Vertex)),
                              static_cast<u32>(sizeof(Vertex)));

    m_indexBuffer.Initialize(device,
                             indices.data(),
                             static_cast<u32>(indices.size()));
}

void Mesh::FinishUpload()
{
    m_vertexBuffer.FinishUpload();
    m_indexBuffer.FinishUpload();
}

void Mesh::InitializeAsBox(GraphicsDevice& device)
{
    // 24 vertices (4 per face), 36 indices (6 per face)
    // Center at origin, size 1x1x1 => half-extent = 0.5
    const f32 h = 0.5f;

    const XMFLOAT4 white = {1, 1, 1, 1};

    // UV corners
    const XMFLOAT2 uv00 = {0, 0};
    const XMFLOAT2 uv10 = {1, 0};
    const XMFLOAT2 uv11 = {1, 1};
    const XMFLOAT2 uv01 = {0, 1};

    std::vector<Vertex> vertices =
    {
        // +Y face (top) - normal (0, 1, 0)
        { {-h,  h, -h}, { 0,  1,  0}, white, uv00 },
        { {-h,  h,  h}, { 0,  1,  0}, white, uv10 },
        { { h,  h,  h}, { 0,  1,  0}, white, uv11 },
        { { h,  h, -h}, { 0,  1,  0}, white, uv01 },

        // -Y face (bottom) - normal (0, -1, 0)
        { {-h, -h,  h}, { 0, -1,  0}, white, uv00 },
        { {-h, -h, -h}, { 0, -1,  0}, white, uv10 },
        { { h, -h, -h}, { 0, -1,  0}, white, uv11 },
        { { h, -h,  h}, { 0, -1,  0}, white, uv01 },

        // +X face (right) - normal (1, 0, 0)
        { { h, -h, -h}, { 1,  0,  0}, white, uv00 },
        { { h,  h, -h}, { 1,  0,  0}, white, uv10 },
        { { h,  h,  h}, { 1,  0,  0}, white, uv11 },
        { { h, -h,  h}, { 1,  0,  0}, white, uv01 },

        // -X face (left) - normal (-1, 0, 0)
        { {-h, -h,  h}, {-1,  0,  0}, white, uv00 },
        { {-h,  h,  h}, {-1,  0,  0}, white, uv10 },
        { {-h,  h, -h}, {-1,  0,  0}, white, uv11 },
        { {-h, -h, -h}, {-1,  0,  0}, white, uv01 },

        // +Z face (front) - normal (0, 0, 1)
        { {-h, -h,  h}, { 0,  0,  1}, white, uv00 },
        { { h, -h,  h}, { 0,  0,  1}, white, uv10 },
        { { h,  h,  h}, { 0,  0,  1}, white, uv11 },
        { {-h,  h,  h}, { 0,  0,  1}, white, uv01 },

        // -Z face (back) - normal (0, 0, -1)
        { { h, -h, -h}, { 0,  0, -1}, white, uv00 },
        { {-h, -h, -h}, { 0,  0, -1}, white, uv10 },
        { {-h,  h, -h}, { 0,  0, -1}, white, uv11 },
        { { h,  h, -h}, { 0,  0, -1}, white, uv01 },
    };

    std::vector<u32> indices =
    {
        // +Y
         0,  1,  2,   0,  2,  3,
        // -Y
         4,  5,  6,   4,  6,  7,
        // +X
         8,  9, 10,   8, 10, 11,
        // -X
        12, 13, 14,  12, 14, 15,
        // +Z
        16, 17, 18,  16, 18, 19,
        // -Z
        20, 21, 22,  20, 22, 23,
    };

    Initialize(device, vertices, indices);
}

const D3D12_INPUT_ELEMENT_DESC* Mesh::GetInputLayout()
{
    return kInputLayout;
}

u32 Mesh::GetInputLayoutCount()
{
    return static_cast<u32>(std::size(kInputLayout));
}

} // namespace dx12e
