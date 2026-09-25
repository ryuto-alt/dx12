// 知覚層（dx12_perceive）の GPU 側。設計の説明は PerceptionPass.h の先頭。
#include "renderer/PerceptionPass.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/PipelineState.h"
#include "renderer/Mesh.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"

#include <algorithm>
#include <cstring>

namespace dx12e
{
namespace
{
constexpr DXGI_FORMAT kIdFormat   = DXGI_FORMAT_R32_UINT;
constexpr DXGI_FORMAT kPosFormat  = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr DXGI_FORMAT kNrmFormat  = DXGI_FORMAT_R8G8B8A8_SNORM;
constexpr DXGI_FORMAT kIsoFormat  = DXGI_FORMAT_R8_UINT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

// PerceptionId.hlsl の cbuffer とバイト単位で一致させること。
struct PerDrawCB
{
    DirectX::XMFLOAT4X4 model;   // transpose(world)
    u32   entityId;
    u32   flags;                 // bit0 = MASK
    float alphaCutoff;
    float pad;
};
static_assert(sizeof(PerDrawCB) == 20 * 4, "PerDrawCB は PerceptionId.hlsl の PerDraw(b0) と一致させること");
struct PerPassCB
{
    DirectX::XMFLOAT4X4 viewProj;   // transpose(viewProj)
    DirectX::XMFLOAT3   cameraPos;
    float               pad;
};
static_assert(sizeof(PerPassCB) == 20 * 4, "PerPassCB は PerceptionId.hlsl の PerPass(b1) と一致させること");

constexpr u32 kSlotPerDraw = 0, kSlotPerPass = 1, kSlotMask = 2, kSlotAlbedo = 3, kSlotBones = 4;

u64 AlignUp(u64 v, u64 a) { return (v + a - 1) / a * a; }

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
             D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    br.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource   = res;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter  = b;
    br.Transition.Subresource = 0;
    cmd->ResourceBarrier(1, &br);
}
} // namespace

void PerceptionPass::Initialize(GraphicsDevice& device, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // ---- ルートシグネチャ（46 DWORD。メインの 62/64 とは完全に別物）----
    {
        D3D12_DESCRIPTOR_RANGE albedoRange{};
        albedoRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        albedoRange.NumDescriptors     = 1;
        albedoRange.BaseShaderRegister = 0;   // t0
        D3D12_DESCRIPTOR_RANGE boneRange{};
        boneRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        boneRange.NumDescriptors     = 1;
        boneRange.BaseShaderRegister = 3;     // t3

        D3D12_ROOT_PARAMETER p[5]{};
        p[kSlotPerDraw].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kSlotPerDraw].Constants.ShaderRegister = 0;
        p[kSlotPerDraw].Constants.Num32BitValues = sizeof(PerDrawCB) / 4;
        p[kSlotPerDraw].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        p[kSlotPerPass].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kSlotPerPass].Constants.ShaderRegister = 1;
        p[kSlotPerPass].Constants.Num32BitValues = sizeof(PerPassCB) / 4;
        p[kSlotPerPass].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        p[kSlotMask].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kSlotMask].Constants.ShaderRegister = 2;
        p[kSlotMask].Constants.Num32BitValues = 4;
        p[kSlotMask].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
        p[kSlotAlbedo].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[kSlotAlbedo].DescriptorTable.NumDescriptorRanges = 1;
        p[kSlotAlbedo].DescriptorTable.pDescriptorRanges   = &albedoRange;
        p[kSlotAlbedo].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        p[kSlotBones].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[kSlotBones].DescriptorTable.NumDescriptorRanges = 1;
        p[kSlotBones].DescriptorTable.pDescriptorRanges   = &boneRange;
        p[kSlotBones].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderRegister   = 0;   // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = _countof(p);
        desc.pParameters       = p;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&m_rootSig)));
    }

    // ---- PSO（両面・深度 LESS。フォワードと同じく CULL_NONE）----
    const auto vs   = ShaderCompiler::LoadFromFile(shaderDir + L"PerceptionId_VS.cso");
    const auto vsSk = ShaderCompiler::LoadFromFile(shaderDir + L"PerceptionIdSkinned_VS.cso");
    const auto ps   = ShaderCompiler::LoadFromFile(shaderDir + L"PerceptionId_PS.cso");
    const DXGI_FORMAT mrt[3] = { kIdFormat, kPosFormat, kNrmFormat };
    auto build = [&](const ShaderCompiler::ShaderBytecode& v, bool isolation)
    {
        PipelineStateBuilder b;
        b.SetRootSignature(m_rootSig.Get())
         .SetVertexShader(v.GetData(), v.GetSize())
         .SetPixelShader(ps.GetData(), ps.GetSize())
         .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
         .SetCullMode(D3D12_CULL_MODE_NONE);
        if (isolation)
        {
            // 被覆マスク: 対象だけを深度なしで描く（自分どうしの重なりは同じ値なので順序に依らない）
            b.SetRenderTargetFormat(kIsoFormat)
             .SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN)
             .SetDepthEnabled(false);
        }
        else
        {
            b.SetRenderTargetFormats(3, mrt)
             .SetDepthStencilFormat(kDepthFormat)
             .SetDepthEnabled(true);
        }
        return b.Build(device);
    };
    m_psoStatic     = build(vs, false);
    m_psoSkinned    = build(vsSk, false);
    m_psoIsoStatic  = build(vs, true);
    m_psoIsoSkinned = build(vsSk, true);

    // RTV 4 本（ID / 位置 / 法線 / 被覆）+ DSV 1 本。シェーダ不可視の専用ヒープ。
    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 4;
    ThrowIfFailed(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_rtvHeap)));
    m_rtvStride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    ThrowIfFailed(dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_dsvHeap)));
}

void PerceptionPass::CreateReadback(ID3D12Device* dev, const D3D12_RESOURCE_DESC& texDesc, u32 count,
                                    ReadbackBuf& out, u64& strideOut)
{
    u32 rows = 0;
    u64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &out.fp, &rows, &rowSize, &total);
    // 複数枚を 1 本に並べるときの配置オフセットは 512 バイト境界が必須。
    strideOut = AlignUp(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    out.bytes = (count <= 1) ? total : strideOut * count;

    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = out.bytes;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    out.buf.Reset();
    ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out.buf)));
}

void PerceptionPass::CopyToReadback(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex,
                                    const ReadbackBuf& rb, u64 offset)
{
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = tex;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = rb.buf.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = rb.fp;
    dst.PlacedFootprint.Offset = offset;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

void PerceptionPass::EnsureTargets(GraphicsDevice& device, u32 w, u32 h, u32 isolationSlots)
{
    isolationSlots = (std::max)(isolationSlots, 1u);
    if (w == m_w && h == m_h && m_idTex && isolationSlots <= m_isoSlots) return;
    auto* dev = device.GetDevice();

    auto makeTex = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                       const D3D12_CLEAR_VALUE* clear, Microsoft::WRL::ComPtr<ID3D12Resource>& out)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC d{};
        d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width            = w;
        d.Height           = h;
        d.DepthOrArraySize = 1;
        d.MipLevels        = 1;
        d.Format           = fmt;
        d.SampleDesc.Count = 1;
        d.Flags            = flags;
        out.Reset();
        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, clear,
                                                   IID_PPV_ARGS(&out)));
        out->SetName(L"PerceptionPass");
    };
    auto rtClear = [](DXGI_FORMAT f) { D3D12_CLEAR_VALUE c{}; c.Format = f; return c; };   // 全部 0
    const D3D12_CLEAR_VALUE cId = rtClear(kIdFormat), cPos = rtClear(kPosFormat),
                            cNrm = rtClear(kNrmFormat), cIso = rtClear(kIsoFormat);
    D3D12_CLEAR_VALUE cDepth{};
    cDepth.Format = kDepthFormat;
    cDepth.DepthStencil.Depth = 1.0f;

    // ★RT は「使っていない間は RENDER_TARGET」で持つ（記録の終わりに必ず戻す）。
    makeTex(kIdFormat,  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cId,  m_idTex);
    makeTex(kPosFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cPos, m_posTex);
    makeTex(kNrmFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cNrm, m_nrmTex);
    makeTex(kIsoFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cIso, m_isoTex);
    makeTex(kDepthFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cDepth, m_depthTex);

    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    ID3D12Resource* rts[4] = { m_idTex.Get(), m_posTex.Get(), m_nrmTex.Get(), m_isoTex.Get() };
    for (u32 i = 0; i < 4; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE hnd{ rtv.ptr + static_cast<SIZE_T>(i) * m_rtvStride };
        dev->CreateRenderTargetView(rts[i], nullptr, hnd);
    }
    dev->CreateDepthStencilView(m_depthTex.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    u64 stride = 0;
    CreateReadback(dev, m_idTex->GetDesc(),  1, m_rbId,  stride);
    CreateReadback(dev, m_posTex->GetDesc(), 1, m_rbPos, stride);
    CreateReadback(dev, m_nrmTex->GetDesc(), 1, m_rbNrm, stride);
    CreateReadback(dev, m_isoTex->GetDesc(), isolationSlots, m_rbIso, m_isoStride);

    m_w = w;
    m_h = h;
    m_isoSlots = isolationSlots;
}

bool PerceptionPass::CaptureColor(ID3D12GraphicsCommandList* cmd, ID3D12Resource* backBuffer,
                                  u32 x, u32 y, u32 w, u32 h, std::string& err)
{
    m_colorValid = false;
    if (!cmd || !backBuffer) { err = "back buffer not ready"; return false; }
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    backBuffer->GetDevice(IID_PPV_ARGS(&dev));
    const D3D12_RESOURCE_DESC bb = backBuffer->GetDesc();
    const u32 fullW = static_cast<u32>(bb.Width), fullH = bb.Height;
    if (w == 0 || h == 0 || x >= fullW || y >= fullH) { err = "viewport rect is empty"; return false; }
    w = (std::min)(w, fullW - x);
    h = (std::min)(h, fullH - y);

    if (!m_rbColor.buf || w != m_colorW || h != m_colorH || static_cast<u32>(bb.Format) != m_colorFormat)
    {
        D3D12_RESOURCE_DESC region = bb;
        region.Width  = w;
        region.Height = h;
        u64 stride = 0;
        CreateReadback(dev.Get(), region, 1, m_rbColor, stride);
        m_colorW = w;
        m_colorH = h;
        m_colorFormat = static_cast<u32>(bb.Format);
    }
    // バックバッファは RENDER_TARGET（CaptureFinalBackBufferRegion と同じ契約）。往復させる。
    Barrier(cmd, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = backBuffer;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = m_rbColor.buf.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = m_rbColor.fp;
    const D3D12_BOX box{ x, y, 0, x + w, y + h, 1 };
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Barrier(cmd, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_colorValid = true;
    return true;
}

void PerceptionPass::BeginIds(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap,
                              const DirectX::XMMATRIX& viewProj, const DirectX::XMFLOAT3& cameraPos,
                              D3D12_GPU_DESCRIPTOR_HANDLE defaultAlbedo)
{
    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());

    const auto base = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3];
    for (u32 i = 0; i < 3; ++i) rtvs[i].ptr = base.ptr + static_cast<SIZE_T>(i) * m_rtvStride;
    const auto dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float zero[4] = { 0, 0, 0, 0 };
    for (auto& r : rtvs) cmd->ClearRenderTargetView(r, zero, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

    const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(m_w), static_cast<float>(m_h), 0.0f, 1.0f };
    const D3D12_RECT sc{ 0, 0, static_cast<LONG>(m_w), static_cast<LONG>(m_h) };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    PerPassCB pp{};
    DirectX::XMStoreFloat4x4(&pp.viewProj, DirectX::XMMatrixTranspose(viewProj));
    pp.cameraPos = cameraPos;
    cmd->SetGraphicsRoot32BitConstants(kSlotPerPass, sizeof(PerPassCB) / 4, &pp, 0);
    const float ident[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
    cmd->SetGraphicsRoot32BitConstants(kSlotMask, 4, ident, 0);
    cmd->SetGraphicsRootDescriptorTable(kSlotAlbedo, defaultAlbedo);
    // t3 はスキンドのドローが必ず張る（静的の VS は読まない）。未設定のままだと検証層が
    // 騒ぐことがあるので、既定値として同じ白を張っておく。
    cmd->SetGraphicsRootDescriptorTable(kSlotBones, defaultAlbedo);

    m_lastPso   = nullptr;
    m_isolation = false;
}

void PerceptionPass::SetDraw(ID3D12GraphicsCommandList* cmd, bool skinned, const DirectX::XMMATRIX& world,
                             u32 entityId, bool mask, float alphaCutoff, const float uvScaleOffset[4],
                             D3D12_GPU_DESCRIPTOR_HANDLE albedo, D3D12_GPU_DESCRIPTOR_HANDLE bones)
{
    ID3D12PipelineState* pso = m_isolation ? (skinned ? m_psoIsoSkinned.Get() : m_psoIsoStatic.Get())
                                           : (skinned ? m_psoSkinned.Get()    : m_psoStatic.Get());
    if (pso != m_lastPso) { cmd->SetPipelineState(pso); m_lastPso = pso; }

    PerDrawCB cb{};
    DirectX::XMStoreFloat4x4(&cb.model, DirectX::XMMatrixTranspose(world));
    cb.entityId    = entityId;
    cb.flags       = mask ? 1u : 0u;
    cb.alphaCutoff = alphaCutoff;
    cmd->SetGraphicsRoot32BitConstants(kSlotPerDraw, sizeof(PerDrawCB) / 4, &cb, 0);
    if (mask)
    {
        cmd->SetGraphicsRoot32BitConstants(kSlotMask, 4, uvScaleOffset, 0);
        cmd->SetGraphicsRootDescriptorTable(kSlotAlbedo, albedo);
    }
    if (skinned) cmd->SetGraphicsRootDescriptorTable(kSlotBones, bones);
}

void PerceptionPass::EndIds(ID3D12GraphicsCommandList* cmd)
{
    ID3D12Resource* rts[3] = { m_idTex.Get(), m_posTex.Get(), m_nrmTex.Get() };
    const ReadbackBuf* rbs[3] = { &m_rbId, &m_rbPos, &m_rbNrm };
    for (u32 i = 0; i < 3; ++i)
    {
        Barrier(cmd, rts[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyToReadback(cmd, rts[i], *rbs[i], 0);
        Barrier(cmd, rts[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
}

void PerceptionPass::BeginIsolation(ID3D12GraphicsCommandList* cmd, u32 /*slot*/)
{
    const auto base = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{ base.ptr + static_cast<SIZE_T>(3) * m_rtvStride };
    const float zero[4] = { 0, 0, 0, 0 };
    cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_lastPso   = nullptr;
    m_isolation = true;
}

void PerceptionPass::EndIsolation(ID3D12GraphicsCommandList* cmd, u32 slot)
{
    Barrier(cmd, m_isoTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyToReadback(cmd, m_isoTex.Get(), m_rbIso, static_cast<u64>(slot) * m_isoStride);
    Barrier(cmd, m_isoTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

bool PerceptionPass::Readback(CpuFrame& out, u32 isolationUsed, std::string& err)
{
    const u32 w = m_w, h = m_h;
    const size_t n = static_cast<size_t>(w) * h;
    out.w = w;
    out.h = h;

    auto mapRows = [&](const ReadbackBuf& rb, u64 offset, u32 bytesPerRow, auto&& fn) -> bool
    {
        void* p = nullptr;
        const D3D12_RANGE rr{ 0, static_cast<SIZE_T>(rb.bytes) };
        if (FAILED(rb.buf->Map(0, &rr, &p))) return false;
        const auto* base = static_cast<const u8*>(p) + offset;
        for (u32 y = 0; y < h; ++y)
            fn(y, base + static_cast<size_t>(rb.fp.Footprint.RowPitch) * y, bytesPerRow);
        const D3D12_RANGE wr{ 0, 0 };
        rb.buf->Unmap(0, &wr);
        return true;
    };

    out.ids.resize(n);
    out.posDist.resize(n * 4);
    out.normal.resize(n * 4);
    bool ok = mapRows(m_rbId, 0, w * 4, [&](u32 y, const u8* row, u32 bytes) {
        std::memcpy(&out.ids[static_cast<size_t>(y) * w], row, bytes); });
    ok = ok && mapRows(m_rbPos, 0, w * 16, [&](u32 y, const u8* row, u32 bytes) {
        std::memcpy(&out.posDist[static_cast<size_t>(y) * w * 4], row, bytes); });
    ok = ok && mapRows(m_rbNrm, 0, w * 4, [&](u32 y, const u8* row, u32) {
        const auto* s = reinterpret_cast<const int8_t*>(row);
        float* d = &out.normal[static_cast<size_t>(y) * w * 4];
        for (u32 x = 0; x < w * 4; ++x) d[x] = (std::max)(static_cast<float>(s[x]) / 127.0f, -1.0f);
    });
    out.isolation.assign(isolationUsed, {});
    for (u32 s = 0; ok && s < isolationUsed && s < m_isoSlots; ++s)
    {
        out.isolation[s].resize(n);
        ok = mapRows(m_rbIso, static_cast<u64>(s) * m_isoStride, w, [&](u32 y, const u8* row, u32 bytes) {
            std::memcpy(&out.isolation[s][static_cast<size_t>(y) * w], row, bytes); });
    }
    if (!ok) { err = "readback map failed"; return false; }

    // ---- 最終画: 元の大きさで取り出し、解析解像度へ最近傍で合わせる ----
    if (!m_colorValid) { err = "final color was not captured"; return false; }
    const bool bgra = (m_colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM || m_colorFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    const u32 cw = m_colorW, ch = m_colorH;
    out.colorW = cw;
    out.colorH = ch;
    out.colorRgba.resize(static_cast<size_t>(cw) * ch * 4);
    {
        void* p = nullptr;
        const D3D12_RANGE rr{ 0, static_cast<SIZE_T>(m_rbColor.bytes) };
        if (FAILED(m_rbColor.buf->Map(0, &rr, &p))) { err = "color readback map failed"; return false; }
        const auto* base = static_cast<const u8*>(p);
        for (u32 y = 0; y < ch; ++y)
        {
            const u8* in = base + static_cast<size_t>(m_rbColor.fp.Footprint.RowPitch) * y;
            u8* o = &out.colorRgba[static_cast<size_t>(y) * cw * 4];
            for (u32 x = 0; x < cw; ++x)
            {
                o[x * 4 + 0] = bgra ? in[x * 4 + 2] : in[x * 4 + 0];
                o[x * 4 + 1] = in[x * 4 + 1];
                o[x * 4 + 2] = bgra ? in[x * 4 + 0] : in[x * 4 + 2];
                o[x * 4 + 3] = 255;
            }
        }
        const D3D12_RANGE wr{ 0, 0 };
        m_rbColor.buf->Unmap(0, &wr);
    }
    out.rgba.resize(n * 4);
    for (u32 y = 0; y < h; ++y)
    {
        const u32 sy = (std::min)(ch - 1, static_cast<u32>((y + 0.5) * ch / h));
        for (u32 x = 0; x < w; ++x)
        {
            const u32 sx = (std::min)(cw - 1, static_cast<u32>((x + 0.5) * cw / w));
            std::memcpy(&out.rgba[(static_cast<size_t>(y) * w + x) * 4],
                        &out.colorRgba[(static_cast<size_t>(sy) * cw + sx) * 4], 4);
        }
    }
    return true;
}

} // namespace dx12e
