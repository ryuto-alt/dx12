#include "renderer/vfx/VfxSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "ecs/Components.h"
#include "ecs/components/VfxComponent.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/DeferredRelease.h"
#include "graphics/GpuMarker.h"
#include "graphics/GpuTimer.h"
#include "renderer/ViewDesc.h"
#include "renderer/vfx/VfxCurl.h"
#include "renderer/vfx/VfxFlipbook.h"
#include "renderer/FrameConstants.h"
#include "renderer/vfx/VfxRuntime.h"
#include "resource/ShaderCompiler.h"

using Microsoft::WRL::ComPtr;

namespace dx12e::vfx
{

namespace
{
// HLSL の SimCB / DrawCB と一致（36 DWORD）
struct SimCB
{
    float wRow[3][4];
    float prev[4];
    u32   uA[4];   // base, cap, firstId, count
    u32   uB[4];   // initStart, initCount, updStart, updCount
    float fA[4];   // simTime, stepDt
    u32   uC[4];   // seed, statIdx, simCount, flags
    u32   uD[4];   // curlSrv
};
static_assert(sizeof(SimCB) == 36 * 4, "SimCB は 36 DWORD");

struct DrawCB
{
    float viewProj[16];
    float camRight[4];
    float camUp[4];
    float params[4];
    float params2[4];
    u32   uP[4];
};
static_assert(sizeof(DrawCB) == 36 * 4, "DrawCB は 36 DWORD");

constexpr DXGI_FORMAT kSceneColorFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;   // kSceneColorFormat と同じ
constexpr u32 kNone = 0xFFFFFFFFu;

u32 Groups(u32 n) { return (n + kSimThreads - 1) / kSimThreads; }

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE t)
{
    D3D12_HEAP_PROPERTIES h{};
    h.Type = t;
    return h;
}

bool CreateBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, u64 size, D3D12_RESOURCE_FLAGS flags,
                  D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    const D3D12_HEAP_PROPERTIES hp = HeapProps(type);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out)));
}

void MatrixRows(const DirectX::XMMATRIX& m, float rows[12])
{
    DirectX::XMFLOAT4X4 f;
    DirectX::XMStoreFloat4x4(&f, m);
    // 行ベクトル規約 p' = p * M → 列形式の行 j = (M_0j, M_1j, M_2j, M_3j)
    rows[0] = f._11; rows[1] = f._21; rows[2]  = f._31; rows[3]  = f._41;
    rows[4] = f._12; rows[5] = f._22; rows[6]  = f._32; rows[7]  = f._42;
    rows[8] = f._13; rows[9] = f._23; rows[10] = f._33; rows[11] = f._43;
}
} // namespace

VfxSystem::VfxSystem() = default;
VfxSystem::~VfxSystem()
{
    if (m_opsMapped && m_opsUpload) m_opsUpload->Unmap(0, nullptr);
    for (int i = 0; i < 4; ++i)
        if (m_readbackMapped[i] && m_readback[i]) m_readback[i]->Unmap(0, nullptr);
    // GPU がまだ使っているかもしれないものは、フェンスを待つ DeferredRelease へ渡す（無効のときは即時解放）。
    // 記述子（m_curlSrv / m_dummyDepthSrv）は返さない: VfxSystem は FeatureRegistry の最後のメンバで、SRV ヒープは
    // Application の終了処理で先に破棄済み。VfxSystem は 1 つだけで再確保もしないので、ヒープごと消えるのが正。
    ComPtr<ID3D12Resource>* const owned[] = { &m_pool, &m_stats, &m_opsUpload, &m_curl, &m_dummyDepth, &m_readback[0], &m_readback[1], &m_readback[2], &m_readback[3] };
    for (ComPtr<ID3D12Resource>* r : owned)
        if (*r) DeferredRelease::Defer(std::move(*r), nullptr);
    for (auto& kv : m_flipTex)
        if (kv.second.res) DeferredRelease::Defer(std::move(kv.second.res), nullptr);
}

// ===========================================================================
// 初期化
// ===========================================================================
bool VfxSystem::CreatePipelines(ID3D12Device* dev)
{
    {
        D3D12_ROOT_PARAMETER p[4]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 36;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;   p[1].Descriptor.ShaderRegister = 0;   // t0 = ops
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;   p[2].Descriptor.ShaderRegister = 0;   // u0 = pool
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;   p[3].Descriptor.ShaderRegister = 1;   // u1 = stats
        D3D12_STATIC_SAMPLER_DESC s{};
        s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = 0;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 4; d.pParameters = p; d.NumStaticSamplers = 1; d.pStaticSamplers = &s;
        d.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
        if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_simRS)))) return false;
    }
    {
        D3D12_ROOT_PARAMETER p[3]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = 36;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor.ShaderRegister = 0;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        // b1 = FlipCB（フリップブック。VfxSpriteDraw.hlsl）。36 + 2 + 24 = 62 DWORD（上限 64）
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[2].Constants.ShaderRegister = 1; p[2].Constants.Num32BitValues = 24;
        D3D12_STATIC_SAMPLER_DESC ss{};   // s0 = 線形・クランプ（フリップブックは枠の半テクセル内側へ自前でクランプする）
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderRegister = 0;
        ss.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 3; d.pParameters = p; d.NumStaticSamplers = 1; d.pStaticSamplers = &ss;
        d.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
        if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_drawRS)))) return false;
    }
    const std::wstring dir = PathResolver::ShaderDirW();
    auto cs = [&](const wchar_t* name, ComPtr<ID3D12PipelineState>& out) -> bool
    {
        auto bc = ShaderCompiler::LoadFromFile(dir + name);
        if (bc.GetSize() == 0) { Logger::Error("UnoFX: コンピュートシェーダを読めない（ビルドの Shaders を確認）");
            (void)name; return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_simRS.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        return SUCCEEDED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    if (!cs(L"VfxEmit_CS.cso", m_psoEmit) || !cs(L"VfxSim_CS.cso", m_psoSim) ||
        !cs(L"VfxClear_CS.cso", m_psoClear) || !cs(L"VfxStatClear_CS.cso", m_psoStatClear)) return false;

    auto vs = ShaderCompiler::LoadFromFile(dir + L"VfxDraw_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(dir + L"VfxDraw_PS.cso");
    if (vs.GetSize() == 0 || ps.GetSize() == 0) { Logger::Error("UnoFX: 描画シェーダを読めない"); return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
    g.pRootSignature = m_drawRS.Get();
    g.VS = { vs.GetData(), vs.GetSize() };
    g.PS = { ps.GetData(), ps.GetSize() };
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable = TRUE;
    auto& rt = g.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;  rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO; rt.DestBlendAlpha = D3D12_BLEND_ONE; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;   // シーンの α は変えない
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.DepthStencilState.DepthEnable = FALSE;
    g.SampleMask = UINT_MAX;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = kSceneColorFmt;
    g.SampleDesc = {1, 0};
    if (FAILED(dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&m_psoDraw)))) return false;
    // フリップブック（BD7）。無くても UnoFX は動く（flipbook はスプライトへ縮退する）
    auto fvs = ShaderCompiler::LoadFromFile(dir + L"VfxSpriteDraw_VS.cso");
    auto fps = ShaderCompiler::LoadFromFile(dir + L"VfxSpriteDraw_PS.cso");
    if (fvs.GetSize() == 0 || fps.GetSize() == 0) { Logger::Warn("UnoFX: フリップブックの描画シェーダを読めない（Shaders を先にビルド）。flipbook はスプライトで描く"); return true; }
    g.VS = { fvs.GetData(), fvs.GetSize() };
    g.PS = { fps.GetData(), fps.GetSize() };
    if (FAILED(dev->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&m_psoFlip)))) Logger::Warn("UnoFX: フリップブックの PSO を作れない。flipbook はスプライトで描く");
    return true;
}

bool VfxSystem::CreateBuffers(ID3D12Device* dev, StageCtx& ctx)
{
    // バッファは COMMON で作られる（初期状態の指定は無視される）。最初の使用の前に明示の遷移を入れる
    if (!CreateBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, u64(kPoolCapacity) * sizeof(GPart), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, m_pool)) return false;
    m_poolState = D3D12_RESOURCE_STATE_COMMON;
    if (!CreateBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, kMaxStatSlots * sizeof(u32), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, m_stats)) return false;
    m_statsState = D3D12_RESOURCE_STATE_COMMON;
    if (!CreateBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, u64(kMaxOpsTotal) * sizeof(GOp), D3D12_RESOURCE_FLAG_NONE,
                      D3D12_RESOURCE_STATE_GENERIC_READ, m_opsUpload)) return false;
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(m_opsUpload->Map(0, &none, &mapped))) return false;
    m_opsMapped = static_cast<GOp*>(mapped);
    for (int i = 0; i < 4; ++i)
    {
        if (!CreateBuffer(dev, D3D12_HEAP_TYPE_READBACK, kMaxStatSlots * sizeof(u32), D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, m_readback[i])) return false;
        void* rm = nullptr;
        if (FAILED(m_readback[i]->Map(0, nullptr, &rm))) return false;
        m_readbackMapped[i] = static_cast<u32*>(rm);
        std::memset(rm, 0, kMaxStatSlots * sizeof(u32));
    }
    // 深度が無いビュー用のダミー SRV（GBV: 未使用でも有効な記述子を張っておく）
    {
        const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT; d.SampleDesc = {1, 0};
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&m_dummyDepth)))) return false;
        m_dummyDepthSrv = ctx.descriptors->AllocateIndex();
        if (m_dummyDepthSrv == kNone) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(m_dummyDepth.Get(), &sv, ctx.descriptors->GetCpuHandle(m_dummyDepthSrv));
    }
    m_vram = u64(kPoolCapacity) * sizeof(GPart) + kMaxStatSlots * 4 + u64(kMaxOpsTotal) * sizeof(GOp)
           + u64(kCurlRes) * kCurlRes * kCurlRes * 8;
    return true;
}

bool VfxSystem::CreateCurlField(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, StageCtx& ctx, const std::vector<u16>& half)
{
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = kCurlRes; d.Height = kCurlRes; d.DepthOrArraySize = static_cast<UINT16>(kCurlRes); d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc = {1, 0};
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_curl)))) return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ComPtr<ID3D12Resource> upload;
    if (!CreateBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, upload)) return false;
    u8* p = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&p)))) return false;
    for (u32 z = 0; z < kCurlRes; ++z)
        for (u32 y = 0; y < kCurlRes; ++y)
        {
            u8* dst = p + u64(z) * fp.Footprint.RowPitch * kCurlRes + u64(y) * fp.Footprint.RowPitch;
            const u16* src = &half[(u64(z) * kCurlRes * kCurlRes + u64(y) * kCurlRes) * 4];
            std::memcpy(dst, src, kCurlRes * 8);
        }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{}, srcLoc{};
    dstLoc.pResource = m_curl.Get(); dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstLoc.SubresourceIndex = 0;
    srcLoc.pResource = upload.Get(); srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; srcLoc.PlacedFootprint = fp;
    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    Transition(cmd, m_curl.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    DeferredRelease::Defer(std::move(upload), nullptr);

    m_curlSrv = ctx.descriptors->AllocateIndex();
    if (m_curlSrv == kNone) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = d.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture3D.MipLevels = 1;
    dev->CreateShaderResourceView(m_curl.Get(), &sv, ctx.descriptors->GetCpuHandle(m_curlSrv));
    return true;
}

bool VfxSystem::EnsureGpu(StageCtx& ctx, bool wait)
{
    if (m_gpuReady) return true;
    if (m_gpuFailed) return false;
    ComPtr<ID3D12Device> dev;
    if (!ctx.cmd || !ctx.descriptors || FAILED(ctx.cmd->GetDevice(IID_PPV_ARGS(&dev))))
    {
        m_gpuFailed = true; m_gpuError = "UnoFX: コマンドリストかデスクリプタヒープが無い";
        return false;
    }
    if (!m_gpuBase)
    {
        if (!CreatePipelines(dev.Get()) || !CreateBuffers(dev.Get(), ctx))
        {
            m_gpuFailed = true; m_gpuError = "UnoFX: GPU リソース / パイプラインを作れなかった（ログとシェーダの .cso を確認）";
            Logger::Error("{}。UnoFX を無効にします", m_gpuError);
            return false;
        }
        // 64^3 のカール場の生成（約 240 万回のノイズ評価）は別スレッドへ。出来るまで今フレームは見送る
        m_curlFuture = std::async(std::launch::async, [] { return GenerateCurlFieldHalf(); });
        m_gpuBase = true;
    }
    if (!wait && m_curlFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    const std::vector<u16> half = m_curlFuture.get();
    if (!CreateCurlField(dev.Get(), ctx.cmd, ctx, half))
    {
        m_gpuFailed = true; m_gpuError = "UnoFX: カール場のテクスチャを作れなかった";
        Logger::Error("{}。UnoFX を無効にします", m_gpuError);
        return false;
    }
    m_ranges.clear();
    m_ranges.push_back({0, kPoolCapacity, false});
    m_gpuReady = true;
    Logger::Info("UnoFX: 初期化（プール {} 粒子 x {}B、カール場 {}^3）", kPoolCapacity, sizeof(GPart), kCurlRes);
    return true;
}

// ===========================================================================
// フリップブックのテクスチャ（BD7）
// ===========================================================================
VfxSystem::FlipTex* VfxSystem::GetFlipTex(StageCtx& ctx, const std::string& path)
{
    auto it = m_flipTex.find(path);
    if (it != m_flipTex.end() && (it->second.ok || it->second.gen == m_assetGen)) return it->second.ok ? &it->second : nullptr;
    FlipTex& t = m_flipTex[path];
    t.gen = m_assetGen; t.ok = false; t.error.clear();
    const Host& host = Runtime::Get().GetHost();
    const std::vector<u8> bytes = host.readAsset ? host.readAsset(host.user, path) : vfs::ReadAsset(path);
    if (bytes.empty()) { t.error = "ファイルが無い / 読めない: " + path; Logger::Warn("UnoFX: {}", t.error); return nullptr; }
    flip::DdsInfo info; std::string err;
    if (!flip::ParseDdsHeader(bytes.data(), bytes.size(), info, err)) { t.error = path + ": " + err; Logger::Warn("UnoFX: {}", t.error); return nullptr; }
    ComPtr<ID3D12Device> dev;
    if (!ctx.cmd || !ctx.descriptors || FAILED(ctx.cmd->GetDevice(IID_PPV_ARGS(&dev)))) { t.error = "コマンドリストが無い"; return nullptr; }

    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = info.w; d.Height = info.h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = static_cast<DXGI_FORMAT>(info.dxgi); d.SampleDesc = {1, 0};
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res))))
    { t.error = path + ": テクスチャを作れない"; Logger::Warn("UnoFX: {}", t.error); return nullptr; }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ComPtr<ID3D12Resource> upload;
    if (!CreateBuffer(dev.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, upload))
    { t.error = path + ": アップロード用バッファを作れない"; return nullptr; }
    u8* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mapped)))) { t.error = path + ": アップロード用バッファを開けない"; return nullptr; }
    const u8* src = bytes.data() + info.dataOffset;
    for (u32 y = 0; y < info.h; ++y)
        std::memcpy(mapped + u64(y) * fp.Footprint.RowPitch, src + u64(y) * info.w * info.bpp, u64(info.w) * info.bpp);
    upload->Unmap(0, nullptr);
    // SRV を先に確保する（コピーを記録したあとで失敗すると、記録済みのコマンドが参照する res がすぐ解放される）
    const u32 srv = ctx.descriptors->AllocateIndex();
    if (srv == kNone) { t.error = "SRV ヒープが一杯"; Logger::Warn("UnoFX: {}", t.error); return nullptr; }
    D3D12_TEXTURE_COPY_LOCATION dstLoc{}, srcLoc{};
    dstLoc.pResource = res.Get(); dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.pResource = upload.Get(); srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; srcLoc.PlacedFootprint = fp;
    ctx.cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    Transition(ctx.cmd, res.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    DeferredRelease::Defer(std::move(upload), nullptr);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = d.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(res.Get(), &sv, ctx.descriptors->GetCpuHandle(srv));
    // 読めたテクスチャは再読込しない（ok の間は上の早期 return）。DDS を焼き直したときはエンジンの再起動が要る
    t.res = std::move(res); t.srv = srv; t.w = info.w; t.h = info.h; t.bpp = info.bpp; t.dxgi = info.dxgi; t.ok = true;
    m_vram += u64(info.w) * info.h * info.bpp;
    Logger::Info("UnoFX: フリップブックのテクスチャ {}（{}x{}）", path, info.w, info.h);
    return &t;
}

void VfxSystem::PrepareFlipbook(StageCtx& ctx, const RenderDesc& r)
{
    if (r.kind != RenderKind::Flipbook || r.texture.empty()) return;
    GetFlipTex(ctx, r.texture);
    if (!r.flipDbf.empty()) GetFlipTex(ctx, r.flipDbf);
    if (!r.flipMv.empty()) GetFlipTex(ctx, r.flipMv);
}

const VfxSystem::FlipTex* VfxSystem::FindFlipTex(const std::string& path) const
{
    auto it = m_flipTex.find(path);
    return it != m_flipTex.end() && it->second.ok ? &it->second : nullptr;
}

// FlipCB（24 DWORD。VfxSpriteDraw.hlsl の b1）を組む。テクスチャが足りなければ false。
bool VfxSystem::BindFlipbook(const RenderDesc& r, u32 out[24], const StageCtx& ctx) const
{
    if (!m_psoFlip || r.kind != RenderKind::Flipbook) return false;
    const FlipTex* t0 = FindFlipTex(r.texture);
    if (!t0) return false;
    // 役割と形式の検査（間違った入力が黙って変な絵になるのを防ぐ。理由は 1 回だけ警告）
    auto bad = [&](const FlipTex* t, const std::string& path, flip::TexRole role) -> bool
    {
        std::string why = flip::CheckTex(role, t->dxgi, t->w, t->h, r.flipCols, r.flipRows);
        if (why.empty() && role != flip::TexRole::Color) why = flip::CheckSameGrid(role, t0->w, t0->h, t->w, t->h);
        if (why.empty()) return false;
        if (m_flipWarned.insert("check|" + path).second) Logger::Warn("UnoFX: フリップブック {}: {}", path, why);
        return true;
    };
    if (bad(t0, r.texture, flip::TexRole::Color)) return false;
    const bool smoke = r.flipChannels == "smoke6way";
    const FlipTex* t1 = smoke ? FindFlipTex(r.flipDbf) : nullptr;
    if (smoke && (!t1 || bad(t1, r.flipDbf, flip::TexRole::Dbf))) return false;
    const FlipTex* tm = r.flipMv.empty() ? nullptr : FindFlipTex(r.flipMv);   // MV が読めない / 合わなければ補間なし（絵は出す）
    if (tm && bad(tm, r.flipMv, flip::TexRole::Mv)) tm = nullptr;
    const u32 channel = smoke ? 1u : (r.flipChannels == "color" ? 2u : 0u);
    const u32 frames = r.flipFrames ? r.flipFrames : r.flipCols * r.flipRows;
    // 光: 太陽（表面から光へ = 進む向きの逆）、環境光は灰色のスカラー（IBL の色味は使わない仮置き。W2 の VFX-05 で TLV に置き換える）
    float L[3] = {0.0f, 1.0f, 0.0f}, sun[3] = {0, 0, 0}, amb[3] = {0.3f, 0.3f, 0.3f};
    if (const FrameConstantsData* fc = ctx.perFrameCpu)
    {
        L[0] = -fc->lightDir.x; L[1] = -fc->lightDir.y; L[2] = -fc->lightDir.z;
        sun[0] = fc->lightColor.x; sun[1] = fc->lightColor.y; sun[2] = fc->lightColor.z;
        const float a = fc->ambientStrength * (fc->hasIBL ? fc->iblIntensity : 1.0f);
        amb[0] = amb[1] = amb[2] = a;
    }
    const float f[16] = {
        r.flipFps, r.flipMvRange, r.flipErodeStart, r.flipErodeWidth,
        L[0], L[1], L[2], r.flipTempMin,
        sun[0], sun[1], sun[2], r.flipTempMax,
        amb[0], amb[1], amb[2], r.flipEmission };
    out[0] = t0->srv; out[1] = t1 ? t1->srv : kNone; out[2] = tm ? tm->srv : kNone;
    out[3] = channel | (r.AnchorBottom() ? 16u : 0u);
    out[4] = r.flipCols; out[5] = r.flipRows; out[6] = frames;
    std::memcpy(out + 7, &r.flipRootFade, sizeof(float));
    std::memcpy(out + 8, f, sizeof(f));
    return true;
}

// ===========================================================================
// アセット / インスタンス
// ===========================================================================
VfxSystem::AssetEntry* VfxSystem::GetAsset(const std::string& path)
{
    auto it = m_assets.find(path);
    if (it != m_assets.end() && it->second.gen == m_assetGen) return it->second.ok ? &it->second : nullptr;
    // 初回、またはアセット世代が進んだ（置き直した）: 内容を読む。同じ内容なら作り直さない
    AssetEntry& e = m_assets[path];
    const bool had = it != m_assets.end();
    e.gen = m_assetGen;
    const Host& host = Runtime::Get().GetHost();
    const std::vector<u8> bytes = host.readAsset ? host.readAsset(host.user, path) : vfs::ReadAsset(path);
    if (bytes.empty())
    {
        if (e.ok) return &e;   // 一時的に読めないだけなら、動いているものは止めない
        e.error = "ファイルが無い / 読めない: " + path; Logger::Warn("UnoFX: {}", e.error); return nullptr;
    }
    u64 h = 1469598103934665603ull;   // FNV-1a
    for (u8 c : bytes) { h ^= c; h *= 1099511628211ull; }
    if (had && e.hash == h) return e.ok ? &e : nullptr;   // 同じ内容（失敗していたものも同じ理由で失敗するので再試行しない）
    e.ok = false; e.hash = h; e.error.clear();
    ParseResult pr = ParseUvfx(std::string(bytes.begin(), bytes.end()));
    if (!pr.ok) { e.error = path + ": " + pr.error; Logger::Warn("UnoFX: {}", e.error); return nullptr; }
    for (const std::string& w : pr.warnings) Logger::Warn("UnoFX: {}: {}", path, w);
    // 命令列をアップロード用バッファへ追記する（追記のみ。使用中のフレームが読む領域は書き換えない）
    std::vector<CompiledEmitter> compiled;
    u32 need = 0;
    for (const EmitterDesc& em : pr.asset.emitters)
    {
        CompiledEmitter ce; std::string err;
        if (!CompileEmitter(em, ce, err)) { e.error = path + ": " + err; Logger::Warn("UnoFX: {}", e.error); return nullptr; }
        need += static_cast<u32>(ce.init.size() + ce.update.size());
        compiled.push_back(std::move(ce));
    }
    if (m_opsUsed + need > kMaxOpsTotal)
    {
        e.error = path + ": 命令の置き場が足りない（" + std::to_string(kMaxOpsTotal) + " 命令。再起動で戻る）";
        if (!m_opsFullWarned) { m_opsFullWarned = true; Logger::Warn("UnoFX: {}", e.error); }
        return nullptr;
    }
    e.asset = std::move(pr.asset);
    e.em.clear();
    for (const CompiledEmitter& ce : compiled)
    {
        AssetEntry::Em em;
        em.initStart = m_opsUsed; em.initCount = static_cast<u32>(ce.init.size());
        std::memcpy(m_opsMapped + m_opsUsed, ce.init.data(), ce.init.size() * sizeof(GOp));
        m_opsUsed += em.initCount;
        em.updStart = m_opsUsed; em.updCount = static_cast<u32>(ce.update.size());
        std::memcpy(m_opsMapped + m_opsUsed, ce.update.data(), ce.update.size() * sizeof(GOp));
        m_opsUsed += em.updCount;
        em.maxLife = ce.maxLife;
        e.em.push_back(em);
    }
    e.ok = true;
    return &e;
}

bool VfxSystem::AllocRange(u32 size, u32& base)
{
    for (size_t i = 0; i < m_ranges.size(); ++i)
    {
        Range& r = m_ranges[i];
        if (r.used || r.size < size) continue;
        base = r.base;
        if (r.size > size) m_ranges.insert(m_ranges.begin() + static_cast<ptrdiff_t>(i) + 1, Range{r.base + size, r.size - size, false});
        m_ranges[i].size = size; m_ranges[i].used = true;
        return true;
    }
    return false;
}

void VfxSystem::FreeRange(u32 base, u32 size)
{
    for (size_t i = 0; i < m_ranges.size(); ++i)
        if (m_ranges[i].base == base && m_ranges[i].used && m_ranges[i].size == size)
        {
            m_ranges[i].used = false;
            if (i + 1 < m_ranges.size() && !m_ranges[i + 1].used) { m_ranges[i].size += m_ranges[i + 1].size; m_ranges.erase(m_ranges.begin() + static_cast<ptrdiff_t>(i) + 1); }
            if (i > 0 && !m_ranges[i - 1].used) { m_ranges[i - 1].size += m_ranges[i].size; m_ranges.erase(m_ranges.begin() + static_cast<ptrdiff_t>(i)); }
            return;
        }
}

void VfxSystem::Release(Instance& inst)
{
    FreeRange(inst.base, inst.cap);
    if (inst.statSlot < kMaxStatSlots) m_statUsed[inst.statSlot] = false;
}

VfxSystem::Instance* VfxSystem::FindOrCreate(entt::entity e, u32 emitter, const std::string& assetPath, u32 cap, u32 seed, bool autoplay)
{
    const u64 key = (u64(entt::to_integral(e)) << 8) | emitter;
    auto it = m_instances.find(key);
    if (it != m_instances.end())
    {
        Instance& in = it->second;
        if (in.assetPath == assetPath && in.cap == cap && in.seed == seed) return &in;
        Release(in);
        m_instances.erase(it);
    }
    Instance in;
    in.entity = e; in.emitter = emitter; in.assetPath = assetPath; in.cap = cap; in.seed = seed;
    if (!AllocRange(cap, in.base))
    {
        if (!m_poolWarned) { m_poolWarned = true; Logger::Warn("UnoFX: 粒子プールが足りない（{} 個）", cap); }   // 毎フレームは出さない
        return nullptr;
    }
    in.statSlot = kMaxStatSlots;
    for (u32 s = 0; s < kMaxStatSlots; ++s) if (!m_statUsed[s]) { in.statSlot = s; m_statUsed[s] = true; break; }
    if (in.statSlot == kMaxStatSlots)
    {
        FreeRange(in.base, cap);
        if (!m_poolWarned) { m_poolWarned = true; Logger::Warn("UnoFX: 統計のスロットが足りない（{} 個まで）", kMaxStatSlots); }
        return nullptr;
    }
    m_poolWarned = false;
    in.paused = !autoplay;
    in.fresh = true;
    auto res = m_instances.emplace(key, std::move(in));
    return &res.first->second;
}

// ===========================================================================
// 記録
// ===========================================================================
void VfxSystem::UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cmd->ResourceBarrier(1, &b);
}

void VfxSystem::Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

void VfxSystem::BeginCompute(StageCtx& ctx)
{
    if (m_computeBound) return;
    m_computeBound = true;
    ID3D12GraphicsCommandList* cmd = ctx.cmd;
    Transition(cmd, m_pool.Get(), m_poolState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_poolState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Transition(cmd, m_stats.Get(), m_statsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_statsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12DescriptorHeap* heaps[] = { ctx.descriptors->GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_simRS.Get());
    cmd->SetComputeRootShaderResourceView(1, m_opsUpload->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, m_pool->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, m_stats->GetGPUVirtualAddress());
}

VfxSystem::PendingStep VfxSystem::PlanStep(Instance& inst, const AssetEntry& a, const EmitterDesc& em, const AssetEntry::Em& aem,
                                           const float rows[12], const float curPos[3], double stepDt, bool isLast, float emissionMul)
{
    const double t0 = inst.clk.time;
    const SpawnStep plan = PlanSpawnStep(em.spawn, a.asset.loop, a.asset.duration, stepDt, emissionMul, inst.cap, inst.clk);
    PendingStep ps;
    ps.simCount = std::min(plan.firstId, inst.cap);
    ps.emitCount = plan.count;
    ps.statClear = isLast;

    SimCB cb{};
    std::memcpy(cb.wRow, rows, sizeof(cb.wRow));
    cb.prev[0] = inst.havePrev ? inst.prevPos[0] : curPos[0];
    cb.prev[1] = inst.havePrev ? inst.prevPos[1] : curPos[1];
    cb.prev[2] = inst.havePrev ? inst.prevPos[2] : curPos[2];
    cb.uA[0] = inst.base; cb.uA[1] = inst.cap; cb.uA[2] = plan.firstId; cb.uA[3] = plan.count;
    cb.uB[0] = aem.initStart; cb.uB[1] = aem.initCount; cb.uB[2] = aem.updStart; cb.uB[3] = aem.updCount;
    cb.fA[0] = static_cast<float>(t0); cb.fA[1] = static_cast<float>(stepDt);
    cb.uC[0] = inst.seed; cb.uC[1] = inst.statSlot; cb.uC[2] = ps.simCount; cb.uC[3] = isLast ? 1u : 0u;
    cb.uD[0] = m_curlSrv;
    static_assert(sizeof(ps.cb) == sizeof(cb), "PendingStep::cb は SimCB と同じ大きさ");
    std::memcpy(ps.cb, &cb, sizeof(cb));
    return ps;
}

// 計画した全インスタンスのステップを、フェーズごとにまとめて記録する。
//   インスタンスのプール区間は互いに素なので、同じフェーズの中ではバリアが要らない。
//   フェーズの境目（統計クリア -> sim -> emit）にだけフルのバリアを 1 つ入れる。seek の多段は「ラウンド」で全インスタンスを同時に進める。
void VfxSystem::Execute(StageCtx& ctx, std::vector<Work>& work)
{
    bool any = false;
    for (const Work& w : work) if (w.clear || !w.steps.empty()) { any = true; break; }
    if (!any) return;   // 何も走らせない: 遷移もバリアも記録しない
    ID3D12GraphicsCommandList* cmd = ctx.cmd;
    BeginCompute(ctx);
    m_statsDirty = 4;

    bool anyClear = false;
    for (const Work& w : work)
    {
        if (!w.clear) continue;
        if (!anyClear) { cmd->SetPipelineState(m_psoClear.Get()); anyClear = true; }
        SimCB cb{};
        cb.uA[0] = w.in->base; cb.uA[1] = w.in->cap;
        cmd->SetComputeRoot32BitConstants(0, 36, &cb, 0);
        cmd->Dispatch(Groups(w.in->cap), 1, 1);
    }
    if (anyClear) UavBarrier(cmd, m_pool.Get());

    size_t rounds = 0;
    for (const Work& w : work) rounds = std::max(rounds, w.steps.size());
    for (size_t r = 0; r < rounds; ++r)
    {
        bool did = false;
        for (const Work& w : work)
        {
            if (r >= w.steps.size() || !w.steps[r].statClear) continue;
            if (!did) { cmd->SetPipelineState(m_psoStatClear.Get()); did = true; }
            cmd->SetComputeRoot32BitConstants(0, 36, w.steps[r].cb, 0);
            cmd->Dispatch(1, 1, 1);
        }
        if (did) UavBarrier(cmd, m_stats.Get());

        did = false;
        for (const Work& w : work)
        {
            if (r >= w.steps.size() || w.steps[r].simCount == 0) continue;
            if (!did) { cmd->SetPipelineState(m_psoSim.Get()); did = true; }
            cmd->SetComputeRoot32BitConstants(0, 36, w.steps[r].cb, 0);
            cmd->Dispatch(Groups(w.steps[r].simCount), 1, 1);
        }
        if (did) UavBarrier(cmd, m_pool.Get());

        did = false;
        for (const Work& w : work)
        {
            if (r >= w.steps.size() || w.steps[r].emitCount == 0) continue;
            if (!did) { cmd->SetPipelineState(m_psoEmit.Get()); did = true; }
            cmd->SetComputeRoot32BitConstants(0, 36, w.steps[r].cb, 0);
            cmd->Dispatch(Groups(w.steps[r].emitCount), 1, 1);
        }
        if (did) UavBarrier(cmd, m_pool.Get());
    }
}

void VfxSystem::PublishStats(ID3D12GraphicsCommandList* cmd)
{
    // GPU の統計（生存数）を読み戻しリングへ。インクリメント後に (written - 4) % 4 を読む = 3 コピー前に書いた分。
    // 安全性は BeginFrame がフレーム N-3 のフェンスを待つこと（多重化 3）に依る。多重化数を増やすならここでフェンスを明示すること。
    // 計算が走ったフレームから 4 フレームだけコピーを続ける（止まっている間は何も記録しない）。
    if (cmd && m_gpuReady && !m_instances.empty() && m_statsDirty > 0)
    {
        --m_statsDirty;
        const u32 w = m_readbackWritten % 4;
        Transition(cmd, m_stats.Get(), m_statsState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(m_readback[w].Get(), 0, m_stats.Get(), 0, kMaxStatSlots * sizeof(u32));
        Transition(cmd, m_stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_statsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ++m_readbackWritten;
        if (m_readbackWritten >= 4)
            std::memcpy(m_alive, m_readbackMapped[(m_readbackWritten - 4) % 4], sizeof(m_alive));
    }
    Stats st;
    st.gpuReady = m_gpuReady;
    st.gpuFailed = m_gpuFailed;
    st.poolCapacity = kPoolCapacity;
    st.vramBytes = m_gpuReady ? m_vram : 0;
    for (const auto& kv : m_instances)
    {
        const Instance& in = kv.second;
        InstanceStat s;
        s.entity = static_cast<u32>(entt::to_integral(in.entity));
        s.emitter = in.emitter; s.cap = in.cap; s.spawned = in.clk.spawned;
        s.alive = m_alive[in.statSlot]; s.time = in.clk.time; s.paused = in.paused; s.finished = in.finished;
        st.list.push_back(s);
        st.poolUsed += in.cap;
    }
    st.instances = static_cast<u32>(m_instances.size());
    for (const auto& kv : m_assets) if (!kv.second.ok && !kv.second.error.empty()) st.lastError = kv.second.error;
    if (!m_gpuError.empty()) st.lastError = m_gpuError;
    Runtime::Get().PublishStats(std::move(st));
}

bool VfxSystem::WantsUpdate() const
{
    Runtime& rt = Runtime::Get();
    if (!rt.HasHost()) return false;
    if (!m_instances.empty() || rt.HasSeeks()) return true;
    const Host& host = rt.GetHost();
    entt::registry* reg = host.registry(host.user);
    return reg && !reg->view<VfxComponent>().empty();
}

bool VfxSystem::WantsDraw() const { return m_gpuReady && !m_instances.empty(); }

void VfxSystem::Update(StageCtx& ctx)
{
    Runtime& rt = Runtime::Get();
    if (!rt.HasHost()) return;
    const Host& host = rt.GetHost();
    // 保留中の配置: アプリの毎フレームの更新（VfxTick）が消化する。結線されていない間だけ、ここで消化する（従来の道）
    if (!rt.TickDriven() && host.pollPending) host.pollPending(host.user);
    entt::registry* reg = host.registry(host.user);
    if (!reg) return;

    auto view = reg->view<VfxComponent, Transform>();
    const bool any = view.begin() != view.end();
    if (!any && m_instances.empty())   // 何も無い: コマンドは 1 つも記録しない
    {
        Stats s; s.gpuReady = m_gpuReady; s.gpuFailed = m_gpuFailed; s.lastError = m_gpuError;
        s.poolCapacity = m_gpuReady ? kPoolCapacity : 0; s.vramBytes = m_gpuReady ? m_vram : 0;
        rt.PublishStats(std::move(s));
        rt.TakeSeeks();
        return;
    }

    const float dtHost = host.frameDt ? host.frameDt(host.user) : 0.0f;
    // 決定論の経路（seek 待ち / dt = 0 のキャプチャ）では、カール場の生成を待ってでも今フレームで始める
    if (!EnsureGpu(ctx, rt.HasSeeks() || dtHost <= 0.0f)) return;
    std::vector<SeekRequest> seeks = rt.TakeSeeks();
    m_assetGen = rt.AssetGeneration();

    float emissionMul = 1.0f; u32 qualityCap = 0;
    if (host.quality) host.quality(host.user, emissionMul, qualityCap);

    m_computeBound = false;
    for (auto& kv : m_instances) kv.second.touched = false;

    {
        GpuScope scope(ctx.cmd, ctx.timer, GpuTimer::VfxSim);
        std::vector<Work> work;
        // 品質プリセットの maxParticles は「シーン全体」の予算。エンティティ順に確保し、尽きたエミッターは出さない
        u64 budgetLeft = qualityCap > 0 ? qualityCap : ~0ull;
        for (entt::entity e : view)
        {
            const VfxComponent& comp = view.get<VfxComponent>(e);
            if (comp.asset.empty()) continue;
            AssetEntry* a = GetAsset(comp.asset);
            if (!a) continue;
            const DirectX::XMMATRIX world = ComputeWorldMatrix(*reg, e);
            float rows[12]; MatrixRows(world, rows);
            const float curPos[3] = { rows[3], rows[7], rows[11] };

            for (u32 ei = 0; ei < a->asset.emitters.size(); ++ei)
            {
                const EmitterDesc& em = a->asset.emitters[ei];
                const u32 cap = static_cast<u32>(std::min<u64>(em.maxParticles, budgetLeft));
                if (cap == 0) continue;   // 予算切れ（このエミッターは出ない）
                const u32 seed = comp.seed * 0x9E3779B1u + ei * 7919u + 1u;
                Instance* in = FindOrCreate(e, ei, comp.asset, cap, seed, comp.autoplay);
                if (!in) continue;
                budgetLeft -= cap;
                in->touched = true;
                const AssetEntry::Em& aem = a->em[ei];
                PrepareFlipbook(ctx, em.render);

                const SeekRequest* sk = nullptr;
                for (const SeekRequest& s : seeks)
                    if (s.entity == entt::null || s.entity == e) sk = &s;

                Work w; w.in = in;
                if (in->fresh) { w.clear = true; in->fresh = false; }   // 確保の直後: 再利用した範囲に残る前の粒子を消す
                if (sk)
                {
                    // 決定論の seek: 巻き戻して、固定ステップで t まで再シミュレーションする（放出体は今の位置に固定）
                    w.clear = true;
                    in->clk = SpawnClock{};
                    in->finished = false;
                    in->havePrev = true;
                    std::memcpy(in->prevPos, curPos, sizeof(curPos));
                    const double t = std::max(0.0, sk->t);
                    u32 n = static_cast<u32>(std::floor(t / kSeekStepDt + 1.0e-6));
                    if (n > kMaxSeekSteps) { Logger::Warn("UnoFX: seek が長すぎる（{} 秒まで）", kMaxSeekSteps / 60); n = kMaxSeekSteps; }
                    const double rem = t - double(n) * kSeekStepDt;
                    const bool hasRem = rem > 1.0e-6 && n < kMaxSeekSteps;
                    w.steps.reserve(n + 1);
                    for (u32 i = 0; i < n; ++i)
                        w.steps.push_back(PlanStep(*in, *a, em, aem, rows, curPos, kSeekStepDt, !hasRem && i + 1 == n, emissionMul));
                    if (hasRem) w.steps.push_back(PlanStep(*in, *a, em, aem, rows, curPos, rem, true, emissionMul));
                    in->paused = !sk->resume;
                }
                else if (!in->paused && !in->finished && dtHost > 0.0f)
                {
                    const double dt = std::min(double(dtHost) * std::max(0.0f, comp.timeScale), double(kMaxLiveDt));
                    if (dt > 0.0) w.steps.push_back(PlanStep(*in, *a, em, aem, rows, curPos, dt, true, emissionMul));
                }
                if (w.clear || !w.steps.empty()) work.push_back(std::move(w));
                std::memcpy(in->prevPos, curPos, sizeof(curPos));
                in->havePrev = true;
                if (!a->asset.loop && a->asset.duration > 0.0f &&
                    in->clk.time >= double(a->asset.duration) + double(em.spawn.startDelay) + double(aem.maxLife))
                    in->finished = true;
            }
        }
        Execute(ctx, work);
    }

    // 消えたエンティティ / コンポーネントのインスタンスを返す
    for (auto it = m_instances.begin(); it != m_instances.end();)
    {
        if (!it->second.touched) { Release(it->second); it = m_instances.erase(it); }
        else ++it;
    }
    ++m_frame;
    PublishStats(ctx.cmd);
}

void VfxSystem::Draw(StageCtx& ctx)
{
    if (!m_gpuReady || m_instances.empty() || !ctx.view || ctx.sceneRtv.ptr == 0 || !ctx.cmd || !ctx.descriptors) return;
    // 粒子を描かないビュー（反射プローブの撮影面など）には出さない。従来の粒子経路（kViewParticles）と同じ判定
    if (!ctx.view->Has(kViewParticles)) return;
    bool anyDraw = false;
    for (const auto& kv : m_instances) if (!kv.second.finished && kv.second.clk.spawned > 0) { anyDraw = true; break; }
    if (!anyDraw) return;

    ID3D12GraphicsCommandList* cmd = ctx.cmd;
    GpuScope scope(cmd, ctx.timer, GpuTimer::VfxDraw);
    Transition(cmd, m_pool.Get(), m_poolState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_poolState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // 深度（奥の断片の破棄とソフトパーティクル）。無いビューではダミー SRV を渡し、uP.w = 0 で読ませない
    u32 depthIdx = m_dummyDepthSrv;
    bool hasDepth = false;
    if (ctx.depth && ctx.depthSrv.ptr != 0)
    {
        ctx.RequireDepth(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const u64 start = ctx.descriptors->GetGpuHandle(0).ptr;
        depthIdx = static_cast<u32>((ctx.depthSrv.ptr - start) / ctx.descriptors->GetDescriptorSize());
        hasDepth = true;
    }

    ID3D12DescriptorHeap* heaps[] = { ctx.descriptors->GetHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_drawRS.Get());
    cmd->SetPipelineState(m_psoDraw.Get());
    cmd->SetGraphicsRootShaderResourceView(1, m_pool->GetGPUVirtualAddress());
    cmd->OMSetRenderTargets(1, &ctx.sceneRtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, float(ctx.internalW), float(ctx.internalH), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, LONG(ctx.internalW), LONG(ctx.internalH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const ViewDesc& v = *ctx.view;
    DrawCB cb{};
    DirectX::XMFLOAT4X4 vpT;
    DirectX::XMStoreFloat4x4(&vpT, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&v.viewProjJittered)));
    std::memcpy(cb.viewProj, &vpT, sizeof(cb.viewProj));
    // カメラの右 / 上（view 行列の回転の列）
    cb.camRight[0] = v.view._11; cb.camRight[1] = v.view._21; cb.camRight[2] = v.view._31;
    cb.camUp[0]    = v.view._12; cb.camUp[1]    = v.view._22; cb.camUp[2]    = v.view._32;
    cb.params2[0] = v.proj._33; cb.params2[1] = v.proj._43;
    cb.params2[2] = ctx.internalW ? 1.0f / float(ctx.internalW) : 0.0f;
    cb.params2[3] = ctx.internalH ? 1.0f / float(ctx.internalH) : 0.0f;
    cb.uP[1] = depthIdx;
    cb.uP[3] = hasDepth ? 1u : 0u;

    bool curFlip = false;
    for (auto& kv : m_instances)
    {
        const Instance& in = kv.second;
        if (in.finished || in.clk.spawned == 0) continue;
        auto ai = m_assets.find(in.assetPath);
        if (ai == m_assets.end() || !ai->second.ok || in.emitter >= ai->second.asset.emitters.size()) continue;
        const RenderDesc& r = ai->second.asset.emitters[in.emitter].render;
        float fade = 3.0f;
        switch (r.fade) { case Fade::None: fade = 0.0f; break; case Fade::Linear: fade = 1.0f; break; case Fade::Quad: fade = 2.0f; break; default: break; }
        cb.params[0] = r.intensity; cb.params[1] = r.softDistance; cb.params[2] = fade; cb.params[3] = r.stretch;
        cb.uP[0] = in.base;
        cb.uP[2] = (r.blend == Blend::Alpha ? 1u : 0u) | (v.orthographic ? 2u : 0u);
        u32 flipCb[24];
        const bool flipbook = BindFlipbook(r, flipCb, ctx);
        if (r.kind == RenderKind::Flipbook && !flipbook && m_flipWarned.insert(in.assetPath + "|" + r.texture).second)
        {
            Logger::Warn("UnoFX: flipbook のテクスチャ / シェーダが無いので、スプライトで描く（{}）", in.assetPath);
        }
        if (flipbook != curFlip)
        {
            cmd->SetPipelineState(flipbook ? m_psoFlip.Get() : m_psoDraw.Get());
            curFlip = flipbook;
        }
        if (flipbook) cmd->SetGraphicsRoot32BitConstants(2, 24, flipCb, 0);
        cmd->SetGraphicsRoot32BitConstants(0, 36, &cb, 0);
        cmd->DrawInstanced(6, std::min(in.clk.spawned, in.cap), 0, 0);
    }
}

} // namespace dx12e::vfx
