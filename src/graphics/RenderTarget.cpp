#include "graphics/RenderTarget.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/CommandList.h"
#include "core/Assert.h"

namespace dx12e
{
void RenderTarget::Initialize(GraphicsDevice& device,
                              DescriptorHeap* rtvHeap,
                              DescriptorHeap* srvHeap,
                              u32 width, u32 height,
                              DXGI_FORMAT format,
                              const float clearColor[4])
{
    m_rtvHeap = rtvHeap;
    m_srvHeap = srvHeap;
    m_width   = (width  > 0) ? width  : 1;
    m_height  = (height > 0) ? height : 1;
    m_format  = format;
    for (int i = 0; i < 4; ++i) m_clear[i] = clearColor[i];

    CreateResourceAndViews(device, /*allocateViews*/true);
}

void RenderTarget::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    if (width == 0 || height == 0) return;
    m_width  = width;
    m_height = height;
    m_resource.Reset();
    // RTV ハンドル / SRV インデックスは既存のものへ作り直す（再確保しない）
    CreateResourceAndViews(device, /*allocateViews*/false);
}

void RenderTarget::CreateResourceAndViews(GraphicsDevice& device, bool allocateViews)
{
    auto* dev = device.GetDevice();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = m_width;
    desc.Height           = m_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = m_format;
    desc.SampleDesc       = {1, 0};
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = m_format;
    clear.Color[0] = m_clear[0];
    clear.Color[1] = m_clear[1];
    clear.Color[2] = m_clear[2];
    clear.Color[3] = m_clear[3];

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ThrowIfFailed(dev->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &desc, m_state, &clear, IID_PPV_ARGS(&m_resource)));

    if (allocateViews)
    {
        m_rtv      = m_rtvHeap->Allocate();
        m_srvIndex = m_srvHeap->AllocateIndex();
    }

    // RTV
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format        = m_format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(m_resource.Get(), &rtvDesc, m_rtv);

    // SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                  = m_format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    dev->CreateShaderResourceView(m_resource.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_srvIndex));
}

void RenderTarget::Transition(CommandList& cmd, D3D12_RESOURCE_STATES newState)
{
    if (m_state == newState) return;
    cmd.TransitionResource(m_resource.Get(), m_state, newState);
    m_state = newState;
}

} // namespace dx12e
