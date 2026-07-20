#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <array>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class CommandQueue;

class FrameResources
{
public:
    static constexpr u32 kFrameCount = 3;

    FrameResources() = default;
    ~FrameResources() = default;

    FrameResources(const FrameResources&) = delete;
    FrameResources& operator=(const FrameResources&) = delete;

    void Initialize(GraphicsDevice& device, CommandQueue& queue);

    ID3D12GraphicsCommandList* BeginFrame(CommandQueue& queue);
    void                       EndFrame(CommandQueue& queue);

    // フレーム途中の例外からの復旧用。open のままの cmdList を実行せずに Close する。
    // Close せず放置すると次の BeginFrame の Reset が失敗し続けて復帰不能になる。
    void                       AbortFrame();

    ID3D12CommandAllocator*     GetCurrentAllocator() const { return m_frames[m_currentFrame].commandAllocator.Get(); }
    ID3D12GraphicsCommandList*  GetCommandList()      const { return m_commandList.Get(); }

private:
    struct PerFrame
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        u64 fenceValue = 0;
    };

    std::array<PerFrame, kFrameCount>                      m_frames;
    u32                                                    m_currentFrame = 0;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>      m_commandList;
};

} // namespace dx12e
