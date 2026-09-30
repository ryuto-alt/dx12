#include "renderer/PathTracer.h"

#include "resource/ShaderCompiler.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace dx12e::pt
{

namespace
{
constexpr uint32_t kMaxUnitsPerCall = 96;   // 1 回の Record が積むユニットの上限(見積もりが外れたときの保険)

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, uint64_t bytes, D3D12_HEAP_TYPE heap,
                                  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc = {1, 0};
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    if (heap == D3D12_HEAP_TYPE_UPLOAD) state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (heap == D3D12_HEAP_TYPE_READBACK) state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    if (name) r->SetName(name);
    return r;
}

void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cmd->ResourceBarrier(1, &b);
}
} // namespace

bool PathTracer::Initialize(ID3D12Device5* device, ID3D12CommandQueue* queue, const std::wstring& shaderDir, std::string* err)
{
    Shutdown();
    m_device = device;
    m_queue = queue;
    if (!m_device || !m_queue) { if (err) *err = "device / queue が null"; return false; }

    // SM 6.6 + Resource Binding Tier 3 + DXR 1.1(inline)。
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
    { if (err) *err = "シェーダモデル 6.6 が使えない"; return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) || o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
    { if (err) *err = "リソースバインディング Tier 3 が使えない"; return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))) || o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    { if (err) *err = "DXR 1.1(inline raytracing)が使えない"; return false; }

    if (!CreateRootSignatureAndPso(shaderDir, err)) { Shutdown(); return false; }

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) { if (err) *err = "フェンス作成に失敗"; Shutdown(); return false; }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(m_queue->GetTimestampFrequency(&m_tsFreq))) m_tsFreq = 0;

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kSlots * 2;
    if (SUCCEEDED(m_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_queryHeap))))
        m_queryReadback = MakeBuffer(m_device, kSlots * 2 * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"PT timestamps");
    return true;
}

void PathTracer::Shutdown()
{
    if (m_fence && m_fenceEvent && m_queue) WaitForGpu();
    if (m_fenceEvent) { CloseHandle(static_cast<HANDLE>(m_fenceEvent)); m_fenceEvent = nullptr; }
    m_accum.Reset(); m_stats.Reset(); m_jobCb.Reset(); m_readback.Reset();
    m_queryHeap.Reset(); m_queryReadback.Reset();
    m_pso.Reset(); m_psoAtmos.Reset(); m_rootSig.Reset(); m_fence.Reset();
    m_state = State::Idle;
    m_device = nullptr; m_queue = nullptr;
}

bool PathTracer::CreateRootSignatureAndPso(const std::wstring& shaderDir, std::string* err)
{
    // 0: CBV b0(ジョブ) / 1: 32bit 定数 b1(タイル 8 DW) / 2..6: SRV t0..t4 / 7..8: UAV u0..u1。計 2+8+10+4 = 24 DWORD。
    D3D12_ROOT_PARAMETER p[9]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.ShaderRegister = 0;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 1;
    p[1].Constants.Num32BitValues = sizeof(TileConstants) / 4;
    for (int i = 0; i < 5; ++i)
    {
        p[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[2 + i].Descriptor.ShaderRegister = static_cast<UINT>(i);
    }
    p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[7].Descriptor.ShaderRegister = 0;
    p[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[8].Descriptor.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC ss[2]{};
    for (int i = 0; i < 2; ++i)
    {
        ss[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = (i == 0) ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[i].MaxLOD = D3D12_FLOAT32_MAX;
        ss[i].ShaderRegister = static_cast<UINT>(i);
        ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = _countof(p);
    rsd.pParameters = p;
    rsd.NumStaticSamplers = 2;
    rsd.pStaticSamplers = ss;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;   // バインドレス(ResourceDescriptorHeap[])

    ComPtr<ID3DBlob> blob, eb;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &eb)))
    {
        if (err) *err = std::string("ルートシグネチャのシリアライズ: ") + (eb ? static_cast<const char*>(eb->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
    { if (err) *err = "ルートシグネチャの作成に失敗"; return false; }

    const auto bc = dx12e::ShaderCompiler::LoadFromFile(shaderDir + L"PathTrace_CS.cso");
    if (bc.GetSize() == 0) { if (err) *err = "PathTrace_CS.cso が読めない(シェーダをビルドしたか / shaderDir が正しいか)"; return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = m_rootSig.Get();
    pd.CS = {bc.GetData(), bc.GetSize()};
    if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_pso))))
    { if (err) *err = "PathTrace の PSO を作れない"; return false; }
    // 物理大気(A1)版。従来の .cso は DXIL が 1 ビットも変わらないよう別ファイルにしてある。無ければ大気の空だけが使えない(従来の PT は動く)。
    const auto bcA = dx12e::ShaderCompiler::LoadFromFile(shaderDir + L"PathTraceAtmos_CS.cso");
    if (bcA.GetSize() != 0)
    {
        pd.CS = {bcA.GetData(), bcA.GetSize()};
        if (FAILED(m_device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&m_psoAtmos)))) m_psoAtmos.Reset();
    }
    return true;
}

bool PathTracer::BeginJob(const JobDesc& desc, const SceneGpu& scene, std::string* err)
{
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!IsReady()) return fail("PathTracer が未初期化");
    if (!scene.valid) return fail("シーンが未構築");
    if (desc.width == 0 || desc.height == 0 || desc.width > 16384 || desc.height > 16384) return fail("解像度が範囲外(1..16384)");
    if (desc.spp == 0) return fail("spp は 1 以上");
    if (desc.env.atmosSrvBase != kNoIndex && !m_psoAtmos) return fail("PathTraceAtmos_CS.cso が読めない(物理大気の空に要る。Shaders をビルドしたか)");
    WaitForGpu();

    m_job = desc;
    m_job.tileSize = std::clamp<uint32_t>(desc.tileSize, 16, 2048);
    m_job.samplesPerDispatch = std::clamp<uint32_t>(desc.samplesPerDispatch, 1, 256);
    m_job.bounces = std::clamp<uint32_t>(desc.bounces, 1, 64);
    m_scene = scene;
    m_samplesPerDispatch = m_job.samplesPerDispatch;

    const uint64_t accumBytes = static_cast<uint64_t>(desc.width) * desc.height * 16;
    m_accum = MakeBuffer(m_device, accumBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"PT accum");
    m_stats = MakeBuffer(m_device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"PT stats");
    m_jobCb = MakeBuffer(m_device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, L"PT job cb");
    if (!m_accum || !m_stats || !m_jobCb) return fail("累積バッファを確保できない(VRAM 不足)");
    m_readback.Reset();
    m_readbackSize = 0;

    JobConstants jc{};
    jc.width = desc.width; jc.height = desc.height; jc.bounces = m_job.bounces; jc.seed = desc.seed;
    jc.flags = desc.flags;
    jc.maxRadiance = desc.maxRadiance;
    jc.envCubeSrv = desc.env.cubeSrv;
    jc.lightCount = scene.lightCount;
    std::memcpy(jc.camPos, desc.cam.pos, 12);   jc.tanHalfFovY = desc.cam.tanHalfFovY;
    std::memcpy(jc.camRight, desc.cam.right, 12); jc.aspect = desc.cam.aspect;
    std::memcpy(jc.camUp, desc.cam.up, 12);     jc.emissiveCount = scene.emissiveCount;
    std::memcpy(jc.camFwd, desc.cam.fwd, 12);   jc.instanceCount = scene.instanceCount;
    std::memcpy(jc.sunToLight, desc.sun.toLight, 12); jc.sunTanRadius = desc.sun.tanRadius;
    std::memcpy(jc.sunE, desc.sun.E, 12);       jc.sunEnabled = desc.sun.enabled ? 1.0f : 0.0f;
    std::memcpy(jc.envConst, desc.env.uniform, 12); jc.envLightScale = desc.env.lightScale;
    jc.envBgScale = desc.env.bgScale;
    jc.rrStart = desc.rrStart;
    jc.lensRadius = desc.cam.lensRadius;
    jc.focusDist = desc.cam.focusDist;
    jc.pixelFilter = desc.pixelFilter;
    jc.countRays = desc.countStats ? 1u : 0u;
    if (desc.env.nee) jc.flags |= kFlagEnvNee; else jc.flags &= ~kFlagEnvNee;
    if (desc.env.atmosSrvBase != kNoIndex) { jc.flags |= kFlagAtmosphere; jc.atmosSrvBase = desc.env.atmosSrvBase; }
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(m_jobCb->Map(0, &none, &mapped))) return fail("ジョブ定数を Map できない");
    std::memset(mapped, 0, 256);
    std::memcpy(mapped, &jc, sizeof(jc));
    m_jobCb->Unmap(0, nullptr);

    m_tilesX = (desc.width + m_job.tileSize - 1) / m_job.tileSize;
    m_tilesY = (desc.height + m_job.tileSize - 1) / m_job.tileSize;
    m_passes = (desc.spp + m_samplesPerDispatch - 1) / m_samplesPerDispatch;
    m_passIndex = 0;
    m_tileIndex = 0;
    m_dispatches = 0;
    m_msPerUnit = 0.0;
    m_gpuMsTotal = 0.0;
    m_prevSlot = -1;
    for (Slot& s : m_slots) s = Slot{};
    m_state = State::Running;
    return true;
}

void PathTracer::HarvestTimestamps()
{
    if (!m_fence || !m_queryReadback || m_tsFreq == 0) return;
    const uint64_t completed = m_fence->GetCompletedValue();
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        Slot& s = m_slots[i];
        if (!s.used || s.marker == 0 || completed < s.marker) continue;
        uint64_t* ts = nullptr;
        D3D12_RANGE rr{i * 2 * sizeof(uint64_t), (i * 2 + 2) * sizeof(uint64_t)};
        if (SUCCEEDED(m_queryReadback->Map(0, &rr, reinterpret_cast<void**>(&ts))))
        {
            const uint64_t t0 = ts[i * 2], t1 = ts[i * 2 + 1];
            D3D12_RANGE wr{0, 0};
            m_queryReadback->Unmap(0, &wr);
            if (t1 > t0 && s.units > 0)
            {
                const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(m_tsFreq);
                m_gpuMsTotal += ms;
                const double per = ms / static_cast<double>(s.units);
                m_msPerUnit = (m_msPerUnit <= 0.0) ? per : (0.6 * m_msPerUnit + 0.4 * per);
            }
        }
        s = Slot{};
    }
}

uint32_t PathTracer::Record(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap, float budgetMs, uint32_t maxUnits)
{
    if (m_state != State::Running || !cmd) return 0;

    // 前回 Record したリストは提出済み(契約)。ここでフェンスを打って、そのスロットの完了印にする。
    if (m_prevSlot >= 0 && m_fence)
    {
        ++m_fenceValue;
        m_queue->Signal(m_fence.Get(), m_fenceValue);
        m_slots[m_prevSlot].marker = m_fenceValue;
        m_prevSlot = -1;
    }
    HarvestTimestamps();

    const uint64_t totalUnits = static_cast<uint64_t>(m_passes) * m_tilesX * m_tilesY;
    const uint64_t doneUnits = static_cast<uint64_t>(m_passIndex) * m_tilesX * m_tilesY + m_tileIndex;
    if (doneUnits >= totalUnits) { m_state = State::Finished; return 0; }

    uint32_t units;
    if (budgetMs > 0.0f)
    {
        units = (m_msPerUnit > 0.0) ? static_cast<uint32_t>(std::floor(budgetMs / m_msPerUnit)) : 1u;
        units = std::clamp<uint32_t>(units, 1u, kMaxUnitsPerCall);
    }
    else
        units = (maxUnits > 0) ? maxUnits : (m_tilesX * m_tilesY);
    units = static_cast<uint32_t>(std::min<uint64_t>(units, totalUnits - doneUnits));

    // 空きスロット(タイムスタンプ用)
    int slot = -1;
    if (m_queryHeap)
        for (uint32_t k = 0; k < kSlots; ++k)
        {
            const uint32_t i = (m_slotCursor + k) % kSlots;
            if (!m_slots[i].used) { slot = static_cast<int>(i); m_slotCursor = (i + 1) % kSlots; break; }
        }
    if (m_queryHeap && slot < 0) return 0;   // 計測待ちが詰まっている。このフレームは積まない

    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    if (srvHeap) cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetPipelineState((m_job.env.atmosSrvBase != kNoIndex && m_psoAtmos) ? m_psoAtmos.Get() : m_pso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_jobCb->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, m_scene.tlas);
    cmd->SetComputeRootShaderResourceView(3, m_scene.instances);
    cmd->SetComputeRootShaderResourceView(4, m_scene.materials);
    cmd->SetComputeRootShaderResourceView(5, m_scene.lights);
    cmd->SetComputeRootShaderResourceView(6, m_scene.emissive);
    cmd->SetComputeRootUnorderedAccessView(7, m_accum->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, m_stats->GetGPUVirtualAddress());

    if (slot >= 0) cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot) * 2);

    for (uint32_t u = 0; u < units; ++u)
    {
        const uint32_t tx = m_tileIndex % m_tilesX, ty = m_tileIndex / m_tilesX;
        TileConstants tc{};
        tc.tileX = tx * m_job.tileSize;
        tc.tileY = ty * m_job.tileSize;
        tc.tileW = std::min(m_job.tileSize, m_job.width - tc.tileX);
        tc.tileH = std::min(m_job.tileSize, m_job.height - tc.tileY);
        tc.sampleBase = m_passIndex * m_samplesPerDispatch;
        tc.sampleCount = std::min(m_samplesPerDispatch, m_job.spp - tc.sampleBase);
        cmd->SetComputeRoot32BitConstants(1, sizeof(tc) / 4, &tc, 0);
        cmd->Dispatch((tc.tileW + 7) / 8, (tc.tileH + 7) / 8, 1);
        // 同じ画素へ次パスが読み書きするので直列化(異なるタイルは独立だが、UAV バリアの費用は無視できる)。
        UavBarrier(cmd, m_accum.Get());
        ++m_dispatches;
        if (++m_tileIndex >= m_tilesX * m_tilesY) { m_tileIndex = 0; ++m_passIndex; }
    }

    if (slot >= 0)
    {
        cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot) * 2 + 1);
        cmd->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot) * 2, 2,
                              m_queryReadback.Get(), static_cast<UINT64>(slot) * 2 * sizeof(uint64_t));
        m_slots[slot].used = true;
        m_slots[slot].units = units;
        m_slots[slot].marker = 0;
        m_prevSlot = slot;
    }

    if (static_cast<uint64_t>(m_passIndex) * m_tilesX * m_tilesY + m_tileIndex >= totalUnits)
        m_state = State::Finished;   // 全ユニットを積んだ(GPU の完了はこれから。ReadbackAccum が待つ)
    return units;
}

void PathTracer::Cancel()
{
    if (m_state == State::Running) m_state = State::Finished;
}

PtProgress PathTracer::GetProgress() const
{
    PtProgress p;
    const uint64_t tiles = static_cast<uint64_t>(m_tilesX) * m_tilesY;
    const uint64_t total = tiles * m_passes;
    const uint64_t done = static_cast<uint64_t>(m_passIndex) * tiles + m_tileIndex;
    p.samplesDone = std::min(m_job.spp, m_passIndex * m_samplesPerDispatch);
    p.samplesTarget = m_job.spp;
    p.fraction = (total > 0) ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    p.gpuMsTotal = m_gpuMsTotal;
    p.msPerUnit = m_msPerUnit;
    p.dispatches = m_dispatches;
    p.running = (m_state == State::Running);
    p.finished = (m_state == State::Finished);
    return p;
}

void PathTracer::ReleaseJobBuffers()
{
    WaitForGpu();
    m_accum.Reset(); m_stats.Reset(); m_jobCb.Reset(); m_readback.Reset();
    m_readbackSize = 0;
    m_scene = SceneGpu{};
    if (m_state == State::Running) m_state = State::Finished;
}

void PathTracer::WaitForGpu()
{
    if (!m_queue || !m_fence || !m_fenceEvent) return;
    ++m_fenceValue;
    m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(m_fenceEvent));
        WaitForSingleObject(static_cast<HANDLE>(m_fenceEvent), INFINITE);
    }
}

bool PathTracer::ReadbackAccum(std::vector<float>& out, std::string* err)
{
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!m_accum || m_job.width == 0) return fail("累積バッファが無い");
    const uint64_t accumBytes = static_cast<uint64_t>(m_job.width) * m_job.height * 16;
    const uint64_t need = accumBytes + 256;
    if (!m_readback || m_readbackSize < need)
    {
        m_readback = MakeBuffer(m_device, need, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST, L"PT readback");
        m_readbackSize = need;
        if (!m_readback) return fail("リードバックバッファを確保できない");
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))
     || FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))))
        return fail("コマンドリストを作れない");

    D3D12_RESOURCE_BARRIER b[2]{};
    for (int i = 0; i < 2; ++i)
    {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    b[0].Transition.pResource = m_accum.Get();
    b[1].Transition.pResource = m_stats.Get();
    list->ResourceBarrier(2, b);
    list->CopyBufferRegion(m_readback.Get(), 0, m_accum.Get(), 0, accumBytes);
    list->CopyBufferRegion(m_readback.Get(), accumBytes, m_stats.Get(), 0, 256);
    for (int i = 0; i < 2; ++i) std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
    list->ResourceBarrier(2, b);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();

    void* mapped = nullptr;
    D3D12_RANGE rr{0, static_cast<SIZE_T>(need)};
    if (FAILED(m_readback->Map(0, &rr, &mapped))) return fail("リードバックを Map できない");
    out.resize(static_cast<size_t>(m_job.width) * m_job.height * 4);
    std::memcpy(out.data(), mapped, static_cast<size_t>(accumBytes));
    D3D12_RANGE wr{0, 0};
    m_readback->Unmap(0, &wr);
    return true;
}

bool PathTracer::ReadbackAverage(std::vector<float>& rgb, uint32_t* samples, std::string* err)
{
    std::vector<float> acc;
    if (!ReadbackAccum(acc, err)) return false;
    const size_t n = static_cast<size_t>(m_job.width) * m_job.height;
    rgb.resize(n * 3);
    float cnt = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        const float c = acc[i * 4 + 3];
        cnt = std::max(cnt, c);
        const float inv = (c > 0.0f) ? 1.0f / c : 0.0f;
        rgb[i * 3 + 0] = acc[i * 4 + 0] * inv;
        rgb[i * 3 + 1] = acc[i * 4 + 1] * inv;
        rgb[i * 3 + 2] = acc[i * 4 + 2] * inv;
    }
    if (samples) *samples = static_cast<uint32_t>(cnt + 0.5f);
    return true;
}

bool PathTracer::ReadbackStats(PtStatsOut& out)
{
    if (!m_readback || !m_stats) return false;
    const uint64_t accumBytes = static_cast<uint64_t>(m_job.width) * m_job.height * 16;
    void* mapped = nullptr;
    D3D12_RANGE rr{static_cast<SIZE_T>(accumBytes), static_cast<SIZE_T>(accumBytes + 256)};
    if (FAILED(m_readback->Map(0, &rr, &mapped))) return false;
    const uint32_t* s = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) + accumBytes);
    out.nanSamples = s[0];
    out.paths = static_cast<uint64_t>(s[1]) | (static_cast<uint64_t>(s[4]) << 32);
    out.clampedSamples = s[2];
    out.rrTerminated = s[3];
    D3D12_RANGE wr{0, 0};
    m_readback->Unmap(0, &wr);
    return true;
}

} // namespace dx12e::pt
