#include "graphics/RenderTarget.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/CommandList.h"
#include "core/Assert.h"

namespace dx12e
{

namespace
{
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr float       kClearColor[4] = {0.392f, 0.584f, 0.929f, 1.0f};  // コーンフラワーブルー (SceneView と同じ)
}

const float* RenderTarget::GetClearColor()
{
    return kClearColor;
}

RenderTarget::~RenderTarget()
{
    Release();
}

void RenderTarget::Initialize(GraphicsDevice& device,
                              DescriptorHeap& srvHeap,
                              u32 width, u32 height)
{
    // 二重初期化を禁止 (再初期化したい場合は Release してから)
    DX_ASSERT(m_srvIndex == 0xFFFFFFFFu,
              "RenderTarget::Initialize called twice without Release()");

    m_srvHeap  = &srvHeap;
    m_srvIndex = srvHeap.AllocateIndex();
    m_imguiTextureId = static_cast<u64>(srvHeap.GetGpuHandle(m_srvIndex).ptr);

    m_rtvHeap = std::make_unique<DescriptorHeap>();
    m_rtvHeap->Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    m_rtvHandle = m_rtvHeap->Allocate();

    m_dsvHeap = std::make_unique<DescriptorHeap>();
    m_dsvHeap->Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);
    m_dsvHandle = m_dsvHeap->Allocate();

    m_width  = (width  > 0) ? width  : 1;
    m_height = (height > 0) ? height : 1;

    CreateResources(device);
    CreateViews(device);
}

void RenderTarget::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;
    if (width == m_width && height == m_height) return;

    m_color.Reset();
    m_depth.Reset();
    m_width  = width;
    m_height = height;
    // 新リソースの初期 state は CreateResources で PIXEL_SHADER_RESOURCE
    m_colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    CreateResources(device);
    CreateViews(device);  // RTV/DSV/SRV を同じハンドルに再作成
}

void RenderTarget::TransitionColorTo(CommandList& cmd, D3D12_RESOURCE_STATES newState)
{
    if (m_colorState == newState) return;
    cmd.TransitionResource(m_color.Get(), m_colorState, newState);
    m_colorState = newState;
}

void RenderTarget::Release()
{
    m_color.Reset();
    m_depth.Reset();
    m_rtvHeap.reset();
    m_dsvHeap.reset();
    m_rtvHandle = {};
    m_dsvHandle = {};
    m_srvHeap         = nullptr;
    m_srvIndex        = 0xFFFFFFFFu;
    m_imguiTextureId  = 0;
    m_width = 0;
    m_height = 0;
    m_colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void RenderTarget::CreateResources(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    // --- Color ---
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = m_width;
        desc.Height           = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kColorFormat;
        desc.SampleDesc       = {1, 0};
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal{};
        clearVal.Format   = kColorFormat;
        clearVal.Color[0] = kClearColor[0];
        clearVal.Color[1] = kClearColor[1];
        clearVal.Color[2] = kClearColor[2];
        clearVal.Color[3] = kClearColor[3];

        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        ThrowIfFailed(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // ImGui::Image 読み出し可能な状態で開始
            &clearVal, IID_PPV_ARGS(&m_color)));
    }

    // --- Depth ---
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = m_width;
        desc.Height           = m_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kDepthFormat;
        desc.SampleDesc       = {1, 0};
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearVal{};
        clearVal.Format               = kDepthFormat;
        clearVal.DepthStencil.Depth   = 1.0f;
        clearVal.DepthStencil.Stencil = 0;

        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        ThrowIfFailed(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearVal, IID_PPV_ARGS(&m_depth)));
    }
}

void RenderTarget::CreateViews(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    // RTV
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format        = kColorFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(m_color.Get(), &rtvDesc, m_rtvHandle);

    // DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = kDepthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(m_depth.Get(), &dsvDesc, m_dsvHandle);

    // SRV (shader-visible heap の確保済みスロットに上書き)
    if (m_srvHeap && m_srvIndex != 0xFFFFFFFFu)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = kColorFormat;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        dev->CreateShaderResourceView(m_color.Get(), &srvDesc,
                                      m_srvHeap->GetCpuHandle(m_srvIndex));
    }
}

} // namespace dx12e
