#include "renderer/Mesh.h"

using namespace DirectX;

namespace dx12e
{

static const D3D12_INPUT_ELEMENT_DESC kInputLayout[] =
{
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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

    // Face colors:
    //   +Y = Red,     -Y = Green
    //   +X = Blue,    -X = Yellow
    //   +Z = Magenta, -Z = Cyan
    const XMFLOAT4 red     = {1, 0, 0, 1};
    const XMFLOAT4 green   = {0, 1, 0, 1};
    const XMFLOAT4 blue    = {0, 0, 1, 1};
    const XMFLOAT4 yellow  = {1, 1, 0, 1};
    const XMFLOAT4 magenta = {1, 0, 1, 1};
    const XMFLOAT4 cyan    = {0, 1, 1, 1};

    std::vector<Vertex> vertices =
    {
        // +Y face (top) - Red - normal (0, 1, 0)
        { {-h,  h, -h}, { 0,  1,  0}, red },
        { {-h,  h,  h}, { 0,  1,  0}, red },
        { { h,  h,  h}, { 0,  1,  0}, red },
        { { h,  h, -h}, { 0,  1,  0}, red },

        // -Y face (bottom) - Green - normal (0, -1, 0)
        { {-h, -h,  h}, { 0, -1,  0}, green },
        { {-h, -h, -h}, { 0, -1,  0}, green },
        { { h, -h, -h}, { 0, -1,  0}, green },
        { { h, -h,  h}, { 0, -1,  0}, green },

        // +X face (right) - Blue - normal (1, 0, 0)
        { { h, -h, -h}, { 1,  0,  0}, blue },
        { { h,  h, -h}, { 1,  0,  0}, blue },
        { { h,  h,  h}, { 1,  0,  0}, blue },
        { { h, -h,  h}, { 1,  0,  0}, blue },

        // -X face (left) - Yellow - normal (-1, 0, 0)
        { {-h, -h,  h}, {-1,  0,  0}, yellow },
        { {-h,  h,  h}, {-1,  0,  0}, yellow },
        { {-h,  h, -h}, {-1,  0,  0}, yellow },
        { {-h, -h, -h}, {-1,  0,  0}, yellow },

        // +Z face (front) - Magenta - normal (0, 0, 1)
        { {-h, -h,  h}, { 0,  0,  1}, magenta },
        { { h, -h,  h}, { 0,  0,  1}, magenta },
        { { h,  h,  h}, { 0,  0,  1}, magenta },
        { {-h,  h,  h}, { 0,  0,  1}, magenta },

        // -Z face (back) - Cyan - normal (0, 0, -1)
        { { h, -h, -h}, { 0,  0, -1}, cyan },
        { {-h, -h, -h}, { 0,  0, -1}, cyan },
        { {-h,  h, -h}, { 0,  0, -1}, cyan },
        { { h,  h, -h}, { 0,  0, -1}, cyan },
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
