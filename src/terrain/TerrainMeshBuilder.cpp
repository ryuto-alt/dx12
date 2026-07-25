#include "terrain/TerrainMeshBuilder.h"

#include <algorithm>

using namespace DirectX;

namespace dx12e
{

void TerrainMeshBuilder::Build(const HeightField& hf, f32 uvTiles, const XMFLOAT4& color,
                               std::vector<Vertex>& outVertices, std::vector<u32>& outIndices)
{
    outVertices.clear();
    outIndices.clear();
    if (!hf.IsValid()) return;

    const u32 n = hf.Resolution();
    outVertices.resize(static_cast<size_t>(n) * static_cast<size_t>(n));
    UpdateVertices(hf, uvTiles, color, hf.FullRect(), outVertices);

    outIndices.reserve(static_cast<size_t>(n - 1) * static_cast<size_t>(n - 1) * 6);
    for (u32 z = 0; z + 1 < n; ++z)
    {
        for (u32 x = 0; x + 1 < n; ++x)
        {
            const u32 i0 = z * n + x;        // (x,   z)
            const u32 i1 = i0 + 1;           // (x+1, z)
            const u32 i2 = i0 + n;           // (x,   z+1)
            const u32 i3 = i2 + 1;           // (x+1, z+1)
            // 巻き順は Mesh::InitializeAsPlane と同一（上向きが表）。
            outIndices.push_back(i0); outIndices.push_back(i2); outIndices.push_back(i1);
            outIndices.push_back(i1); outIndices.push_back(i2); outIndices.push_back(i3);
        }
    }
}

void TerrainMeshBuilder::UpdateVertices(const HeightField& hf, f32 uvTiles, const XMFLOAT4& color,
                                        const HeightField::Rect& dirty, std::vector<Vertex>& vertices)
{
    if (!hf.IsValid() || !dirty.Valid()) return;

    const u32 n = hf.Resolution();
    if (vertices.size() != static_cast<size_t>(n) * static_cast<size_t>(n)) return;

    // 法線は左右上下のサンプルを見るので、影響範囲を 1 セル広げてから書き換える。
    HeightField::Rect r = dirty;
    r.x0 -= 1; r.z0 -= 1; r.x1 += 1; r.z1 += 1;
    r = hf.ClampRect(r);
    if (!r.Valid()) return;

    const f32 cell   = hf.CellSize();
    const f32 uvStep = (n > 1) ? (uvTiles / static_cast<f32>(n - 1)) : uvTiles;

    for (i32 z = r.z0; z <= r.z1; ++z)
    {
        const f32 wz = hf.GridToLocalZ(z);
        for (i32 x = r.x0; x <= r.x1; ++x)
        {
            const f32 wx = hf.GridToLocalX(x);
            Vertex& v = vertices[static_cast<size_t>(z) * static_cast<size_t>(n)
                                 + static_cast<size_t>(x)];

            v.position = XMFLOAT3{ wx, hf.At(x, z), wz };

            const f32 hl = hf.At(x - 1, z);
            const f32 hr = hf.At(x + 1, z);
            const f32 hd = hf.At(x, z - 1);
            const f32 hu = hf.At(x, z + 1);
            XMStoreFloat3(&v.normal,
                          XMVector3Normalize(XMVectorSet(hl - hr, 2.0f * cell, hd - hu, 0.0f)));

            v.color    = color;
            v.texCoord = XMFLOAT2{ static_cast<f32>(x) * uvStep, static_cast<f32>(z) * uvStep };
            // 接線は +X（UV の U 方向）。法線マップ付きの .dxmat を貼っても破綻しないように。
            v.tangent  = XMFLOAT4{ 1.0f, 0.0f, 0.0f, 1.0f };
            v.boneIndices = XMUINT4{ 0, 0, 0, 0 };
            v.boneWeights = XMFLOAT4{ 0.0f, 0.0f, 0.0f, 0.0f };
        }
    }
}

} // namespace dx12e
