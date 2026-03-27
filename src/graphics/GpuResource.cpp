#include "graphics/GpuResource.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

GpuResource::~GpuResource()
{
    ReleaseResource();
}

void GpuResource::CreateResource(
    D3D12MA::Allocator* allocator,
    const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES initialState,
    const D3D12MA::ALLOCATION_DESC& allocDesc,
    const D3D12_CLEAR_VALUE* clearValue)
{
    DX_ASSERT(allocator, "Allocator must not be null");

    ReleaseResource();

    ThrowIfFailed(allocator->CreateResource(
        &allocDesc,
        &desc,
        initialState,
        clearValue,
        &m_allocation,
        IID_PPV_ARGS(&m_resource)));

    m_currentState = initialState;
}

void GpuResource::ReleaseResource()
{
    m_resource.Reset();
    if (m_allocation)
    {
        m_allocation->Release();
        m_allocation = nullptr;
    }
}

} // namespace dx12e
