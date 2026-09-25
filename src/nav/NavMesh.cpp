// ============================================================================
// ナビメッシュ 段階 8: ランタイム表現の構築 / シリアライズ
// ============================================================================
// ポリゴンは「経路の位相」を、高さサンプル格子は「坂道の正確な高さ」と
// 「位置 → ポリゴンの空間索引」を担当する。detail mesh を持たない代わりに
// ボクセル分解能そのままの高さが引けるので、坂の上でも足が浮かない。
// ============================================================================

#include "nav/NavTypes.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dx12e
{
namespace nav
{

namespace
{
constexpr u32 kNavMagic   = 0x564E4B44;   // 'DKNV'
constexpr u32 kNavVersion = 1;

// 点がボクセル座標の凸ポリゴン内にあるか（xz 平面）
bool PointInPolyVox(f32 px, f32 pz, const u16* verts, const u32* idx, u32 nv)
{
    bool inside = false;
    for (u32 i = 0, j = nv - 1; i < nv; j = i++)
    {
        const f32 xi = static_cast<f32>(verts[idx[i] * 3 + 0]);
        const f32 zi = static_cast<f32>(verts[idx[i] * 3 + 2]);
        const f32 xj = static_cast<f32>(verts[idx[j] * 3 + 0]);
        const f32 zj = static_cast<f32>(verts[idx[j] * 3 + 2]);
        if (((zi > pz) != (zj > pz)) &&
            (px < (xj - xi) * (pz - zi) / (zj - zi + 1e-20f) + xi))
            inside = !inside;
    }
    return inside;
}
} // namespace

void NavMesh::Clear()
{
    m_polys.clear(); m_verts.clear(); m_polyVerts.clear(); m_neis.clear();
    m_grid.clear(); m_samples.clear();
    m_bucketStart.clear(); m_bucketPolys.clear(); m_polyBounds.clear();
    m_bw = m_bh = 0;
    m_gw = m_gh = 0;
    for (i32 k = 0; k < 3; ++k) { m_contentMin[k] = m_contentMax[k] = 0.0f; }
    m_stats = Stats{};
    BumpGeneration();
}

// 世代はプロセス全体で一意（別シーンのメッシュの ref を渡されても、世代が違うので弾ける）
void NavMesh::BumpGeneration()
{
    static std::atomic<u32> s_gen{ 0 };
    u32 g = ++s_gen;
    if (g == 0) g = ++s_gen;   // 0 は「一度も作っていない」に取っておく
    m_generation = g;
}

NavPolyRef NavMesh::MakeRef(i32 poly) const
{
    if (!IsValidPoly(poly)) return kNullPolyRef;
    return (static_cast<NavPolyRef>(m_generation) << 32) | static_cast<NavPolyRef>(static_cast<u32>(poly) + 1u);
}

i32 NavMesh::DecodeRef(NavPolyRef ref) const
{
    if (ref == kNullPolyRef) return -1;
    if (static_cast<u32>(ref >> 32) != m_generation) return -1;
    const u32 lo = static_cast<u32>(ref & 0xffffffffull);
    if (lo == 0) return -1;
    const i32 poly = static_cast<i32>(lo - 1u);
    return IsValidPoly(poly) ? poly : -1;
}

void NavMesh::BuildPolyIndex()
{
    m_bucketStart.clear(); m_bucketPolys.clear(); m_polyBounds.clear();
    m_bw = m_bh = 0;
    if (m_polys.empty()) return;

    // ---- ポリゴンごとの AABB ----
    m_polyBounds.resize(m_polys.size() * 6);
    f32 xmin = FLT_MAX, zmin = FLT_MAX, xmax = -FLT_MAX, zmax = -FLT_MAX;
    for (size_t p = 0; p < m_polys.size(); ++p)
    {
        const NavPoly& poly = m_polys[p];
        f32* b = &m_polyBounds[p * 6];
        b[0] = b[1] = b[2] = FLT_MAX;
        b[3] = b[4] = b[5] = -FLT_MAX;
        for (u32 i = 0; i < poly.vertCount; ++i)
        {
            const u32 vi = m_polyVerts[poly.firstVert + i];
            for (i32 k = 0; k < 3; ++k)
            {
                b[k]     = (std::min)(b[k], m_verts[vi * 3 + k]);
                b[k + 3] = (std::max)(b[k + 3], m_verts[vi * 3 + k]);
            }
        }
        xmin = (std::min)(xmin, b[0]); zmin = (std::min)(zmin, b[2]);
        xmax = (std::max)(xmax, b[3]); zmax = (std::max)(zmax, b[5]);
    }

    // ---- 粗いバケット（一辺 128 前後に収める）----
    const f32 span = (std::max)(xmax - xmin, zmax - zmin);
    m_bucketSize = (std::max)(1.0f, span / 128.0f);
    m_bucketOrigin[0] = xmin;
    m_bucketOrigin[1] = zmin;
    m_bw = (std::max)(1, static_cast<i32>(std::floor((xmax - xmin) / m_bucketSize)) + 1);
    m_bh = (std::max)(1, static_cast<i32>(std::floor((zmax - zmin) / m_bucketSize)) + 1);

    auto range = [&](const f32* b, i32& x0, i32& z0, i32& x1, i32& z1)
    {
        x0 = std::clamp(static_cast<i32>(std::floor((b[0] - xmin) / m_bucketSize)), 0, m_bw - 1);
        z0 = std::clamp(static_cast<i32>(std::floor((b[2] - zmin) / m_bucketSize)), 0, m_bh - 1);
        x1 = std::clamp(static_cast<i32>(std::floor((b[3] - xmin) / m_bucketSize)), 0, m_bw - 1);
        z1 = std::clamp(static_cast<i32>(std::floor((b[5] - zmin) / m_bucketSize)), 0, m_bh - 1);
    };

    // 2 パス（数える → 詰める）。ポリゴン番号の昇順で入る＝候補の並びが決定論的
    std::vector<u32> counts(static_cast<size_t>(m_bw) * m_bh + 1, 0);
    for (size_t p = 0; p < m_polys.size(); ++p)
    {
        i32 x0, z0, x1, z1;
        range(&m_polyBounds[p * 6], x0, z0, x1, z1);
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x) ++counts[static_cast<size_t>(x + z * m_bw)];
    }
    m_bucketStart.assign(counts.size(), 0);
    u32 acc = 0;
    for (size_t i = 0; i + 1 < counts.size(); ++i) { m_bucketStart[i] = acc; acc += counts[i]; }
    m_bucketStart.back() = acc;
    m_bucketPolys.assign(acc, 0);
    std::vector<u32> fill(m_bucketStart.begin(), m_bucketStart.end() - 1);
    for (size_t p = 0; p < m_polys.size(); ++p)
    {
        i32 x0, z0, x1, z1;
        range(&m_polyBounds[p * 6], x0, z0, x1, z1);
        for (i32 z = z0; z <= z1; ++z)
            for (i32 x = x0; x <= x1; ++x)
                m_bucketPolys[fill[static_cast<size_t>(x + z * m_bw)]++] = static_cast<u32>(p);
    }
}

void NavMesh::GetBounds(f32 outMin[3], f32 outMax[3]) const
{
    for (i32 k = 0; k < 3; ++k) { outMin[k] = m_contentMin[k]; outMax[k] = m_contentMax[k]; }
}

void NavMesh::GetPolyVert(const NavPoly& p, u32 i, f32 out[3]) const
{
    const u32 vi = m_polyVerts[p.firstVert + i];
    out[0] = m_verts[vi * 3 + 0];
    out[1] = m_verts[vi * 3 + 1];
    out[2] = m_verts[vi * 3 + 2];
}

i32 NavMesh::CellIndex(f32 x, f32 z, i32& cx, i32& cz) const
{
    cx = static_cast<i32>(std::floor((x - m_bmin[0]) / m_cs));
    cz = static_cast<i32>(std::floor((z - m_bmin[2]) / m_cs));
    if (cx < 0 || cz < 0 || cx >= m_gw || cz >= m_gh) return -1;
    return cx + cz * m_gw;
}

void NavMesh::ComputeStats()
{
    // 実際にポリゴンが載っている範囲。格子の bmax[1] は 0xffff*ch のセンチネルで
    // 数千メートルになるため、そのまま出すと「範囲がおかしい」と誤解される。
    if (m_verts.empty())
    {
        for (i32 k = 0; k < 3; ++k) { m_contentMin[k] = m_contentMax[k] = 0.0f; }
    }
    else
    {
        for (i32 k = 0; k < 3; ++k) { m_contentMin[k] = m_contentMax[k] = m_verts[k]; }
        for (size_t i = 3; i + 2 < m_verts.size(); i += 3)
            for (i32 k = 0; k < 3; ++k)
            {
                m_contentMin[k] = (std::min)(m_contentMin[k], m_verts[i + k]);
                m_contentMax[k] = (std::max)(m_contentMax[k], m_verts[i + k]);
            }
    }

    m_stats.polyCount   = static_cast<i32>(m_polys.size());
    m_stats.vertCount   = static_cast<i32>(m_verts.size() / 3);
    m_stats.sampleCount = static_cast<i32>(m_samples.size());
    m_stats.gridW = m_gw; m_stats.gridH = m_gh;
    // 歩行面積 = サンプル数 × セル面積（重なりも別々に数える＝多層でも正しい）
    m_stats.walkableArea = static_cast<f32>(m_samples.size()) * m_cs * m_cs;
    m_stats.memoryBytes =
        m_polys.size() * sizeof(NavPoly) + m_verts.size() * sizeof(f32) +
        m_polyVerts.size() * sizeof(u32) + m_neis.size() * sizeof(u32) +
        m_grid.size() * sizeof(NavGridCell) + m_samples.size() * sizeof(NavHeightSample);
}

void NavMesh::BuildFromPolyMesh(const NavPolyMeshRaw& pm, const NavCompactHeightfield& chf,
                                const NavBuildConfig& cfg)
{
    Clear();
    m_cfg = cfg;
    m_cs = pm.cs; m_ch = pm.ch;
    for (i32 k = 0; k < 3; ++k) { m_bmin[k] = pm.bmin[k]; m_bmax[k] = pm.bmax[k]; }

    // ---- 頂点（ボクセル → ワールド）----
    m_verts.resize(static_cast<size_t>(pm.nverts) * 3);
    for (i32 i = 0; i < pm.nverts; ++i)
    {
        m_verts[static_cast<size_t>(i) * 3 + 0] = pm.bmin[0] + static_cast<f32>(pm.verts[static_cast<size_t>(i) * 3 + 0]) * pm.cs;
        m_verts[static_cast<size_t>(i) * 3 + 1] = pm.bmin[1] + static_cast<f32>(pm.verts[static_cast<size_t>(i) * 3 + 1]) * pm.ch;
        m_verts[static_cast<size_t>(i) * 3 + 2] = pm.bmin[2] + static_cast<f32>(pm.verts[static_cast<size_t>(i) * 3 + 2]) * pm.cs;
    }

    // ---- ポリゴン ----
    m_polys.reserve(static_cast<size_t>(pm.npolys));
    m_polyVerts.reserve(static_cast<size_t>(pm.npolys) * 4);
    m_neis.reserve(static_cast<size_t>(pm.npolys) * 4);

    std::vector<i32> polyRemap(static_cast<size_t>(pm.npolys), -1);
    for (i32 p = 0; p < pm.npolys; ++p)
    {
        const u16* src = &pm.polys[static_cast<size_t>(p) * pm.nvp * 2];
        u32 nv = 0;
        while (nv < static_cast<u32>(pm.nvp) && src[nv] != kMeshNullIdx) ++nv;
        if (nv < 3) continue;

        NavPoly poly;
        poly.firstVert = static_cast<u32>(m_polyVerts.size());
        poly.vertCount = nv;
        poly.firstNei  = static_cast<u32>(m_neis.size());
        poly.area      = pm.areas[static_cast<size_t>(p)];

        f32 cx = 0.0f, cy = 0.0f, cz = 0.0f;
        for (u32 k = 0; k < nv; ++k)
        {
            const u32 vi = src[k];
            m_polyVerts.push_back(vi);
            cx += m_verts[vi * 3 + 0];
            cy += m_verts[vi * 3 + 1];
            cz += m_verts[vi * 3 + 2];
        }
        poly.center[0] = cx / static_cast<f32>(nv);
        poly.center[1] = cy / static_cast<f32>(nv);
        poly.center[2] = cz / static_cast<f32>(nv);

        for (u32 k = 0; k < nv; ++k)
        {
            const u16 n = src[pm.nvp + k];
            m_neis.push_back(n == kMeshNullIdx ? 0xffffffffu : static_cast<u32>(n));
        }

        polyRemap[static_cast<size_t>(p)] = static_cast<i32>(m_polys.size());
        m_polys.push_back(poly);
    }

    // 隣接インデックスを詰め直し（3 頂点未満で落としたポリゴンぶんずれるため）
    for (u32& n : m_neis)
    {
        if (n == 0xffffffffu) continue;
        const i32 r = (n < polyRemap.size()) ? polyRemap[n] : -1;
        n = (r < 0) ? 0xffffffffu : static_cast<u32>(r);
    }

    // ---- 高さサンプル格子 ----
    // 領域 → ポリゴン一覧を作り、各歩行 span をセル中心の内外判定でポリゴンへ割り当てる。
    m_gw = chf.w; m_gh = chf.h;
    m_grid.assign(static_cast<size_t>(m_gw) * m_gh, NavGridCell{});

    u16 maxReg = 0;
    for (i32 p = 0; p < pm.npolys; ++p) maxReg = (std::max)(maxReg, pm.regs[static_cast<size_t>(p)]);
    std::vector<std::vector<u32>> regionPolys(static_cast<size_t>(maxReg) + 1);
    for (i32 p = 0; p < pm.npolys; ++p)
    {
        if (polyRemap[static_cast<size_t>(p)] < 0) continue;
        regionPolys[pm.regs[static_cast<size_t>(p)]].push_back(static_cast<u32>(polyRemap[static_cast<size_t>(p)]));
    }

    std::vector<NavHeightSample> samples;
    samples.reserve(static_cast<size_t>(chf.spanCount));
    for (i32 z = 0; z < chf.h; ++z)
        for (i32 x = 0; x < chf.w; ++x)
        {
            const NavCompactCell& c = chf.cells[static_cast<size_t>(x) + static_cast<size_t>(z) * chf.w];
            NavGridCell& gc = m_grid[static_cast<size_t>(x) + static_cast<size_t>(z) * m_gw];
            gc.first = static_cast<i32>(samples.size());
            gc.count = 0;
            for (u32 i = c.index, ni = c.index + c.count; i < ni; ++i)
            {
                if (chf.areas[i] == kNullArea) continue;
                const u16 reg = chf.spans[i].reg;
                if (!reg || (reg & kBorderRegion) || reg >= regionPolys.size()) continue;

                u16 found = kMeshNullIdx;
                const f32 px = static_cast<f32>(x) + 0.5f;
                const f32 pz = static_cast<f32>(z) + 0.5f;
                for (u32 pi : regionPolys[reg])
                {
                    const NavPoly& poly = m_polys[pi];
                    if (PointInPolyVox(px, pz, pm.verts.data(),
                                       &m_polyVerts[poly.firstVert], poly.vertCount))
                    { found = static_cast<u16>(pi); break; }
                }
                // 中心が境界の外に出るセル（ポリゴンの縁）は、その領域の先頭ポリゴンで代用する
                if (found == kMeshNullIdx && !regionPolys[reg].empty())
                    found = static_cast<u16>(regionPolys[reg][0]);
                if (found == kMeshNullIdx) continue;

                samples.push_back({ chf.spans[i].y, found });
                ++gc.count;
            }
            if (gc.count == 0) gc.first = -1;
        }
    m_samples.swap(samples);

    ComputeStats();
    BuildPolyIndex();
}

// ---------------------------------------------------------------------------
// シリアライズ（単純なバイナリ。エディタで焼いてランタイムで読む）
// ---------------------------------------------------------------------------
namespace
{
template <typename T>
void WriteVec(std::vector<u8>& out, const std::vector<T>& v)
{
    const u32 n = static_cast<u32>(v.size());
    const u8* p = reinterpret_cast<const u8*>(&n);
    out.insert(out.end(), p, p + sizeof(u32));
    if (n == 0) return;
    const u8* d = reinterpret_cast<const u8*>(v.data());
    out.insert(out.end(), d, d + sizeof(T) * n);
}

template <typename T>
bool ReadVec(const u8*& p, const u8* end, std::vector<T>& v)
{
    if (p + sizeof(u32) > end) return false;
    u32 n = 0; std::memcpy(&n, p, sizeof(u32)); p += sizeof(u32);
    if (p + sizeof(T) * static_cast<size_t>(n) > end) return false;
    v.resize(n);
    if (n) std::memcpy(v.data(), p, sizeof(T) * static_cast<size_t>(n));
    p += sizeof(T) * static_cast<size_t>(n);
    return true;
}

template <typename T>
void WritePod(std::vector<u8>& out, const T& v)
{
    const u8* p = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool ReadPod(const u8*& p, const u8* end, T& v)
{
    if (p + sizeof(T) > end) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}
} // namespace

bool NavMesh::Save(const std::string& absPath, std::string& err) const
{
    std::vector<u8> buf;
    buf.reserve(1 << 16);
    WritePod(buf, kNavMagic);
    WritePod(buf, kNavVersion);
    WritePod(buf, m_cfg);
    WritePod(buf, m_bmin[0]); WritePod(buf, m_bmin[1]); WritePod(buf, m_bmin[2]);
    WritePod(buf, m_bmax[0]); WritePod(buf, m_bmax[1]); WritePod(buf, m_bmax[2]);
    WritePod(buf, m_cs); WritePod(buf, m_ch);
    WritePod(buf, m_gw); WritePod(buf, m_gh);
    WriteVec(buf, m_polys);
    WriteVec(buf, m_verts);
    WriteVec(buf, m_polyVerts);
    WriteVec(buf, m_neis);
    WriteVec(buf, m_grid);
    WriteVec(buf, m_samples);

    FILE* f = nullptr;
    if (fopen_s(&f, absPath.c_str(), "wb") != 0 || !f)
    {
        err = "ナビメッシュの書き出しに失敗: " + absPath;
        return false;
    }
    const size_t wrote = std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (wrote != buf.size()) { err = "ナビメッシュの書き込みが途中で切れた"; return false; }
    return true;
}

bool NavMesh::LoadFromMemory(const u8* data, size_t size, std::string& err)
{
    Clear();
    const u8* p = data;
    const u8* end = data + size;

    u32 magic = 0, version = 0;
    if (!ReadPod(p, end, magic) || magic != kNavMagic) { err = "ナビメッシュのフォーマットが違う"; return false; }
    if (!ReadPod(p, end, version) || version != kNavVersion)
    { err = "ナビメッシュのバージョンが違う（作り直しが必要）"; return false; }
    if (!ReadPod(p, end, m_cfg)) { err = "設定の読み込みに失敗"; return false; }
    for (i32 k = 0; k < 3; ++k) if (!ReadPod(p, end, m_bmin[k])) { err = "bmin"; return false; }
    for (i32 k = 0; k < 3; ++k) if (!ReadPod(p, end, m_bmax[k])) { err = "bmax"; return false; }
    if (!ReadPod(p, end, m_cs) || !ReadPod(p, end, m_ch)) { err = "cs/ch"; return false; }
    if (!ReadPod(p, end, m_gw) || !ReadPod(p, end, m_gh)) { err = "grid size"; return false; }
    if (!ReadVec(p, end, m_polys))     { err = "polys";     return false; }
    if (!ReadVec(p, end, m_verts))     { err = "verts";     return false; }
    if (!ReadVec(p, end, m_polyVerts)) { err = "polyVerts"; return false; }
    if (!ReadVec(p, end, m_neis))      { err = "neis";      return false; }
    if (!ReadVec(p, end, m_grid))      { err = "grid";      return false; }
    if (!ReadVec(p, end, m_samples))   { err = "samples";   return false; }

    if (m_grid.size() != static_cast<size_t>(m_gw) * static_cast<size_t>(m_gh))
    { err = "格子サイズが不整合"; Clear(); return false; }

    ComputeStats();
    BuildPolyIndex();
    return true;
}

bool NavMesh::Load(const std::string& absPath, std::string& err)
{
    FILE* f = nullptr;
    if (fopen_s(&f, absPath.c_str(), "rb") != 0 || !f)
    {
        err = "ナビメッシュが開けない: " + absPath;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); err = "ナビメッシュが空"; return false; }
    std::vector<u8> buf(static_cast<size_t>(sz));
    const size_t rd = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (rd != buf.size()) { err = "ナビメッシュの読み込みが途中で切れた"; return false; }
    return LoadFromMemory(buf.data(), buf.size(), err);
}

} // namespace nav
} // namespace dx12e
