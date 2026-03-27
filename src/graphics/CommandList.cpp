#include "graphics/CommandList.h"
#include "graphics/PipelineState.h"
#include "graphics/RootSignature.h"
#include "core/Assert.h"

namespace dx12e
{

void CommandList::Wrap(ID3D12GraphicsCommandList* cmdList)
{
    DX_ASSERT(cmdList, "CommandList must not be null");
    m_cmdList = cmdList;
}

void CommandList::TransitionResource(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;

    m_cmdList->ResourceBarrier(1, &barrier);
}

void CommandList::ClearRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float color[4])
{
    m_cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void CommandList::ClearDepthStencil(D3D12_CPU_DESCRIPTOR_HANDLE dsv, float depth)
{
    m_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
}

void CommandList::SetViewportAndScissor(u32 width, u32 height)
{
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<float>(width);
    viewport.Height   = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_cmdList->RSSetViewports(1, &viewport);

    D3D12_RECT scissor{};
    scissor.left   = 0;
    scissor.top    = 0;
    scissor.right  = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);
    m_cmdList->RSSetScissorRects(1, &scissor);
}

void CommandList::SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv)
{
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

void CommandList::SetPipelineState(PipelineState& pso)
{
    m_cmdList->SetPipelineState(pso.Get());
}

void CommandList::SetRootSignature(RootSignature& rs)
{
    m_cmdList->SetGraphicsRootSignature(rs.Get());
}

void CommandList::SetPerFrameCBV(u32 slot, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress)
{
    m_cmdList->SetGraphicsRootConstantBufferView(slot, gpuAddress);
}

void CommandList::SetPerObjectConstants(u32 slot, u32 num32BitValues, const void* data)
{
    m_cmdList->SetGraphicsRoot32BitConstants(slot, num32BitValues, data, 0);
}

void CommandList::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology)
{
    m_cmdList->IASetPrimitiveTopology(topology);
}

void CommandList::SetVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& vbv)
{
    m_cmdList->IASetVertexBuffers(0, 1, &vbv);
}

void CommandList::SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& ibv)
{
    m_cmdList->IASetIndexBuffer(&ibv);
}

void CommandList::DrawIndexedInstanced(u32 indexCount, u32 instanceCount)
{
    m_cmdList->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
}

void CommandList::SetDescriptorHeap(ID3D12DescriptorHeap* heap)
{
    ID3D12DescriptorHeap* heaps[] = { heap };
    m_cmdList->SetDescriptorHeaps(1, heaps);
}

void CommandList::SetSRVTable(u32 slot, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
{
    m_cmdList->SetGraphicsRootDescriptorTable(slot, gpuHandle);
}

void CommandList::Close()
{
    ThrowIfFailed(m_cmdList->Close());
}

} // namespace dx12e
