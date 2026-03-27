#include "Application.h"
#include "Logger.h"
#include "Assert.h"

// Graphics module headers
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandQueue.h"
#include "graphics/SwapChain.h"
#include "graphics/FrameResources.h"
#include "graphics/DescriptorHeap.h"

#include <directx/d3d12.h>

namespace dx12e
{

Application::Application() = default;

Application::~Application()
{
    if (m_isRunning)
    {
        Shutdown();
    }
}

void Application::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    // ロガー初期化
    Logger::Init();
    Logger::Info("Application initializing...");

    // ウィンドウ作成
    m_window = std::make_unique<Window>();
    m_window->Initialize(hInstance, nCmdShow, 1280, 720, L"DX12 Engine");

    // グラフィックスデバイス初期化
    m_graphicsDevice = std::make_unique<GraphicsDevice>();
    m_graphicsDevice->Initialize(*m_window);

    // コマンドキュー作成
    m_commandQueue = std::make_unique<CommandQueue>();
    m_commandQueue->Initialize(*m_graphicsDevice, D3D12_COMMAND_LIST_TYPE_DIRECT);

    // ディスクリプタヒープ作成（RTV用）
    m_descriptorHeap = std::make_unique<DescriptorHeap>();
    m_descriptorHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3, false);

    // スワップチェイン初期化
    m_swapChain = std::make_unique<SwapChain>();
    m_swapChain->Initialize(*m_window, *m_graphicsDevice, *m_commandQueue, *m_descriptorHeap);

    // フレームリソース初期化
    m_frameResources = std::make_unique<FrameResources>();
    m_frameResources->Initialize(*m_graphicsDevice, *m_commandQueue);

    // ゲームクロックリセット
    m_gameClock.Reset();

    m_isRunning = true;
    Logger::Info("Application initialized successfully");
}

void Application::Run()
{
    Logger::Info("Application running...");

    while (!m_window->ShouldClose())
    {
        m_window->ProcessMessages();

        if (m_window->ShouldClose())
            break;

        m_gameClock.Tick();
        Update();
        Render();
    }

    Logger::Info("Main loop ended");
}

void Application::Shutdown()
{
    Logger::Info("Application shutting down...");

    // GPU の処理完了を待機
    if (m_commandQueue)
    {
        m_commandQueue->WaitIdle();
    }

    // リソース解放（逆順）
    m_frameResources.reset();
    m_swapChain.reset();
    m_descriptorHeap.reset();
    m_commandQueue.reset();
    m_graphicsDevice.reset();
    m_window.reset();

    m_isRunning = false;

    Logger::Info("Application shut down complete");
    Logger::Shutdown();
}

void Application::Update()
{
    // 将来のゲームロジック用（現在は空）
}

void Application::Render()
{
    // フレーム開始：GPU完了待ち + コマンドリストリセット
    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

    // バックバッファを RENDER_TARGET にバリア遷移
    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    D3D12_RESOURCE_BARRIER barrierToRT = {};
    barrierToRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrierToRT.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrierToRT.Transition.pResource = backBuffer;
    barrierToRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrierToRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrierToRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmdList->ResourceBarrier(1, &barrierToRT);

    // レンダーターゲットビューをクリア（コーンフラワーブルー）
    auto rtv = m_swapChain->GetCurrentRTV();
    constexpr float cornflowerBlue[4] = { 0.392f, 0.584f, 0.929f, 1.0f };
    cmdList->ClearRenderTargetView(rtv, cornflowerBlue, 0, nullptr);

    // バックバッファを PRESENT にバリア遷移
    D3D12_RESOURCE_BARRIER barrierToPresent = {};
    barrierToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrierToPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrierToPresent.Transition.pResource = backBuffer;
    barrierToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrierToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrierToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &barrierToPresent);

    // コマンドリストを閉じて実行
    ThrowIfFailed(cmdList->Close());
    m_commandQueue->ExecuteCommandList(cmdList);

    // プレゼント（VSync有効）
    m_swapChain->Present(true);

    // フレーム終了
    m_frameResources->EndFrame(*m_commandQueue);
}

} // namespace dx12e
