#include "renderer/RaytracingScene.h"

#include "graphics/GraphicsDevice.h"
#include "renderer/Mesh.h"
#include "renderer/SkinningCompute.h"   // kOutStride（変形後頂点のストライド）
#include "core/Logger.h"

#include <algorithm>
#include <cstring>

namespace dx12e
{
using namespace DirectX;
using Microsoft::WRL::ComPtr;

bool RaytracingScene::Initialize(GraphicsDevice& device)
{
    Shutdown();

    // Gate 1/2 は GraphicsDevice が集約している（Tier 1.1 かつ SM 6.5）。
    if (!device.SupportsInlineRaytracing()) return false;
    // Gate 3: ID3D12Device5。このリポジトリは最初から Device5 を作っているので実質常に成功するが、
    //         将来デバイス生成を触った人が黙って壊さないように明示的に見る。
    if (device.GetDevice() == nullptr) return false;

    // Gate 6: 加速構造バッファが確保できるか。
    // ★D3D12MA が初期状態 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE を
    //   取れるかは計画09 §12 の未確認項目 7。ここで実物を 1 本作って確認する。
    {
        RtBuffer probe;
        if (!probe.Initialize(device, kAsAlignment, RtBuffer::Kind::AccelStruct, L"RT AS probe"))
        {
            Logger::Warn("加速構造バッファを確保できませんでした。レイトレーシングは無効のままにします");
            return false;
        }
        probe.Release();
    }

    m_initialized = true;
    m_pending.reserve(4096);
    Logger::Info("RaytracingScene initialized (BLAS=LOD0固定 / TLAS=毎フレーム再構築 / 上限 {} インスタンス)",
                 kMaxRtInstances);
    return true;
}

void RaytracingScene::Shutdown()
{
    m_blas.clear();
    m_skinnedBlas.clear();
    m_blasScratch.Release();
    m_tlas.Release();
    m_tlasScratch.Release();
    for (auto& b : m_instanceDescs) b.Release();
    m_pending.clear();
    m_pending.shrink_to_fit();
    m_tlasValid   = false;
    m_initialized = false;
    m_stats = Stats{};
}

void RaytracingScene::Invalidate()
{
    m_blas.clear();
    m_skinnedBlas.clear();   // エンティティ id が再利用されるので必ず捨てる（N30 と同じ理由）
    m_tlasValid = false;
    m_stats.blasBytes        = 0;
    m_stats.blasTriangles    = 0;
    m_stats.blasCount        = 0;
    m_stats.skinnedBlasBytes = 0;
    m_stats.skinnedTriangles = 0;
}

void RaytracingScene::BeginFrame(const XMFLOAT3& cameraPos, u32 skippedSkinned, u32 skippedTransparent)
{
    m_pending.clear();
    m_cameraPos = cameraPos;
    m_tlasValid = false;
    m_stats.instances          = 0;
    m_stats.blasBuiltThisFrame = 0;
    m_stats.droppedOverLimit   = 0;
    m_stats.skippedSkinned     = skippedSkinned;
    m_stats.skippedTransparent = skippedTransparent;
    m_stats.skinnedInstances   = 0;
    m_stats.skinnedRebuilds    = 0;
    m_stats.skinnedStale       = 0;
    m_stats.geoInfoWritten     = 0;
    m_stats.geoInfoWithAlbedo  = 0;
}

void RaytracingScene::AddInstance(const Mesh* mesh, const XMMATRIX& world, const XMFLOAT3& center,
                                  const GeometryInfo& geo)
{
    if (!mesh || !m_initialized) return;
    PendingInstance p;
    p.mesh = mesh;
    p.geo  = geo;
    XMStoreFloat4x4(&p.world, world);
    const XMVECTOR c = XMLoadFloat3(&center);
    p.distSq = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(c, XMLoadFloat3(&m_cameraPos))));
    m_pending.push_back(p);
}

void RaytracingScene::AddSkinnedInstance(u64 key, const Mesh* mesh,
                                         D3D12_GPU_VIRTUAL_ADDRESS deformedVb, u32 vertexCount,
                                         u64 poseHash,
                                         const XMMATRIX& world, const XMFLOAT3& center,
                                         const GeometryInfo& geo)
{
    if (!mesh || !m_initialized || key == 0 || deformedVb == 0 || vertexCount == 0) return;
    PendingInstance p;
    p.mesh        = mesh;
    p.geo         = geo;
    p.geo.flags  |= kGeomFlagSkinned;
    p.skinnedKey  = key;
    p.deformedVb  = deformedVb;
    p.vertexCount = vertexCount;
    p.poseHash    = poseHash;
    XMStoreFloat4x4(&p.world, world);
    const XMVECTOR c = XMLoadFloat3(&center);
    p.distSq = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(c, XMLoadFloat3(&m_cameraPos))));
    m_pending.push_back(p);
}

// スキンド BLAS。変形後頂点が毎フレーム変わるので毎フレーム同じ AS バッファへ再構築する。
// ★コンパクションは使わない: ALLOW_UPDATE と併用すると効果が大幅に減るうえ、
//   コンパクション後サイズの CPU 読み戻しが毎フレームには成立しない（NVIDIA / DXR 仕様）。
// ★動的 BLAS は 60〜80 B/三角形（静的コンパクション済みの約 3 倍）。VRAM 見積もりに注意。
const RaytracingScene::BlasEntry* RaytracingScene::EnsureSkinnedBlas(
    GraphicsDevice& device, ID3D12GraphicsCommandList4* cmd, u64 key, const Mesh* mesh,
    D3D12_GPU_VIRTUAL_ADDRESS deformedVb, u32 vertexCount, u64 poseHash, u32& triangleBudget)
{
    const auto& ibView = mesh->GetIndexBufferLod(0).GetView();   // ★静的と同じく LOD0 固定
    const u32   indexCount = mesh->GetIndexCountLod(0);
    if (ibView.BufferLocation == 0 || indexCount < 3)
        return nullptr;

    const u32 triangles = indexCount / 3;

    auto it = m_skinnedBlas.find(key);
    const bool haveUsable = (it != m_skinnedBlas.end())
                         && it->second.indexCount == indexCount
                         && it->second.vbAddress  == deformedVb;

    // ポーズが前フレームと同じなら変形後頂点も同じ＝BLAS は再構築不要。
    // （SkinningCompute 側でスキニング自体も省いている）
    if (haveUsable && it->second.poseHash == poseHash && poseHash != 0)
    {
        it->second.lastUsedFrame = m_frameCounter;
        return &it->second;
    }

    // 予算切れ: 既にある BLAS をそのまま使う（＝ポーズが 1 フレーム古くなるだけ）。
    // 無いなら今フレームは諦める（次フレームで作られる）。
    if (triangleBudget < triangles)
    {
        if (haveUsable)
        {
            it->second.lastUsedFrame = m_frameCounter;
            ++m_stats.skinnedStale;
            return &it->second;
        }
        return nullptr;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.Transform3x4               = 0;
    geom.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
    geom.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.IndexCount                 = indexCount;
    geom.Triangles.VertexCount                = vertexCount;
    geom.Triangles.IndexBuffer                = ibView.BufferLocation;
    // 変形後バッファは「位置だけを詰めた float3 配列」。インデックスは元メッシュのものを共有する
    // （compute は LOD0 の全頂点を書くので、インデックスの並びは変わらない）。
    geom.Triangles.VertexBuffer.StartAddress  = deformedVb;
    geom.Triangles.VertexBuffer.StrideInBytes = SkinningCompute::kOutStride;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // 毎フレーム完全再構築なので ALLOW_UPDATE は付けない（付けるとサイズが増えるだけ損）。
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return nullptr;

    BlasEntry& entry = m_skinnedBlas[key];
    // AS バッファは形状が変わらない限り使い回す（毎フレーム作り直すと当然もたない）。
    if (entry.sizeInBytes != info.ResultDataMaxSizeInBytes)
    {
        if (entry.sizeInBytes != 0)
            m_stats.skinnedBlasBytes -= (std::min)(m_stats.skinnedBlasBytes, entry.sizeInBytes);
        if (!entry.as.Initialize(device, info.ResultDataMaxSizeInBytes,
                                 RtBuffer::Kind::AccelStruct, L"Skinned BLAS"))
        {
            m_skinnedBlas.erase(key);
            return nullptr;
        }
        entry.sizeInBytes = info.ResultDataMaxSizeInBytes;
        m_stats.skinnedBlasBytes += entry.sizeInBytes;
    }
    if (!m_blasScratch.EnsureCapacity(device, info.ScratchDataSizeInBytes,
                                      RtBuffer::Kind::Scratch, L"BLAS scratch"))
        return nullptr;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs                           = inputs;
    build.DestAccelerationStructureData    = entry.as.GetGpuAddress();
    build.ScratchAccelerationStructureData = m_blasScratch.GetGpuAddress();
    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    // 静的と同じく、同じスクラッチを次の BLAS が使うので直列化する。
    // ponytail: スキンドが増えるとここが GPU 直列化のボトルネックになる。
    //   スクラッチをリング化するか、まとめて 1 回の Build に入れる方向で解くこと。
    RtUavBarrier(cmd);

    entry.vbAddress     = deformedVb;
    entry.ibAddress     = ibView.BufferLocation;
    entry.indexCount    = indexCount;
    entry.triangles     = triangles;
    entry.lastUsedFrame = m_frameCounter;
    entry.poseHash      = poseHash;

    triangleBudget -= triangles;
    ++m_stats.skinnedRebuilds;
    return &entry;
}

const RaytracingScene::BlasEntry* RaytracingScene::EnsureBlas(
    GraphicsDevice& device, ID3D12GraphicsCommandList4* cmd, const Mesh* mesh, u32& budget)
{
    const auto& vbView = mesh->GetVertexBuffer().GetView();
    const auto& ibView = mesh->GetIndexBufferLod(0).GetView();   // ★LOD0 固定
    const u32   indexCount = mesh->GetIndexCountLod(0);

    if (vbView.BufferLocation == 0 || ibView.BufferLocation == 0
        || indexCount < 3 || vbView.StrideInBytes == 0)
        return nullptr;

    auto it = m_blas.find(mesh);
    if (it != m_blas.end())
    {
        BlasEntry& e = it->second;
        const bool stale = e.geometryVersion != mesh->GetGeometryVersion()
                        || e.vbAddress  != vbView.BufferLocation
                        || e.ibAddress  != ibView.BufferLocation
                        || e.indexCount != indexCount;
        if (!stale)
        {
            e.lastUsedFrame = m_frameCounter;
            return &e;
        }
        // 頂点バッファが作り直された（地形/スカルプトのブラシ・UV スケール・頂点カラー）。
        // 古い BLAS は壊れたアドレスを指しているので捨てて作り直す（計画09 R2）。
        m_stats.blasBytes     -= (std::min)(m_stats.blasBytes, e.sizeInBytes);
        m_stats.blasTriangles -= (std::min)(m_stats.blasTriangles, static_cast<u64>(e.triangles));
        m_blas.erase(it);
    }

    if (budget == 0) return nullptr;   // 今フレームの構築予算切れ。次フレームで作る

    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    // ★OPAQUE を必ず立てる。any-hit を一切使わない設計なので固定機能のトラバースが効く
    //   （NVIDIA: "Mark geometry as OPAQUE whenever possible"）。
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.Transform3x4               = 0;
    geom.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;   // IndexBuffer は u32 固定
    geom.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.IndexCount                 = indexCount;
    geom.Triangles.VertexCount                = vbView.SizeInBytes / vbView.StrideInBytes;
    geom.Triangles.IndexBuffer                = ibView.BufferLocation;
    geom.Triangles.VertexBuffer.StartAddress  = vbView.BufferLocation;  // Vertex::position はオフセット 0
    geom.Triangles.VertexBuffer.StrideInBytes = vbView.StrideInBytes;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // 静的ジオメトリなので FAST_TRACE。v1 ではコンパクションを実装しない（計画09 §3.2）。
    inputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs       = 1;
    inputs.pGeometryDescs = &geom;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return nullptr;

    // ★BlasEntry は RtBuffer（GpuResource 派生 = コピーもムーブも不可）を持つので、
    //   ローカルに作って map へ move することはできない。必ずマップ側で既定構築してから埋める。
    BlasEntry& entry = m_blas[mesh];
    if (!entry.as.Initialize(device, info.ResultDataMaxSizeInBytes,
                             RtBuffer::Kind::AccelStruct, L"BLAS"))
    {
        m_blas.erase(mesh);
        return nullptr;
    }
    if (!m_blasScratch.EnsureCapacity(device, info.ScratchDataSizeInBytes,
                                      RtBuffer::Kind::Scratch, L"BLAS scratch"))
    {
        m_blas.erase(mesh);
        return nullptr;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs                           = inputs;
    build.DestAccelerationStructureData    = entry.as.GetGpuAddress();
    build.ScratchAccelerationStructureData = m_blasScratch.GetGpuAddress();
    // ★VB/IB は Buffer.cpp の RecordBufferUpload が GENERIC_READ で置いていくので、
    //   AS ビルド入力のための遷移バリアは要らない（GENERIC_READ は NON_PIXEL_SHADER_RESOURCE を含む）。
    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    // 同じスクラッチを次の BLAS でも使うので、必ずここで直列化する（計画09 R8）。
    RtUavBarrier(cmd);

    entry.geometryVersion = mesh->GetGeometryVersion();
    entry.vbAddress     = vbView.BufferLocation;
    entry.ibAddress     = ibView.BufferLocation;
    entry.indexCount    = indexCount;
    entry.triangles     = indexCount / 3;
    entry.sizeInBytes   = info.ResultDataMaxSizeInBytes;
    entry.lastUsedFrame = m_frameCounter;

    m_stats.blasBytes     += entry.sizeInBytes;
    m_stats.blasTriangles += entry.triangles;
    ++m_stats.blasBuiltThisFrame;
    --budget;
    return &entry;
}

bool RaytracingScene::Build(GraphicsDevice& device, ID3D12GraphicsCommandList* cmd, const BuildDesc& d)
{
    m_tlasValid = false;
    if (!m_initialized || m_cmdList4Failed || !cmd) return false;

    // Gate 4: ID3D12GraphicsCommandList4。BuildRaytracingAccelerationStructure は List4 のメソッド。
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4))))
    {
        m_cmdList4Failed = true;
        Logger::Warn("ID3D12GraphicsCommandList4 を取得できませんでした。レイトレーシングを無効にします");
        return false;
    }

    ++m_frameCounter;
    m_stats.blasCount = static_cast<u32>(m_blas.size());

    const u32 limit = (std::min)((d.maxInstances > 0) ? d.maxInstances : kMaxRtInstances,
                                 kMaxRtInstances);
    if (m_pending.size() > limit)
    {
        // 上限超過。カメラに近い順で残す（遠景を落としても影 / AO への寄与は小さい）。
        std::nth_element(m_pending.begin(), m_pending.begin() + limit, m_pending.end(),
            [](const PendingInstance& a, const PendingInstance& b) { return a.distSq < b.distSq; });
        m_stats.droppedOverLimit = static_cast<u32>(m_pending.size()) - limit;
        m_pending.resize(limit);
    }
    if (m_pending.empty()) return false;

    // ---- インスタンス記述を書く（BLAS は必要になった時点で遅延構築）----
    RtBuffer& descBuf = m_instanceDescs[d.frameIndex % FrameResources::kFrameCount];
    const u64 needBytes = static_cast<u64>(m_pending.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    if (!descBuf.EnsureCapacity(device, needBytes, RtBuffer::Kind::Upload, L"TLAS instance descs"))
        return false;

    auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(descBuf.GetMappedPtr());
    if (!descs) return false;

    // GeometryInfo テーブル（ヒット点で頂点属性 / アルベドを引くため）。
    // ★インスタンス記述とまったく同じ添字で書く。下のループの count が両方の添字になる。
    RtBuffer& geoBuf = m_geoInfos[d.frameIndex % FrameResources::kFrameCount];
    GeometryInfo* geos = nullptr;
    if (geoBuf.EnsureCapacity(device, static_cast<u64>(m_pending.size()) * sizeof(GeometryInfo),
                              RtBuffer::Kind::Upload, L"RT geometry info"))
        geos = static_cast<GeometryInfo*>(geoBuf.GetMappedPtr());
    m_geoInfoAddress = geos ? geoBuf.GetGpuAddress() : 0;

    u32 budget      = d.maxBlasBuildsPerFrame;
    u32 skinBudget  = d.maxSkinnedTrianglesPerFrame;
    u32 count       = 0;
    // 近いものから処理する＝予算が切れたときに古いポーズで残るのは遠くのキャラになる。
    // （Metro Exodus の「見えている/近いものを優先」と同じ考え方の、最小の実装）
    std::sort(m_pending.begin(), m_pending.end(),
              [](const PendingInstance& a, const PendingInstance& b) { return a.distSq < b.distSq; });
    m_stats.skinnedTriangles = 0;
    for (const PendingInstance& p : m_pending)
    {
        const BlasEntry* blas = (p.skinnedKey != 0)
            ? EnsureSkinnedBlas(device, cmd4.Get(), p.skinnedKey, p.mesh,
                                p.deformedVb, p.vertexCount, p.poseHash, skinBudget)
            : EnsureBlas(device, cmd4.Get(), p.mesh, budget);
        if (!blas) continue;   // まだ建っていない（予算切れ / 不正なメッシュ）→ 次フレームで入る
        if (p.skinnedKey != 0)
        {
            ++m_stats.skinnedInstances;
            m_stats.skinnedTriangles += blas->triangles;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& dst = descs[count];
        // ★Transform[3][4] は row-major 3x4（平行移動が 4 列目）。
        //   DirectXMath の行ベクトル規約のワールド行列 W に対して、これは
        //   transpose(W) の先頭 3 行そのもの。XMStoreFloat3x4 がまさに転置して書くので
        //   自前で並べ替えるコードは要らない（バイト一致。計画09 §4.3）。
        XMFLOAT3X4 t;
        XMStoreFloat3x4(&t, XMLoadFloat4x4(&p.world));
        static_assert(sizeof(t.m) == sizeof(dst.Transform), "3x4 transform layout mismatch");
        std::memcpy(dst.Transform, t.m, sizeof(dst.Transform));

        // ★GeometryInfo テーブルの添字。24bit なので kMaxRtInstances(32768) には十分。
        //   m_pending の添字ではなく「実際に書き出した順」であることが重要
        //   （上で距離順にソートしたうえ、BLAS が建っていないものは continue で飛んでいる）。
        dst.InstanceID                          = count;
        if (geos)
        {
            geos[count] = p.geo;
            if (p.geo.vbSrvIndex != 0xFFFFFFFFu) ++m_stats.geoInfoWritten;
            if (p.geo.baseColorSrvIndex != 0xFFFFFFFFu) ++m_stats.geoInfoWithAlbedo;
        }
        dst.InstanceMask                        = 0xFF;
        dst.InstanceContributionToHitGroupIndex = 0;
        dst.Flags                               = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        dst.AccelerationStructure               = blas->as.GetGpuAddress();
        ++count;
    }

    m_stats.blasCount = static_cast<u32>(m_blas.size());
    if (count == 0) return false;

    // ---- TLAS を毎フレーム再構築 ----
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs      = count;
    inputs.InstanceDescs = descBuf.GetGpuAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return false;

    if (!m_tlas.EnsureCapacity(device, info.ResultDataMaxSizeInBytes,
                               RtBuffer::Kind::AccelStruct, L"TLAS"))
        return false;
    if (!m_tlasScratch.EnsureCapacity(device, info.ScratchDataSizeInBytes,
                                      RtBuffer::Kind::Scratch, L"TLAS scratch"))
        return false;

    // 前フレームの RayQuery がまだこの TLAS を読んでいる可能性がある（AS は状態遷移できないので
    // WAR の面倒も UAV バリアで見る）。BLAS ビルド → TLAS ビルドの依存もここで閉じる。
    RtUavBarrier(cmd);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs                           = inputs;
    build.DestAccelerationStructureData    = m_tlas.GetGpuAddress();
    build.ScratchAccelerationStructureData = m_tlasScratch.GetGpuAddress();
    cmd4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    // TLAS ビルド → RayQuery（読み）。これを忘れるとゴミの BVH を読む（デバッグレイヤは黙っている）。
    RtUavBarrier(cmd);

    m_stats.instances         = count;
    m_stats.tlasBytes         = info.ResultDataMaxSizeInBytes;
    m_stats.scratchBytes      = m_blasScratch.GetSizeInBytes() + m_tlasScratch.GetSizeInBytes();
    m_stats.instanceDescBytes = descBuf.GetSizeInBytes();
    m_tlasValid = true;
    return true;
}

} // namespace dx12e
