#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <D3D12MemAlloc.h>

#include "core/Types.h"

namespace dx12e
{

class GpuResource
{
public:
    GpuResource() = default;
    virtual ~GpuResource();

    GpuResource(const GpuResource&) = delete;
    GpuResource& operator=(const GpuResource&) = delete;

    ID3D12Resource*       GetResource()     const { return m_resource.Get(); }
    D3D12_RESOURCE_STATES GetCurrentState() const { return m_currentState; }
    void SetCurrentState(D3D12_RESOURCE_STATES state) { m_currentState = state; }

protected:
    void CreateResource(
        D3D12MA::Allocator* allocator,
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initialState,
        const D3D12MA::ALLOCATION_DESC& allocDesc,
        const D3D12_CLEAR_VALUE* clearValue = nullptr);
    void ReleaseResource();

    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    D3D12MA::Allocation*                   m_allocation    = nullptr;
    D3D12_RESOURCE_STATES                  m_currentState  = D3D12_RESOURCE_STATE_COMMON;
};

} // namespace dx12e
