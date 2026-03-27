#pragma once

#include "graphics/GpuResource.h"

#include <wrl/client.h>

namespace dx12e
{

class GraphicsDevice;

class Texture : public GpuResource
{
public:
    void Initialize(
        GraphicsDevice& device,
        ID3D12GraphicsCommandList* cmdList,
        const D3D12_RESOURCE_DESC& desc,
        const D3D12_SUBRESOURCE_DATA* subresources,
        u32 subresourceCount);

    void CreateSRV(GraphicsDevice& device, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
    void FinishUpload();

    u32         GetWidth()    const { return m_width; }
    u32         GetHeight()   const { return m_height; }
    DXGI_FORMAT GetFormat()   const { return m_format; }
    u32         GetSrvIndex() const { return m_srvIndex; }
    void        SetSrvIndex(u32 index) { m_srvIndex = index; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    u32         m_width    = 0;
    u32         m_height   = 0;
    DXGI_FORMAT m_format   = DXGI_FORMAT_UNKNOWN;
    u32         m_srvIndex = UINT32_MAX;
};

} // namespace dx12e
