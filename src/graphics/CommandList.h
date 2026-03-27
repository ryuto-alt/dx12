#pragma once

#include <directx/d3d12.h>

#include "core/Types.h"

namespace dx12e
{

class PipelineState;
class RootSignature;

class CommandList
{
public:
    void Wrap(ID3D12GraphicsCommandList* cmdList);

    void TransitionResource(ID3D12Resource* resource,
                            D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after);
    void ClearRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const float color[4]);
    void ClearDepthStencil(D3D12_CPU_DESCRIPTOR_HANDLE dsv, float depth = 1.0f);
    void SetViewportAndScissor(u32 width, u32 height);
    void SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void SetPipelineState(PipelineState& pso);
    void SetRootSignature(RootSignature& rs);
    void SetPerFrameCBV(u32 slot, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress);
    void SetPerObjectConstants(u32 slot, u32 num32BitValues, const void* data);
    void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology);
    void SetVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& vbv);
    void SetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW& ibv);
    void DrawIndexedInstanced(u32 indexCount, u32 instanceCount = 1);
    void SetDescriptorHeap(ID3D12DescriptorHeap* heap);
    void SetSRVTable(u32 slot, D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    void Close();

    ID3D12GraphicsCommandList* GetNative() const { return m_cmdList; }

private:
    ID3D12GraphicsCommandList* m_cmdList = nullptr;
};

} // namespace dx12e
