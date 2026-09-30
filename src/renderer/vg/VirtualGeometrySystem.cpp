#include "renderer/vg/VirtualGeometrySystem.h"

#include "core/Logger.h"
#include "graphics/MeshPipelineState.h"
#include "renderer/vg/VgeoFormat.h"
#include "resource/ShaderCompiler.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace dx12e::vg
{
using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint32_t kCbSlotBytes    = 768;      // VgCullConstants(560 B)を 256 整列で
static_assert(sizeof(VgCullConstants) <= kCbSlotBytes, "VgCullConstants が CB のスロットに収まらない");
constexpr uint32_t kMaxFrameSlots  = 8;
constexpr uint32_t kStatsReadbackBytes = (VG_CNT_COUNT + VG_STAT_COUNT) * 4u;   // カウンタ + 統計（連続領域）

// 固定ディスクリプタの配置（VgCommon.hlsli の gHeap* と一致させる）。★絶対添字は m_fixedBase + これ（D(k)）
constexpr uint32_t kDescWorkUav  = 0;
constexpr uint32_t kDescExtraUav = 1;
constexpr uint32_t kDescArgsUav  = 2;
constexpr uint32_t kDescMaterials = 3;                // P4: 材質表（StructuredBuffer<VgMaterialGpu>）
constexpr uint32_t kDescFrameBase = 4;                // フレームスロット f: kDescFrameBase + 8 * f + {assets, instances, hzbPrev, hzbCur, depthSrv, visSrv, overdrawUav, visUav}
constexpr uint32_t kDescPerFrame = 8;
constexpr uint32_t kQueriesPerSlot = 10;              // 0 開始 / 1,2 一相目ラスタの前後 / 3,4 二相目ラスタの前後 / 5 終了 / P4: 6,7 G-Buffer の前後 / 8,9 resolve の前後
constexpr DXGI_FORMAT kVisFormat = DXGI_FORMAT_R32_UINT;

HRESULT MakeBuffer(ID3D12Device* dev, uint64_t bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                   D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heapType;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out));
    if (SUCCEEDED(hr) && name) out->SetName(name);
    return hr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return x;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    x.UAV.pResource = r;
    return x;
}

std::string HrStr(HRESULT hr)
{
    char b[32];
    std::snprintf(b, sizeof(b), "0x%08X", static_cast<unsigned>(hr));
    return b;
}

// .cso を読む。★ShaderCompiler::LoadFromFile はファイルが無いと例外を投げる（捕まえないとプロセスごと落ちる）ので、
//   VG では「読めない = その機能を無効」に縮退させるために空で返す。
ShaderCompiler::ShaderBytecode LoadCsoNoThrow(const std::wstring& path)
{
    try { return ShaderCompiler::LoadFromFile(path); }
    catch (const std::exception&) { return ShaderCompiler::ShaderBytecode{}; }
}

// アセット相対の BVH 深さ（根 = 0）。子ノードの番号は親より大きい（VGEO_SPEC C10）ので前から 1 回で足りる。
uint32_t ComputeBvhDepth(const std::vector<HierNode>& nodes)
{
    if (nodes.empty()) return 0;
    std::vector<uint32_t> depth(nodes.size(), 0);
    uint32_t best = 0;
    for (size_t i = 0; i < nodes.size(); ++i)
        for (int c = 0; c < 4; ++c)
        {
            const uint32_t ref = nodes[i].child[c].ref;
            if (ref == kNone || NodeRefIsLeaf(ref) || ref >= nodes.size()) continue;
            depth[ref] = std::max(depth[ref], depth[i] + 1);
            best = std::max(best, depth[ref]);
        }
    return best;
}
} // namespace

const char* LoadCodeName(LoadCode c)
{
    switch (c)
    {
    case LoadCode::Ok: return "Ok";
    case LoadCode::OpenFailed: return "OpenFailed";
    case LoadCode::Invalid: return "Invalid";
    case LoadCode::Unsupported: return "Unsupported";
    case LoadCode::OverBudget: return "OverBudget";
    case LoadCode::TooManyAssets: return "TooManyAssets";
    case LoadCode::DeviceFailure: return "DeviceFailure";
    case LoadCode::NotInitialized: return "NotInitialized";
    }
    return "?";
}

// ── アップロード用（COPY キュー + ステージング 2 面）─────────────────────────────
// 1 アセットずつ直列（m_up->mtx）。ステージングを 2 面で交互に使い、CPU のファイル読みと GPU のコピーを重ねる。
struct VirtualGeometrySystem::Uploader
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[2];
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fenceValue = 0;
    uint64_t sliceDone[2] = {0, 0};
    ComPtr<ID3D12Resource> staging[2];
    uint8_t* mapped[2] = {nullptr, nullptr};
    uint64_t sliceBytes = 0;
    HANDLE event = nullptr;
    std::mutex mtx;
    bool open = false;      // list が Reset 済みで記録中か

    bool Init(ID3D12Device* dev, uint64_t sliceB, std::string* err)
    {
        sliceBytes = sliceB;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr)) { if (err) *err = "COPY キューを作れない " + HrStr(hr); return false; }
        for (int i = 0; i < 2; ++i)
            if (FAILED(hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&alloc[i]))))
            { if (err) *err = "COPY アロケータ " + HrStr(hr); return false; }
        if (FAILED(hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, alloc[0].Get(), nullptr, IID_PPV_ARGS(&list))))
        { if (err) *err = "COPY コマンドリスト " + HrStr(hr); return false; }
        list->Close();
        if (FAILED(hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        { if (err) *err = "フェンス " + HrStr(hr); return false; }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        for (int i = 0; i < 2; ++i)
        {
            if (FAILED(hr = MakeBuffer(dev, sliceBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, staging[i], L"VgUploadStaging")))
            { if (err) *err = "ステージング " + HrStr(hr); return false; }
            D3D12_RANGE nr{0, 0};
            void* p = nullptr;
            if (FAILED(hr = staging[i]->Map(0, &nr, &p))) { if (err) *err = "ステージング Map " + HrStr(hr); return false; }
            mapped[i] = static_cast<uint8_t*>(p);
        }
        return true;
    }

    void WaitFence(uint64_t v)
    {
        if (v == 0 || fence->GetCompletedValue() >= v) return;
        fence->SetEventOnCompletion(v, event);
        WaitForSingleObject(event, INFINITE);
    }

    void Shutdown()
    {
        WaitFence(fenceValue);
        for (int i = 0; i < 2; ++i) if (staging[i] && mapped[i]) staging[i]->Unmap(0, nullptr);
        if (event) CloseHandle(event);
        event = nullptr;
    }

    // dst の [dstOffset, dstOffset + total) へ、fill(srcOffset, out, n) が作るバイト列を送る。
    // fill は false を返したら失敗（部分的に送った分は捨てる）。
    template <class Fill>
    bool Upload(ID3D12Resource* dst, uint64_t dstOffset, uint64_t total, Fill&& fill, std::string* err)
    {
        uint64_t done = 0;
        int k = 0;
        while (done < total)
        {
            const uint64_t n = std::min<uint64_t>(sliceBytes, total - done);
            WaitFence(sliceDone[k]);              // このステージング面が空くまで
            if (FAILED(alloc[k]->Reset())) { if (err) *err = "COPY アロケータ Reset"; return false; }
            if (FAILED(list->Reset(alloc[k].Get(), nullptr))) { if (err) *err = "COPY リスト Reset"; return false; }
            if (!fill(done, mapped[k], n)) { list->Close(); if (err && err->empty()) *err = "ファイル読みに失敗"; return false; }
            list->CopyBufferRegion(dst, dstOffset + done, staging[k].Get(), 0, n);
            if (FAILED(list->Close())) { if (err) *err = "COPY リスト Close"; return false; }
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            ++fenceValue;
            queue->Signal(fence.Get(), fenceValue);
            sliceDone[k] = fenceValue;
            done += n;
            k ^= 1;
        }
        return true;
    }
    void Flush() { WaitFence(fenceValue); }
};

// ── 初期化 ───────────────────────────────────────────────────────────────────
VirtualGeometrySystem::VirtualGeometrySystem() = default;
VirtualGeometrySystem::~VirtualGeometrySystem() { Shutdown(); }

D3D12_CPU_DESCRIPTOR_HANDLE VirtualGeometrySystem::CpuHandle(uint32_t i) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heapRaw->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * m_descSize;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE VirtualGeometrySystem::GpuHandle(uint32_t i) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_heapRaw->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(i) * m_descSize;
    return h;
}

// ディスクリプタのブロック確保（専用ヒープ = 内部の IndexAllocator / externalHeap = アプリのヒープのコールバック）
uint32_t VirtualGeometrySystem::AllocDescBlock(uint32_t count)
{
    std::lock_guard<std::mutex> lk(m_descMutex);
    uint32_t base = IndexAllocator::kInvalid;
    if (m_extHeap) base = m_desc.externalHeap.allocBlock ? m_desc.externalHeap.allocBlock(count) : IndexAllocator::kInvalid;
    else           base = m_descAlloc.AllocateBlock(count);
    if (base != IndexAllocator::kInvalid) m_descInUse += count;
    return base;
}
void VirtualGeometrySystem::FreeDescBlock(uint32_t base, uint32_t count)
{
    if (base == IndexAllocator::kInvalid || count == 0) return;
    std::lock_guard<std::mutex> lk(m_descMutex);
    if (m_extHeap) { if (m_desc.externalHeap.freeBlock) m_desc.externalHeap.freeBlock(base, count); }
    else           m_descAlloc.FreeBlock(base, count);
    m_descInUse = (m_descInUse >= count) ? m_descInUse - count : 0;
}

uint32_t VirtualGeometrySystem::DescriptorsInUse() const { return m_descInUse; }

bool VirtualGeometrySystem::Initialize(const SystemDesc& desc, std::string* err)
{
    if (m_ready) return true;
    auto fail = [&](const std::string& m) { if (err) *err = m; Logger::Warn("VG: 初期化に失敗: {}", m); Shutdown(); return false; };
    if (!desc.device) return fail("device が null");
    m_desc = desc;
    m_desc.frameCount = std::clamp<uint32_t>(desc.frameCount, 1, kMaxFrameSlots);
    m_desc.maxInstances = std::clamp<uint32_t>(desc.maxInstances, 1, 1u << 20);
    m_desc.maxAssets = std::clamp<uint32_t>(desc.maxAssets, 1, 4096);
    m_device = desc.device;
    m_vramBudgetMB = desc.vramBudgetMB;

    // SM 6.6 + Resource Binding Tier 3（Dynamic Resources）の確認。
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
            return fail("シェーダモデル 6.6 が使えない");
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) || o.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3)
            return fail("リソースバインディング Tier 3 が使えない");
    }

    if (desc.directQueue)
    {
        if (FAILED(desc.directQueue->GetTimestampFrequency(&m_tsFreq))) m_tsFreq = 0;
    }

    std::string e;
    if (!CreatePipelines(&e)) return fail(e);
    if (!CreateBuffers(&e)) return fail(e);
    if (desc.enableRaster) CreateRasterPipelines();     // 失敗は非致命（m_rasterOk = false のままカリングだけ動く）

    m_up = std::make_unique<Uploader>();
    if (!m_up->Init(m_device.Get(), static_cast<uint64_t>(std::max<uint32_t>(desc.uploadSliceMB, 1)) << 20, &e)) return fail(e);

    m_quit = false;
    if (desc.asyncLoad) m_worker = std::thread([this] { WorkerMain(); });
    m_ready = true;
    Logger::Info("VG: 初期化した（インスタンス上限 {} / アセット上限 {} / VRAM 予算 {} MB / 作業バッファ {} MB / メッシュシェーダ ラスタ {}）",
                 m_desc.maxInstances, m_desc.maxAssets, m_vramBudgetMB, m_layout.totalBytes >> 20, m_rasterOk ? "使用可" : "使用不可");
    return true;
}

bool VirtualGeometrySystem::CreatePipelines(std::string* err)
{
    // ルートシグネチャ: b0 = ルート CBV / b1 = 32bit 定数 8 個。バインドレス（HEAP_DIRECTLY_INDEXED）。
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = 8;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2;
    rd.pParameters = params;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, eb;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &eb);
    if (FAILED(hr)) { if (err) *err = std::string("ルートシグネチャのシリアライズ: ") + (eb ? static_cast<const char*>(eb->GetBufferPointer()) : HrStr(hr)); return false; }
    hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
    if (FAILED(hr)) { if (err) *err = "ルートシグネチャ " + HrStr(hr); return false; }

    auto make = [&](const wchar_t* cso, ComPtr<ID3D12PipelineState>& out) -> bool
    {
        auto bc = LoadCsoNoThrow(m_desc.shaderDir + cso);
        if (bc.GetSize() == 0) { if (err) *err = "シェーダが読めない（未ビルド？）"; return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = {bc.GetData(), bc.GetSize()};
        const HRESULT h = m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out));
        if (FAILED(h)) { if (err) *err = std::string("PSO ") + HrStr(h); return false; }
        return true;
    };
    if (!make(L"VgReset_CS.cso", m_psoReset) || !make(L"VgArgs_CS.cso", m_psoArgs) || !make(L"VgInstanceCull_CS.cso", m_psoInstance) ||
        !make(L"VgNodeTraverse_CS.cso", m_psoTraverse) || !make(L"VgClusterCull_CS.cso", m_psoCluster))
        return false;
    // P4: 安定ソート（決定論）。無くても VG は動く（stableOrder が効かないだけ）。
    {
        std::string e2;
        std::string* keep = err;
        err = &e2;
        if (!make(L"VgSortStep_CS.cso", m_psoSort))
        {
            m_psoSort.Reset();
            Logger::Warn("VG: 安定ソートのシェーダ（VgSortStep_CS.cso）が使えない（{}）。stableOrder は無効", e2);
        }
        err = keep;
    }

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC cs{};
    cs.ByteStride = VG_ARGS_STRIDE;
    cs.NumArgumentDescs = 1;
    cs.pArgumentDescs = &arg;
    hr = m_device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&m_cmdSigDispatch));
    if (FAILED(hr)) { if (err) *err = "コマンドシグネチャ " + HrStr(hr); return false; }
    return true;
}

// P3: メッシュシェーダのラスタ（MS / AS / PS の PSO・引数生成・コマンドシグネチャ）。非致命。
//   ★同じ専用ルートシグネチャ（b0 CBV + b1 定数 8 個 / HEAP_DIRECTLY_INDEXED）をグラフィックスでもそのまま使う。
bool VirtualGeometrySystem::CreateRasterPipelines()
{
    m_rasterOk = false;
    // メッシュシェーダ Tier 1 + SM 6.5。DX12_DISABLE_MESHSHADER=1 で非対応として扱う（縮退検証用）。
    {
        char buf[8]{};
        size_t len = 0;
        if (::getenv_s(&len, buf, sizeof(buf), "DX12_DISABLE_MESHSHADER") == 0 && len > 0 && buf[0] == '1')
        {
            Logger::Warn("VG: DX12_DISABLE_MESHSHADER=1 のためラスタを無効にする");
            return false;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
        if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))) || o7.MeshShaderTier < D3D12_MESH_SHADER_TIER_1)
        {
            Logger::Info("VG: メッシュシェーダ非対応の GPU（ラスタ無効。プロキシが従来経路で描かれる）");
            return false;
        }
    }
    auto load = [&](const wchar_t* cso, std::vector<uint8_t>& out) -> bool
    {
        auto bc = LoadCsoNoThrow(m_desc.shaderDir + cso);
        if (bc.GetSize() == 0) return false;
        out = std::move(bc.data);
        return true;
    };
    if (!load(L"VgRaster_MS.cso", m_bcMsPlain) || !load(L"VgRasterAs_MS.cso", m_bcMsAs) || !load(L"VgRaster_AS.cso", m_bcAs) ||
        !load(L"VgRaster_PS.cso", m_bcPs) || !load(L"VgRasterCount_PS.cso", m_bcPsCount) ||
        !load(L"VgDebug_VS.cso", m_bcDbgVs) || !load(L"VgDebug_PS.cso", m_bcDbgPs))
    {
        Logger::Warn("VG: ラスタのシェーダ（VgRaster_*.cso / VgDebug_*.cso）が読めない（未ビルド？）。ラスタ無効");
        return false;
    }
    // P4: G-Buffer / resolve（無ければその機能だけ無効。resolve が無いとアプリは暫定シェーディングへ縮退する）
    if (!load(L"VgGBuffer_VS.cso", m_bcGbVs) || !load(L"VgGBuffer_PS.cso", m_bcGbPs))
    {
        m_bcGbVs.clear(); m_bcGbPs.clear();
        Logger::Warn("VG: G-Buffer のシェーダ（VgGBuffer_*.cso）が読めない。VG 画素の速度 / G-Buffer は書かれない");
    }
    if (!load(L"VgResolve_VS.cso", m_bcResolveVs) || !load(L"VgResolve_PS.cso", m_bcResolvePs))
    {
        m_bcResolveVs.clear(); m_bcResolvePs.clear();
        Logger::Warn("VG: resolve のシェーダ（VgResolve_*.cso）が読めない。VG は暫定シェーディングで描かれる");
    }
    auto makeCs = [&](const wchar_t* cso, ComPtr<ID3D12PipelineState>& out) -> bool
    {
        auto bc = LoadCsoNoThrow(m_desc.shaderDir + cso);
        if (bc.GetSize() == 0) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = {bc.GetData(), bc.GetSize()};
        return SUCCEEDED(m_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    if (!makeCs(L"VgRasterArgs_CS.cso", m_psoRasterArgs) || !makeCs(L"VgCoverage_CS.cso", m_psoCoverage))
    {
        Logger::Warn("VG: ラスタの補助カーネル（VgRasterArgs / VgCoverage）が作れない。ラスタ無効");
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    D3D12_COMMAND_SIGNATURE_DESC cs{};
    cs.ByteStride = VG_ARGS_STRIDE;
    cs.NumArgumentDescs = 1;
    cs.pArgumentDescs = &arg;
    HRESULT hr = m_device->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&m_cmdSigMesh));
    if (FAILED(hr)) { Logger::Warn("VG: DispatchMesh のコマンドシグネチャが作れない {}。ラスタ無効", HrStr(hr)); return false; }

    // RTV ヒープ（可視性バッファ）と UAV クリア用の CPU ヒープ（0 = 可視性バッファ、1 = オーバードロー画像）
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC ch{};
    ch.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ch.NumDescriptors = 2;
    if (FAILED(m_device->CreateDescriptorHeap(&ch, IID_PPV_ARGS(&m_clearHeap)))) return false;

    // 既定の PSO（MS のみ・計測なし）を作って、この GPU / ドライバで通ることを確かめる
    if (!GetRasterPso(false, false)) return false;
    m_rasterOk = true;
    return true;
}

ID3D12PipelineState* VirtualGeometrySystem::GetRasterPso(bool useAs, bool count)
{
    ComPtr<ID3D12PipelineState>& slot = m_psoRaster[useAs ? 1 : 0][count ? 1 : 0];
    if (slot) return slot.Get();
    MeshPipelineStateBuilder b;
    b.SetRootSignature(m_rootSig.Get());
    const auto& ms = useAs ? m_bcMsAs : m_bcMsPlain;
    const auto& ps = count ? m_bcPsCount : m_bcPs;
    if (useAs) b.SetAmplificationShader(m_bcAs.data(), m_bcAs.size());
    b.SetMeshShader(ms.data(), ms.size()).SetPixelShader(ps.data(), ps.size())
     .SetRenderTargetFormat(kVisFormat).SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
     .SetDepthFunc(D3D12_COMPARISON_FUNC_LESS).SetCullMode(D3D12_CULL_MODE_NONE);
    std::string err;
    slot = b.Build(m_device.Get(), &err);
    if (!slot) Logger::Warn("VG: ラスタの PSO を作れない（AS={} count={}）: {}", useAs, count, err);
    return slot.Get();
}

// 可視性バッファ（R32_UINT）とオーバードロー画像を用意する。寸法が変わったら作り直す（古い物は GPU の使用が終わるまで保持）。
bool VirtualGeometrySystem::EnsureVisBuffers(uint32_t w, uint32_t h, bool overdraw)
{
    if (w == 0 || h == 0) return false;
    const bool needVis = !m_vis || m_visW != w || m_visH != h;
    const bool needOd = overdraw && (!m_overdraw || needVis);
    if (!needVis && !needOd) return true;

    auto makeTex = [&](ComPtr<ID3D12Resource>& out, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, const wchar_t* name) -> bool
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = kVisFormat; d.SampleDesc = {1, 0}; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = kVisFormat;
        cv.Color[0] = cv.Color[1] = cv.Color[2] = cv.Color[3] = 0.0f;
        const bool rt = (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, rt ? &cv : nullptr, IID_PPV_ARGS(&out)))) return false;
        out->SetName(name);
        return true;
    };
    if (needVis)
    {
        if (m_vis) m_retired.push_back({m_vis, m_overdraw, m_frameCounter});
        else if (m_overdraw) m_retired.push_back({nullptr, m_overdraw, m_frameCounter});
        m_vis.Reset(); m_overdraw.Reset();
        if (!makeTex(m_vis, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"VgVisibility"))
        { m_visW = m_visH = 0; return false; }
        m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_visW = w; m_visH = h;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = kVisFormat;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_vis.Get(), &rv, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = kVisFormat;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE c0 = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateUnorderedAccessView(m_vis.Get(), nullptr, &uv, c0);
        m_fixedVram += static_cast<uint64_t>(w) * h * 4;
    }
    if (overdraw && !m_overdraw)
    {
        if (!makeTex(m_overdraw, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"VgOverdraw")) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = kVisFormat;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE c1 = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
        c1.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_device->CreateUnorderedAccessView(m_overdraw.Get(), nullptr, &uv, c1);
    }
    return true;
}

bool VirtualGeometrySystem::CreateBuffers(std::string* err)
{
    ID3D12Device* dev = m_device.Get();
    m_layout = MakeWorkLayout(m_desc.nodeQueueCap, m_desc.groupQueueCap, m_desc.visibleCap, m_desc.deferredCap);

    // ディスクリプタヒープ: P4 の externalHeap（アプリの SRV ヒープ）があればそこへ、無ければ専用ヒープ（テスト用）
    HRESULT hr = S_OK;
    m_descSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_descInUse = 0;
    if (m_desc.externalHeap.heap && m_desc.externalHeap.allocBlock && m_desc.externalHeap.freeBlock)
    {
        m_extHeap = true;
        m_heapRaw = m_desc.externalHeap.heap;
        m_fixedBase = AllocDescBlock(kFixedDescs);
        if (m_fixedBase == IndexAllocator::kInvalid) { m_extHeap = false; m_heapRaw = nullptr; if (err) *err = "アプリの SRV ヒープに VG の固定領域（128 個）を確保できない"; return false; }
    }
    else
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kHeapCapacity;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap));
        if (FAILED(hr)) { if (err) *err = "ディスクリプタヒープ " + HrStr(hr); return false; }
        m_heapRaw = m_heap.Get();
        m_extHeap = false;
        m_descAlloc.Initialize(kHeapCapacity);
        m_fixedBase = AllocDescBlock(kFixedDescs);   // 先頭の固定領域を予約（= 0）
    }

    auto chk = [&](HRESULT h, const char* what) -> bool { if (FAILED(h)) { if (err) *err = std::string(what) + " " + HrStr(h); return false; } return true; };

    // 作業バッファ / 引数 / インスタンス補助
    if (!chk(MakeBuffer(dev, m_layout.totalBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_work, L"VgWork"), "VgWork")) return false;
    if (!chk(MakeBuffer(dev, static_cast<uint64_t>(m_desc.maxInstances) * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_extra, L"VgInstExtra"), "VgInstExtra")) return false;
    if (!chk(MakeBuffer(dev, VG_ARGS_STRIDE * VG_ARGS_SLOTS, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, m_args, L"VgArgs"), "VgArgs")) return false;
    m_argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_fixedVram = m_layout.totalBytes + static_cast<uint64_t>(m_desc.maxInstances) * 16 + VG_ARGS_STRIDE * VG_ARGS_SLOTS;

    {   // work UAV（raw）
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = static_cast<UINT>(m_layout.totalBytes / 4);
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(m_work.Get(), nullptr, &u, CpuHandle(D(kDescWorkUav)));
        D3D12_UNORDERED_ACCESS_VIEW_DESC a{};
        a.Format = DXGI_FORMAT_R32_TYPELESS;
        a.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        a.Buffer.NumElements = VG_ARGS_STRIDE * VG_ARGS_SLOTS / 4;
        a.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        dev->CreateUnorderedAccessView(m_args.Get(), nullptr, &a, CpuHandle(D(kDescArgsUav)));
        D3D12_UNORDERED_ACCESS_VIEW_DESC x{};
        x.Format = DXGI_FORMAT_UNKNOWN;
        x.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        x.Buffer.NumElements = m_desc.maxInstances;
        x.Buffer.StructureByteStride = 16;
        dev->CreateUnorderedAccessView(m_extra.Get(), nullptr, &x, CpuHandle(D(kDescExtraUav)));
    }

    // P4: 材質表（UPLOAD。アセットの登録時だけ CPU が書く）
    {
        m_desc.maxMaterials = std::max<uint32_t>(m_desc.maxMaterials, 16);
        const uint64_t bytes = static_cast<uint64_t>(m_desc.maxMaterials) * sizeof(VgMaterialGpu);
        if (!chk(MakeBuffer(dev, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, m_matBuf, L"VgMaterials"), "VgMaterials")) return false;
        D3D12_RANGE nr{0, 0};
        void* p = nullptr;
        if (!chk(m_matBuf->Map(0, &nr, &p), "VgMaterials Map")) return false;
        m_matMapped = static_cast<uint8_t*>(p);
        std::memset(m_matMapped, 0, static_cast<size_t>(bytes));
        m_matAlloc.Initialize(m_desc.maxMaterials);
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.Buffer.NumElements = m_desc.maxMaterials;
        s.Buffer.StructureByteStride = sizeof(VgMaterialGpu);
        dev->CreateShaderResourceView(m_matBuf.Get(), &s, CpuHandle(D(kDescMaterials)));
        m_fixedVram += bytes;
    }

    for (uint32_t f = 0; f < m_desc.frameCount; ++f)
    {
        if (!chk(MakeBuffer(dev, static_cast<uint64_t>(m_desc.maxInstances) * sizeof(VgInstance), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, m_instBuf[f], L"VgInstances"), "VgInstances")) return false;
        if (!chk(MakeBuffer(dev, static_cast<uint64_t>(m_desc.maxAssets) * sizeof(VgAssetGpu), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, m_assetBuf[f], L"VgAssets"), "VgAssets")) return false;
        if (!chk(MakeBuffer(dev, kCbSlotBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, m_cbBuf[f], L"VgCullCB"), "VgCullCB")) return false;
        if (!chk(MakeBuffer(dev, kStatsReadbackBytes + 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_statsReadback[f], L"VgStatsReadback"), "VgStatsReadback")) return false;
        D3D12_RANGE nr{0, 0};
        void* p = nullptr;
        m_instBuf[f]->Map(0, &nr, &p);  m_instMapped[f] = static_cast<uint8_t*>(p);
        m_assetBuf[f]->Map(0, &nr, &p); m_assetMapped[f] = static_cast<uint8_t*>(p);
        m_cbBuf[f]->Map(0, &nr, &p);    m_cbMapped[f] = static_cast<uint8_t*>(p);
        m_fixedVram += static_cast<uint64_t>(m_desc.maxInstances) * sizeof(VgInstance) + static_cast<uint64_t>(m_desc.maxAssets) * sizeof(VgAssetGpu) + kCbSlotBytes;

        const uint32_t base = D(kDescFrameBase + kDescPerFrame * f);
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.Buffer.NumElements = m_desc.maxAssets;
        s.Buffer.StructureByteStride = sizeof(VgAssetGpu);
        dev->CreateShaderResourceView(m_assetBuf[f].Get(), &s, CpuHandle(base + 0));
        s.Buffer.NumElements = m_desc.maxInstances;
        s.Buffer.StructureByteStride = sizeof(VgInstance);
        dev->CreateShaderResourceView(m_instBuf[f].Get(), &s, CpuHandle(base + 1));
        // HZB は Execute で張る（無効時のため null SRV を置いておく）
        D3D12_SHADER_RESOURCE_VIEW_DESC h{};
        h.Format = DXGI_FORMAT_R32_FLOAT;
        h.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        h.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        h.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &h, CpuHandle(base + 2));
        dev->CreateShaderResourceView(nullptr, &h, CpuHandle(base + 3));
    }
    if (!chk(MakeBuffer(dev, static_cast<uint64_t>(m_desc.visibleCap) * 8 + 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_dumpReadback, L"VgDumpReadback"), "VgDumpReadback")) return false;

    // タイムスタンプ（フレームスロットごとに 2 個）
    if (m_tsFreq != 0)
    {
        D3D12_QUERY_HEAP_DESC q{};
        q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        q.Count = m_desc.frameCount * kQueriesPerSlot;
        if (SUCCEEDED(dev->CreateQueryHeap(&q, IID_PPV_ARGS(&m_queryHeap))))
        {
            if (FAILED(MakeBuffer(dev, static_cast<uint64_t>(q.Count) * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, m_queryReadback, L"VgTimestamps")))
                m_queryHeap.Reset();
        }
    }
    return true;
}

void VirtualGeometrySystem::Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_quit = true;
    }
    m_jobCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_up) { m_up->Shutdown(); m_up.reset(); }
    for (uint32_t f = 0; f < kMaxFrameSlots; ++f)
    {
        if (m_instBuf[f] && m_instMapped[f]) m_instBuf[f]->Unmap(0, nullptr);
        if (m_assetBuf[f] && m_assetMapped[f]) m_assetBuf[f]->Unmap(0, nullptr);
        if (m_cbBuf[f] && m_cbMapped[f]) m_cbBuf[f]->Unmap(0, nullptr);
        m_instMapped[f] = m_assetMapped[f] = m_cbMapped[f] = nullptr;
        m_instBuf[f].Reset(); m_assetBuf[f].Reset(); m_cbBuf[f].Reset(); m_statsReadback[f].Reset();
    }
    // アセットのディスクリプタ（externalHeap のときはアプリのヒープへ返す。呼び出し側は GPU の完了を待ってから Shutdown すること）
    for (auto& up : m_assets)
        if (up && up->descBase != 0xFFFFFFFFu) { FreeDescBlock(up->descBase, up->descCount); up->descBase = 0xFFFFFFFFu; }
    m_assets.clear();
    m_pathToId.clear();
    if (m_heapRaw && m_fixedBase != IndexAllocator::kInvalid) FreeDescBlock(m_fixedBase, kFixedDescs);
    m_fixedBase = 0;
    if (m_matBuf && m_matMapped) m_matBuf->Unmap(0, nullptr);
    m_matMapped = nullptr; m_matBuf.Reset();
    m_psoSort.Reset(); m_psoGBuffer.Reset(); m_psoResolve.Reset(); m_psoResolveRs = nullptr;
    m_bcGbVs.clear(); m_bcGbPs.clear(); m_bcResolveVs.clear(); m_bcResolvePs.clear();
    m_work.Reset(); m_extra.Reset(); m_args.Reset(); m_dumpReadback.Reset();
    m_queryHeap.Reset(); m_queryReadback.Reset();
    m_psoReset.Reset(); m_psoArgs.Reset(); m_psoInstance.Reset(); m_psoTraverse.Reset(); m_psoCluster.Reset();
    m_psoRasterArgs.Reset(); m_psoCoverage.Reset(); m_psoDebug.Reset(); m_cmdSigMesh.Reset();
    for (auto& r : m_psoRaster) for (auto& p : r) p.Reset();
    m_vis.Reset(); m_overdraw.Reset(); m_retired.clear(); m_rtvHeap.Reset(); m_clearHeap.Reset();
    m_visW = m_visH = 0; m_rasterOk = false;
    for (uint32_t f = 0; f < kMaxFrameSlots; ++f) { m_visValid[f] = false; m_rasterOn[f] = false; m_tsRaster[f] = false; m_tsGb[f] = false; m_tsRv[f] = false; }
    m_cmdSigDispatch.Reset(); m_rootSig.Reset(); m_heap.Reset(); m_heapRaw = nullptr; m_extHeap = false;
    m_device.Reset();
    m_vramUsed = 0;
    m_pending = 0;
    m_jobs.clear();
    m_ready = false;
}

// ── アセットの読込 ───────────────────────────────────────────────────────────
uint32_t VirtualGeometrySystem::AllocAssetSlot(const std::string& path, LoadError* err)
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    if (!path.empty())
    {
        auto it = m_pathToId.find(path);
        if (it != m_pathToId.end()) return it->second;
    }
    // 解放済み（state 4）のスロットを再利用する（長いセッションで上限に達しないように）
    uint32_t slot = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < m_assets.size(); ++i)
        if (m_assets[i]->state.load() == 4) { slot = i; break; }
    if (slot == 0xFFFFFFFFu)
    {
        if (m_assets.size() >= m_desc.maxAssets)
        {
            if (err) { err->code = LoadCode::TooManyAssets; err->message = "アセット数の上限（" + std::to_string(m_desc.maxAssets) + "）"; }
            return 0xFFFFFFFFu;
        }
        slot = static_cast<uint32_t>(m_assets.size());
        m_assets.push_back(nullptr);
    }
    auto a = std::make_unique<Asset>();
    a->path = path;
    a->info.path = path;
    a->info.id = slot;
    a->lastUse = m_frameCounter;
    m_assets[slot] = std::move(a);
    if (!path.empty()) m_pathToId[path] = slot;
    return slot;
}

LoadError VirtualGeometrySystem::LoadImpl(const ByteSource& src, Asset& a)
{
    LoadError le;
    auto bad = [&](LoadCode c, std::string m) { le.code = c; le.message = std::move(m); return le; };

    ReadOptions ro;
    ro.verifyCrc = true;        // ヘッダ / 小セクションの CRC は常に検証（安い）
    VgeoMeta meta;
    const VgeoError me = LoadMeta(src, meta, ro);
    if (!me.ok()) return bad(LoadCode::Invalid, me.ToString());
    const VgeoHeader& h = meta.header;
    if (h.flags & kFlagCompressedPages) return bad(LoadCode::Unsupported, "圧縮ページは未対応（P5）");
    if (h.clusterCount == 0 || h.pageCount == 0) return bad(LoadCode::Invalid, "クラスタが 0 個のアセットは VG で使えない");

    // VRAM 予算
    const uint64_t pageBytes = static_cast<uint64_t>(h.pageCount) * kVgPageBytes;
    const uint64_t nodeBytes = static_cast<uint64_t>(meta.nodes.size()) * sizeof(HierNode);
    const uint64_t need = pageBytes + nodeBytes;
    const uint64_t budget = static_cast<uint64_t>(m_vramBudgetMB) << 20;
    {
        const uint64_t already = m_vramUsed.fetch_add(need);
        a.reservedBytes = need;
        if (already + need + m_fixedVram > budget)
        {
            m_vramUsed.fetch_sub(need);
            a.reservedBytes = 0;
            char b[256];
            std::snprintf(b, sizeof(b), "VRAM 予算 %u MB を超える（このアセット %.1f MB + 使用中 %.1f MB + 固定 %.1f MB）",
                          m_vramBudgetMB, need / 1048576.0, already / 1048576.0, m_fixedVram / 1048576.0);
            return bad(LoadCode::OverBudget, b);
        }
    }
    auto release = [&]() { if (a.reservedBytes) { m_vramUsed.fetch_sub(a.reservedBytes); a.reservedBytes = 0; } };

    // GPU リソース
    const uint32_t chunkCount = (h.pageCount + kVgChunkPages - 1) / kVgChunkPages;
    a.chunks.resize(chunkCount);
    for (uint32_t c = 0; c < chunkCount; ++c)
    {
        const uint32_t pages = std::min<uint32_t>(kVgChunkPages, h.pageCount - c * kVgChunkPages);
        const HRESULT hr = MakeBuffer(m_device.Get(), static_cast<uint64_t>(pages) * kVgPageBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COMMON, a.chunks[c], L"VgPagePool");
        if (FAILED(hr)) { release(); a.chunks.clear(); return bad(LoadCode::DeviceFailure, "ページプールの確保に失敗 " + HrStr(hr)); }
    }
    {
        const HRESULT hr = MakeBuffer(m_device.Get(), nodeBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, a.nodes, L"VgNodes");
        if (FAILED(hr)) { release(); a.chunks.clear(); return bad(LoadCode::DeviceFailure, "BVH バッファの確保に失敗 " + HrStr(hr)); }
    }

    // アップロード（1 アセットずつ直列）
    {
        std::lock_guard<std::mutex> lk(m_up->mtx);
        std::string uerr;
        // BVH
        {
            const uint8_t* np = reinterpret_cast<const uint8_t*>(meta.nodes.data());
            if (!m_up->Upload(a.nodes.Get(), 0, nodeBytes, [&](uint64_t off, uint8_t* out, uint64_t n) { std::memcpy(out, np + off, static_cast<size_t>(n)); return true; }, &uerr))
            { release(); a.chunks.clear(); a.nodes.Reset(); return bad(LoadCode::DeviceFailure, "BVH のアップロード: " + uerr); }
        }
        // ページ（チャンクごとに、ファイル内で連続している範囲を大きな塊で読む）
        for (uint32_t c = 0; c < chunkCount; ++c)
        {
            const uint32_t first = c * kVgChunkPages;
            const uint32_t pages = std::min<uint32_t>(kVgChunkPages, h.pageCount - first);
            const uint64_t total = static_cast<uint64_t>(pages) * kVgPageBytes;
            std::string readErr;
            auto fill = [&](uint64_t off, uint8_t* out, uint64_t n) -> bool
            {
                // 無圧縮のページは PAGES セクション内で連続（fileOffset = base + i * 131072）
                const uint32_t p0 = first + static_cast<uint32_t>(off / kVgPageBytes);
                if (!src.Read(meta.pageTable[p0].fileOffset, out, n)) { readErr = "ページの読み込みに失敗（ページ " + std::to_string(p0) + "）"; return false; }
                if (m_desc.verifyPageCrc)
                    for (uint64_t k = 0; k < n / kVgPageBytes; ++k)
                        if (Crc32(out + k * kVgPageBytes, kVgPageBytes) != meta.pageTable[p0 + k].pageCrc32)
                        { readErr = "ページ " + std::to_string(p0 + k) + " の CRC32 が一致しない"; return false; }
                return true;
            };
            // ★連続性の前提を確認（ファイルの並びが崩れていたら拒否）
            bool contiguous = true;
            for (uint32_t k = 1; k < pages && contiguous; ++k)
                contiguous = meta.pageTable[first + k].fileOffset == meta.pageTable[first].fileOffset + static_cast<uint64_t>(k) * kVgPageBytes;
            if (!contiguous) { release(); a.chunks.clear(); a.nodes.Reset(); return bad(LoadCode::Invalid, "ページがファイル内で連続していない"); }
            uerr.clear();
            if (!m_up->Upload(a.chunks[c].Get(), 0, total, fill, &uerr))
            { release(); a.chunks.clear(); a.nodes.Reset(); return bad(readErr.empty() ? LoadCode::DeviceFailure : LoadCode::Invalid, readErr.empty() ? "アップロード: " + uerr : readErr); }
        }
        m_up->Flush();
    }

    // 情報
    AssetInfo& in = a.info;
    in.vramBytes = need;
    in.pageCount = h.pageCount; in.clusterCount = h.clusterCount; in.nodeCount = static_cast<uint32_t>(meta.nodes.size());
    in.levelCount = h.levelCount; in.chunkCount = chunkCount;
    in.bvhDepth = ComputeBvhDepth(meta.nodes);
    in.sourceTriangles = h.sourceTriangleCount;
    std::memcpy(in.boundingSphere, h.boundingSphere, 16);
    std::memcpy(in.aabbMin, h.aabbMin, 12);
    std::memcpy(in.aabbMax, h.aabbMax, 12);
    in.proxyError = h.proxyError;

    VgAssetGpu& g = a.gpu;
    g = VgAssetGpu{};
    std::memcpy(g.boundingSphere, h.boundingSphere, 16);
    std::memcpy(g.aabbMin, h.aabbMin, 12);
    std::memcpy(g.aabbMax, h.aabbMax, 12);
    std::memcpy(g.posOrigin, h.posOrigin, 12);
    g.posStep = h.posStep;
    g.rootNode = h.rootNode;
    g.pageCount = h.pageCount;
    g.sourceTriangles = static_cast<uint32_t>(std::min<uint64_t>(h.sourceTriangleCount, 0xFFFFFFFFull));
    g.levelCount = h.levelCount;
    g.clusterCount = h.clusterCount;
    g.materialCount = h.materialCount;
    g.materialBase = VG_NONE;          // P4: 材質表は SetAssetMaterials で登録する（未登録は既定材質で描く）

    // P4: 材質（テクスチャの相対パス付き）。アプリがテクスチャを読んで SetAssetMaterials で登録する。
    a.materials.clear();
    a.materials.reserve(meta.materials.size());
    for (const MaterialRecord& r : meta.materials)
    {
        AssetMaterial m;
        auto str = [&](u32 off) -> std::string { const char* s = (off == kNone) ? nullptr : meta.String(off); return s ? std::string(s) : std::string(); };
        m.name = str(r.nameOff);
        m.albedo = str(r.albedoPathOff);
        m.normal = str(r.normalPathOff);
        m.metalRough = str(r.metalRoughPathOff);
        m.emissive = str(r.emissivePathOff);
        m.flags = r.flags;
        m.metallic = r.metallic;
        m.roughness = r.roughness;
        std::memcpy(m.emissiveColor, r.emissiveColor, 12);
        m.emissiveIntensity = r.emissiveIntensity;
        std::memcpy(m.uvScaleOffset, r.uvScaleOffset, 16);
        m.sectionKind = r.sectionKind;
        a.materials.push_back(std::move(m));
    }
    in.materialCount = static_cast<uint32_t>(a.materials.size());
    return LoadError{};
}

bool VirtualGeometrySystem::LoadAssetFromSource(const ByteSource& src, const std::string& name, uint32_t* outId, LoadError* err)
{
    LoadError le;
    if (!m_ready) { le.code = LoadCode::NotInitialized; if (err) *err = le; return false; }
    const uint32_t id = AllocAssetSlot(name, &le);
    if (id == 0xFFFFFFFFu) { if (err) *err = le; return false; }
    Asset* a = nullptr;
    { std::lock_guard<std::mutex> lk(m_assetsMutex); a = m_assets[id].get(); }
    // 状態: 0 新規 / 1 読込済み(未有効化) / 2 有効 / 3 失敗 / 4 解放済み / 5 非同期の待ち / 6 読込中
    for (int st = a->state.load(); st == 5 || st == 6; st = a->state.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));      // 非同期読込が走っている間は待つ
    const int st = a->state.load();
    if (st == 2 || st == 1) { if (st == 1) { Pump(); } if (outId) *outId = id; return true; }
    if (st == 3) { if (err) *err = a->error; return false; }
    if (st == 4) { if (err) *err = {LoadCode::OpenFailed, "解放済みのアセット"}; return false; }
    a->state.store(6);
    le = LoadImpl(src, *a);
    if (!le.ok())
    {
        a->error = le;
        a->info.failed = true;
        a->state.store(3);
        Logger::Warn("VG: アセットの読込に失敗: {} → {}", name, le.ToString());
        if (err) *err = le;
        return false;
    }
    a->state.store(1, std::memory_order_release);
    Pump();
    if (outId) *outId = id;
    return true;
}

bool VirtualGeometrySystem::LoadAssetSync(const std::string& path, uint32_t* outId, LoadError* err)
{
    FileSource fs;
    if (!fs.Open(path))
    {
        LoadError le{LoadCode::OpenFailed, "ファイルを開けない: " + path};
        if (err) *err = le;
        return false;
    }
    return LoadAssetFromSource(fs, path, outId, err);
}

uint32_t VirtualGeometrySystem::RequestAsset(const std::string& path)
{
    if (!m_ready) return 0xFFFFFFFFu;
    LoadError le;
    const uint32_t id = AllocAssetSlot(path, &le);
    if (id == 0xFFFFFFFFu) return id;
    Asset* a = nullptr;
    { std::lock_guard<std::mutex> lk(m_assetsMutex); a = m_assets[id].get(); }
    // 新規のものだけジョブへ積む（state 0 で、まだ誰も積んでいない）
    static_cast<void>(le);
    int expected = 0;
    if (a->state.compare_exchange_strong(expected, 5))   // 5 = 読込要求済み（ワーカー待ち）
    {
        if (!m_desc.asyncLoad)
        {
            a->state.store(0);
            uint32_t got = 0;
            LoadError e2;
            if (!LoadAssetSync(path, &got, &e2)) return 0xFFFFFFFFu;
            return id;
        }
        ++m_pending;
        std::lock_guard<std::mutex> lk(m_jobMutex);
        m_jobs.push_back(id);
        m_jobCv.notify_one();
    }
    return id;
}

void VirtualGeometrySystem::WorkerMain()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;)
    {
        uint32_t id = 0;
        {
            std::unique_lock<std::mutex> lk(m_jobMutex);
            m_jobCv.wait(lk, [&] { return m_quit || !m_jobs.empty(); });
            if (m_quit) return;
            id = m_jobs.front();
            m_jobs.pop_front();
        }
        Asset* a = nullptr;
        { std::lock_guard<std::mutex> lk(m_assetsMutex); a = m_assets[id].get(); }
        a->state.store(6);
        const auto t0 = std::chrono::steady_clock::now();
        FileSource fs;
        LoadError le;
        if (!fs.Open(a->path)) { le.code = LoadCode::OpenFailed; le.message = "ファイルを開けない: " + a->path; }
        else le = LoadImpl(fs, *a);
        if (le.ok())
        {
            a->state.store(1, std::memory_order_release);
            Logger::Info("VG: 読込完了 {} ({:.1f} MB, {:.2f} 秒)", a->path, a->info.vramBytes / 1048576.0,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        else
        {
            a->error = le;
            a->info.failed = true;
            a->state.store(3, std::memory_order_release);
            Logger::Warn("VG: アセットの読込に失敗: {} → {}", a->path, le.ToString());
        }
        --m_pending;
    }
}

void VirtualGeometrySystem::FinalizeAsset(Asset& a)
{
    // ディスクリプタ: [nodes SRV][chunk SRV × n]
    const uint32_t count = 1 + static_cast<uint32_t>(a.chunks.size());
    const uint32_t base = AllocDescBlock(count);
    if (base == IndexAllocator::kInvalid)
    {
        a.error = {LoadCode::DeviceFailure, "ディスクリプタが足りない"};
        a.info.failed = true;
        a.chunks.clear(); a.nodes.Reset();
        m_vramUsed.fetch_sub(a.reservedBytes); a.reservedBytes = 0;
        a.state.store(3);
        return;
    }
    a.descBase = base; a.descCount = count;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = a.info.nodeCount;
        s.Buffer.StructureByteStride = sizeof(HierNode);
        m_device->CreateShaderResourceView(a.nodes.Get(), &s, CpuHandle(base));
    }
    for (uint32_t c = 0; c < a.chunks.size(); ++c)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R32_TYPELESS;
        s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.NumElements = static_cast<UINT>(a.chunks[c]->GetDesc().Width / 4);
        s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device->CreateShaderResourceView(a.chunks[c].Get(), &s, CpuHandle(base + 1 + c));
    }
    a.gpu.nodesSrv = base;
    a.gpu.poolSrvBase = base + 1;
    a.info.ready = true;
    m_maxDepth = std::max(m_maxDepth, a.info.bvhDepth);
    a.state.store(2, std::memory_order_release);
}

void VirtualGeometrySystem::Pump()
{
    std::vector<uint32_t> idle;
    {
        std::lock_guard<std::mutex> lk(m_assetsMutex);
        for (auto& up : m_assets)
        {
            Asset& a = *up;
            const int st = a.state.load(std::memory_order_acquire);
            if (st == 1) { a.lastUse = m_frameCounter; FinalizeAsset(a); }
            else if (st == 2 && m_desc.idleUnloadFrames > 0 && m_frameCounter > a.lastUse + m_desc.idleUnloadFrames) idle.push_back(a.info.id);
        }
        m_gpuAssetCount = static_cast<uint32_t>(m_assets.size());
    }
    // 長く使われていないアセットを解放（GPU の使用は 3 フレーム以内に終わっているので安全）
    for (uint32_t id : idle)
    {
        Logger::Info("VG: 長く使われていないアセットを解放: {}", m_assets[id]->path);
        UnloadAsset(id);
    }
}

bool VirtualGeometrySystem::IsAssetReady(uint32_t id) const
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    return id < m_assets.size() && m_assets[id]->state.load(std::memory_order_acquire) == 2;
}

bool VirtualGeometrySystem::GetAssetInfo(uint32_t id, AssetInfo& out) const
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    if (id >= m_assets.size()) return false;
    const Asset& a = *m_assets[id];
    const int st = a.state.load(std::memory_order_acquire);
    if (st == 0 || st == 5 || st == 6)   // 読込中のアセットの info はワーカーが書いている最中なので触らない
    {
        out = AssetInfo{};
        out.id = id;
        out.path = a.path;
        return true;
    }
    out = a.info;
    out.ready = st == 2;
    out.failed = st == 3;
    out.error = a.error;
    return true;
}

uint32_t VirtualGeometrySystem::FindAsset(const std::string& path) const
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    auto it = m_pathToId.find(path);
    return it == m_pathToId.end() ? 0xFFFFFFFFu : it->second;
}

uint32_t VirtualGeometrySystem::AssetCount() const
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    return static_cast<uint32_t>(m_assets.size());
}

void VirtualGeometrySystem::UnloadAsset(uint32_t id)
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    if (id >= m_assets.size()) return;
    Asset& a = *m_assets[id];
    if (a.state.load() != 2) return;
    a.chunks.clear();
    a.nodes.Reset();
    if (a.descBase != 0xFFFFFFFFu)
    {
        FreeDescBlock(a.descBase, a.descCount);
        a.descBase = 0xFFFFFFFFu;
    }
    if (a.matBase != 0xFFFFFFFFu)
    {
        m_matAlloc.FreeBlock(a.matBase, a.matCount);
        a.matBase = 0xFFFFFFFFu; a.matCount = 0;
        a.gpu.materialBase = VG_NONE;
        a.info.materialsBound = false;
    }
    m_vramUsed.fetch_sub(a.reservedBytes);
    a.reservedBytes = 0;
    a.info.ready = false;
    a.state.store(4);
    if (!a.path.empty()) m_pathToId.erase(a.path);
}

// ── 毎フレーム ───────────────────────────────────────────────────────────────
void VirtualGeometrySystem::UpdateHzbSrv(uint32_t slot, uint32_t which, ID3D12Resource* res)
{
    const uint32_t idx = D(kDescFrameBase + kDescPerFrame * slot + 2 + which);
    D3D12_SHADER_RESOURCE_VIEW_DESC h{};
    h.Format = DXGI_FORMAT_R32_FLOAT;
    h.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    h.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (res)
    {
        h.Texture2D.MipLevels = res->GetDesc().MipLevels;
        m_device->CreateShaderResourceView(res, &h, CpuHandle(idx));
    }
    else
    {
        h.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullptr, &h, CpuHandle(idx));
    }
    m_hzbRes[slot][which] = res;
}

void VirtualGeometrySystem::BindCompute(ID3D12GraphicsCommandList* cmd, D3D12_GPU_VIRTUAL_ADDRESS cb)
{
    ID3D12DescriptorHeap* heaps[] = {m_heapRaw};
    cmd->SetDescriptorHeaps(1, heaps);           // ★SetRootSignature より前（HEAP_DIRECTLY_INDEXED の規約）
    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRootConstantBufferView(0, cb);
}

bool VirtualGeometrySystem::Execute(const ExecuteDesc& d)
{
    if (!m_ready || !d.cmd || d.frameSlot >= m_desc.frameCount) return false;
    ID3D12GraphicsCommandList* cmd = d.cmd;
    const uint32_t f = d.frameSlot;

    // ── インスタンス / アセット表を詰める ────────────────────────────────────
    uint32_t n = 0, dropped = 0;
    {
        std::lock_guard<std::mutex> lk(m_assetsMutex);
        auto* dst = reinterpret_cast<VgInstance*>(m_instMapped[f]);
        for (uint32_t i = 0; i < d.instanceCount; ++i)
        {
            const VgInstanceInput& in = d.instances[i];
            if (in.assetId >= m_assets.size() || m_assets[in.assetId]->state.load(std::memory_order_acquire) != 2) continue;
            if (n >= m_desc.maxInstances) { ++dropped; continue; }
            m_assets[in.assetId]->lastUse = m_frameCounter;
            dst[n++] = MakeInstance(in, in.assetId);
        }
        auto* ad = reinterpret_cast<VgAssetGpu*>(m_assetMapped[f]);
        const uint32_t na = std::min<uint32_t>(static_cast<uint32_t>(m_assets.size()), m_desc.maxAssets);
        for (uint32_t i = 0; i < na; ++i)
            ad[i] = (m_assets[i]->state.load(std::memory_order_acquire) == 2) ? m_assets[i]->gpu : VgAssetGpu{};
    }
    m_instUploaded[f] = n;
    m_instDropped[f] = dropped;

    // ── 定数 ────────────────────────────────────────────────────────────────
    const bool rasterReq = d.raster.enabled && m_rasterOk && d.raster.depth && d.raster.width > 0 && d.raster.height > 0 &&
                           d.raster.depthToRaster && d.raster.depthToSample;
    const bool measure = rasterReq && d.raster.measure;
    const bool odImage = measure && d.raster.overdrawImage;
    // P4: 安定ソート（決定論）。AS 経由は子グループのラスタ順が仕様上不定なので、安定モードでは MS のみにする。
    const bool stable = rasterReq && d.settings.stableOrder && m_psoSort != nullptr;
    const bool useAs = rasterReq && d.raster.useAs && !stable;
    // 古い可視性バッファの解放（リサイズ後、GPU が使い終わるまで 16 Execute 待つ）
    for (size_t i = 0; i < m_retired.size();)
    {
        if (m_frameCounter > m_retired[i].frame + 16) m_retired.erase(m_retired.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    const bool raster = rasterReq && EnsureVisBuffers(d.raster.width, d.raster.height, odImage);

    const float tau = d.settings.forceLod0 ? 1.0e-6f
                                           : std::clamp(d.settings.tauPx, 0.05f, 64.0f) * (d.settings.autoTau ? m_tauScale : 1.0f);
    m_tauUsed[f] = tau;
    VgCullConstants c{};
    auto transposeTo = [](const float* m, float* o) { for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) o[k * 4 + r] = m[r * 4 + k]; };
    transposeTo(d.viewProj, c.viewProj);
    transposeTo(d.prevViewProj, c.prevViewProj);
    FrustumPlanes fp;
    ExtractFrustumPlanes(d.viewProj, fp);
    std::memcpy(c.planes, fp.p, sizeof(c.planes));
    c.camPos[0] = d.camPos[0]; c.camPos[1] = d.camPos[1]; c.camPos[2] = d.camPos[2]; c.camPos[3] = d.zNear;
    c.viewport[0] = d.vpX; c.viewport[1] = d.vpY; c.viewport[2] = d.vpW; c.viewport[3] = d.vpH;
    c.lodParams[0] = tau; c.lodParams[1] = d.projScale; c.lodParams[2] = d.settings.instanceMinPx; c.lodParams[3] = 0.0f;
    const bool hzbOn = d.settings.hzbCulling && d.hzb.width > 0 && d.hzb.mips > 0;
    const bool prevOk = hzbOn && d.hzb.prev && d.hzb.prevValid;
    const bool curOk = hzbOn && d.hzb.cur && (d.hzb.curValid || d.hzb.buildBetweenPhases);
    c.hzbParams[0] = static_cast<float>(d.hzb.width); c.hzbParams[1] = static_cast<float>(d.hzb.height);
    c.hzbParams[2] = static_cast<float>(d.hzb.mips);
    c.counts[0] = n; c.counts[1] = prevOk ? 1 : 0; c.counts[2] = curOk ? 1 : 0; c.counts[3] = d.settings.coneCulling ? 1 : 0;
    const uint32_t fb = D(kDescFrameBase + kDescPerFrame * f);
    c.heap0[0] = fb + 0; c.heap0[1] = fb + 1; c.heap0[2] = D(kDescWorkUav); c.heap0[3] = D(kDescExtraUav);
    c.heap1[0] = fb + 2; c.heap1[1] = fb + 3; c.heap1[2] = D(kDescArgsUav); c.heap1[3] = D(kDescMaterials);
    // P4: 速度（G-Buffer パス）のジッタ無し VP。渡されなければジッタ付きで代用（テスト / 速度を使わない経路）
    transposeTo(d.hasVelocityVp ? d.viewProjNJ : d.viewProj, c.viewProjNJ);
    transposeTo(d.hasVelocityVp ? d.prevViewProjNJ : d.viewProj, c.prevViewProjNJ);
    c.caps[0] = m_layout.nodeQCap; c.caps[1] = m_layout.groupQCap; c.caps[2] = m_layout.visibleCap; c.caps[3] = m_layout.deferredCap;
    c.offs0[0] = m_layout.nodeQ0; c.offs0[1] = m_layout.nodeQ1; c.offs0[2] = m_layout.deferred; c.offs0[3] = m_layout.groupQ0;
    c.offs1[0] = m_layout.groupQ1; c.offs1[1] = m_layout.visible; c.offs1[2] = m_layout.counters; c.offs1[3] = m_layout.stats;
    c.heap2[0] = fb + 4; c.heap2[1] = fb + 5; c.heap2[2] = odImage ? fb + 6 : VG_NONE; c.heap2[3] = fb + 7;
    uint32_t rflags = 0;
    if (raster && d.raster.smallPrimCull) rflags |= VG_RF_SMALLPRIM_CULL;
    if (measure) rflags |= VG_RF_COUNT;
    if (rasterReq && d.raster.edgeHist) rflags |= VG_RF_EDGE_HIST;
    c.rast[0] = rflags; c.rast[1] = raster ? m_visW : 0; c.rast[2] = raster ? m_visH : 0; c.rast[3] = 0;
    c.dbg[0] = 100.0f; c.dbg[1] = 8.0f; c.dbg[2] = 1000.0f; c.dbg[3] = 0.0f;
    std::memcpy(m_cbMapped[f], &c, sizeof(c));

    // HZB の SRV（このフレームスロット専用のディスクリプタを毎回張り直す。リサイズで同じアドレスに作り直されても安全）
    UpdateHzbSrv(f, 0, prevOk ? d.hzb.prev : nullptr);
    UpdateHzbSrv(f, 1, curOk ? d.hzb.cur : nullptr);
    if (raster)
    {
        // 深度（R32_FLOAT の SRV。デバッグ表示が読む）/ 可視性バッファの SRV・UAV / オーバードロー画像の UAV
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(d.raster.depth, &sd, CpuHandle(fb + 4));
        D3D12_SHADER_RESOURCE_VIEW_DESC vs{};
        vs.Format = kVisFormat;
        vs.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        vs.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vs.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_vis.Get(), &vs, CpuHandle(fb + 5));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = kVisFormat;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(m_vis.Get(), nullptr, &uv, CpuHandle(fb + 7));
        if (odImage) m_device->CreateUnorderedAccessView(m_overdraw.Get(), nullptr, &uv, CpuHandle(fb + 6));
    }

    const D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_cbBuf[f]->GetGPUVirtualAddress();
    const uint32_t depthLevels = m_maxDepth + 1;
    const bool ts = m_queryHeap != nullptr;
    const uint32_t q0 = f * kQueriesPerSlot;
    auto stamp = [&](uint32_t i) { if (ts) cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + i); };

    stamp(0);

    BindCompute(cmd, cbAddr);
    const D3D12_RESOURCE_BARRIER uavAll = UavBarrier(nullptr);
    auto setPass = [&](uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3)
    {
        const uint32_t v[8] = {a0, a1, a2, a3, b0, b1, b2, b3};
        cmd->SetComputeRoot32BitConstants(1, 8, v, 0);
    };
    if (m_argsState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto b = Transition(m_args.Get(), m_argsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b);
        m_argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // Args → ExecuteIndirect(Dispatch) → 後始末
    auto argsThenIndirect = [&](ID3D12PipelineState* pso, uint32_t mode, uint32_t readCnt, uint32_t clearCnt, uint32_t cap, uint32_t slot)
    {
        cmd->SetPipelineState(m_psoArgs.Get());
        setPass(mode, readCnt, clearCnt, cap, slot, 0, 0, 0);
        cmd->Dispatch(1, 1, 1);
        const D3D12_RESOURCE_BARRIER b1[2] = {uavAll, Transition(m_args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
        cmd->ResourceBarrier(2, b1);
        cmd->SetPipelineState(pso);
        return slot;
    };
    auto endIndirect = [&]()
    {
        const D3D12_RESOURCE_BARRIER b2[2] = {Transition(m_args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), uavAll};
        cmd->ResourceBarrier(2, b2);
    };

    // 0. リセット
    cmd->SetPipelineState(m_psoReset.Get());
    setPass(0, 0, 0, 0, 0, 0, 0, 0);
    cmd->Dispatch((VG_CNT_COUNT + VG_STAT_COUNT + VG_THREADS - 1) / VG_THREADS, 1, 1);
    cmd->ResourceBarrier(1, &uavAll);

    // 0b. 可視性バッファを「空」で埋める（ClearUnorderedAccessViewUint。UINT 形式に ClearRenderTargetView は使えない）+ オーバードロー画像を 0 で埋める
    if (raster)
    {
        if (m_visState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            const auto b = Transition(m_vis.Get(), m_visState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->ResourceBarrier(1, &b);
            m_visState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        const UINT32 empty[4] = {VG_VIS_EMPTY, VG_VIS_EMPTY, VG_VIS_EMPTY, VG_VIS_EMPTY};
        const UINT32 zero[4] = {0, 0, 0, 0};
        D3D12_CPU_DESCRIPTOR_HANDLE cpuVis = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
        cmd->ClearUnorderedAccessViewUint(GpuHandle(fb + 7), cpuVis, m_vis.Get(), empty, 0, nullptr);
        if (odImage)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuOd = cpuVis;
            cpuOd.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cmd->ClearUnorderedAccessViewUint(GpuHandle(fb + 6), cpuOd, m_overdraw.Get(), zero, 0, nullptr);
        }
        const D3D12_RESOURCE_BARRIER b = Transition(m_vis.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &b);
        m_visState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // 1. K0 インスタンスカリング（一相目）
    cmd->SetPipelineState(m_psoInstance.Get());
    setPass(0, 0, 0, 0, 0, 0, 0, 0);
    cmd->Dispatch(std::max<uint32_t>((n + VG_THREADS - 1) / VG_THREADS, 1u), 1, 1);
    cmd->ResourceBarrier(1, &uavAll);

    struct Q { uint32_t off, cnt; };
    const Q nq[2] = {{m_layout.nodeQ0, VG_CNT_NODEQ0}, {m_layout.nodeQ1, VG_CNT_NODEQ1}};
    const Q defQ = {m_layout.deferred, VG_CNT_DEFERRED};

    auto traversePhase = [&](uint32_t phase)
    {
        for (uint32_t L = 0; L < depthLevels; ++L)
        {
            // 入力: 一相目 = NQ0（L=0）から交互 / 二相目の L=0 だけ Deferred。出力は入力と逆側のキュー。
            Q in, out;
            if (phase == 1 && L == 0) { in = defQ; out = nq[0]; }
            else if (phase == 1)      { in = nq[(L - 1) & 1]; out = nq[L & 1]; }
            else                      { in = nq[L & 1]; out = nq[(L + 1) & 1]; }
            const uint32_t inCap = (in.cnt == VG_CNT_DEFERRED) ? m_layout.deferredCap : m_layout.nodeQCap;
            argsThenIndirect(m_psoTraverse.Get(), 0, in.cnt, out.cnt, inCap, VG_ARGS_TRAVERSE);
            setPass(phase, in.off, in.cnt, inCap, out.off, out.cnt, m_layout.nodeQCap, 0);
            cmd->ExecuteIndirect(m_cmdSigDispatch.Get(), 1, m_args.Get(), VG_ARGS_TRAVERSE * VG_ARGS_STRIDE, nullptr, 0);
            endIndirect();
        }
    };
    auto clusterPhase = [&](uint32_t phase)
    {
        const uint32_t off = phase == 0 ? m_layout.groupQ0 : m_layout.groupQ1;
        const uint32_t cnt = phase == 0 ? VG_CNT_GROUPQ0 : VG_CNT_GROUPQ1;
        argsThenIndirect(m_psoCluster.Get(), 1, cnt, VG_NONE, m_layout.groupQCap, VG_ARGS_CLUSTER);
        setPass(phase, off, cnt, m_layout.groupQCap, 0, 0, 0, 0);
        cmd->ExecuteIndirect(m_cmdSigDispatch.Get(), 1, m_args.Get(), VG_ARGS_CLUSTER * VG_ARGS_STRIDE, nullptr, 0);
        endIndirect();
    };

    // P3: このフェーズで K2 が出した可視クラスタ（[start, 可視数)）を可視性バッファ + 深度へ描く。
    //   引数生成（compute）→ 深度を書き込み状態へ → メッシュシェーダ（ExecuteIndirect(DispatchMesh)）→ 深度を読み取り状態へ。
    auto rasterPhase = [&](uint32_t phase)
    {
        stamp(1 + phase * 2);
        // 引数（範囲と DispatchMesh 引数）
        cmd->SetPipelineState(m_psoRasterArgs.Get());
        setPass(phase, VG_ARGS_RASTER, useAs ? 1u : 0u, 0, 0, 0, 0, 0);
        cmd->Dispatch(1, 1, 1);
        // P4: 安定ソート（決定論）。このフェーズの可視範囲 [start, start + count) を {instance, clusterRef} 順へ（VgUtil.hlsl CSSortStep）。
        //   長さは前回の統計の可視数から決める（2 倍の余裕。足りなければ GPU 側でそのフェーズだけ並べ替えを諦めて統計で知らせる）。
        if (stable)
        {
            cmd->ResourceBarrier(1, &uavAll);
            uint32_t want = std::max<uint32_t>(m_prevVisible[phase] * 2u + 64u, 256u);
            uint32_t K = 8;
            while ((1u << K) < want && K < VG_SORT_MAX_LOG2) ++K;
            const uint32_t pairs = (1u << K) >> 1;
            const uint32_t groups = std::max<uint32_t>((pairs + VG_SORT_THREADS - 1) / VG_SORT_THREADS, 1u);
            cmd->SetPipelineState(m_psoSort.Get());
            bool first = true;
            for (uint32_t k = 2; k <= (1u << K); k <<= 1)
            {
                setPass(0, k, first ? 1u : 0u, 0, K, 0, 0, 0);          // 反転
                first = false;
                cmd->Dispatch(groups, 1, 1);
                cmd->ResourceBarrier(1, &uavAll);
                for (uint32_t j = k >> 2; j >= 1; j >>= 1)
                {
                    setPass(1, j, 0, 0, K, 0, 0, 0);                    // 半クリーナー
                    cmd->Dispatch(groups, 1, 1);
                    cmd->ResourceBarrier(1, &uavAll);
                }
            }
        }
        const D3D12_RESOURCE_BARRIER b1[2] = {uavAll, Transition(m_args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};
        cmd->ResourceBarrier(2, b1);

        d.raster.depthToRaster(cmd);
        ID3D12DescriptorHeap* heaps[] = {m_heapRaw};
        cmd->SetDescriptorHeaps(1, heaps);                 // ★SetRootSignature より前
        cmd->SetGraphicsRootSignature(m_rootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
        const uint32_t v[8] = {rflags, phase, 0, 0, 0, 0, 0, 0};
        cmd->SetGraphicsRoot32BitConstants(1, 8, v, 0);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &d.raster.dsv);
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_visW), static_cast<float>(m_visH), 0.0f, 1.0f};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(m_visW), static_cast<LONG>(m_visH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->SetPipelineState(GetRasterPso(useAs, measure));
        cmd->ExecuteIndirect(m_cmdSigMesh.Get(), 1, m_args.Get(), VG_ARGS_RASTER * VG_ARGS_STRIDE, nullptr, 0);
        const D3D12_RESOURCE_BARRIER b2[2] = {Transition(m_args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), uavAll};
        cmd->ResourceBarrier(2, b2);
        d.raster.depthToSample(cmd);
        stamp(2 + phase * 2);
    };

    // 2. 一相目: BVH 走査 → クラスタカリング（前フレーム HZB）→ ラスタ
    traversePhase(0);
    clusterPhase(0);
    if (raster) rasterPhase(0);

    // 3. HZB を今フレームの深度（非 VG + 一相目の VG）から作り直す（呼び出し側）
    if (d.hzb.buildBetweenPhases)
    {
        d.hzb.buildBetweenPhases(cmd);
        BindCompute(cmd, cbAddr);
    }

    // 4. 二相目: 一相目の HZB で落ちたものだけを、今フレームの HZB で再判定 → ラスタ
    traversePhase(1);
    clusterPhase(1);
    if (raster)
    {
        rasterPhase(1);
        // 5. 最終深度から HZB を作り直す（次フレームの一相目の入力）。二相目の HZB より新しい（二相目のラスタが入っている）
        if (d.hzb.buildFinal)
        {
            d.hzb.buildFinal(cmd);
            BindCompute(cmd, cbAddr);
        }
        // 可視性バッファを読み取り状態へ（デバッグ表示 / P4 の resolve / 読み戻しが読む）
        const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const D3D12_RESOURCE_BARRIER b = Transition(m_vis.Get(), m_visState, rd);
        cmd->ResourceBarrier(1, &b);
        m_visState = rd;
        // 被覆画素数（計測）
        if (measure)
        {
            BindCompute(cmd, cbAddr);
            cmd->SetPipelineState(m_psoCoverage.Get());
            setPass(0, 0, 0, 0, 0, 0, 0, 0);
            cmd->Dispatch((m_visW + 7) / 8, (m_visH + 7) / 8, 1);
            cmd->ResourceBarrier(1, &uavAll);
        }
    }

    // 6. 統計 / 可視クラスタ表の読み戻し
    {
        const D3D12_RESOURCE_BARRIER b = Transition(m_work.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->ResourceBarrier(1, &b);
        cmd->CopyBufferRegion(m_statsReadback[f].Get(), 0, m_work.Get(), 0, kStatsReadbackBytes);
        if (d.dumpVisible)
            cmd->CopyBufferRegion(m_dumpReadback.Get(), 0, m_work.Get(), m_layout.visible, static_cast<uint64_t>(m_layout.visibleCap) * 8);
        const D3D12_RESOURCE_BARRIER b2 = Transition(m_work.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &b2);
    }
    if (ts)
    {
        stamp(5);
        // ★書いた範囲だけ解決する（一度も書いていないクエリの Resolve はデバッグレイヤのエラー = ID 1319）。ラスタが無いフレームは [1, 4] を飛ばす
        const uint64_t rb = static_cast<uint64_t>(f) * kQueriesPerSlot * 8;
        cmd->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0, 1, m_queryReadback.Get(), rb);
        if (raster) cmd->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1, 4, m_queryReadback.Get(), rb + 8);
        cmd->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + 5, 1, m_queryReadback.Get(), rb + 40);
    }
    m_statsPending[f] = true;
    m_dumpPending[f] = d.dumpVisible;
    m_stableOn[f] = stable && raster;
    m_rasterOn[f] = raster;
    m_rasterUsedAs[f] = useAs;
    m_visValid[f] = raster;
    m_tsRaster[f] = raster;
    m_tsGb[f] = false;   // P4: G-Buffer / resolve はこの後で呼ばれたら立つ
    m_tsRv[f] = false;
    ++m_frameCounter;

    if (d.restoreHeap)
    {
        ID3D12DescriptorHeap* heaps[] = {d.restoreHeap};
        cmd->SetDescriptorHeaps(1, heaps);
    }
    return true;
}

// 可視性バッファを全画面で表示する（暫定シェーディング / デバッグ可視化）。
bool VirtualGeometrySystem::DrawDebug(const DebugDrawDesc& d)
{
    if (!m_ready || !m_rasterOk || !d.cmd || d.frameSlot >= m_desc.frameCount || !m_visValid[d.frameSlot] || !m_vis) return false;
    ID3D12GraphicsCommandList* cmd = d.cmd;
    const uint32_t f = d.frameSlot;
    if (!m_psoDebug || m_psoDebugFormat != d.rtFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_rootSig.Get();
        pd.VS = {m_bcDbgVs.data(), m_bcDbgVs.size()};
        pd.PS = {m_bcDbgPs.data(), m_bcDbgPs.size()};
        auto& rt = pd.BlendState.RenderTarget[0];
        // P4: R32G32B32A32_FLOAT はテスト用の生データ（VG_DBG_RAW_*）= ブレンドしない
        m_psoDebugRaw = (d.rtFormat == DXGI_FORMAT_R32G32B32A32_FLOAT);
        rt.BlendEnable = m_psoDebugRaw ? FALSE : TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = d.rtFormat;
        pd.SampleDesc.Count = 1;
        m_psoDebug.Reset();
        if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoDebug))))
        {
            Logger::Warn("VG: デバッグ描画の PSO を作れない");
            return false;
        }
        m_psoDebugFormat = d.rtFormat;
    }
    // 表示パラメータ（CPU の書き込みは GPU の実行より前に済む＝同フレーム内の全パスに効くが、dbg を読むのはデバッグ描画だけ）
    float dbg[4] = {d.depthRange, d.overdrawMax, d.zFar, 0.0f};
    std::memcpy(m_cbMapped[f] + offsetof(VgCullConstants, dbg), dbg, sizeof(dbg));

    ID3D12DescriptorHeap* heaps[] = {m_heapRaw};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cbBuf[f]->GetGPUVirtualAddress());
    const uint32_t v[8] = {d.mode, 0, 0, 0, 0, 0, 0, 0};
    cmd->SetGraphicsRoot32BitConstants(1, 8, v, 0);
    cmd->OMSetRenderTargets(1, &d.rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetPipelineState(m_psoDebug.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    if (d.restoreHeap)
    {
        ID3D12DescriptorHeap* h2[] = {d.restoreHeap};
        cmd->SetDescriptorHeaps(1, h2);
    }
    return true;
}

void VirtualGeometrySystem::CollectStats(uint32_t f)
{
    if (!m_ready || f >= m_desc.frameCount || !m_statsPending[f]) return;
    void* p = nullptr;
    D3D12_RANGE rr{0, kStatsReadbackBytes};
    if (FAILED(m_statsReadback[f]->Map(0, &rr, &p)) || !p) return;
    const uint32_t* cnt = static_cast<const uint32_t*>(p);
    const uint32_t* s = cnt + VG_CNT_COUNT;
    FrameStats st;
    st.valid = true;
    st.instances = s[VG_STAT_INSTANCES];
    st.instancesFrustum = s[VG_STAT_INST_FRUSTUM];
    st.instancesHzbRejected = s[VG_STAT_INST_HZB_REJ];
    st.nodesVisited = s[VG_STAT_NODES_VISITED];
    st.childrenTested = s[VG_STAT_CHILDREN_TESTED];
    st.groups = s[VG_STAT_GROUPS];
    st.clustersTested = s[VG_STAT_CLUSTERS_TESTED];
    st.clustersSelected = s[VG_STAT_CLUSTERS_LOD];
    st.visibleP1 = s[VG_STAT_VISIBLE_P1];
    st.visibleP2 = s[VG_STAT_VISIBLE_P2];
    st.visibleClusters = st.visibleP1 + st.visibleP2;
    st.phase2Clusters = st.visibleP2;
    st.trianglesDrawn = s[VG_STAT_TRIS];
    st.sourceTrianglesInFrustum = (static_cast<uint64_t>(s[VG_STAT_SRC_TRIS_HI]) << 32) | s[VG_STAT_SRC_TRIS_LO];
    st.clustersCulledFrustum = s[VG_STAT_CL_CULL_FRUSTUM];
    st.clustersCulledCone = s[VG_STAT_CL_CULL_CONE];
    st.clustersDeferred = s[VG_STAT_CL_DEFERRED];
    st.clustersHzbDropped = s[VG_STAT_CL_HZB_DROP];
    st.nodesCulledFrustum = s[VG_STAT_NODE_CULL_FRUSTUM];
    st.nodesCulledLod = s[VG_STAT_NODE_CULL_LOD];
    st.nodesDeferred = s[VG_STAT_NODE_DEFERRED];
    st.overflowNode = s[VG_STAT_OVF_NODEQ];
    st.overflowGroup = s[VG_STAT_OVF_GROUPQ];
    st.overflowVisible = s[VG_STAT_OVF_VISIBLE];
    st.overflowDeferred = s[VG_STAT_OVF_DEFERRED];
    st.overflow = (st.overflowNode | st.overflowGroup | st.overflowVisible | st.overflowDeferred) != 0;
    for (uint32_t i = 0; i < VG_HIST_LEVELS; ++i) st.levelHistogram[i] = s[VG_STAT_HIST_BASE + i];
    st.rasterActive = m_rasterOn[f];
    st.rasterUsedAs = m_rasterUsedAs[f];
    st.rasterClusters = s[VG_STAT_RASTER_CLUSTERS];
    st.asCulled = s[VG_STAT_AS_CULLED];
    st.msTrisOut = s[VG_STAT_MS_TRIS_OUT];
    st.msPrimCulled = s[VG_STAT_MS_PRIM_CULLED];
    st.psInvocations = s[VG_STAT_PS_INVOC];
    st.coveredPixels = s[VG_STAT_COVERED_PIXELS];
    for (uint32_t i = 0; i < VG_EDGE_BINS; ++i) { st.edgeClusters[i] = s[VG_STAT_EDGE_CL_BASE + i]; st.edgeTris[i] = s[VG_STAT_EDGE_TRI_BASE + i]; }
    st.stableOrder = m_stableOn[f];
    st.sortedClusters = s[VG_STAT_SORTED];
    st.sortSkipped = s[VG_STAT_SORT_SKIPPED];
    // 安定ソートの長さの見積もり。ソートを諦めたフレームの次は最大長で回す（1 フレームで決定論へ戻る）。
    m_prevVisible[0] = st.sortSkipped ? (1u << VG_SORT_MAX_LOG2) : st.visibleP1;
    m_prevVisible[1] = st.sortSkipped ? (1u << VG_SORT_MAX_LOG2) : st.visibleP2;
    const uint32_t visCount = std::min(cnt[VG_CNT_VISIBLE], m_layout.visibleCap);
    D3D12_RANGE nw{0, 0};
    m_statsReadback[f]->Unmap(0, &nw);

    if (m_queryReadback)
    {
        void* q = nullptr;
        const SIZE_T qb = static_cast<SIZE_T>(f) * kQueriesPerSlot * 8;
        D3D12_RANGE qr{qb, qb + kQueriesPerSlot * 8};
        if (SUCCEEDED(m_queryReadback->Map(0, &qr, &q)) && q)
        {
            const uint64_t* t = static_cast<const uint64_t*>(q) + static_cast<size_t>(f) * kQueriesPerSlot;
            auto ms = [&](uint32_t a, uint32_t b) { return (t[b] > t[a] && m_tsFreq) ? static_cast<float>(static_cast<double>(t[b] - t[a]) * 1000.0 / static_cast<double>(m_tsFreq)) : 0.0f; };
            st.executeGpuMs = ms(0, 5);
            if (m_tsRaster[f])
            {
                st.rasterPhaseGpuMs[0] = ms(1, 2);
                st.rasterPhaseGpuMs[1] = ms(3, 4);
                st.rasterGpuMs = st.rasterPhaseGpuMs[0] + st.rasterPhaseGpuMs[1];
            }
            st.cullGpuMs = std::max(0.0f, st.executeGpuMs - st.rasterGpuMs);
            st.gbufferActive = m_tsGb[f];
            st.resolveActive = m_tsRv[f];
            if (m_tsGb[f]) st.gbufferGpuMs = ms(6, 7);
            if (m_tsRv[f]) st.resolveGpuMs = ms(8, 9);
            m_queryReadback->Unmap(0, &nw);
        }
    }
    st.vramBytes = m_fixedVram + m_vramUsed.load();
    {
        std::lock_guard<std::mutex> lk(m_assetsMutex);
        uint32_t loaded = 0;
        for (auto& a : m_assets) if (a->state.load() == 2) ++loaded;
        st.assetsLoaded = loaded;
    }
    st.tauUsed = m_tauUsed[f];
    st.instancesDropped = m_instDropped[f];

    // キュー溢れの安全弁: 次フレームの τ を 1.25 倍（最大 8 倍）。溢れが無ければゆっくり戻す。
    if (st.overflow) m_tauScale = std::min(m_tauScale * 1.25f, 8.0f);
    else if (m_tauScale > 1.0f) m_tauScale = std::max(1.0f, m_tauScale * 0.97f);

    if (m_dumpPending[f])
    {
        m_dump.clear();
        void* dp = nullptr;
        D3D12_RANGE dr{0, static_cast<SIZE_T>(visCount) * 8};
        if (visCount > 0 && SUCCEEDED(m_dumpReadback->Map(0, &dr, &dp)) && dp)
        {
            const uint32_t* v = static_cast<const uint32_t*>(dp);
            m_dump.reserve(visCount);
            for (uint32_t i = 0; i < visCount; ++i) m_dump.emplace_back(v[i * 2], v[i * 2 + 1]);
            m_dumpReadback->Unmap(0, &nw);
        }
        m_dumpPending[f] = false;
    }
    m_stats = st;
    m_statsPending[f] = false;
}

// ── P4: 材質表 ───────────────────────────────────────────────────────────────
bool VirtualGeometrySystem::GetAssetMaterials(uint32_t id, std::vector<AssetMaterial>& out) const
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    out.clear();
    if (id >= m_assets.size()) return false;
    const Asset& a = *m_assets[id];
    if (a.state.load(std::memory_order_acquire) != 2) return false;
    out = a.materials;
    return true;
}

bool VirtualGeometrySystem::AssetMaterialsBound(uint32_t id) const
{
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    return id < m_assets.size() && m_assets[id]->state.load(std::memory_order_acquire) == 2 && m_assets[id]->matBase != 0xFFFFFFFFu;
}

bool VirtualGeometrySystem::SetAssetMaterials(uint32_t id, const VgMaterialGpu* mats, uint32_t count)
{
    if (!m_ready || !m_matMapped || !mats) return false;
    std::lock_guard<std::mutex> lk(m_assetsMutex);
    if (id >= m_assets.size()) return false;
    Asset& a = *m_assets[id];
    if (a.state.load(std::memory_order_acquire) != 2) return false;
    count = std::min<uint32_t>(count, static_cast<uint32_t>(a.materials.size()));
    if (count == 0) return false;
    // 同じ個数なら同じ区間へ上書き（再登録。GPU が読んでいる途中でも値は同じか新しい物になるだけ）。個数が変わったら取り直す。
    if (a.matBase == 0xFFFFFFFFu || a.matCount != count)
    {
        if (a.matBase != 0xFFFFFFFFu) m_matAlloc.FreeBlock(a.matBase, a.matCount);
        a.matBase = m_matAlloc.AllocateBlock(count);
        if (a.matBase == IndexAllocator::kInvalid)
        {
            a.matBase = 0xFFFFFFFFu; a.matCount = 0; a.gpu.materialBase = VG_NONE; a.info.materialsBound = false;
            Logger::Warn("VG: 材質表がいっぱい（{} 個）。{} は既定材質で描かれる", m_desc.maxMaterials, a.path);
            return false;
        }
        a.matCount = count;
    }
    std::memcpy(m_matMapped + static_cast<size_t>(a.matBase) * sizeof(VgMaterialGpu), mats, static_cast<size_t>(count) * sizeof(VgMaterialGpu));
    a.gpu.materialBase = a.matBase;
    a.gpu.materialCount = count;
    a.info.materialsBound = true;
    return true;
}

// P4: G-Buffer / resolve の前後のタイムスタンプ（q = 6 / 8。end で 2 本まとめて解決する）
void VirtualGeometrySystem::StampPair(ID3D12GraphicsCommandList* cmd, uint32_t f, uint32_t q, bool end)
{
    if (!m_queryHeap) return;
    const uint32_t q0 = f * kQueriesPerSlot;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + q + (end ? 1u : 0u));
    if (end)
        cmd->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q0 + q, 2, m_queryReadback.Get(),
                              (static_cast<uint64_t>(q0) + q) * 8ull);
}

// ── P4（H2）: G-Buffer + 速度 ─────────────────────────────────────────────────
bool VirtualGeometrySystem::DrawGBuffer(const GBufferDesc& d)
{
    if (!m_ready || !m_rasterOk || !d.cmd || d.frameSlot >= m_desc.frameCount || !m_visValid[d.frameSlot] || !m_vis) return false;
    if (m_bcGbVs.empty() || m_bcGbPs.empty()) return false;
    ID3D12GraphicsCommandList* cmd = d.cmd;
    const uint32_t f = d.frameSlot;
    if (!m_psoGBuffer || m_psoGbFormats[0] != d.velocityFormat || m_psoGbFormats[1] != d.gbufferFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_rootSig.Get();
        pd.VS = {m_bcGbVs.data(), m_bcGbVs.size()};
        pd.PS = {m_bcGbPs.data(), m_bcGbPs.size()};
        for (int k = 0; k < 2; ++k) pd.BlendState.RenderTarget[k].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 2;
        pd.RTVFormats[0] = d.velocityFormat;
        pd.RTVFormats[1] = d.gbufferFormat;
        pd.SampleDesc.Count = 1;
        m_psoGBuffer.Reset();
        if (FAILED(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoGBuffer))))
        {
            Logger::Warn("VG: G-Buffer の PSO を作れない");
            m_bcGbVs.clear();   // 以後は試さない（毎フレームの失敗ログを避ける）
            return false;
        }
        m_psoGbFormats[0] = d.velocityFormat;
        m_psoGbFormats[1] = d.gbufferFormat;
    }
    ID3D12DescriptorHeap* heaps[] = {m_heapRaw};
    cmd->SetDescriptorHeaps(1, heaps);                   // ★SetRootSignature より前
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_cbBuf[f]->GetGPUVirtualAddress());
    const uint32_t v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    cmd->SetGraphicsRoot32BitConstants(1, 8, v, 0);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {d.velocityRtv, d.gbufferRtv};
    cmd->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_visW), static_cast<float>(m_visH), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(m_visW), static_cast<LONG>(m_visH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetPipelineState(m_psoGBuffer.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    StampPair(cmd, f, 6, false);
    cmd->DrawInstanced(3, 1, 0, 0);
    StampPair(cmd, f, 6, true);
    m_tsGb[f] = m_queryHeap != nullptr;
    if (d.restoreHeap && d.restoreHeap != m_heapRaw)
    {
        ID3D12DescriptorHeap* h2[] = {d.restoreHeap};
        cmd->SetDescriptorHeaps(1, h2);
    }
    return true;
}

// ── P4（H3）: 材質 resolve（メイン RS。テーブル類は呼び出し側が張り済み）──────────────────
bool VirtualGeometrySystem::DrawResolve(const ResolveDesc& d)
{
    if (!ResolveSupported() || !m_ready || !d.cmd || !d.rootSig || d.frameSlot >= m_desc.frameCount || !m_visValid[d.frameSlot] || !m_vis)
        return false;
    ID3D12GraphicsCommandList* cmd = d.cmd;
    const uint32_t f = d.frameSlot;
    if (!m_psoResolve || m_psoResolveRs != d.rootSig || m_psoResolveFormat != d.rtFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = d.rootSig;
        pd.VS = {m_bcResolveVs.data(), m_bcResolveVs.size()};
        pd.PS = {m_bcResolvePs.data(), m_bcResolvePs.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = d.rtFormat;
        pd.SampleDesc.Count = 1;
        m_psoResolve.Reset();
        const HRESULT hr = m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_psoResolve));
        if (FAILED(hr))
        {
            Logger::Warn("VG: resolve の PSO を作れない {}（メイン RS にバインドレスのフラグが無い？）。暫定シェーディングへ縮退", HrStr(hr));
            m_bcResolvePs.clear();   // ResolveSupported() = false にして以後は試さない
            return false;
        }
        m_psoResolveRs = d.rootSig;
        m_psoResolveFormat = d.rtFormat;
    }
    const uint32_t fb = D(kDescFrameBase + kDescPerFrame * f);
    VgResolveConstants c{};
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) c.viewProjJ[k * 4 + r] = d.viewProjJ[r * 4 + k];   // 転置
    c.heap0[0] = fb + 5; c.heap0[1] = D(kDescWorkUav); c.heap0[2] = fb + 1; c.heap0[3] = fb + 0;
    c.heap1[0] = D(kDescMaterials); c.heap1[1] = m_layout.visible; c.heap1[2] = 0; c.heap1[3] = 0;
    c.viewport[0] = static_cast<float>(m_visW); c.viewport[1] = static_cast<float>(m_visH);
    c.viewport[2] = 1.0f / static_cast<float>(std::max<uint32_t>(m_visW, 1u));
    c.viewport[3] = 1.0f / static_cast<float>(std::max<uint32_t>(m_visH, 1u));
    cmd->SetGraphicsRoot32BitConstants(d.rootConstSlot, sizeof(c) / 4, &c, 0);
    cmd->OMSetRenderTargets(1, &d.rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_visW), static_cast<float>(m_visH), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(m_visW), static_cast<LONG>(m_visH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetPipelineState(m_psoResolve.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    StampPair(cmd, f, 8, false);
    cmd->DrawInstanced(3, 1, 0, 0);
    StampPair(cmd, f, 8, true);
    m_tsRv[f] = m_queryHeap != nullptr;
    return true;
}

} // namespace dx12e::vg
