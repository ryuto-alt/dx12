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

    // compute スキニング（計画09 Step 4）がルート SRV でボーン行列を読むために使う。
    // ★UPLOAD ヒープなので compute から読むと PCIe 越えになるが、256 ボーン × 64B = 16KB と
    //   小さく、頂点シェーダが既に同じバッファを毎フレーム読んでいるので新規の負荷ではない。
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress(u32 frameIndex) const
    {
        return m_frames[frameIndex].resource->GetGPUVirtualAddress();
    }

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
