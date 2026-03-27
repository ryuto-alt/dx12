#include "FrameResources.h"

#include "GraphicsDevice.h"
#include "CommandQueue.h"
#include "core/Assert.h"
#include "core/Logger.h"

using Microsoft::WRL::ComPtr;

namespace dx12e
{

void FrameResources::Initialize(GraphicsDevice& device, CommandQueue& /*queue*/)
{
    auto* d3dDevice = device.GetDevice();

    // 各フレーム分のコマンドアロケーター作成
    for (u32 i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(d3dDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_frames[i].commandAllocator)));
        m_frames[i].fenceValue = 0;
    }

    // コマンドリスト1つ作成 (初期状態はClose)
    ThrowIfFailed(d3dDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_frames[0].commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList)));

    ThrowIfFailed(m_commandList->Close());

    Logger::Info("FrameResources initialized ({} frames)", kFrameCount);
}

ID3D12GraphicsCommandList* FrameResources::BeginFrame(CommandQueue& queue)
{
    auto& frame = m_frames[m_currentFrame];

    // GPU完了待ち
    queue.WaitForValue(frame.fenceValue);

    // コマンドアロケーターリセット
    ThrowIfFailed(frame.commandAllocator->Reset());

    // コマンドリストリセット
    ThrowIfFailed(m_commandList->Reset(frame.commandAllocator.Get(), nullptr));

    return m_commandList.Get();
}

void FrameResources::EndFrame(CommandQueue& queue)
{
    // Signal してフェンス値を保存
    m_frames[m_currentFrame].fenceValue = queue.Signal();

    // 次フレームへ
    m_currentFrame = (m_currentFrame + 1) % kFrameCount;
}

} // namespace dx12e
