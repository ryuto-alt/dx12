#include "renderer/Mesh.h"
#include "core/Logger.h"
#include <algorithm>
#include <meshoptimizer.h>

using namespace DirectX;

namespace dx12e
{

static const D3D12_INPUT_ELEMENT_DESC kInputLayout[] =
{
    { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

void Mesh::Initialize(GraphicsDevice& device,
                      const std::vector<Vertex>& inVertices,
                      const std::vector<u32>& inIndices,
                      ID3D12GraphicsCommandList* cmd)
{
    // meshoptimizer 定石パイプライン: 完全重複頂点の融合 → 頂点キャッシュ最適化 → フェッチ最適化。
    // DCC エクスポート(GLB 等)は UV/法線シームで頂点が分割され、素通しだと
    // ポストトランスフォームキャッシュがほぼ効かず、ハイポリ大量描画で頂点処理律速になる。
    std::vector<Vertex> vertices;
    std::vector<u32>    indices;
    if (!inVertices.empty() && !inIndices.empty())
    {
        std::vector<u32> remap(inVertices.size());
        const size_t unique = meshopt_generateVertexRemap(remap.data(),
            inIndices.data(), inIndices.size(),
            inVertices.data(), inVertices.size(), sizeof(Vertex));
        vertices.resize(unique);
        indices.resize(inIndices.size());
        meshopt_remapVertexBuffer(vertices.data(), inVertices.data(), inVertices.size(),
                                  sizeof(Vertex), remap.data());
        meshopt_remapIndexBuffer(indices.data(), inIndices.data(), inIndices.size(), remap.data());
        meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), unique);
        meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indices.size(),
                                    vertices.data(), unique, sizeof(Vertex));
    }
    else
    {
        vertices = inVertices;
        indices  = inIndices;
    }

    // AABB + 頂点データキャッシュ
    m_verticesCache = vertices;  // UV スケール用に全頂点を保持
    if (!vertices.empty())
    {
        m_aabbMin = m_aabbMax = vertices[0].position;
        m_positions.reserve(vertices.size());
        for (const auto& v : vertices)
        {
            m_positions.push_back(v.position);
            m_aabbMin.x = (std::min)(m_aabbMin.x, v.position.x);
            m_aabbMin.y = (std::min)(m_aabbMin.y, v.position.y);
            m_aabbMin.z = (std::min)(m_aabbMin.z, v.position.z);
            m_aabbMax.x = (std::max)(m_aabbMax.x, v.position.x);
            m_aabbMax.y = (std::max)(m_aabbMax.y, v.position.y);
            m_aabbMax.z = (std::max)(m_aabbMax.z, v.position.z);
        }
    }

    m_vertexBuffer.Initialize(device,
                              vertices.data(),
                              static_cast<u32>(vertices.size() * sizeof(Vertex)),
                              static_cast<u32>(sizeof(Vertex)),
                              cmd);

    m_indexBuffer.Initialize(device,
                             indices.data(),
                             static_cast<u32>(indices.size()),
                             cmd);

    GenerateLods(device, vertices, indices, cmd);
}

// meshoptimizer で LOD1(~30%) / LOD2(~10%) のインデックスを生成する。
// 頂点バッファは全LODで共有（simplify はインデックスの間引きのみ＝スキニングもそのまま動く）。
// 前LODからさらに簡略化する方式（高速で遷移も滑らか）。
void Mesh::GenerateLods(GraphicsDevice& device,
                        const std::vector<Vertex>& vertices,
                        const std::vector<u32>& indices,
                        ID3D12GraphicsCommandList* cmd)
{
    m_lodCount = 1;
    // 小さいメッシュはLOD不要（切替の手間のほうが高くつく）
    if (vertices.empty() || indices.size() < 3000) return;

    struct LodSpec { f32 targetRatio; f32 maxError; };
    static constexpr LodSpec kSpecs[kMaxLods - 1] =
        { {0.30f, 0.05f}, {0.10f, 0.25f}, {0.03f, 0.45f}, {0.01f, 0.65f} };

    // Permissive: DCCエクスポート品はUV/法線シームで頂点が全スプリットされがちで、
    // 既定では全エッジが境界扱い＝簡略化がほぼ進まない（例: Blender GLB が 755712→755706）。
    // シームをまたぐコラプスを許可して実効を出す。Prune で分離小片も除去。
    constexpr unsigned kSimplifyOpts = meshopt_SimplifyPermissive | meshopt_SimplifyPrune;

    const u32* srcIdx   = indices.data();
    size_t     srcCount = indices.size();
    std::vector<u32> prev;
    for (const auto& spec : kSpecs)
    {
        size_t target = static_cast<size_t>(static_cast<f64>(indices.size()) * spec.targetRatio);
        target = (std::max<size_t>)(target - target % 3, 96);

        std::vector<u32> dst(srcCount);
        f32 err = 0.0f;
        const size_t n = meshopt_simplify(dst.data(), srcIdx, srcCount,
                                          &vertices[0].position.x, vertices.size(), sizeof(Vertex),
                                          target, spec.maxError, kSimplifyOpts, &err);
        // 有意に減らなければ以降のLODは諦める（UV/法線の切れ目が多い形状など）
        if (n == 0 || n >= srcCount - srcCount / 10)
        {
            Logger::Info("Mesh LOD{}: 簡略化が効かず打ち切り ({} -> {} idx, err={:.4f})",
                         m_lodCount, srcCount, n, err);
            break;
        }
        Logger::Info("Mesh LOD{}: {} -> {} idx (err={:.4f})", m_lodCount, srcCount, n, err);

        dst.resize(n);
        meshopt_optimizeVertexCache(dst.data(), dst.data(), n, vertices.size());
        m_lodIndexBuffers[m_lodCount - 1].Initialize(device, dst.data(), static_cast<u32>(n), cmd);
        ++m_lodCount;

        prev     = std::move(dst);
        srcIdx   = prev.data();
        srcCount = prev.size();
    }
}

void Mesh::FinishUpload()
{
    m_vertexBuffer.FinishUpload();
    m_indexBuffer.FinishUpload();
    for (u32 i = 1; i < m_lodCount; ++i)
        m_lodIndexBuffers[i - 1].FinishUpload();
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

void Mesh::InitializeAsPlane(GraphicsDevice& device, f32 size, u32 subdivisions)
{
    const f32 half = size * 0.5f;
    const XMFLOAT3 normal = {0, 1, 0};
    const XMFLOAT4 white = {1, 1, 1, 1};
    const f32 uvScale = size;  // 1m = 1UV unit（グリッドシェーダー用）

    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    u32 vertsPerSide = subdivisions + 1;
    f32 step = size / static_cast<f32>(subdivisions);

    for (u32 z = 0; z < vertsPerSide; ++z)
    {
        for (u32 x = 0; x < vertsPerSide; ++x)
        {
            f32 px = -half + static_cast<f32>(x) * step;
            f32 pz = -half + static_cast<f32>(z) * step;
            f32 u = static_cast<f32>(x) / static_cast<f32>(subdivisions) * uvScale;
            f32 v = static_cast<f32>(z) / static_cast<f32>(subdivisions) * uvScale;
            vertices.push_back({{px, 0, pz}, normal, white, {u, v}});
        }
    }

    for (u32 z = 0; z < subdivisions; ++z)
    {
        for (u32 x = 0; x < subdivisions; ++x)
        {
            u32 i0 = z * vertsPerSide + x;
            u32 i1 = i0 + 1;
            u32 i2 = i0 + vertsPerSide;
            u32 i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

    Initialize(device, vertices, indices);
}

void Mesh::InitializeAsSphere(GraphicsDevice& device, f32 radius, u32 slices, u32 stacks)
{
    const XMFLOAT4 white = {1, 1, 1, 1};

    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    for (u32 stack = 0; stack <= stacks; ++stack)
    {
        f32 phi = XM_PI * static_cast<f32>(stack) / static_cast<f32>(stacks);
        f32 sinPhi = sinf(phi);
        f32 cosPhi = cosf(phi);

        for (u32 slice = 0; slice <= slices; ++slice)
        {
            f32 theta = XM_2PI * static_cast<f32>(slice) / static_cast<f32>(slices);
            f32 sinTheta = sinf(theta);
            f32 cosTheta = cosf(theta);

            XMFLOAT3 normal = {sinPhi * cosTheta, cosPhi, sinPhi * sinTheta};
            XMFLOAT3 pos = {normal.x * radius, normal.y * radius, normal.z * radius};
            XMFLOAT2 uv = {
                static_cast<f32>(slice) / static_cast<f32>(slices),
                static_cast<f32>(stack) / static_cast<f32>(stacks)
            };
            vertices.push_back({pos, normal, white, uv});
        }
    }

    for (u32 stack = 0; stack < stacks; ++stack)
    {
        for (u32 slice = 0; slice < slices; ++slice)
        {
            u32 i0 = stack * (slices + 1) + slice;
            u32 i1 = i0 + 1;
            u32 i2 = i0 + (slices + 1);
            u32 i3 = i2 + 1;
            indices.insert(indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }

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

// slot0 = 既存の頂点レイアウト全7要素、slot1 = MeshInstanceData(64B, PER_INSTANCE)。
static const D3D12_INPUT_ELEMENT_DESC kInstancedInputLayout[] =
{
    { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",        0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    // --- slot1 PER_INSTANCE: MeshInstanceData ---
    { "TEXCOORD",  8, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "TEXCOORD",  9, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "TEXCOORD", 10, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "TEXCOORD", 11, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
};

const D3D12_INPUT_ELEMENT_DESC* Mesh::GetInstancedInputLayout()
{
    return kInstancedInputLayout;
}

u32 Mesh::GetInstancedInputLayoutCount()
{
    return static_cast<u32>(std::size(kInstancedInputLayout));
}

void Mesh::ApplyUVScale(GraphicsDevice& device, float scaleU, float scaleV)
{
    if (m_verticesCache.empty()) return;

    // オリジナル UV からスケール（累積しない）
    auto scaled = m_verticesCache;
    for (auto& v : scaled)
    {
        v.texCoord.x *= scaleU;
        v.texCoord.y *= scaleV;
    }

    m_vertexBuffer.Initialize(device,
                              scaled.data(),
                              static_cast<u32>(scaled.size() * sizeof(Vertex)),
                              static_cast<u32>(sizeof(Vertex)));
}

void Mesh::SetVertexColor(GraphicsDevice& device, float r, float g, float b, float a)
{
    if (m_verticesCache.empty()) return;

    // キャッシュごと色を確定（以後の ApplyUVScale でも色が保たれる）
    for (auto& v : m_verticesCache)
        v.color = { r, g, b, a };

    m_vertexBuffer.Initialize(device,
                              m_verticesCache.data(),
                              static_cast<u32>(m_verticesCache.size() * sizeof(Vertex)),
                              static_cast<u32>(sizeof(Vertex)));
}

} // namespace dx12e
