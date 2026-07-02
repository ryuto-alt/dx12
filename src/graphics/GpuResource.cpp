#include "graphics/GpuResource.h"
#include "graphics/DeferredRelease.h"
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
    // フレーム多重化中は前フレームのGPUがまだこのリソースを読んでいる可能性がある。
    // DeferredRelease 経由で解放する（有効時はフェンス完了まで遅延、無効時は即時。
    // メッシュ再生成/シーンClear/RT再作成など、全GpuResource派生の解放が対象）。
    if (m_resource || m_allocation)
    {
        DeferredRelease::Defer(std::move(m_resource), m_allocation);
        m_allocation = nullptr;
    }
}

} // namespace dx12e
