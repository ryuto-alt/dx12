#pragma once
// ===========================================================================
// GPU 駆動のインスタンス群（設計: docs/SCENE_FORMAT_DESIGN.md §4.2 = 4-3）の CPU 側の純ロジックと参照実装。
// ---------------------------------------------------------------------------
//   ・GPU に依存しない（ctest から直接叩ける）。shaders/instancegpu/InstanceCull.hlsl の式と一対一。片方だけ直すとずれる
//     （tests/instance_gpu_test.cpp が GPU = CPU を見張る）。
//   ・インスタンスの中心 / 半径 / LOD の式は Application::BuildDrawList（emitItem）と同じ。
//     = 「GPU 経路 ON/OFF で同じ絵」を保つための契約。emitItem の式を変えたらここも変えること。
//   ・チャンクは「インスタンス番号の連続 128 個」。Morton 並べ替えはしない: 出力の並びを CPU 経路（guid → instanceIndex 昇順）と
//     同じにして、深度が同点の断片の勝者（描画順）まで ON/OFF で一致させるため。
// ===========================================================================
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/Types.h"
#include "renderer/Frustum.h"

namespace dx12e::instgpu
{

inline constexpr u32 kChunkSize   = 128;   // 1 チャンク = 1 スレッドグループ
inline constexpr u32 kLists       = 5;     // LOD 0..4（Mesh::kMaxLods と同じ）
inline constexpr u32 kInvalidList = 0xFFFFFFFFu;

// 永続バッファのインスタンス 1 個（64B）。r0..r2 = XMMatrixTranspose(world) の先頭 3 行（MeshInstanceData と同じ並び）。
// 中心 / 半径は BuildDrawList の DrawItem::center / radius と同じ値（視錐台と LOD に使う保守的な球）。
struct InstanceRecord
{
    f32 r0[4];
    f32 r1[4];
    f32 r2[4];
    f32 center[3];
    f32 radius;
};
static_assert(sizeof(InstanceRecord) == 64, "InstanceCull.hlsl の record と一致させること");

// チャンク（32B）: 全インスタンスの球（中心 ± 半径）を包む AABB と、インスタンス番号の範囲。
struct ChunkRecord
{
    f32 mn[3];
    u32 first;
    f32 mx[3];
    u32 count;
};
static_assert(sizeof(ChunkRecord) == 32, "InstanceCull.hlsl の chunk と一致させること");

// 前フレームのワールド（速度用。MeshInstancePrevData と同じ並び: transpose(prevWorld) の先頭 3 行 = 48B）。
struct PrevRecord
{
    f32 p0[4];
    f32 p1[4];
    f32 p2[4];
};
static_assert(sizeof(PrevRecord) == 48, "MeshInstancePrevData と一致させること");

// モデルのローカル境界（全サブメッシュの AABB の合成）。BuildDrawList の lmn / lmx。
struct LocalBounds
{
    DirectX::XMFLOAT3 mn{0, 0, 0};
    DirectX::XMFLOAT3 mx{0, 0, 0};
    bool valid = false;   // false = AABB を持つメッシュが 1 つも無い（BuildDrawList の hasAabb = false。球は center=world 原点 / radius 1）
};

// world（行ベクトル規約）と境界から record を作る。BuildDrawList::emitItem の静的メッシュ（スキン / ノードアニメ無し）と同じ式:
//   ms     = ワールド行列の行ベクトル長の最大
//   radius = max(1, 0.5 * |lmx - lmn|) * ms * 1.25
//   center = (lmn + lmx) / 2 をワールドへ移した点
inline InstanceRecord MakeRecord(const DirectX::XMFLOAT4X4& worldF, const LocalBounds& b)
{
    using namespace DirectX;
    const XMMATRIX world = XMLoadFloat4x4(&worldF);
    const f32 ms = (std::max)((std::max)(XMVectorGetX(XMVector3Length(world.r[0])), XMVectorGetX(XMVector3Length(world.r[1]))),
                              XMVectorGetX(XMVector3Length(world.r[2])));
    f32 meshRadius = 1.0f;
    XMVECTOR localCenter = XMVectorZero();
    if (b.valid)
    {
        const XMVECTOR lmn = XMLoadFloat3(&b.mn), lmx = XMLoadFloat3(&b.mx);
        localCenter = XMVectorScale(XMVectorAdd(lmn, lmx), 0.5f);
        meshRadius = (std::max)(1.0f, 0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(lmx, lmn))));
    }
    XMFLOAT3 c;
    XMStoreFloat3(&c, XMVector3Transform(localCenter, world));
    InstanceRecord r{};
    const XMMATRIX t = XMMatrixTranspose(world);
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(r.r0), t.r[0]);
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(r.r1), t.r[1]);
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(r.r2), t.r[2]);
    r.center[0] = c.x; r.center[1] = c.y; r.center[2] = c.z;
    r.radius = meshRadius * ms * 1.25f;
    return r;
}

inline PrevRecord MakePrevRecord(const DirectX::XMFLOAT4X4& prevWorldF)
{
    using namespace DirectX;
    const XMMATRIX t = XMMatrixTranspose(XMLoadFloat4x4(&prevWorldF));
    PrevRecord p{};
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(p.p0), t.r[0]);
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(p.p1), t.r[1]);
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(p.p2), t.r[2]);
    return p;
}

// 連続 kChunkSize 個ごとのチャンク表。球の AABB は中心 ± 半径（球を AABB で包む＝チャンクが落ちるなら中身の球も全部落ちる）。
inline std::vector<ChunkRecord> BuildChunks(const InstanceRecord* rec, u32 count)
{
    std::vector<ChunkRecord> out;
    out.reserve((count + kChunkSize - 1) / kChunkSize);
    for (u32 first = 0; first < count; first += kChunkSize)
    {
        const u32 n = (std::min)(kChunkSize, count - first);
        ChunkRecord c{};
        c.first = first;
        c.count = n;
        for (int k = 0; k < 3; ++k) { c.mn[k] = 3.0e38f; c.mx[k] = -3.0e38f; }
        for (u32 i = 0; i < n; ++i)
        {
            const InstanceRecord& r = rec[first + i];
            for (int k = 0; k < 3; ++k)
            {
                c.mn[k] = (std::min)(c.mn[k], r.center[k] - r.radius);
                c.mx[k] = (std::max)(c.mx[k], r.center[k] + r.radius);
            }
        }
        out.push_back(c);
    }
    return out;
}

// 1 ビュー分のカリング・LOD の入力。
struct ViewParams
{
    f32 planes[6][4] = {};   // Frustum::FromViewProj の 6 平面（a,b,c,d。内側 a*x+b*y+c*z+d >= 0）
    f32 camPos[3] = {0, 0, 0};   // LOD 選択の基準（メインカメラ位置。全ビュー共通）
    u32 lodBias = 0;             // 影パスは 1
    f32 texelWorld = 0.0f;       // 影カスケードの 1 テクセルが何 m か（0 = 無効）
    u32 maxList = kLists - 1;    // 描く最大 LOD（サブメッシュの最大 LOD 数 - 1。これを超える LOD は丸める）
    f32 lodScale = 1.0f;         // BuildDrawList の kLodScale
};

inline void PlanesFromViewProj(const DirectX::XMFLOAT4X4& viewProj, f32 out[6][4])
{
    using namespace DirectX;
    const Frustum f = Frustum::FromViewProj(XMLoadFloat4x4(&viewProj));
    for (int i = 0; i < 6; ++i)
    {
        XMFLOAT4 p;
        XMStoreFloat4(&p, f.planes[i]);
        out[i][0] = p.x; out[i][1] = p.y; out[i][2] = p.z; out[i][3] = p.w;
    }
}

inline bool SphereInPlanes(const f32 planes[6][4], const f32 c[3], f32 r)
{
    for (int i = 0; i < 6; ++i)
        if (planes[i][0] * c[0] + planes[i][1] * c[1] + planes[i][2] * c[2] + planes[i][3] < -r) return false;
    return true;
}

// チャンクの AABB が視錐台と交差しうるか（AABB は球の外接なので、false なら中身の球は全部 SphereInPlanes で落ちる）。
inline bool ChunkInPlanes(const f32 planes[6][4], const ChunkRecord& ch)
{
    for (int i = 0; i < 6; ++i)
    {
        const f32 px = planes[i][0] >= 0.0f ? ch.mx[0] : ch.mn[0];
        const f32 py = planes[i][1] >= 0.0f ? ch.mx[1] : ch.mn[1];
        const f32 pz = planes[i][2] >= 0.0f ? ch.mx[2] : ch.mn[2];
        if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0.0f) return false;
    }
    return true;
}

// 見かけの大きさによる LOD（BuildDrawList の閾値式）。dist = カメラから球の中心までの距離。
inline u32 NominalLod(f32 radius, f32 dist, f32 lodScale)
{
    const f32 apparent = radius / (std::max)(dist, 1e-3f) * lodScale;
    return (apparent < 0.008f) ? 4u : (apparent < 0.020f) ? 3u : (apparent < 0.055f) ? 2u : (apparent < 0.125f) ? 1u : 0u;
}

// このビューでの出力先 list（LOD）。kInvalidList = 描かない。RenderDepthOnlyScene の passLod と同じ式（+ maxList への丸め）。
inline u32 SelectList(const ViewParams& v, const InstanceRecord& r)
{
    if (!SphereInPlanes(v.planes, r.center, r.radius)) return kInvalidList;
    const f32 dx = r.center[0] - v.camPos[0], dy = r.center[1] - v.camPos[1], dz = r.center[2] - v.camPos[2];
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    u32 lod = NominalLod(r.radius, dist, v.lodScale) + v.lodBias;
    if (v.texelWorld > 0.0f)
    {
        const f32 texels = 2.0f * r.radius / v.texelWorld;
        const u32 tl = (texels < 32.0f) ? 4u : (texels < 96.0f) ? 3u : (texels < 288.0f) ? 2u : (texels < 864.0f) ? 1u : 0u;
        lod = (std::max)(lod, tl);
    }
    return (std::min)(lod, v.maxList);
}

// CPU 参照のカリング結果: list ごとの「可視インスタンス番号」。並び = チャンク順 → チャンク内の番号順（= 番号の昇順）。
struct CullResult
{
    std::vector<u32> lists[kLists];
    u32 chunksVisible = 0;
    u32 Total() const { u32 n = 0; for (const auto& l : lists) n += static_cast<u32>(l.size()); return n; }
};

inline CullResult CullReference(const InstanceRecord* rec, const std::vector<ChunkRecord>& chunks, const ViewParams& v)
{
    CullResult out;
    for (const ChunkRecord& ch : chunks)
    {
        if (!ChunkInPlanes(v.planes, ch)) continue;
        ++out.chunksVisible;
        for (u32 i = 0; i < ch.count; ++i)
        {
            const u32 idx = ch.first + i;
            const u32 l = SelectList(v, rec[idx]);
            if (l != kInvalidList) out.lists[l].push_back(idx);
        }
    }
    return out;
}

} // namespace dx12e::instgpu
