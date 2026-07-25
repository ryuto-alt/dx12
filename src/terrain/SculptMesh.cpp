#include "terrain/SculptMesh.h"

#include "terrain/TerrainBrush.h"   // TerrainBrushWeight / TerrainFbm（地形と共有する重み関数とノイズ）

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace dx12e
{

namespace
{

constexpr f32 kPi  = 3.14159265358979323846f;
constexpr f32 k2Pi = 6.28318530717958647692f;

// 三角形 tri の面法線（正規化しない＝面積で重み付けされる）。
// 巻き順の規約は Mesh::InitializeAsPlane / TerrainMeshBuilder と同じで
// cross(v1-v0, v2-v0) が「表」を向く（＝Jolt が MeshShape に要求する CCW とも一致する）。
XMVECTOR TriNormalRaw(const std::vector<XMFLOAT3>& pos, const std::vector<u32>& idx, u32 tri)
{
    const size_t base = static_cast<size_t>(tri) * 3;
    const XMVECTOR v0 = XMLoadFloat3(&pos[idx[base + 0]]);
    const XMVECTOR v1 = XMLoadFloat3(&pos[idx[base + 1]]);
    const XMVECTOR v2 = XMLoadFloat3(&pos[idx[base + 2]]);
    return XMVector3Cross(XMVectorSubtract(v1, v0), XMVectorSubtract(v2, v0));
}

// ---- 素体プリミティブの生成 ----

// 1 枚の四角形グリッドを追記する。uDir/vDir は面内の単位方向、normal は外向き法線。
// 頂点 (a,b) の位置 = center + uDir*((a/seg-0.5)*size) + vDir*((b/seg-0.5)*size)。
// 巻き順 (i0,i2,i1)/(i1,i2,i3) は Mesh::InitializeAsPlane と同一で、
// cross(v1-v0, v2-v0) が normal を向く（uDir/vDir は cross(vDir,uDir)==normal を満たすこと）。
void AppendGrid(std::vector<XMFLOAT3>& pos, std::vector<XMFLOAT3>& nrm,
                std::vector<XMFLOAT2>& uv, std::vector<u32>& idx,
                const XMFLOAT3& center, const XMFLOAT3& uDir, const XMFLOAT3& vDir,
                const XMFLOAT3& normal, f32 size, u32 seg)
{
    const u32 base = static_cast<u32>(pos.size());
    const u32 line = seg + 1;
    const f32 fseg = static_cast<f32>(seg);

    for (u32 b = 0; b < line; ++b)
    {
        const f32 fv = static_cast<f32>(b) / fseg;
        const f32 sv = (fv - 0.5f) * size;
        for (u32 a = 0; a < line; ++a)
        {
            const f32 fu = static_cast<f32>(a) / fseg;
            const f32 su = (fu - 0.5f) * size;
            pos.push_back(XMFLOAT3{ center.x + uDir.x * su + vDir.x * sv,
                                    center.y + uDir.y * su + vDir.y * sv,
                                    center.z + uDir.z * su + vDir.z * sv });
            nrm.push_back(normal);
            uv.push_back(XMFLOAT2{ fu, fv });
        }
    }

    for (u32 b = 0; b < seg; ++b)
    {
        for (u32 a = 0; a < seg; ++a)
        {
            const u32 i0 = base + b * line + a;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + line;
            const u32 i3 = i2 + 1;
            idx.push_back(i0); idx.push_back(i2); idx.push_back(i1);
            idx.push_back(i1); idx.push_back(i2); idx.push_back(i3);
        }
    }
}

void AppendSphere(std::vector<XMFLOAT3>& pos, std::vector<XMFLOAT3>& nrm,
                  std::vector<XMFLOAT2>& uv, std::vector<u32>& idx,
                  f32 radius, u32 slices, u32 stacks)
{
    const u32 base = static_cast<u32>(pos.size());
    for (u32 st = 0; st <= stacks; ++st)
    {
        const f32 phi = kPi * static_cast<f32>(st) / static_cast<f32>(stacks);
        const f32 sp = std::sin(phi);
        const f32 cp = std::cos(phi);
        for (u32 sl = 0; sl <= slices; ++sl)
        {
            const f32 th = k2Pi * static_cast<f32>(sl) / static_cast<f32>(slices);
            const XMFLOAT3 n{ sp * std::cos(th), cp, sp * std::sin(th) };
            pos.push_back(XMFLOAT3{ n.x * radius, n.y * radius, n.z * radius });
            nrm.push_back(n);
            uv.push_back(XMFLOAT2{ static_cast<f32>(sl) / static_cast<f32>(slices),
                                   static_cast<f32>(st) / static_cast<f32>(stacks) });
        }
    }

    // Mesh::InitializeAsSphere は (i0,i2,i1) で張っているが、それだと面法線が内向きになる。
    // ここは Box/Plane と同じ「cross(v1-v0,v2-v0) が外向き」に揃える（Jolt の MeshShape が
    // CCW を要求するため、コリジョンを載せる以上こちらの向きでないと困る）。
    const u32 line = slices + 1;
    for (u32 st = 0; st < stacks; ++st)
    {
        for (u32 sl = 0; sl < slices; ++sl)
        {
            const u32 i0 = base + st * line + sl;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + line;
            const u32 i3 = i2 + 1;
            idx.push_back(i0); idx.push_back(i1); idx.push_back(i2);
            idx.push_back(i1); idx.push_back(i3); idx.push_back(i2);
        }
    }
}

void AppendCylinder(std::vector<XMFLOAT3>& pos, std::vector<XMFLOAT3>& nrm,
                    std::vector<XMFLOAT2>& uv, std::vector<u32>& idx,
                    f32 radius, f32 height, u32 slices, u32 hseg)
{
    const f32 half = height * 0.5f;

    // 側面
    const u32 sideBase = static_cast<u32>(pos.size());
    for (u32 h = 0; h <= hseg; ++h)
    {
        const f32 fy = static_cast<f32>(h) / static_cast<f32>(hseg);
        const f32 y  = -half + fy * height;
        for (u32 s = 0; s <= slices; ++s)
        {
            const f32 th = k2Pi * static_cast<f32>(s) / static_cast<f32>(slices);
            const f32 cs = std::cos(th);
            const f32 sn = std::sin(th);
            pos.push_back(XMFLOAT3{ cs * radius, y, sn * radius });
            nrm.push_back(XMFLOAT3{ cs, 0.0f, sn });
            uv.push_back(XMFLOAT2{ static_cast<f32>(s) / static_cast<f32>(slices), fy });
        }
    }
    const u32 line = slices + 1;
    for (u32 h = 0; h < hseg; ++h)
    {
        for (u32 s = 0; s < slices; ++s)
        {
            const u32 i0 = sideBase + h * line + s;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + line;
            const u32 i3 = i2 + 1;
            idx.push_back(i0); idx.push_back(i2); idx.push_back(i1);
            idx.push_back(i1); idx.push_back(i2); idx.push_back(i3);
        }
    }

    // 上下のふた（中心 + 扇）
    for (int cap = 0; cap < 2; ++cap)
    {
        const bool top = (cap == 0);
        const f32  y   = top ? half : -half;
        const XMFLOAT3 n{ 0.0f, top ? 1.0f : -1.0f, 0.0f };

        const u32 cbase = static_cast<u32>(pos.size());
        pos.push_back(XMFLOAT3{ 0.0f, y, 0.0f });
        nrm.push_back(n);
        uv.push_back(XMFLOAT2{ 0.5f, 0.5f });
        for (u32 s = 0; s <= slices; ++s)
        {
            const f32 th = k2Pi * static_cast<f32>(s) / static_cast<f32>(slices);
            const f32 cs = std::cos(th);
            const f32 sn = std::sin(th);
            pos.push_back(XMFLOAT3{ cs * radius, y, sn * radius });
            nrm.push_back(n);
            uv.push_back(XMFLOAT2{ 0.5f + 0.5f * cs, 0.5f + 0.5f * sn });
        }
        for (u32 s = 0; s < slices; ++s)
        {
            const u32 r0 = cbase + 1 + s;
            const u32 r1 = r0 + 1;
            if (top) { idx.push_back(cbase); idx.push_back(r1); idx.push_back(r0); }
            else     { idx.push_back(cbase); idx.push_back(r0); idx.push_back(r1); }
        }
    }
}

// Box の 6 面。cross(v, u) == n を満たす組み合わせ（AppendGrid の巻き順の前提）。
struct BoxFace { XMFLOAT3 n, u, v; };
const BoxFace kBoxFaces[6] = {
    { { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f} },   // +X : cross(+Y,+Z)=+X
    { {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} },   // -X : cross(+Z,+Y)=-X
    { { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f} },   // +Y : cross(+Z,+X)=+Y
    { { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f} },   // -Y : cross(+X,+Z)=-Y
    { { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f} },   // +Z : cross(+X,+Y)=+Z
    { { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f} },   // -Z : cross(+Y,+X)=-Z
};

} // anonymous namespace

f32 SculptFbm3(const XMFLOAT3& p, f32 frequency, i32 octaves, f32 ridged, u32 seed)
{
    const f32 a = TerrainFbm(p.x, p.z, frequency, octaves, ridged, seed);
    const f32 b = TerrainFbm(p.y, p.x, frequency, octaves, ridged, seed + 7919u);
    const f32 c = TerrainFbm(p.z, p.y, frequency, octaves, ridged, seed + 15731u);
    return (a + b + c) * (1.0f / 3.0f);
}

// ==========================================================================
//  生成
// ==========================================================================
void SculptMeshData::BuildPrimitive(SculptPrimitive type, u32 subdivisions, f32 size)
{
    const u32 seg = std::clamp(subdivisions, 1u, 128u);
    const f32 s   = (size > 0.001f) ? size : 1.0f;

    m_positions.clear();
    m_normals.clear();
    m_uvs.clear();
    m_indices.clear();

    switch (type)
    {
    case SculptPrimitive::Sphere:
        AppendSphere(m_positions, m_normals, m_uvs, m_indices,
                     s * 0.5f, (std::max)(3u, seg * 2u), (std::max)(2u, seg));
        break;

    case SculptPrimitive::Plane:
        AppendGrid(m_positions, m_normals, m_uvs, m_indices,
                   XMFLOAT3{0.0f, 0.0f, 0.0f}, XMFLOAT3{1.0f, 0.0f, 0.0f},
                   XMFLOAT3{0.0f, 0.0f, 1.0f}, XMFLOAT3{0.0f, 1.0f, 0.0f}, s, seg);
        break;

    case SculptPrimitive::Cylinder:
        AppendCylinder(m_positions, m_normals, m_uvs, m_indices,
                       s * 0.5f, s, (std::max)(3u, seg * 2u), (std::max)(1u, seg));
        break;

    case SculptPrimitive::Box:
    default:
        for (const BoxFace& f : kBoxFaces)
        {
            const XMFLOAT3 center{ f.n.x * s * 0.5f, f.n.y * s * 0.5f, f.n.z * s * 0.5f };
            AppendGrid(m_positions, m_normals, m_uvs, m_indices,
                       center, f.u, f.v, f.n, s, seg);
        }
        break;
    }

    BuildTopology();
}

bool SculptMeshData::BuildFrom(const std::vector<XMFLOAT3>& positions,
                               const std::vector<XMFLOAT3>& normals,
                               const std::vector<XMFLOAT2>& uvs,
                               const std::vector<u32>& indices)
{
    if (positions.empty() || indices.size() < 3 || (indices.size() % 3) != 0) return false;
    if (positions.size() > kMaxVertices || indices.size() > kMaxIndices) return false;

    const u32 vcount = static_cast<u32>(positions.size());
    for (const u32 i : indices)
        if (i >= vcount) return false;

    m_positions = positions;
    m_indices   = indices;

    const bool hasNormals = (normals.size() == positions.size());
    m_normals = hasNormals ? normals
                           : std::vector<XMFLOAT3>(positions.size(), XMFLOAT3{0.0f, 1.0f, 0.0f});
    m_uvs     = (uvs.size() == positions.size())
                ? uvs
                : std::vector<XMFLOAT2>(positions.size(), XMFLOAT2{0.0f, 0.0f});

    BuildTopology();
    // 元メッシュの法線はそのまま活かす（ハードエッジを勝手に丸めない）。
    // 無い場合だけここで作る。彫った所は ApplyBrush が部分再計算する。
    if (!hasNormals) RecomputeAllNormals();
    return true;
}

// ==========================================================================
//  トポロジ（溶接 + 隣接）
// ==========================================================================
void SculptMeshData::BuildTopology()
{
    const size_t V = m_positions.size();

    m_weldRoot.assign(V, 0u);
    m_roots.clear();
    m_groupStart.assign(V + 1, 0u);
    m_groupList.assign(V, 0u);
    m_adjStart.assign(V + 1, 0u);
    m_adjList.clear();
    m_triStart.assign(V + 1, 0u);
    m_triList.clear();
    m_triStamp.assign(m_indices.size() / 3, 0u);
    m_vertStamp.assign(V, 0u);
    m_stamp = 0;
    if (V == 0) return;

    // ---- ① 溶接: 位置が eps 以内の頂点を 1 グループにまとめる ----
    XMFLOAT3 mn = m_positions[0];
    XMFLOAT3 mx = m_positions[0];
    for (const XMFLOAT3& p : m_positions)
    {
        mn.x = (std::min)(mn.x, p.x); mn.y = (std::min)(mn.y, p.y); mn.z = (std::min)(mn.z, p.z);
        mx.x = (std::max)(mx.x, p.x); mx.y = (std::max)(mx.y, p.y); mx.z = (std::max)(mx.z, p.z);
    }
    const f32 ext = (std::max)((std::max)(mx.x - mn.x, mx.y - mn.y),
                               (std::max)(mx.z - mn.z, 1.0f));
    const f32 eps    = ext * 1e-5f;
    const f32 eps2   = eps * eps;
    const f32 invEps = 1.0f / eps;

    // セル幅 = eps の空間ハッシュ。半径 eps 以内の点は必ず隣接 27 セルのどれかに入る。
    std::unordered_map<u64, std::vector<u32>> grid;
    grid.reserve(V);
    const auto cellOf = [invEps](f32 v) -> i64 {
        return static_cast<i64>(std::floor(v * invEps));
    };
    const auto hashOf = [](i64 cx, i64 cy, i64 cz) -> u64 {
        return static_cast<u64>(cx) * 73856093ull
             ^ static_cast<u64>(cy) * 19349663ull
             ^ static_cast<u64>(cz) * 83492791ull;
    };

    for (size_t v = 0; v < V; ++v)
    {
        const XMFLOAT3& p = m_positions[v];
        const i64 cx = cellOf(p.x);
        const i64 cy = cellOf(p.y);
        const i64 cz = cellOf(p.z);

        bool welded = false;
        for (i64 dz = -1; dz <= 1 && !welded; ++dz)
        {
            for (i64 dy = -1; dy <= 1 && !welded; ++dy)
            {
                for (i64 dx = -1; dx <= 1 && !welded; ++dx)
                {
                    const auto it = grid.find(hashOf(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (const u32 cand : it->second)
                    {
                        const XMFLOAT3& q = m_positions[cand];
                        const f32 ddx = q.x - p.x;
                        const f32 ddy = q.y - p.y;
                        const f32 ddz = q.z - p.z;
                        if (ddx * ddx + ddy * ddy + ddz * ddz <= eps2)
                        {
                            m_weldRoot[v] = cand;
                            welded = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!welded)
        {
            const u32 self = static_cast<u32>(v);
            m_weldRoot[v] = self;
            m_roots.push_back(self);
            grid[hashOf(cx, cy, cz)].push_back(self);
        }
    }

    // ---- ② 溶接グループの CSR（代表頂点 → 同じ位置の生頂点たち）----
    for (size_t v = 0; v < V; ++v)
        ++m_groupStart[static_cast<size_t>(m_weldRoot[v]) + 1];
    for (size_t i = 0; i < V; ++i)
        m_groupStart[i + 1] += m_groupStart[i];
    {
        std::vector<u32> cursor(m_groupStart.begin(),
                                m_groupStart.begin() + static_cast<std::ptrdiff_t>(V));
        for (size_t v = 0; v < V; ++v)
            m_groupList[cursor[m_weldRoot[v]]++] = static_cast<u32>(v);
    }

    const size_t T = m_indices.size() / 3;

    // 三角形 t の 3 頂点の代表番号を取り、同じ代表が重複する分は落とす（退化三角形対策）。
    const auto triRoots = [this](size_t t, u32 out[3]) -> void {
        out[0] = m_weldRoot[m_indices[t * 3 + 0]];
        out[1] = m_weldRoot[m_indices[t * 3 + 1]];
        out[2] = m_weldRoot[m_indices[t * 3 + 2]];
    };
    const auto skipDup = [](const u32 r[3], int k) -> bool {
        if (k == 1) return r[1] == r[0];
        if (k == 2) return (r[2] == r[0]) || (r[2] == r[1]);
        return false;
    };

    // ---- ③ 頂点 → 三角形 ----
    for (size_t t = 0; t < T; ++t)
    {
        u32 r[3];
        triRoots(t, r);
        for (int k = 0; k < 3; ++k)
        {
            if (skipDup(r, k)) continue;
            ++m_triStart[static_cast<size_t>(r[k]) + 1];
        }
    }
    for (size_t i = 0; i < V; ++i)
        m_triStart[i + 1] += m_triStart[i];
    m_triList.assign(static_cast<size_t>(m_triStart[V]), 0u);
    {
        std::vector<u32> cursor(m_triStart.begin(),
                                m_triStart.begin() + static_cast<std::ptrdiff_t>(V));
        for (size_t t = 0; t < T; ++t)
        {
            u32 r[3];
            triRoots(t, r);
            for (int k = 0; k < 3; ++k)
            {
                if (skipDup(r, k)) continue;
                m_triList[cursor[r[k]]++] = static_cast<u32>(t);
            }
        }
    }

    // ---- ④ 頂点 → 頂点（重複ありで作ってから、並べ替え + 重複除去で前へ詰める）----
    for (size_t t = 0; t < T; ++t)
    {
        u32 r[3];
        triRoots(t, r);
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                if (a != b && r[a] != r[b])
                    ++m_adjStart[static_cast<size_t>(r[a]) + 1];
    }
    for (size_t i = 0; i < V; ++i)
        m_adjStart[i + 1] += m_adjStart[i];
    m_adjList.assign(static_cast<size_t>(m_adjStart[V]), 0u);
    {
        std::vector<u32> cursor(m_adjStart.begin(),
                                m_adjStart.begin() + static_cast<std::ptrdiff_t>(V));
        for (size_t t = 0; t < T; ++t)
        {
            u32 r[3];
            triRoots(t, r);
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    if (a != b && r[a] != r[b])
                        m_adjList[cursor[r[a]]++] = r[b];
        }
    }
    {
        // write は「これまでに確定したユニーク数」＝常に今の代表頂点の開始位置以下なので、
        // 前へ詰めても未読の要素を壊さない（read は必ず write 以上の位置から行う）。
        std::vector<u32> newStart(V + 1, 0u);
        u32 write = 0;
        for (size_t r = 0; r < V; ++r)
        {
            newStart[r] = write;
            const u32 b = m_adjStart[r];
            const u32 e = m_adjStart[r + 1];
            if (b >= e) continue;
            std::sort(m_adjList.begin() + static_cast<std::ptrdiff_t>(b),
                      m_adjList.begin() + static_cast<std::ptrdiff_t>(e));
            u32  prev  = 0;
            bool first = true;
            for (u32 i = b; i < e; ++i)
            {
                const u32 val = m_adjList[i];
                if (first || val != prev)
                {
                    m_adjList[write++] = val;
                    prev  = val;
                    first = false;
                }
            }
        }
        newStart[V] = write;
        m_adjList.resize(static_cast<size_t>(write));
        m_adjStart = std::move(newStart);
    }
}

const u32* SculptMeshData::NeighborsOf(u32 root, u32& outCount) const
{
    outCount = 0;
    if (static_cast<size_t>(root) + 1 >= m_adjStart.size()) return nullptr;
    const u32 b = m_adjStart[root];
    const u32 e = m_adjStart[static_cast<size_t>(root) + 1];
    if (e <= b) return nullptr;
    outCount = e - b;
    return m_adjList.data() + b;
}

const u32* SculptMeshData::TrianglesOf(u32 root, u32& outCount) const
{
    outCount = 0;
    if (static_cast<size_t>(root) + 1 >= m_triStart.size()) return nullptr;
    const u32 b = m_triStart[root];
    const u32 e = m_triStart[static_cast<size_t>(root) + 1];
    if (e <= b) return nullptr;
    outCount = e - b;
    return m_triList.data() + b;
}

// ==========================================================================
//  対称ミラー
// ==========================================================================
int SculptMeshData::BuildBrushInstances(const SculptBrushParams& p, const XMFLOAT3& center,
                                        BrushInstance out[kMaxBrushInstances])
{
    int count = 0;
    const int nx = p.mirrorX ? 2 : 1;
    const int ny = p.mirrorY ? 2 : 1;
    const int nz = p.mirrorZ ? 2 : 1;
    for (int ix = 0; ix < nx; ++ix)
    {
        for (int iy = 0; iy < ny; ++iy)
        {
            for (int iz = 0; iz < nz; ++iz)
            {
                const f32 sx = (ix != 0) ? -1.0f : 1.0f;
                const f32 sy = (iy != 0) ? -1.0f : 1.0f;
                const f32 sz = (iz != 0) ? -1.0f : 1.0f;
                BrushInstance& b = out[count];
                b.center    = XMFLOAT3{ center.x * sx, center.y * sy, center.z * sz };
                b.direction = XMFLOAT3{ p.direction.x * sx, p.direction.y * sy, p.direction.z * sz };
                b.grabDelta = XMFLOAT3{ p.grabDelta.x * sx, p.grabDelta.y * sy, p.grabDelta.z * sz };
                ++count;
            }
        }
    }
    return count;
}

// ==========================================================================
//  ブラシ
// ==========================================================================
void SculptMeshData::GatherAffected(const SculptBrushParams& p, const XMFLOAT3& center,
                                    std::vector<u32>& outRoots) const
{
    if (!IsValid() || !(p.radius > 0.0f)) return;

    BrushInstance inst[kMaxBrushInstances];
    const int n = BuildBrushInstances(p, center, inst);
    const f32 r2 = p.radius * p.radius;

    for (int i = 0; i < n; ++i)
    {
        const XMFLOAT3& c = inst[i].center;
        for (const u32 root : m_roots)
        {
            const XMFLOAT3& q = m_positions[root];
            const f32 dx = q.x - c.x;
            const f32 dy = q.y - c.y;
            const f32 dz = q.z - c.z;
            if (dx * dx + dy * dy + dz * dz > r2) continue;
            outRoots.push_back(root);
        }
    }
}

size_t SculptMeshData::ApplyBrush(const SculptBrushParams& p, const XMFLOAT3& center,
                                  f32 dt, bool invert, std::vector<u32>* outTouched)
{
    if (!IsValid() || !(p.radius > 0.0f)) return 0;
    // Grab は grabDelta がそのまま移動量なので dt を要求しない。他は dt>0 が要る。
    if (p.type != SculptBrushType::Grab && !(dt > 0.0f)) return 0;

    BrushInstance inst[kMaxBrushInstances];
    const int n = BuildBrushInstances(p, center, inst);

    m_touched.clear();
    for (int i = 0; i < n; ++i)
        ApplyBrushAt(p, inst[i], dt, invert);

    if (m_touched.empty()) return 0;

    RecomputeNormals(m_touched);
    if (outTouched) outTouched->insert(outTouched->end(), m_touched.begin(), m_touched.end());
    return m_touched.size();
}

void SculptMeshData::ApplyBrushAt(const SculptBrushParams& p, const BrushInstance& bi,
                                  f32 dt, bool invert)
{
    const f32 r2   = p.radius * p.radius;
    const f32 invR = 1.0f / p.radius;

    m_hitRoots.clear();
    m_hitWeights.clear();
    for (const u32 root : m_roots)
    {
        const XMFLOAT3& q = m_positions[root];
        const f32 dx = q.x - bi.center.x;
        const f32 dy = q.y - bi.center.y;
        const f32 dz = q.z - bi.center.z;
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) continue;
        const f32 w = TerrainBrushWeight(std::sqrt(d2) * invR, p.falloff);
        if (!(w > 0.0f)) continue;
        m_hitRoots.push_back(root);
        m_hitWeights.push_back(w);
    }
    if (m_hitRoots.empty()) return;

    const f32 sign = invert ? -1.0f : 1.0f;

    // Flatten は影響範囲の「平均平面」（重み付き重心 + 重み付き平均法線）へ寄せる。
    XMVECTOR planePoint  = XMVectorZero();
    XMVECTOR planeNormal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (p.type == SculptBrushType::Flatten)
    {
        XMVECTOR csum = XMVectorZero();
        XMVECTOR nsum = XMVectorZero();
        f32 wsum = 0.0f;
        for (size_t i = 0; i < m_hitRoots.size(); ++i)
        {
            const f32 w = m_hitWeights[i];
            csum = XMVectorAdd(csum, XMVectorScale(XMLoadFloat3(&m_positions[m_hitRoots[i]]), w));
            nsum = XMVectorAdd(nsum, XMVectorScale(XMLoadFloat3(&m_normals[m_hitRoots[i]]),   w));
            wsum += w;
        }
        if (wsum > 0.0f) planePoint = XMVectorScale(csum, 1.0f / wsum);
        if (XMVectorGetX(XMVector3LengthSq(nsum)) > 1e-12f) planeNormal = XMVector3Normalize(nsum);
    }

    XMVECTOR dir = XMVectorSet(bi.direction.x, bi.direction.y, bi.direction.z, 0.0f);
    if (XMVectorGetX(XMVector3LengthSq(dir)) > 1e-12f) dir = XMVector3Normalize(dir);
    else                                               dir = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR grab = XMVectorSet(bi.grabDelta.x, bi.grabDelta.y, bi.grabDelta.z, 0.0f);
    const XMVECTOR ctr  = XMVectorSet(bi.center.x, bi.center.y, bi.center.z, 0.0f);

    // 走査順に結果が依存しないよう、いったん新しい位置を作ってから書き戻す
    // （Smooth が隣接頂点の「更新前の位置」を読むために必須）。
    m_newPositions.assign(m_hitRoots.size(), XMFLOAT3{0.0f, 0.0f, 0.0f});

    for (size_t i = 0; i < m_hitRoots.size(); ++i)
    {
        const u32 root = m_hitRoots[i];
        const f32 w    = m_hitWeights[i];
        const XMVECTOR cur = XMLoadFloat3(&m_positions[root]);
        XMVECTOR np = cur;

        switch (p.type)
        {
        case SculptBrushType::Draw:
        {
            const XMVECTOR nrm = XMLoadFloat3(&m_normals[root]);
            np = XMVectorAdd(cur, XMVectorScale(nrm, sign * p.strength * w * dt));
            break;
        }
        case SculptBrushType::Pull:
        case SculptBrushType::Push:
        {
            const f32 s = (p.type == SculptBrushType::Push) ? -sign : sign;
            np = XMVectorAdd(cur, XMVectorScale(dir, s * p.strength * w * dt));
            break;
        }
        case SculptBrushType::Smooth:
        {
            u32 cnt = 0;
            const u32* nb = NeighborsOf(root, cnt);
            if (!nb || cnt == 0) break;
            XMVECTOR sum = XMVectorZero();
            for (u32 k = 0; k < cnt; ++k)
                sum = XMVectorAdd(sum, XMLoadFloat3(&m_positions[nb[k]]));
            const XMVECTOR avg = XMVectorScale(sum, 1.0f / static_cast<f32>(cnt));
            const f32 k2 = std::clamp(p.strength * w * dt, 0.0f, 1.0f);
            np = XMVectorAdd(cur, XMVectorScale(XMVectorSubtract(avg, cur), k2));
            break;
        }
        case SculptBrushType::Flatten:
        {
            const XMVECTOR d    = XMVectorSubtract(cur, planePoint);
            const f32      dist = XMVectorGetX(XMVector3Dot(d, planeNormal));
            const XMVECTOR tgt  = XMVectorSubtract(cur, XMVectorScale(planeNormal, dist));
            const f32      k2   = std::clamp(p.strength * w * dt, 0.0f, 1.0f);
            np = XMVectorAdd(cur, XMVectorScale(XMVectorSubtract(tgt, cur), k2));
            break;
        }
        case SculptBrushType::Pinch:
        {
            const f32 k2 = std::clamp(sign * p.strength * w * dt, -1.0f, 1.0f);
            np = XMVectorAdd(cur, XMVectorScale(XMVectorSubtract(ctr, cur), k2));
            break;
        }
        case SculptBrushType::Noise:
        {
            XMFLOAT3 here;
            XMStoreFloat3(&here, cur);
            const f32 nv = SculptFbm3(here, p.noiseFrequency, p.noiseOctaves,
                                      p.noiseRidged, p.noiseSeed);
            const XMVECTOR nrm = XMLoadFloat3(&m_normals[root]);
            np = XMVectorAdd(cur, XMVectorScale(nrm, sign * nv * p.strength * w * dt));
            break;
        }
        case SculptBrushType::Grab:
            np = XMVectorAdd(cur, XMVectorScale(grab, w));
            break;

        default:
            break;
        }

        XMStoreFloat3(&m_newPositions[i], np);
    }

    for (size_t i = 0; i < m_hitRoots.size(); ++i)
    {
        SetRootPosition(m_hitRoots[i], m_newPositions[i]);
        m_touched.push_back(m_hitRoots[i]);
    }
}

void SculptMeshData::SetRootPosition(u32 root, const XMFLOAT3& pos)
{
    if (static_cast<size_t>(root) >= m_positions.size()) return;
    if (static_cast<size_t>(root) + 1 < m_groupStart.size())
    {
        const u32 b = m_groupStart[root];
        const u32 e = m_groupStart[static_cast<size_t>(root) + 1];
        for (u32 gi = b; gi < e; ++gi)
            m_positions[m_groupList[gi]] = pos;
        if (e > b) return;
    }
    m_positions[root] = pos;   // 溶接情報が無い場合の保険
}

XMFLOAT3 SculptMeshData::GetRootPosition(u32 root) const
{
    if (static_cast<size_t>(root) >= m_positions.size()) return XMFLOAT3{0.0f, 0.0f, 0.0f};
    return m_positions[root];
}

// ==========================================================================
//  法線
// ==========================================================================
void SculptMeshData::RecomputeNormals(const std::vector<u32>& roots)
{
    if (m_positions.empty() || m_indices.size() < 3) return;

    const size_t triCount = m_indices.size() / 3;
    if (m_triStamp.size()  != triCount)          m_triStamp.assign(triCount, 0u);
    if (m_vertStamp.size() != m_positions.size()) m_vertStamp.assign(m_positions.size(), 0u);

    // 世代スタンプは実質溢れないが（1 回の呼び出しで 2 進む）、念のため巻き戻す。
    if (m_stamp > 0xFFFFFFF0u)
    {
        std::fill(m_triStamp.begin(),  m_triStamp.end(),  0u);
        std::fill(m_vertStamp.begin(), m_vertStamp.end(), 0u);
        m_stamp = 0u;
    }

    // ① 動いた代表頂点に接する三角形を集める
    ++m_stamp;
    const u32 triStamp = m_stamp;
    m_dirtyTris.clear();
    for (const u32 root : roots)
    {
        u32 cnt = 0;
        const u32* tris = TrianglesOf(root, cnt);
        if (!tris) continue;
        for (u32 k = 0; k < cnt; ++k)
        {
            const u32 t = tris[k];
            if (m_triStamp[t] == triStamp) continue;
            m_triStamp[t] = triStamp;
            m_dirtyTris.push_back(t);
        }
    }
    if (m_dirtyTris.empty()) return;

    // ② その三角形に接する代表頂点（＝法線が変わりうる頂点）を集める
    ++m_stamp;
    const u32 vertStamp = m_stamp;
    m_dirtyRoots.clear();
    for (const u32 t : m_dirtyTris)
    {
        for (size_t k = 0; k < 3; ++k)
        {
            const u32 root = m_weldRoot[m_indices[static_cast<size_t>(t) * 3 + k]];
            if (m_vertStamp[root] == vertStamp) continue;
            m_vertStamp[root] = vertStamp;
            m_dirtyRoots.push_back(root);
        }
    }

    // ③ 各代表頂点で「接する全三角形」の面法線（面積重み）を足して正規化し、
    //    溶接グループ全員へ同じ値を書く
    for (const u32 root : m_dirtyRoots)
    {
        u32 cnt = 0;
        const u32* tris = TrianglesOf(root, cnt);
        if (!tris) continue;

        XMVECTOR acc = XMVectorZero();
        for (u32 k = 0; k < cnt; ++k)
            acc = XMVectorAdd(acc, TriNormalRaw(m_positions, m_indices, tris[k]));

        XMFLOAT3 n{0.0f, 1.0f, 0.0f};
        if (XMVectorGetX(XMVector3LengthSq(acc)) > 1e-20f)
            XMStoreFloat3(&n, XMVector3Normalize(acc));

        const u32 b = m_groupStart[root];
        const u32 e = m_groupStart[static_cast<size_t>(root) + 1];
        for (u32 gi = b; gi < e; ++gi)
            m_normals[m_groupList[gi]] = n;
    }
}

void SculptMeshData::RecomputeAllNormals()
{
    RecomputeNormals(m_roots);
}

void SculptMeshData::Bounds(XMFLOAT3& outMin, XMFLOAT3& outMax) const
{
    outMin = XMFLOAT3{0.0f, 0.0f, 0.0f};
    outMax = XMFLOAT3{0.0f, 0.0f, 0.0f};
    if (m_positions.empty()) return;

    outMin = m_positions[0];
    outMax = m_positions[0];
    for (const XMFLOAT3& p : m_positions)
    {
        outMin.x = (std::min)(outMin.x, p.x);
        outMin.y = (std::min)(outMin.y, p.y);
        outMin.z = (std::min)(outMin.z, p.z);
        outMax.x = (std::max)(outMax.x, p.x);
        outMax.y = (std::max)(outMax.y, p.y);
        outMax.z = (std::max)(outMax.z, p.z);
    }
}

// ==========================================================================
//  .smsh バイナリ
// ==========================================================================
std::vector<u8> SculptMeshData::Encode() const
{
    std::vector<u8> out;
    if (!IsValid()) return out;

    const u32 magic = kMagic;
    const u32 ver   = kVersion;
    const u32 vc    = static_cast<u32>(m_positions.size());
    const u32 ic    = static_cast<u32>(m_indices.size());

    const size_t posBytes = static_cast<size_t>(vc) * sizeof(XMFLOAT3);
    const size_t uvBytes  = static_cast<size_t>(vc) * sizeof(XMFLOAT2);
    const size_t idxBytes = static_cast<size_t>(ic) * sizeof(u32);

    out.resize(kHeaderSize + posBytes * 2 + uvBytes + idxBytes);
    std::memcpy(out.data() +  0, &magic, sizeof(u32));
    std::memcpy(out.data() +  4, &ver,   sizeof(u32));
    std::memcpy(out.data() +  8, &vc,    sizeof(u32));
    std::memcpy(out.data() + 12, &ic,    sizeof(u32));

    size_t off = kHeaderSize;
    std::memcpy(out.data() + off, m_positions.data(), posBytes); off += posBytes;
    std::memcpy(out.data() + off, m_normals.data(),   posBytes); off += posBytes;
    std::memcpy(out.data() + off, m_uvs.data(),       uvBytes);  off += uvBytes;
    std::memcpy(out.data() + off, m_indices.data(),   idxBytes);
    return out;
}

bool SculptMeshData::Decode(const u8* data, size_t size)
{
    if (!data || size < kHeaderSize) return false;

    u32 magic = 0, ver = 0, vc = 0, ic = 0;
    std::memcpy(&magic, data +  0, sizeof(u32));
    std::memcpy(&ver,   data +  4, sizeof(u32));
    std::memcpy(&vc,    data +  8, sizeof(u32));
    std::memcpy(&ic,    data + 12, sizeof(u32));

    if (magic != kMagic || ver != kVersion) return false;
    if (vc == 0 || ic < 3 || (ic % 3u) != 0u) return false;
    if (static_cast<size_t>(vc) > kMaxVertices || static_cast<size_t>(ic) > kMaxIndices) return false;

    const size_t posBytes = static_cast<size_t>(vc) * sizeof(XMFLOAT3);
    const size_t uvBytes  = static_cast<size_t>(vc) * sizeof(XMFLOAT2);
    const size_t idxBytes = static_cast<size_t>(ic) * sizeof(u32);
    if (size < kHeaderSize + posBytes * 2 + uvBytes + idxBytes) return false;

    std::vector<XMFLOAT3> pos(vc);
    std::vector<XMFLOAT3> nrm(vc);
    std::vector<XMFLOAT2> uv(vc);
    std::vector<u32>      idx(ic);

    size_t off = kHeaderSize;
    std::memcpy(pos.data(), data + off, posBytes); off += posBytes;
    std::memcpy(nrm.data(), data + off, posBytes); off += posBytes;
    std::memcpy(uv.data(),  data + off, uvBytes);  off += uvBytes;
    std::memcpy(idx.data(), data + off, idxBytes);

    // 壊れたファイルで域外アクセスしないよう、インデックスの範囲は必ず確かめる。
    for (const u32 i : idx)
        if (i >= vc) return false;

    m_positions = std::move(pos);
    m_normals   = std::move(nrm);
    m_uvs       = std::move(uv);
    m_indices   = std::move(idx);
    BuildTopology();
    return true;
}

} // namespace dx12e
