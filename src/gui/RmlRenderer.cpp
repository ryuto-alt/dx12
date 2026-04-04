#include "gui/RmlRenderer.h"

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"

#include <DirectXMath.h>
#include <DirectXTex.h>
#include <directx/d3dx12.h>

#include <cstring>

using namespace DirectX;

namespace dx12e
{

// ============================================================
// Lifecycle
// ============================================================

RmlRenderer::~RmlRenderer()
{
    if (m_uploadFenceEvent)
    {
        CloseHandle(m_uploadFenceEvent);
        m_uploadFenceEvent = nullptr;
    }
}

void RmlRenderer::Initialize(GraphicsDevice& device,
                              DescriptorHeap& srvHeap,
                              ID3D12CommandQueue* cmdQueue,
                              DXGI_FORMAT rtvFormat,
                              const std::wstring& shaderDir)
{
    m_device   = device.GetDevice();
    m_srvHeap  = &srvHeap;
    m_cmdQueue = cmdQueue;

    // Create texture-upload infrastructure
    ThrowIfFailed(m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_uploadAllocator)));

    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_uploadAllocator.Get(), nullptr,
        IID_PPV_ARGS(&m_uploadCmdList)));
    ThrowIfFailed(m_uploadCmdList->Close());

    ThrowIfFailed(m_device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_uploadFence)));
    m_uploadFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DX_ASSERT(m_uploadFenceEvent, "Failed to create upload fence event");

    CreatePipelineState(rtvFormat, shaderDir);
    CreateWhiteTexture();
}

// ============================================================
// Pipeline state / root signature creation
// ============================================================

void RmlRenderer::CreatePipelineState(DXGI_FORMAT rtvFormat, const std::wstring& shaderDir)
{
    // ----- Root Signature -----
    // Slot 0: RootConstants b0 (20 DWORD = 4x4 matrix + float2 trans + float2 pad)
    // Slot 1: DescriptorTable t0 (1 SRV) - PIXEL
    // Static Sampler s0: LINEAR CLAMP - PIXEL

    CD3DX12_ROOT_PARAMETER1 params[2] = {};
    params[0].InitAsConstants(20, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    params[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(
        0,                                      // shaderRegister s0
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(_countof(params), params, 1, &sampler,
                    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
        {
            Logger::Error("RootSignature error: {}",
                          static_cast<const char*>(errBlob->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
    }

    ThrowIfFailed(m_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));

    // ----- PSO -----
    // Load compiled shaders
    auto vsData = ShaderCompiler::LoadFromFile(shaderDir + L"UI_VS.cso");
    auto psData = ShaderCompiler::LoadFromFile(shaderDir + L"UI_PS.cso");

    // Input layout matching Rml::Vertex (pos float2, color R8G8B8A8_UNORM, uv float2)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0,  8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vsData.GetData(), vsData.GetSize() };
    psoDesc.PS = { psData.GetData(), psData.GetSize() };
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    // Rasterizer: no culling
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Depth: disabled
    psoDesc.DepthStencilState.DepthEnable    = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    // Blend: premultiplied alpha
    D3D12_RENDER_TARGET_BLEND_DESC& rtBlend = psoDesc.BlendState.RenderTarget[0];
    rtBlend.BlendEnable           = TRUE;
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)));

    Logger::Info("RmlRenderer: PSO created");
}

// ============================================================
// White fallback texture (1x1 RGBA white)
// ============================================================

void RmlRenderer::CreateWhiteTexture()
{
    const u32 white = 0xFFFFFFFF;
    m_whiteTexture = UploadTexture(&white, 1, 1);
}

// ============================================================
// Texture upload helper
// ============================================================

Rml::TextureHandle RmlRenderer::UploadTexture(const void* pixels, u32 width, u32 height)
{
    // Create default-heap texture
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapDefault, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&tex)));

    // Determine upload buffer size
    u64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    u32 numRows = 0;
    u64 rowSizeBytes = 0;
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                    &footprint, &numRows, &rowSizeBytes, &uploadSize);

    // Create upload buffer
    auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadBufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuf;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapUpload, D3D12_HEAP_FLAG_NONE,
        &uploadBufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&uploadBuf)));

    // Map and copy row-by-row (respecting row pitch alignment)
    void* mapped = nullptr;
    ThrowIfFailed(uploadBuf->Map(0, nullptr, &mapped));
    auto* dst = static_cast<u8*>(mapped);
    auto* src = static_cast<const u8*>(pixels);
    const u32 srcRowPitch = width * 4; // RGBA8

    for (u32 row = 0; row < numRows; ++row)
    {
        std::memcpy(dst + footprint.Footprint.RowPitch * row,
                    src + srcRowPitch * row,
                    srcRowPitch);
    }
    uploadBuf->Unmap(0, nullptr);

    // Record copy command
    ThrowIfFailed(m_uploadAllocator->Reset());
    ThrowIfFailed(m_uploadCmdList->Reset(m_uploadAllocator.Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = tex.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource       = uploadBuf.Get();
    srcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    m_uploadCmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition to SRV
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_uploadCmdList->ResourceBarrier(1, &barrier);
    ThrowIfFailed(m_uploadCmdList->Close());

    // Execute and wait
    ID3D12CommandList* lists[] = { m_uploadCmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);

    ++m_uploadFenceValue;
    ThrowIfFailed(m_cmdQueue->Signal(m_uploadFence.Get(), m_uploadFenceValue));
    if (m_uploadFence->GetCompletedValue() < m_uploadFenceValue)
    {
        ThrowIfFailed(m_uploadFence->SetEventOnCompletion(m_uploadFenceValue, m_uploadFenceEvent));
        WaitForSingleObject(m_uploadFenceEvent, INFINITE);
    }

    // Create SRV
    u32 srvIndex = m_srvHeap->AllocateIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels    = 1;

    m_device->CreateShaderResourceView(
        tex.Get(), &srvDesc, m_srvHeap->GetCpuHandle(srvIndex));

    // Store in map
    uintptr_t handle = m_nextTextureHandle++;
    m_textures[handle] = { std::move(tex), srvIndex };

    return static_cast<Rml::TextureHandle>(handle);
}

// ============================================================
// BeginFrame
// ============================================================

void RmlRenderer::BeginFrame(ID3D12GraphicsCommandList* cmdList,
                              f32 viewportWidth, f32 viewportHeight)
{
    m_cmdList        = cmdList;
    m_viewportWidth  = viewportWidth;
    m_viewportHeight = viewportHeight;
    m_scissorEnabled = false;

    // Set pipeline
    m_cmdList->SetPipelineState(m_pso.Get());
    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Set descriptor heap
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap->GetHeap() };
    m_cmdList->SetDescriptorHeaps(1, heaps);

    // Orthographic projection (top-left origin, Y-down)
    // Stored transposed for HLSL row-major root constants
    XMMATRIX ortho = XMMatrixOrthographicOffCenterLH(
        0.0f, viewportWidth, viewportHeight, 0.0f, -1.0f, 1.0f);
    XMMATRIX orthoT = XMMatrixTranspose(ortho);

    XMFLOAT4X4 orthoData;
    XMStoreFloat4x4(&orthoData, orthoT);
    m_cmdList->SetGraphicsRoot32BitConstants(0, 16, &orthoData, 0);

    // Zero translation + pad
    float zeroes[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_cmdList->SetGraphicsRoot32BitConstants(0, 4, zeroes, 16);

    // Viewport
    D3D12_VIEWPORT vp = {};
    vp.Width    = viewportWidth;
    vp.Height   = viewportHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_cmdList->RSSetViewports(1, &vp);

    // Default scissor = full viewport
    D3D12_RECT scissor = { 0, 0,
                           static_cast<LONG>(viewportWidth),
                           static_cast<LONG>(viewportHeight) };
    m_cmdList->RSSetScissorRects(1, &scissor);

    // Topology
    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// ============================================================
// Geometry
// ============================================================

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices,
    Rml::Span<const int> indices)
{
    const u32 vbSize = static_cast<u32>(vertices.size() * sizeof(Rml::Vertex));
    const u32 ibSize = static_cast<u32>(indices.size() * sizeof(int));

    auto heapUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // Vertex buffer (UPLOAD heap)
    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer(vbSize);
    Microsoft::WRL::ComPtr<ID3D12Resource> vb;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapUpload, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&vb)));

    void* vbMapped = nullptr;
    ThrowIfFailed(vb->Map(0, nullptr, &vbMapped));
    std::memcpy(vbMapped, vertices.data(), vbSize);
    vb->Unmap(0, nullptr);

    // Index buffer (UPLOAD heap)
    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer(ibSize);
    Microsoft::WRL::ComPtr<ID3D12Resource> ib;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapUpload, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&ib)));

    void* ibMapped = nullptr;
    ThrowIfFailed(ib->Map(0, nullptr, &ibMapped));
    std::memcpy(ibMapped, indices.data(), ibSize);
    ib->Unmap(0, nullptr);

    CompiledGeometry geom;
    geom.vertexBuffer = std::move(vb);
    geom.indexBuffer  = std::move(ib);
    geom.indexCount   = static_cast<u32>(indices.size());

    geom.vbView.BufferLocation = geom.vertexBuffer->GetGPUVirtualAddress();
    geom.vbView.SizeInBytes    = vbSize;
    geom.vbView.StrideInBytes  = sizeof(Rml::Vertex);

    geom.ibView.BufferLocation = geom.indexBuffer->GetGPUVirtualAddress();
    geom.ibView.SizeInBytes    = ibSize;
    geom.ibView.Format         = DXGI_FORMAT_R32_UINT;

    uintptr_t handle = m_nextGeometryHandle++;
    m_geometries[handle] = std::move(geom);

    return static_cast<Rml::CompiledGeometryHandle>(handle);
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                  Rml::Vector2f translation,
                                  Rml::TextureHandle texture)
{
    auto it = m_geometries.find(static_cast<uintptr_t>(geometry));
    if (it == m_geometries.end())
        return;

    const auto& geom = it->second;

    // Set translation (offset 16 = after the 4x4 matrix, 4 DWORDs: x, y, pad, pad)
    float trans[4] = { translation.x, translation.y, 0.0f, 0.0f };
    m_cmdList->SetGraphicsRoot32BitConstants(0, 4, trans, 16);

    // Texture SRV (use white fallback if no texture)
    Rml::TextureHandle texHandle = (texture != 0) ? texture : m_whiteTexture;
    auto texIt = m_textures.find(static_cast<uintptr_t>(texHandle));
    if (texIt != m_textures.end())
    {
        m_cmdList->SetGraphicsRootDescriptorTable(
            1, m_srvHeap->GetGpuHandle(texIt->second.srvIndex));
    }

    // Draw
    m_cmdList->IASetVertexBuffers(0, 1, &geom.vbView);
    m_cmdList->IASetIndexBuffer(&geom.ibView);
    m_cmdList->DrawIndexedInstanced(geom.indexCount, 1, 0, 0, 0);
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    m_geometries.erase(static_cast<uintptr_t>(geometry));
}

// ============================================================
// Textures
// ============================================================

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions,
                                             const Rml::String& source)
{
    // Convert source path to wide string
    std::wstring widePath;
    widePath.resize(source.size());
    const int len = MultiByteToWideChar(
        CP_UTF8, 0, source.c_str(), static_cast<int>(source.size()),
        widePath.data(), static_cast<int>(widePath.size()));
    widePath.resize(static_cast<size_t>(len));

    // Load via DirectXTex
    DirectX::ScratchImage scratchImage;
    HRESULT hr = DirectX::LoadFromWICFile(
        widePath.c_str(), DirectX::WIC_FLAGS_FORCE_RGB,
        nullptr, scratchImage);

    if (FAILED(hr))
    {
        // Try DDS
        hr = DirectX::LoadFromDDSFile(
            widePath.c_str(), DirectX::DDS_FLAGS_NONE,
            nullptr, scratchImage);
    }

    if (FAILED(hr))
    {
        Logger::Error("RmlRenderer: Failed to load texture '{}'", source);
        return Rml::TextureHandle(0);
    }

    // Convert to RGBA8 if needed
    const DirectX::Image* img = scratchImage.GetImage(0, 0, 0);
    DirectX::ScratchImage converted;
    if (img->format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(
            *img, DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT,
            converted);
        if (FAILED(hr))
        {
            Logger::Error("RmlRenderer: Failed to convert texture format for '{}'", source);
            return Rml::TextureHandle(0);
        }
        img = converted.GetImage(0, 0, 0);
    }

    texture_dimensions.x = static_cast<int>(img->width);
    texture_dimensions.y = static_cast<int>(img->height);

    return UploadTexture(img->pixels,
                         static_cast<u32>(img->width),
                         static_cast<u32>(img->height));
}

Rml::TextureHandle RmlRenderer::GenerateTexture(
    Rml::Span<const Rml::byte> source,
    Rml::Vector2i source_dimensions)
{
    return UploadTexture(source.data(),
                         static_cast<u32>(source_dimensions.x),
                         static_cast<u32>(source_dimensions.y));
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture)
{
    m_textures.erase(static_cast<uintptr_t>(texture));
}

// ============================================================
// Scissor
// ============================================================

void RmlRenderer::EnableScissorRegion(bool enable)
{
    m_scissorEnabled = enable;

    if (!enable)
    {
        // Reset to full viewport
        D3D12_RECT rect = { 0, 0,
                            static_cast<LONG>(m_viewportWidth),
                            static_cast<LONG>(m_viewportHeight) };
        m_cmdList->RSSetScissorRects(1, &rect);
    }
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region)
{
    if (!m_scissorEnabled)
        return;

    D3D12_RECT rect;
    rect.left   = static_cast<LONG>(region.Left());
    rect.top    = static_cast<LONG>(region.Top());
    rect.right  = static_cast<LONG>(region.Right());
    rect.bottom = static_cast<LONG>(region.Bottom());

    m_cmdList->RSSetScissorRects(1, &rect);
}

} // namespace dx12e
