#include "renderer/SpriteRenderer.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

using namespace DirectX;

namespace dx12e
{
void SpriteRenderer::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                                DXGI_FORMAT rtvFormat, const std::wstring& shaderDir)
{
    m_srvHeap = srvHeap;
    auto* dev = device.GetDevice();

    // --- Root Signature: b0(ortho 16 DWORD, VERTEX) + t0(SRV table, PIXEL) + s0 ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 16;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 2;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO: TriangleList / AlphaBlend ON / Depth OFF ---
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + L"Sprite_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + L"Sprite_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.InputLayout = { layout, _countof(layout) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask       = UINT_MAX;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0]    = rtvFormat;
        pso.DSVFormat        = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc       = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
    }

    // --- Dynamic Vertex Buffer (Upload Heap) ---
    {
        const UINT bufferSize = kMaxVertices * sizeof(Vertex);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width            = bufferSize;
        res.Height           = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels        = 1;
        res.SampleDesc       = {1, 0};
        res.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vertexBuffer)));

        m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vbView.StrideInBytes  = sizeof(Vertex);
        m_vbView.SizeInBytes    = bufferSize;
    }

    m_sprites.reserve(256);
    m_initialized = true;
    Logger::Info("SpriteRenderer initialized");
}

void SpriteRenderer::BeginFrame()
{
    m_sprites.clear();
}

void SpriteRenderer::Submit(const SpriteDesc& s)
{
    if (m_sprites.size() >= kMaxSprites) return;
    m_sprites.push_back(s);
}

void SpriteRenderer::Render(ID3D12GraphicsCommandList* cmd, u32 screenW, u32 screenH)
{
    if (!m_initialized || m_sprites.empty()) return;

    // 描画順: layer 昇順（安定ソート）
    std::stable_sort(m_sprites.begin(), m_sprites.end(),
                     [](const SpriteDesc& a, const SpriteDesc& b) { return a.layer < b.layer; });

    // 頂点生成（各スプライト = 2 三角形）
    std::vector<Vertex> verts;
    verts.reserve(m_sprites.size() * 6);
    for (const auto& s : m_sprites)
    {
        float x0 = s.pos.x,           y0 = s.pos.y;
        float x1 = s.pos.x + s.size.x, y1 = s.pos.y + s.size.y;
        XMFLOAT2 uv0 = s.uvMin, uv1 = s.uvMax;
        XMFLOAT4 c = s.color;

        verts.push_back({{x0, y0}, {uv0.x, uv0.y}, c});
        verts.push_back({{x1, y0}, {uv1.x, uv0.y}, c});
        verts.push_back({{x1, y1}, {uv1.x, uv1.y}, c});
        verts.push_back({{x0, y0}, {uv0.x, uv0.y}, c});
        verts.push_back({{x1, y1}, {uv1.x, uv1.y}, c});
        verts.push_back({{x0, y1}, {uv0.x, uv1.y}, c});
    }
    u32 vertexCount = static_cast<u32>(verts.size());
    if (vertexCount > kMaxVertices) vertexCount = kMaxVertices;

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped));
    memcpy(mapped, verts.data(), vertexCount * sizeof(Vertex));
    m_vertexBuffer->Unmap(0, nullptr);

    // 正射影（左上原点）。転置して渡す（HLSL は mul(rowvec, M)）。
    XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(
        0.0f, static_cast<f32>(screenW), static_cast<f32>(screenH), 0.0f, 0.0f, 1.0f);
    XMFLOAT4X4 orthoT;
    XMStoreFloat4x4(&orthoT, XMMatrixTranspose(ortho));

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRoot32BitConstants(0, 16, &orthoT, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);

    // テクスチャ単位でバッチ（連続する同一 srvIndex をまとめて描く）
    u32 vtx = 0;
    size_t i = 0;
    while (i < m_sprites.size() && vtx < vertexCount)
    {
        u32 srv = m_sprites[i].srvIndex;
        size_t runStart = i;
        while (i < m_sprites.size() && m_sprites[i].srvIndex == srv) ++i;
        u32 count = static_cast<u32>((i - runStart) * 6);
        if (vtx + count > vertexCount) count = vertexCount - vtx;

        cmd->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGpuHandle(srv));
        cmd->DrawInstanced(count, 1, vtx, 0);
        vtx += count;
    }
}

// ===== ワールド空間 2D（カメラ連動。HUD 経路と PSO/VB/list を隔離）=====

void SpriteRenderer::InitializeWorld(GraphicsDevice& device, DXGI_FORMAT sceneRtvFormat,
                                     const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // PSO: HUD と同設定（Sprite_VS/PS, AlphaBlend, Depth OFF, CullNone）。RTV のみ sceneRtvFormat。
    // RootSignature は Initialize で作った m_rootSig を共有する。
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + L"Sprite_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + L"Sprite_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.InputLayout = { layout, _countof(layout) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask       = UINT_MAX;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0]    = sceneRtvFormat;
        pso.DSVFormat        = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc       = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_worldPso)));
    }

    // world 用 動的頂点バッファ（Upload Heap）
    {
        const UINT bufferSize = kMaxVertices * sizeof(Vertex);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width            = bufferSize;
        res.Height           = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels        = 1;
        res.SampleDesc       = {1, 0};
        res.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_worldVertexBuffer)));

        m_worldVbView.BufferLocation = m_worldVertexBuffer->GetGPUVirtualAddress();
        m_worldVbView.StrideInBytes  = sizeof(Vertex);
        m_worldVbView.SizeInBytes    = bufferSize;
    }

    m_worldSprites.reserve(256);
    m_worldInitialized = true;
    Logger::Info("SpriteRenderer world path initialized");
}

void SpriteRenderer::BeginWorldFrame()
{
    m_worldSprites.clear();
}

void SpriteRenderer::SubmitWorld(const WorldSpriteDesc& s)
{
    if (m_worldSprites.size() >= kMaxSprites) return;
    m_worldSprites.push_back(s);
}

void SpriteRenderer::RenderWorld(ID3D12GraphicsCommandList* cmd, XMMATRIX viewProj)
{
    if (!m_worldInitialized || m_worldSprites.empty()) return;

    // 描画順: layer 昇順（安定ソート）。Depth OFF なので前後は layer で確定。
    std::stable_sort(m_worldSprites.begin(), m_worldSprites.end(),
                     [](const WorldSpriteDesc& a, const WorldSpriteDesc& b) { return a.layer < b.layer; });

    // ワールド XY 平面に「中心 ± size/2」でクアッド展開（Y 上向き → V を反転して上下正立）
    std::vector<Vertex> verts;
    verts.reserve(m_worldSprites.size() * 6);
    for (const auto& s : m_worldSprites)
    {
        const float hx = s.size.x * 0.5f, hy = s.size.y * 0.5f;
        const float x0 = s.center.x - hx, y0 = s.center.y - hy; // 左/下
        const float x1 = s.center.x + hx, y1 = s.center.y + hy; // 右/上
        const XMFLOAT2 uvmin = s.uvMin, uvmax = s.uvMax;
        const XMFLOAT4 c = s.color;
        // 上端(y1)→テクスチャ上(uvmin.y), 下端(y0)→テクスチャ下(uvmax.y)
        verts.push_back({{x0, y1}, {uvmin.x, uvmin.y}, c});
        verts.push_back({{x1, y1}, {uvmax.x, uvmin.y}, c});
        verts.push_back({{x1, y0}, {uvmax.x, uvmax.y}, c});
        verts.push_back({{x0, y1}, {uvmin.x, uvmin.y}, c});
        verts.push_back({{x1, y0}, {uvmax.x, uvmax.y}, c});
        verts.push_back({{x0, y0}, {uvmin.x, uvmax.y}, c});
    }
    u32 vertexCount = static_cast<u32>(verts.size());
    if (vertexCount > kMaxVertices) vertexCount = kMaxVertices;

    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, 0};
    ThrowIfFailed(m_worldVertexBuffer->Map(0, &readRange, &mapped));
    memcpy(mapped, verts.data(), vertexCount * sizeof(Vertex));
    m_worldVertexBuffer->Unmap(0, nullptr);

    // viewProj を転置して b0 へ（HLSL は mul(rowvec, M)）。正射カメラ時も同じ経路で正しく射影される。
    XMFLOAT4X4 vpT;
    XMStoreFloat4x4(&vpT, XMMatrixTranspose(viewProj));

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    cmd->SetPipelineState(m_worldPso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRoot32BitConstants(0, 16, &vpT, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_worldVbView);

    // テクスチャ単位でバッチ（連続する同一 srvIndex をまとめて描く）
    u32 vtx = 0;
    size_t i = 0;
    while (i < m_worldSprites.size() && vtx < vertexCount)
    {
        u32 srv = m_worldSprites[i].srvIndex;
        size_t runStart = i;
        while (i < m_worldSprites.size() && m_worldSprites[i].srvIndex == srv) ++i;
        u32 count = static_cast<u32>((i - runStart) * 6);
        if (vtx + count > vertexCount) count = vertexCount - vtx;

        cmd->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGpuHandle(srv));
        cmd->DrawInstanced(count, 1, vtx, 0);
        vtx += count;
    }
}

} // namespace dx12e
