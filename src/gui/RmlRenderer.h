#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <directx/d3d12.h>
#include <wrl/client.h>

#include <unordered_map>
#include <string>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

class RmlRenderer : public Rml::RenderInterface
{
public:
    RmlRenderer() = default;
    ~RmlRenderer() override;

    RmlRenderer(const RmlRenderer&) = delete;
    RmlRenderer& operator=(const RmlRenderer&) = delete;

    /// Call once after device creation.
    /// @param rtvFormat  The render target format (e.g. DXGI_FORMAT_R8G8B8A8_UNORM).
    void Initialize(GraphicsDevice& device,
                    DescriptorHeap& srvHeap,
                    ID3D12CommandQueue* cmdQueue,
                    DXGI_FORMAT rtvFormat,
                    const std::wstring& shaderDir);

    /// Call every frame before Rml::Context::Render().
    void BeginFrame(ID3D12GraphicsCommandList* cmdList,
                    f32 viewportWidth, f32 viewportHeight);

    // --- Rml::RenderInterface overrides ---

    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex> vertices,
        Rml::Span<const int> indices) override;

    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;

    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override;

    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    struct CompiledGeometry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbView{};
        D3D12_INDEX_BUFFER_VIEW  ibView{};
        u32 indexCount = 0;
    };

    struct TextureEntry
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        u32 srvIndex = UINT32_MAX;
    };

    void CreatePipelineState(DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);
    void CreateWhiteTexture();
    Rml::TextureHandle UploadTexture(const void* pixels, u32 width, u32 height);

    // Engine references (non-owning)
    ID3D12Device5*       m_device   = nullptr;
    DescriptorHeap*      m_srvHeap  = nullptr;
    ID3D12CommandQueue*  m_cmdQueue = nullptr;

    // Pipeline objects
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;

    // Texture upload resources
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_uploadAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_uploadCmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_uploadFence;
    HANDLE  m_uploadFenceEvent = nullptr;
    u64     m_uploadFenceValue = 0;

    // Per-frame state
    ID3D12GraphicsCommandList* m_cmdList        = nullptr;
    f32                        m_viewportWidth  = 0.0f;
    f32                        m_viewportHeight = 0.0f;
    bool                       m_scissorEnabled = false;

    // Geometry / texture storage
    uintptr_t m_nextGeometryHandle = 1;
    uintptr_t m_nextTextureHandle  = 1;
    std::unordered_map<uintptr_t, CompiledGeometry> m_geometries;
    std::unordered_map<uintptr_t, TextureEntry>     m_textures;

    // White fallback texture handle
    Rml::TextureHandle m_whiteTexture = 0;
};

} // namespace dx12e
