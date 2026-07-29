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
    SkinningBuffer() = default;
    // 確保した SRV インデックスを返す。返さないと Play/Stop・シーン開き直しのたびに
    // スケルタル 1 体につき frameCount 個ずつ SRV ヒープ(容量 4096)を食い潰し、
    // やがて AllocateIndex が例外を投げて描画が止まる(＝突然シーンビューが真っ暗になる)。
    ~SkinningBuffer();
    // ヒープへの生ポインタを持つのでコピー禁止(二重 Free を作らない)。
    // 所有は LuaScript ではなく MeshRenderer 側の unique_ptr 1 本きり。
    SkinningBuffer(const SkinningBuffer&) = delete;
    SkinningBuffer& operator=(const SkinningBuffer&) = delete;

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
        // 未確保は 0 ではなく無効値(IndexAllocator::kInvalid)。0 は正当なインデックスなので、
        // Initialize が途中で例外を吐いた時にデストラクタが他人の 0 番を Free してしまう。
        u32 srvIndex   = 0xFFFFFFFFu;
    };

    std::vector<PerFrame> m_frames;
    // Free 先。Application::m_srvHeap は m_scene より前に宣言されている＝
    // シーン(このバッファの所有者)より後に壊れるので、デストラクタから触って安全。
    DescriptorHeap* m_srvHeap = nullptr;
    u32 m_maxBones   = 0;
    u32 m_bufferSize = 0;
};

} // namespace dx12e
