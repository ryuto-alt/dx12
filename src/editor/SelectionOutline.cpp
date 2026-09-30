// 選択 / ホバーの輪郭。設計の説明は SelectionOutline.h の先頭と shaders/editor/SelectionOutline.hlsl。
#include "editor/SelectionOutline.h"

#include "editor/ViewportLogic.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "animation/SkinningBuffer.h"
#include "ecs/Components.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "renderer/Mesh.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <memory>
#include <unordered_set>

using namespace DirectX;

namespace dx12e
{
namespace
{
constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R8G8_UNORM;

// SelectionOutline.hlsl の cbuffer とバイト単位で一致させること。
struct MaskDrawCB
{
    XMFLOAT4X4 model;       // transpose(world)
    float      mask[4];     // x = 選択, y = ホバー
};
static_assert(sizeof(MaskDrawCB) == 20 * 4, "MaskDrawCB は SelectionOutline.hlsl の PerDraw(b0) と一致させること");

struct CompositeCB
{
    float selColor[4];
    float hoverColor[4];
    float edge[4];          // 芯 px / 光の広がり px / 光の強さ / 探索半径 px
    float maskRect[4];      // 原点 x,y / 幅・高さ
    float spare[4];
};
static_assert(sizeof(CompositeCB) == 20 * 4, "CompositeCB は SelectionOutline.hlsl の Composite(b0) と一致させること");

constexpr u32 kSlotConst = 0, kSlotPass = 1, kSlotBones = 2, kSlotMask = 3;

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
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

void SelectionOutlinePass::Initialize(GraphicsDevice& device, DescriptorHeap& srvHeap, DXGI_FORMAT backBufferFormat,
                                      const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();
    m_device = dev;
    m_srvHeapOwner = &srvHeap;

    // ---- ルートシグネチャ（38 DWORD）----
    {
        D3D12_DESCRIPTOR_RANGE boneRange{};
        boneRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        boneRange.NumDescriptors     = 1;
        boneRange.BaseShaderRegister = 3;   // t3
        D3D12_DESCRIPTOR_RANGE maskRange{};
        maskRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        maskRange.NumDescriptors     = 1;
        maskRange.BaseShaderRegister = 0;   // t0

        D3D12_ROOT_PARAMETER p[4]{};
        p[kSlotConst].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kSlotConst].Constants.ShaderRegister = 0;
        p[kSlotConst].Constants.Num32BitValues = 20;
        p[kSlotConst].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        p[kSlotPass].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[kSlotPass].Constants.ShaderRegister  = 1;
        p[kSlotPass].Constants.Num32BitValues  = 16;
        p[kSlotPass].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
        p[kSlotBones].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[kSlotBones].DescriptorTable.NumDescriptorRanges = 1;
        p[kSlotBones].DescriptorTable.pDescriptorRanges   = &boneRange;
        p[kSlotBones].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;
        p[kSlotMask].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[kSlotMask].DescriptorTable.NumDescriptorRanges = 1;
        p[kSlotMask].DescriptorTable.pDescriptorRanges   = &maskRange;
        p[kSlotMask].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(p);
        desc.pParameters   = p;
        desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    const auto vs    = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionMask_VS.cso");
    const auto vsSk  = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionMaskSkinned_VS.cso");
    const auto ps    = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionMask_PS.cso");
    const auto vsFul = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionComposite_VS.cso");
    const auto psCmp = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionComposite_PS.cso");

    auto baseDesc = [&]()
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        d.pRootSignature = m_rootSig.Get();
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.SampleDesc.Count = 1;
        d.DSVFormat = DXGI_FORMAT_UNKNOWN;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // 両面（フォワードと同じ）
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = FALSE;
        d.DepthStencilState.StencilEnable = FALSE;
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return d;
    };

    // マスク: MAX ブレンド（重なっても 1）
    auto buildMask = [&](const ShaderCompiler::ShaderBytecode& v)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = baseDesc();
        d.VS = { v.GetData(), v.GetSize() };
        d.PS = { ps.GetData(), ps.GetSize() };
        d.InputLayout = { Mesh::GetInputLayout(), Mesh::GetInputLayoutCount() };
        d.RTVFormats[0] = kMaskFormat;
        auto& rt = d.BlendState.RenderTarget[0];
        rt.BlendEnable    = TRUE;
        rt.SrcBlend       = D3D12_BLEND_ONE;
        rt.DestBlend      = D3D12_BLEND_ONE;
        rt.BlendOp        = D3D12_BLEND_OP_MAX;
        rt.SrcBlendAlpha  = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha   = D3D12_BLEND_OP_MAX;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
        return pso;
    };
    m_psoMask        = buildMask(vs);
    m_psoMaskSkinned = buildMask(vsSk);

    // 合成: 通常のアルファブレンドでバックバッファへ
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = baseDesc();
        d.VS = { vsFul.GetData(), vsFul.GetSize() };
        d.PS = { psCmp.GetData(), psCmp.GetSize() };
        d.RTVFormats[0] = backBufferFormat;
        auto& rt = d.BlendState.RenderTarget[0];
        rt.BlendEnable    = TRUE;
        rt.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp        = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha  = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&m_psoComposite)));
    }

    // ワイヤ表示モード: 線（アルファブレンド・ワイヤフレーム）と下地の塗り（上書き）
    const auto psWire = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionWire_PS.cso");
    const auto psFill = ShaderCompiler::LoadFromFile(shaderDir + L"SelectionFill_PS.cso");
    auto buildWire = [&](const ShaderCompiler::ShaderBytecode& v)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = baseDesc();
        d.VS = { v.GetData(), v.GetSize() };
        d.PS = { psWire.GetData(), psWire.GetSize() };
        d.InputLayout = { Mesh::GetInputLayout(), Mesh::GetInputLayoutCount() };
        d.RTVFormats[0] = backBufferFormat;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        auto& rt = d.BlendState.RenderTarget[0];
        rt.BlendEnable    = TRUE;
        rt.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp        = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha  = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
        return pso;
    };
    m_psoWire        = buildWire(vs);
    m_psoWireSkinned = buildWire(vsSk);
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = baseDesc();
        d.VS = { vsFul.GetData(), vsFul.GetSize() };
        d.PS = { psFill.GetData(), psFill.GetSize() };
        d.RTVFormats[0] = backBufferFormat;   // ブレンド無し = 上書き
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&m_psoFill)));
    }

    D3D12_DESCRIPTOR_HEAP_DESC rh{};
    rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 1;
    ThrowIfFailed(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_rtvHeap)));

    // マスク読み取り用の SRV を 1 つだけ確保（以後は同じ番号を使い回す）
    m_srvIndex = srvHeap.AllocateIndex();
    if (m_srvIndex == DescriptorHeap::kInvalidIndex)
        throw std::runtime_error("SelectionOutlinePass: SRV を確保できません");
    m_srvGpu = srvHeap.GetGpuHandle(m_srvIndex);
    // マスクを作るまでは null SRV を入れておく（ワイヤ表示モードは表を空にせず束ねるだけで読まない）
    D3D12_SHADER_RESOURCE_VIEW_DESC nul{};
    nul.Format                  = kMaskFormat;
    nul.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    nul.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nul.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(nullptr, &nul, srvHeap.GetCpuHandle(m_srvIndex));
}

void SelectionOutlinePass::EnsureMask(u32 w, u32 h)
{
    if (m_mask && w == m_maskW && h == m_maskH) return;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = w;
    d.Height           = h;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = kMaskFormat;
    d.SampleDesc.Count = 1;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = kMaskFormat;
    m_mask.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&m_mask)));
    m_mask->SetName(L"SelectionOutlineMask");
    m_device->CreateRenderTargetView(m_mask.Get(), nullptr, m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format                  = kMaskFormat;
    sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_mask.Get(), &sv, m_srvHeapOwner->GetCpuHandle(m_srvIndex));
    m_maskW = w;
    m_maskH = h;
}

bool SelectionOutlinePass::Record(const Args& a)
{
    m_lastDraws = 0;
    m_lastScissorPx = 0;
    const bool haveSel = a.selected && !a.selected->empty();
    const bool haveHov = a.hovered && !a.hovered->empty();
    if (!IsReady() || (!haveSel && !haveHov) || !a.cmd || !a.backBuffer || a.vpW < 2 || a.vpH < 2) return false;

    // ---- 投影 AABB → シザー（対象の周りだけ合成する）----
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    bool behind = false;
    auto grow = [&](const std::vector<Target>* list)
    {
        if (!list) return;
        for (const Target& t : *list)
        {
            if (!t.item) continue;
            const XMMATRIX vp = a.viewProj;
            for (int i = 0; i < 8; ++i)
            {
                const XMVECTOR c = XMVectorSet((i & 1) ? t.item->aabbMax.x : t.item->aabbMin.x,
                                               (i & 2) ? t.item->aabbMax.y : t.item->aabbMin.y,
                                               (i & 4) ? t.item->aabbMax.z : t.item->aabbMin.z, 1.0f);
                const XMVECTOR h = XMVector4Transform(c, vp);   // 行ベクトル規約（GetViewProjMatrix そのまま）
                const float w = XMVectorGetW(h);
                if (w <= 1e-4f) { behind = true; continue; }
                const float ndcX = XMVectorGetX(h) / w, ndcY = XMVectorGetY(h) / w;
                const float sx = a.vpX + (ndcX * 0.5f + 0.5f) * a.vpW;
                const float sy = a.vpY + (1.0f - (ndcY * 0.5f + 0.5f)) * a.vpH;
                minX = std::min(minX, sx); maxX = std::max(maxX, sx);
                minY = std::min(minY, sy); maxY = std::max(maxY, sy);
            }
        }
    };
    grow(a.selected);
    grow(a.hovered);
    const float R = std::ceil((vp::outline::kGlowPx + vp::outline::kCorePx) * a.scale) + 1.0f;   // 探索半径（px）
    vp::Rect sc;
    if (behind || minX > maxX)   // カメラの後ろに回る頂点がある（巨大な床など）→ ビューポート全域
        sc = { static_cast<float>(a.vpX), static_cast<float>(a.vpY), static_cast<float>(a.vpW), static_cast<float>(a.vpH) };
    else
        sc = vp::outline::ScissorFor(minX, minY, maxX, maxY, R + 2.0f, static_cast<float>(a.vpX), static_cast<float>(a.vpY),
                                     static_cast<float>(a.vpW), static_cast<float>(a.vpH));
    if (sc.w < 1.0f || sc.h < 1.0f) return false;   // 画面外

    EnsureMask(a.vpW, a.vpH);
    ID3D12GraphicsCommandList* cmd = a.cmd;

    // ---- 1) マスク ----
    ID3D12DescriptorHeap* heaps[] = { a.srvHeap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    Barrier(cmd, m_mask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE maskRtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float zero[4] = { 0, 0, 0, 0 };
    cmd->ClearRenderTargetView(maskRtv, zero, 0, nullptr);
    cmd->OMSetRenderTargets(1, &maskRtv, FALSE, nullptr);
    const D3D12_VIEWPORT mvp{ 0.0f, 0.0f, static_cast<float>(a.vpW), static_cast<float>(a.vpH), 0.0f, 1.0f };
    const D3D12_RECT msc{ 0, 0, static_cast<LONG>(a.vpW), static_cast<LONG>(a.vpH) };
    cmd->RSSetViewports(1, &mvp);
    cmd->RSSetScissorRects(1, &msc);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    XMFLOAT4X4 vpT;
    XMStoreFloat4x4(&vpT, XMMatrixTranspose(a.viewProj));
    cmd->SetGraphicsRoot32BitConstants(kSlotPass, 16, &vpT, 0);
    cmd->SetGraphicsRootDescriptorTable(kSlotBones, a.defaultSrv);

    ID3D12PipelineState* lastPso = nullptr;
    auto drawList = [&](const std::vector<Target>* list, float selV, float hovV)
    {
        if (!list) return;
        for (const Target& t : *list)
        {
            const DrawItem* it = t.item;
            if (!it || !it->renderer) continue;
            const MeshRenderer& r = *it->renderer;
            const bool skinned = (it->skin != nullptr);
            ID3D12PipelineState* pso = skinned ? m_psoMaskSkinned.Get() : m_psoMask.Get();
            if (pso != lastPso) { cmd->SetPipelineState(pso); lastPso = pso; }
            cmd->SetGraphicsRootDescriptorTable(kSlotBones, skinned ? t.bones : a.defaultSrv);
            const XMMATRIX world = XMLoadFloat4x4(&it->world);
            for (u32 mi = 0; mi < static_cast<u32>(r.meshes.size()); ++mi)
            {
                const Mesh* mesh = r.meshes[mi];
                if (!mesh) continue;
                XMMATRIX mw = world;
                if (it->hasNodeAnim && mi < static_cast<u32>(r.meshNodeTransforms.size()))
                    mw = XMLoadFloat4x4(&r.meshNodeTransforms[mi]) * world;
                MaskDrawCB cb{};
                XMStoreFloat4x4(&cb.model, XMMatrixTranspose(mw));
                cb.mask[0] = selV; cb.mask[1] = hovV;
                cmd->SetGraphicsRoot32BitConstants(kSlotConst, 20, &cb, 0);
                const D3D12_VERTEX_BUFFER_VIEW vbv = mesh->GetVertexBuffer().GetView();
                const D3D12_INDEX_BUFFER_VIEW  ibv = mesh->GetIndexBufferLod(it->lod).GetView();
                cmd->IASetVertexBuffers(0, 1, &vbv);
                cmd->IASetIndexBuffer(&ibv);
                cmd->DrawIndexedInstanced(mesh->GetIndexCountLod(it->lod), 1, 0, 0, 0);
                ++m_lastDraws;
            }
        }
    };
    drawList(a.selected, 1.0f, 0.0f);
    drawList(a.hovered, 0.0f, 1.0f);
    Barrier(cmd, m_mask.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // ---- 2) 合成（バックバッファへ）----
    D3D12_CPU_DESCRIPTOR_HANDLE bb = a.backBufferRtv;
    cmd->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    const D3D12_RESOURCE_DESC bbd = a.backBuffer->GetDesc();
    const D3D12_VIEWPORT fvp{ 0.0f, 0.0f, static_cast<float>(bbd.Width), static_cast<float>(bbd.Height), 0.0f, 1.0f };
    const D3D12_RECT fsc{ static_cast<LONG>(sc.x), static_cast<LONG>(sc.y), static_cast<LONG>(sc.x + sc.w + 0.999f), static_cast<LONG>(sc.y + sc.h + 0.999f) };
    cmd->RSSetViewports(1, &fvp);
    cmd->RSSetScissorRects(1, &fsc);
    cmd->SetPipelineState(m_psoComposite.Get());

    CompositeCB cc{};
    std::memcpy(cc.selColor, a.selColor, sizeof(cc.selColor));
    std::memcpy(cc.hoverColor, a.hoverColor, sizeof(cc.hoverColor));
    cc.edge[0] = vp::outline::kCorePx * a.scale;
    cc.edge[1] = vp::outline::kGlowPx * a.scale;
    cc.edge[2] = vp::outline::kGlowStrength;
    cc.edge[3] = R - 1.0f;
    cc.maskRect[0] = static_cast<float>(a.vpX); cc.maskRect[1] = static_cast<float>(a.vpY);
    cc.maskRect[2] = static_cast<float>(a.vpW); cc.maskRect[3] = static_cast<float>(a.vpH);
    cmd->SetGraphicsRoot32BitConstants(kSlotConst, 20, &cc, 0);
    cmd->SetGraphicsRootDescriptorTable(kSlotMask, m_srvGpu);
    cmd->DrawInstanced(3, 1, 0, 0);
    m_lastScissorPx = static_cast<u32>(sc.w * sc.h);
    return true;
}

bool SelectionOutlinePass::RecordWire(const WireArgs& a)
{
    m_lastDraws = 0;
    if (!IsReady() || !a.items || !a.cmd || !a.backBuffer || a.vpW < 2 || a.vpH < 2) return false;
    ID3D12GraphicsCommandList* cmd = a.cmd;
    ID3D12DescriptorHeap* heaps[] = { a.srvHeap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    D3D12_CPU_DESCRIPTOR_HANDLE bb = a.backBufferRtv;
    cmd->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 下地: ビューポート矩形だけを塗る（フルスクリーン三角形 + シザー）
    const D3D12_RESOURCE_DESC bbd = a.backBuffer->GetDesc();
    const D3D12_VIEWPORT fvp{ 0.0f, 0.0f, static_cast<float>(bbd.Width), static_cast<float>(bbd.Height), 0.0f, 1.0f };
    const D3D12_RECT sc{ static_cast<LONG>(a.vpX), static_cast<LONG>(a.vpY), static_cast<LONG>(a.vpX + a.vpW), static_cast<LONG>(a.vpY + a.vpH) };
    cmd->RSSetViewports(1, &fvp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetPipelineState(m_psoFill.Get());
    CompositeCB cc{};
    std::memcpy(cc.selColor, a.fill, sizeof(cc.selColor));
    cmd->SetGraphicsRoot32BitConstants(kSlotConst, 20, &cc, 0);
    cmd->SetGraphicsRootDescriptorTable(kSlotMask, m_srvGpu);   // 未使用だが表を空にしない
    cmd->DrawInstanced(3, 1, 0, 0);

    // 線: メッシュのビューポートは矩形そのもの（NDC → ビューポート矩形）
    const D3D12_VIEWPORT mvp{ static_cast<float>(a.vpX), static_cast<float>(a.vpY), static_cast<float>(a.vpW), static_cast<float>(a.vpH), 0.0f, 1.0f };
    cmd->RSSetViewports(1, &mvp);
    XMFLOAT4X4 vpT;
    XMStoreFloat4x4(&vpT, XMMatrixTranspose(a.viewProj));
    cmd->SetGraphicsRoot32BitConstants(kSlotPass, 16, &vpT, 0);
    cmd->SetGraphicsRootDescriptorTable(kSlotBones, a.defaultSrv);
    ID3D12PipelineState* lastPso = nullptr;
    for (const Target& t : *a.items)
    {
        const DrawItem* it = t.item;
        if (!it || !it->renderer) continue;
        const MeshRenderer& r = *it->renderer;
        const bool skinned = (it->skin != nullptr);
        ID3D12PipelineState* pso = skinned ? m_psoWireSkinned.Get() : m_psoWire.Get();
        if (pso != lastPso) { cmd->SetPipelineState(pso); lastPso = pso; }
        cmd->SetGraphicsRootDescriptorTable(kSlotBones, skinned ? t.bones : a.defaultSrv);
        const XMMATRIX world = XMLoadFloat4x4(&it->world);
        for (u32 mi = 0; mi < static_cast<u32>(r.meshes.size()); ++mi)
        {
            const Mesh* mesh = r.meshes[mi];
            if (!mesh) continue;
            XMMATRIX mw = world;
            if (it->hasNodeAnim && mi < static_cast<u32>(r.meshNodeTransforms.size()))
                mw = XMLoadFloat4x4(&r.meshNodeTransforms[mi]) * world;
            MaskDrawCB cb{};
            XMStoreFloat4x4(&cb.model, XMMatrixTranspose(mw));
            std::memcpy(cb.mask, a.line, sizeof(cb.mask));
            cmd->SetGraphicsRoot32BitConstants(kSlotConst, 20, &cb, 0);
            const D3D12_VERTEX_BUFFER_VIEW vbv = mesh->GetVertexBuffer().GetView();
            const D3D12_INDEX_BUFFER_VIEW  ibv = mesh->GetIndexBufferLod(it->lod).GetView();
            cmd->IASetVertexBuffers(0, 1, &vbv);
            cmd->IASetIndexBuffer(&ibv);
            cmd->DrawIndexedInstanced(mesh->GetIndexCountLod(it->lod), 1, 0, 0, 0);
            ++m_lastDraws;
        }
    }
    return true;
}

void CollectOutlineTargets(const entt::registry& reg, const std::vector<DrawItem>& drawItems,
                           const std::vector<entt::entity>& roots, std::vector<const DrawItem*>& out, size_t maxItems)
{
    if (roots.empty()) return;
    std::unordered_set<entt::entity> rootSet(roots.begin(), roots.end());
    for (const DrawItem& it : drawItems)
    {
        if (out.size() >= maxItems) break;
        if (!it.renderer) continue;
        bool hit = rootSet.count(it.e) != 0;
        if (!hit)
        {
            // 祖先に選択が居るか（深さ上限つき。循環参照でも止まる）
            const Transform* t = reg.valid(it.e) ? reg.try_get<Transform>(it.e) : nullptr;
            for (int depth = 0; t && t->parent != entt::null && depth < 64; ++depth)
            {
                if (!reg.valid(t->parent)) break;
                if (rootSet.count(t->parent)) { hit = true; break; }
                t = reg.try_get<Transform>(t->parent);
            }
        }
        if (hit) out.push_back(&it);
    }
}

bool RecordEditorViewOverlays(EditorContext& ctx, const EditorViewFrame& f)
{
    if (!f.device || !f.srvHeap || !f.cmd || !f.backBuffer || !f.reg || !f.items) return false;
    const bool wire = (ctx.vpPrefs.viewMode == vp::kViewModeWire);
    const bool wantOutline = ctx.vpPrefs.outline && (ctx.HasSelection() || (ctx.vpPrefs.hoverHighlight && ctx.hoveredEntity != entt::null));
    if (!wire && !wantOutline) return false;

    // 遅延生成（最初にこの機能が要るフレームで 1 回だけ。SRV を 1 つ確保する）
    if (!ctx.outlinePass)
    {
        try
        {
            auto p = std::make_shared<SelectionOutlinePass>();
            p->Initialize(*f.device, *f.srvHeap, f.backBufferFormat, f.shaderDir);
            ctx.outlinePass = std::move(p);
        }
        catch (const std::exception&)
        {
            // シェーダ（.cso）が無い等。二度と試さないよう空のパスを置く（IsReady() が false のまま = 何も描かない）。
            ctx.outlinePass = std::make_shared<SelectionOutlinePass>();
            return false;
        }
    }
    SelectionOutlinePass& pass = *ctx.outlinePass;
    if (!pass.IsReady()) return false;

    auto boneOf = [&](const DrawItem* it) -> D3D12_GPU_DESCRIPTOR_HANDLE
    {
        if (it && it->skin) return f.srvHeap->GetGpuHandle(it->skin->GetSrvIndex(f.frameIndex));
        return f.whiteSrv;
    };
    auto toTargets = [&](const std::vector<const DrawItem*>& src)
    {
        std::vector<SelectionOutlinePass::Target> out;
        out.reserve(src.size());
        for (const DrawItem* it : src) out.push_back({ it, boneOf(it) });
        return out;
    };

    if (wire)
    {
        std::vector<const DrawItem*> all;
        all.reserve(f.items->size());
        for (const DrawItem& it : *f.items)
            if (it.renderer && !f.reg->all_of<GridPlane>(it.e)) all.push_back(&it);
        const auto targets = toTargets(all);
        SelectionOutlinePass::WireArgs w;
        w.cmd = f.cmd; w.backBuffer = f.backBuffer; w.backBufferRtv = f.backBufferRtv;
        w.vpX = f.vpX; w.vpY = f.vpY; w.vpW = f.vpW; w.vpH = f.vpH;
        w.viewProj = f.viewProj; w.srvHeap = f.srvHeap->GetHeap(); w.defaultSrv = f.whiteSrv;
        w.items = &targets;
        const ImVec4 bg = theme::Bg0;
        w.fill[0] = bg.x; w.fill[1] = bg.y; w.fill[2] = bg.z; w.fill[3] = 1.0f;
        const ImVec4 ln = theme::TextMid;
        w.line[0] = ln.x; w.line[1] = ln.y; w.line[2] = ln.z; w.line[3] = 0.55f;
        pass.RecordWire(w);
        if (!wantOutline) return true;
        // ワイヤの上にも選択の輪郭は出す（続けて下で描く）
    }

    std::vector<entt::entity> selRoots(ctx.selectedEntities.begin(), ctx.selectedEntities.end());
    std::vector<const DrawItem*> selItems, hovItems;
    if (ctx.vpPrefs.outline) CollectOutlineTargets(*f.reg, *f.items, selRoots, selItems);
    if (ctx.vpPrefs.outline && ctx.vpPrefs.hoverHighlight && ctx.hoveredEntity != entt::null && !ctx.IsSelected(ctx.hoveredEntity))
    {
        // 選択の子孫を指している時はホバー強調を出さない（選択の輪郭と重なって紛らわしい）
        const std::vector<entt::entity> hovRoot{ ctx.hoveredEntity };
        CollectOutlineTargets(*f.reg, *f.items, hovRoot, hovItems, 512);
        hovItems.erase(std::remove_if(hovItems.begin(), hovItems.end(), [&](const DrawItem* it)
            { return std::find(selItems.begin(), selItems.end(), it) != selItems.end(); }), hovItems.end());
    }
    const auto selT = toTargets(selItems);
    const auto hovT = toTargets(hovItems);

    SelectionOutlinePass::Args a;
    a.cmd = f.cmd; a.backBuffer = f.backBuffer; a.backBufferRtv = f.backBufferRtv;
    a.vpX = f.vpX; a.vpY = f.vpY; a.vpW = f.vpW; a.vpH = f.vpH;
    a.viewProj = f.viewProj;
    a.srvHeap = f.srvHeap->GetHeap();
    a.defaultSrv = f.whiteSrv;
    a.selected = &selT;
    a.hovered = &hovT;
    a.scale = f.uiScale;
    const ImVec4 core = theme::AccentHover;   // 芯は明るいアクセント（光と同系色）
    a.selColor[0] = core.x; a.selColor[1] = core.y; a.selColor[2] = core.z; a.selColor[3] = 1.0f;
    const ImVec4 hv = theme::Text;
    a.hoverColor[0] = hv.x; a.hoverColor[1] = hv.y; a.hoverColor[2] = hv.z; a.hoverColor[3] = 0.55f;
    return pass.Record(a) || wire;
}

} // namespace dx12e
