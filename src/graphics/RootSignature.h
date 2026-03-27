#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;

class RootSignature
{
public:
    void Initialize(GraphicsDevice& device);

    ID3D12RootSignature* Get() const { return m_rootSignature.Get(); }

    static constexpr u32 kSlotPerObject = 0;  // RootConstants b0 (16 DWORD = MVP)
    static constexpr u32 kSlotPerFrame  = 1;  // CBV b1

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
};

} // namespace dx12e
