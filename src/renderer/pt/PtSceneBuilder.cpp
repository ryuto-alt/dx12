#include "renderer/pt/PtSceneBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace dx12e::pt
{

namespace
{
constexpr uint64_t Align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

void UavBarrierAll(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = nullptr;
    cmd->ResourceBarrier(1, &b);
}

// world(行ベクトル規約)→ 3x4(行 = 列ベクトル規約の先頭 3 行)
void Store3x4(const XMMATRIX& m, float out[12])
{
    XMFLOAT3X4 t;
    XMStoreFloat3x4(&t, m);
    std::memcpy(out, t.m, sizeof(float) * 12);
}
} // namespace

bool SceneBuilder::Init(ID3D12Device5* device, const std::wstring& shaderDir)
{
    Reset();
    m_device = device;
    m_shaderDir = shaderDir;
    return m_device != nullptr;
}

void SceneBuilder::Reset()
{
    m_geometries.clear();
    m_geometryByKey.clear();
    m_materials.clear();
    m_instances.clear();
    m_lights.clear();
    m_blas.clear();
    m_blasCursor = 0;
    m_scratch.Reset();
    m_scratchSize = 0;
    m_tlas.Reset(); m_tlasScratch.Reset(); m_instanceDescs.Reset();
    m_instanceTable.Reset(); m_materialTable.Reset(); m_lightTable.Reset(); m_emissiveTable.Reset();
    m_staging.clear();
    m_skinCopies.clear();
    m_scene = SceneGpu{};
    m_stats = BuilderStats{};
    m_instanceGpu.clear();
    m_emissiveBase.clear();
}

ComPtr<ID3D12Resource> SceneBuilder::CreateBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 16);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    if (heap == D3D12_HEAP_TYPE_UPLOAD) state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    if (name) r->SetName(name);
    return r;
}

ComPtr<ID3D12Resource> SceneBuilder::UploadTable(ID3D12GraphicsCommandList4* cmd, const void* data,
                                                 uint64_t bytes, const wchar_t* name)
{
    const uint64_t size = std::max<uint64_t>(bytes, 16);
    ComPtr<ID3D12Resource> dst = CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                              D3D12_RESOURCE_STATE_COPY_DEST, name);
    ComPtr<ID3D12Resource> up = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, L"PT staging");
    if (!dst || !up) return nullptr;
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(up->Map(0, &none, &p))) return nullptr;
    std::memset(p, 0, static_cast<size_t>(size));
    if (data && bytes) std::memcpy(p, data, static_cast<size_t>(bytes));
    up->Unmap(0, nullptr);
    cmd->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, size);
    Barrier(cmd, dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_staging.push_back(std::move(up));
    return dst;
}

uint32_t SceneBuilder::AddGeometry(const GeometryDesc& g)
{
    if (g.key != 0)
    {
        auto it = m_geometryByKey.find(g.key);
        if (it != m_geometryByKey.end()) return it->second;
    }
    Geometry geo;
    geo.desc = g;
    m_geometries.push_back(geo);
    const uint32_t id = static_cast<uint32_t>(m_geometries.size() - 1);
    if (g.key != 0) m_geometryByKey[g.key] = id;
    return id;
}

uint32_t SceneBuilder::AddMaterial(const MaterialGpu& m)
{
    m_materials.push_back(m);
    return static_cast<uint32_t>(m_materials.size() - 1);
}

void SceneBuilder::AddLight(const LightGpu& l) { m_lights.push_back(l); }

uint32_t SceneBuilder::AddInstance(const InstanceDesc& d)
{
    InstanceRec rec;
    rec.desc = d;
    if (d.geometry >= m_geometries.size()) return kNoIndex;
    Geometry& geo = m_geometries[d.geometry];
    const bool skinned = d.deformedVbVA != 0;
    Blas b;
    if (skinned)
    {
        // スキンド: このインスタンス専用の BLAS(変形後位置の float3 詰めバッファから)。
        b.vbVA = d.deformedVbVA;
        b.vbStride = 12;
        b.vertexCount = geo.desc.vertexCount;
        b.ibVA = geo.desc.ibVA;
        b.indexCount = geo.desc.indexCount;
        m_blas.push_back(std::move(b));
        rec.blas = static_cast<int>(m_blas.size() - 1);
    }
    else
    {
        if (geo.blas < 0)
        {
            b.vbVA = geo.desc.vbVA;
            b.vbStride = geo.desc.vbStride;
            b.vertexCount = geo.desc.vertexCount;
            b.ibVA = geo.desc.ibVA;
            b.indexCount = geo.desc.indexCount;
            m_blas.push_back(std::move(b));
            geo.blas = static_cast<int>(m_blas.size() - 1);
        }
        rec.blas = geo.blas;
    }
    m_instances.push_back(rec);
    return static_cast<uint32_t>(m_instances.size() - 1);
}

bool SceneBuilder::RecordSkinnedCopies(ID3D12GraphicsCommandList4* cmd, std::string* err)
{
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    bool any = false;
    for (const InstanceRec& r : m_instances) if (r.desc.deformedVbVA != 0) { any = true; break; }
    if (!any) return true;
    if (!m_srvFactory) return fail("スキンド用の raw SRV ファクトリが未設定");

    if (!m_copyPso)
    {
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 4;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor.ShaderRegister = 0;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[2].Descriptor.ShaderRegister = 0;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 3; rsd.pParameters = p;
        ComPtr<ID3DBlob> blob, eb;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &eb))) return fail("コピー用ルートシグネチャのシリアライズに失敗");
        if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_copyRs)))) return fail("コピー用ルートシグネチャの作成に失敗");
        // .cso の読み込み(Resource モジュールに依存させないので素朴に読む)
        FILE* f = nullptr;
        if (_wfopen_s(&f, (m_shaderDir + L"PtCopy_CS.cso").c_str(), L"rb") != 0 || !f) return fail("PtCopy_CS.cso が読めない");
        std::vector<uint8_t> bc;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bc.resize(static_cast<size_t>(std::max(n, 0L)));
        const size_t rd = bc.empty() ? 0 : std::fread(bc.data(), 1, bc.size(), f);
        std::fclose(f);
        if (rd != bc.size() || bc.empty()) return fail("PtCopy_CS.cso の読み込みに失敗");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_copyRs.Get();
        pd.CS = {bc.data(), bc.size()};
        if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_copyPso)))) return fail("コピー用 PSO を作れない");
    }
    cmd->SetComputeRootSignature(m_copyRs.Get());
    cmd->SetPipelineState(m_copyPso.Get());
    std::vector<ID3D12Resource*> dsts;
    for (InstanceRec& r : m_instances)
    {
        InstanceDesc& d = r.desc;
        if (d.deformedVbVA == 0 || r.blas < 0) continue;
        const Geometry& g = m_geometries[d.geometry];
        const uint64_t bytes = static_cast<uint64_t>(g.desc.vertexCount) * 12;
        ComPtr<ID3D12Resource> dst = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"PT skinned copy");
        if (!dst) return fail("スキンドの写しバッファを確保できない(VRAM 不足)");
        const uint32_t words = static_cast<uint32_t>(bytes / 4);
        const uint32_t cb[4] = {words, 0, 0, 0};
        cmd->SetComputeRoot32BitConstants(0, 4, cb, 0);
        cmd->SetComputeRootShaderResourceView(1, d.deformedVbVA);
        cmd->SetComputeRootUnorderedAccessView(2, dst->GetGPUVirtualAddress());
        cmd->Dispatch((words + 255) / 256, 1, 1);
        dsts.push_back(dst.Get());
        const uint32_t srv = m_srvFactory(dst.Get(), bytes);
        d.deformedVbVA = dst->GetGPUVirtualAddress();
        d.deformedSrv = srv;
        m_blas[r.blas].vbVA = (srv == kNoIndex) ? 0 : d.deformedVbVA;   // vbVA=0 は BeginBuild が空扱いにする
        m_skinCopies.push_back(std::move(dst));
    }
    // 写し → BLAS ビルド入力 / シェーダの SRV 読み。
    std::vector<D3D12_RESOURCE_BARRIER> bs;
    for (ID3D12Resource* res : dsts)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bs.push_back(b);
    }
    if (!bs.empty()) cmd->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
    return true;
}

bool SceneBuilder::BeginBuild(std::string* err)
{
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!m_device) return fail("SceneBuilder が未初期化");
    if (m_instances.empty()) return fail("インスタンスが 1 つも無い(描画対象のメッシュが無い)");
    if (m_instances.size() > kMaxInstances) return fail("インスタンス数が上限を超えた");

    // BLAS のバッファ確保(サイズはプレビルド情報から)。
    for (Blas& b : m_blas)
    {
        if (b.vbVA == 0 || b.ibVA == 0 || b.indexCount < 3 || b.vertexCount == 0)
        {
            b.built = true;   // 空(このジオメトリのインスタンスは TLAS から外す)
            continue;
        }
        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;   // MASK / BLEND はインスタンス側で FORCE_NON_OPAQUE
        geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.IndexCount = b.indexCount;
        geom.Triangles.VertexCount = b.vertexCount;
        geom.Triangles.IndexBuffer = b.ibVA;
        geom.Triangles.VertexBuffer.StartAddress = b.vbVA;
        geom.Triangles.VertexBuffer.StrideInBytes = b.vbStride;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = 1;
        in.pGeometryDescs = &geom;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        if (info.ResultDataMaxSizeInBytes == 0) { b.built = true; continue; }
        b.size = Align(info.ResultDataMaxSizeInBytes, 256);
        b.scratch = Align(info.ScratchDataSizeInBytes, 256);
        b.buffer = CreateBuffer(b.size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"PT BLAS");
        if (!b.buffer) return fail("BLAS バッファを確保できない(VRAM 不足)");
        m_scratchSize = std::max(m_scratchSize, b.scratch);
        m_stats.blasBytes += b.size;
        m_stats.blasTriangles += b.indexCount / 3;
        ++m_stats.blasCount;
    }
    if (m_scratchSize > 0)
    {
        m_scratch = CreateBuffer(m_scratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"PT BLAS scratch");
        if (!m_scratch) return fail("BLAS スクラッチを確保できない");
    }
    m_blasCursor = 0;
    return true;
}

bool SceneBuilder::BuildStep(ID3D12GraphicsCommandList4* cmd, uint64_t triangleBudget)
{
    uint64_t used = 0;
    while (m_blasCursor < m_blas.size())
    {
        Blas& b = m_blas[m_blasCursor];
        if (b.built || !b.buffer) { ++m_blasCursor; continue; }
        const uint64_t tris = b.indexCount / 3;
        if (used > 0 && used + tris > triangleBudget) return false;

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.IndexCount = b.indexCount;
        geom.Triangles.VertexCount = b.vertexCount;
        geom.Triangles.IndexBuffer = b.ibVA;
        geom.Triangles.VertexBuffer.StartAddress = b.vbVA;
        geom.Triangles.VertexBuffer.StrideInBytes = b.vbStride;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.NumDescs = 1;
        build.Inputs.pGeometryDescs = &geom;
        build.DestAccelerationStructureData = b.buffer->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = m_scratch->GetGPUVirtualAddress();
        cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        UavBarrierAll(cmd);   // 同じスクラッチを次のビルドが使う
        b.built = true;
        used += tris;
        ++m_blasCursor;
        if (used >= triangleBudget) break;
    }
    return m_blasCursor >= m_blas.size();
}

// ---------------------------------------------------------------------------
//  エミッシブ三角形表(Walker alias)
// ---------------------------------------------------------------------------
void SceneBuilder::BuildEmissiveTable(std::vector<EmissiveTriGpu>& out)
{
    out.clear();
    m_emissiveBase.assign(m_instances.size(), kNoIndex);
    struct Entry { uint32_t inst, prim; double area; double w; };
    std::vector<Entry> entries;
    double total = 0.0;

    for (uint32_t ii = 0; ii < m_instances.size(); ++ii)
    {
        const InstanceRec& rec = m_instances[ii];
        const InstanceDesc& d = rec.desc;
        if (d.emissiveLuma <= 0.0f) continue;
        const GeometryDesc& g = m_geometries[d.geometry].desc;
        const bool skinned = d.deformedVbVA != 0;
        if (skinned || !g.cpuPositions || !g.cpuIndices || g.indexCount < 3) { ++m_stats.emissiveSkipped; continue; }
        const uint32_t tris = g.indexCount / 3;
        if (entries.size() + tris > kMaxEmissiveTris) { ++m_stats.emissiveSkipped; continue; }

        const XMMATRIX W = XMLoadFloat4x4(&d.world);
        m_emissiveBase[ii] = static_cast<uint32_t>(entries.size());
        ++m_stats.emissiveInstances;
        for (uint32_t t = 0; t < tris; ++t)
        {
            const uint32_t i0 = g.cpuIndices[t * 3 + 0], i1 = g.cpuIndices[t * 3 + 1], i2 = g.cpuIndices[t * 3 + 2];
            double area = 0.0;
            if (i0 < g.vertexCount && i1 < g.vertexCount && i2 < g.vertexCount)
            {
                const XMVECTOR p0 = XMVector3Transform(XMLoadFloat3(&g.cpuPositions[i0]), W);
                const XMVECTOR p1 = XMVector3Transform(XMLoadFloat3(&g.cpuPositions[i1]), W);
                const XMVECTOR p2 = XMVector3Transform(XMLoadFloat3(&g.cpuPositions[i2]), W);
                area = 0.5 * static_cast<double>(XMVectorGetX(XMVector3Length(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0)))));
            }
            const double w = area * static_cast<double>(d.emissiveLuma);
            entries.push_back({ii, t, area, w});
            total += w;
        }
    }
    if (entries.empty() || total <= 0.0)
    {
        std::fill(m_emissiveBase.begin(), m_emissiveBase.end(), kNoIndex);
        return;
    }
    m_stats.emissivePower = total;

    // Vose の alias 法(倍精度で作って float へ)。
    const size_t n = entries.size();
    std::vector<double> prob(n);
    std::vector<uint32_t> alias(n, 0), lows, highs;
    lows.reserve(n); highs.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        prob[i] = entries[i].w * static_cast<double>(n) / total;
        (prob[i] < 1.0 ? lows : highs).push_back(static_cast<uint32_t>(i));
    }
    while (!lows.empty() && !highs.empty())
    {
        const uint32_t s = lows.back(); lows.pop_back();
        const uint32_t l = highs.back(); highs.pop_back();
        alias[s] = l;
        prob[l] = (prob[l] + prob[s]) - 1.0;
        (prob[l] < 1.0 ? lows : highs).push_back(l);
    }
    for (uint32_t i : highs) { prob[i] = 1.0; alias[i] = i; }
    for (uint32_t i : lows) { prob[i] = 1.0; alias[i] = i; }   // 数値誤差の残り

    out.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        EmissiveTriGpu& e = out[i];
        e.instance = entries[i].inst;
        e.prim = entries[i].prim;
        e.pmf = static_cast<float>(entries[i].w / total);
        e.thresh = static_cast<float>(prob[i]);
        e.alias = alias[i];
        e.area = static_cast<float>(entries[i].area);
        e.pad0 = e.pad1 = 0;
    }
    m_stats.emissiveTris = static_cast<uint32_t>(n);
}

bool SceneBuilder::FinishBuild(ID3D12GraphicsCommandList4* cmd, std::string* err)
{
    auto fail = [&](const char* m) { if (err) *err = m; return false; };

    // ---- エミッシブ表 ----
    std::vector<EmissiveTriGpu> emissive;
    BuildEmissiveTable(emissive);

    // ---- インスタンス表 + TLAS の記述 ----
    m_instanceGpu.clear();
    m_instanceGpu.reserve(m_instances.size());
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
    descs.reserve(m_instances.size());
    std::vector<uint32_t> remap(m_instances.size(), kNoIndex);   // BLAS が無い(空)インスタンスは詰める

    for (uint32_t ii = 0; ii < m_instances.size(); ++ii)
    {
        const InstanceRec& rec = m_instances[ii];
        const InstanceDesc& d = rec.desc;
        if (rec.blas < 0) continue;
        const Blas& b = m_blas[rec.blas];
        if (!b.buffer) continue;   // 空ジオメトリ
        const GeometryDesc& g = m_geometries[d.geometry].desc;
        const bool skinned = d.deformedVbVA != 0 && d.deformedSrv != kNoIndex;

        const uint32_t outIndex = static_cast<uint32_t>(m_instanceGpu.size());
        remap[ii] = outIndex;

        InstanceGpu ig{};
        const XMMATRIX W = XMLoadFloat4x4(&d.world);
        Store3x4(W, ig.o2w);
        XMVECTOR det;
        const XMMATRIX Winv = XMMatrixInverse(&det, W);
        Store3x4(Winv, ig.w2o);
        ig.vbSrv = g.vbSrv;
        ig.ibSrv = g.ibSrv;
        ig.materialIndex = std::min<uint32_t>(d.material, static_cast<uint32_t>(std::max<size_t>(m_materials.size(), 1) - 1));
        ig.flags = skinned ? kInstSkinned : 0u;
        ig.emissiveBase = m_emissiveBase[ii];
        ig.defVbSrv = skinned ? d.deformedSrv : kNoIndex;
        m_instanceGpu.push_back(ig);

        D3D12_RAYTRACING_INSTANCE_DESC dd{};
        std::memcpy(dd.Transform, ig.o2w, sizeof(dd.Transform));
        dd.InstanceID = outIndex;
        dd.InstanceMask = 0xFF;
        dd.InstanceContributionToHitGroupIndex = 0;
        const bool nonOpaque = !m_materials.empty() && m_materials[ig.materialIndex].alphaMode != 0;
        dd.Flags = nonOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        dd.AccelerationStructure = b.buffer->GetGPUVirtualAddress();
        descs.push_back(dd);

        if (nonOpaque) ++m_stats.nonOpaqueInstances;
        if (skinned) ++m_stats.skinnedInstances;
        m_stats.triangles += g.indexCount / 3;
    }
    if (descs.empty()) return fail("TLAS に入るインスタンスが無い(全メッシュが空)");

    // emissive 表の instance 添字は「元のインスタンス番号」なので、詰めた後の番号へ書き換える。
    for (EmissiveTriGpu& e : emissive)
    {
        const uint32_t r = remap[e.instance];
        e.instance = (r == kNoIndex) ? 0u : r;
    }
    // 詰めで emissiveBase の意味は変わらない(表内の位置)。

    // ---- TLAS ----
    const uint64_t descBytes = descs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    m_instanceDescs = CreateBuffer(descBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, L"PT TLAS instance descs");
    if (!m_instanceDescs) return fail("TLAS の記述バッファを確保できない");
    {
        void* p = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(m_instanceDescs->Map(0, &none, &p))) return fail("TLAS の記述バッファを Map できない");
        std::memcpy(p, descs.data(), static_cast<size_t>(descBytes));
        m_instanceDescs->Unmap(0, nullptr);
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = static_cast<UINT>(descs.size());
    in.InstanceDescs = m_instanceDescs->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (info.ResultDataMaxSizeInBytes == 0) return fail("TLAS のプレビルド情報が 0");
    m_tlas = CreateBuffer(Align(info.ResultDataMaxSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"PT TLAS");
    m_tlasScratch = CreateBuffer(Align(info.ScratchDataSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"PT TLAS scratch");
    if (!m_tlas || !m_tlasScratch) return fail("TLAS を確保できない(VRAM 不足)");
    UavBarrierAll(cmd);   // BLAS ビルド → TLAS ビルド
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = in;
    build.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    UavBarrierAll(cmd);
    m_stats.tlasBytes = Align(info.ResultDataMaxSizeInBytes, 256);

    // ---- テーブル ----
    m_instanceTable = UploadTable(cmd, m_instanceGpu.data(), m_instanceGpu.size() * sizeof(InstanceGpu), L"PT instances");
    MaterialGpu dummyMat{};
    dummyMat.metallic = 0.0f; dummyMat.roughness = 0.5f;
    dummyMat.tint[0] = dummyMat.tint[1] = dummyMat.tint[2] = 1.0f; dummyMat.opacity = 1.0f;
    dummyMat.albedoSrv = dummyMat.normalSrv = dummyMat.mrSrv = dummyMat.emissiveSrv = kNoIndex;
    dummyMat.uvScaleOffset[0] = dummyMat.uvScaleOffset[1] = 1.0f;
    dummyMat.alphaCutoff = 0.5f;
    std::vector<MaterialGpu> mats = m_materials;
    if (mats.empty()) mats.push_back(dummyMat);
    m_materialTable = UploadTable(cmd, mats.data(), mats.size() * sizeof(MaterialGpu), L"PT materials");
    LightGpu dummyLight{};
    m_lightTable = UploadTable(cmd, m_lights.empty() ? &dummyLight : m_lights.data(),
                               (m_lights.empty() ? 1 : m_lights.size()) * sizeof(LightGpu), L"PT lights");
    EmissiveTriGpu dummyEm{};
    m_emissiveTable = UploadTable(cmd, emissive.empty() ? &dummyEm : emissive.data(),
                                  (emissive.empty() ? 1 : emissive.size()) * sizeof(EmissiveTriGpu), L"PT emissive");
    if (!m_instanceTable || !m_materialTable || !m_lightTable || !m_emissiveTable)
        return fail("テーブルバッファを確保できない");

    m_scene.tlas = m_tlas->GetGPUVirtualAddress();
    m_scene.instances = m_instanceTable->GetGPUVirtualAddress();
    m_scene.materials = m_materialTable->GetGPUVirtualAddress();
    m_scene.lights = m_lightTable->GetGPUVirtualAddress();
    m_scene.emissive = m_emissiveTable->GetGPUVirtualAddress();
    m_scene.instanceCount = static_cast<uint32_t>(m_instanceGpu.size());
    m_scene.materialCount = static_cast<uint32_t>(mats.size());
    m_scene.lightCount = static_cast<uint32_t>(m_lights.size());
    m_scene.emissiveCount = static_cast<uint32_t>(emissive.size());
    m_scene.valid = true;

    m_stats.instances = m_scene.instanceCount;
    m_stats.materials = m_scene.materialCount;
    m_stats.lights = m_scene.lightCount;
    return true;
}

void SceneBuilder::ReleaseTransient()
{
    m_staging.clear();
    m_scratch.Reset();
    m_scratchSize = 0;
    m_tlasScratch.Reset();
    m_instanceDescs.Reset();
}

} // namespace dx12e::pt
