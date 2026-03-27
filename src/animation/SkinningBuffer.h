#pragma once
#include <vector>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

class SkinningBuffer
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap& srvHeap,
                    u32 maxBones, u32 frameCount);

    void Update(const std::vector<DirectX::XMFLOAT4X4>& matrices, u32 frameIndex);

    u32 GetSrvIndex(u32 frameIndex) const { return m_frames[frameIndex].srvIndex; }

private:
    struct PerFrame
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        u8* mappedPtr  = nullptr;
        u32 srvIndex   = 0;
    };

    std::vector<PerFrame> m_frames;
    u32 m_maxBones   = 0;
    u32 m_bufferSize = 0;
};

} // namespace dx12e
