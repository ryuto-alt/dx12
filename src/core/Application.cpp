#include "Application.h"
#include "Logger.h"
#include "Assert.h"

// Graphics module headers
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandQueue.h"
#include "graphics/SwapChain.h"
#include "graphics/FrameResources.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GpuResource.h"
#include "graphics/Buffer.h"
#include "graphics/RootSignature.h"
#include "graphics/PipelineState.h"
#include "graphics/CommandList.h"
#include "graphics/Texture.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "renderer/Camera.h"
#include "resource/ShaderCompiler.h"
#include "resource/ModelLoader.h"
#include "resource/ResourceManager.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "input/InputSystem.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "scripting/ScriptEngine.h"
#include "audio/AudioSystem.h"
#include "gui/ImGuiManager.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <thread>
#include <fstream>
#include <algorithm>
#include <immintrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

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

void Application::Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode)
{
    // ロガー初期化
    Logger::Init();
    m_isGameMode = gameMode;
    Logger::Info("Application initializing... (mode: {})", gameMode ? "game" : "editor");

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

    // Input System
    m_inputSystem = std::make_unique<InputSystem>();
    m_inputSystem->Initialize(m_window->GetHwnd());
    m_window->SetInputSystem(m_inputSystem.get());

    // Audio System
    m_audioSystem = std::make_unique<AudioSystem>();
    m_audioSystem->Initialize(std::string(ASSETS_DIR));

    // Shader Visible SRV ヒープ
    m_srvHeap = std::make_unique<DescriptorHeap>();
    m_srvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true);

    // ResourceManager
    m_resourceManager = std::make_unique<ResourceManager>();
    // ResourceManager は暫定コマンドリストで初期化（デフォルトテクスチャ作成のため）
    // → モデルロード用の BeginFrame の後に初期化する

    // DSV ヒープ
    m_dsvHeap = std::make_unique<DescriptorHeap>();
    m_dsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

    // デプスバッファ作成
    {
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = m_window->GetWidth();
        depthDesc.Height = m_window->GetHeight();
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc = {1, 0};
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue, IID_PPV_ARGS(&m_depthBuffer)));

        m_dsvHandle = m_dsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_graphicsDevice->GetDevice()->CreateDepthStencilView(
            m_depthBuffer.Get(), &dsvDesc, m_dsvHandle);
    }

    // RootSignature
    m_rootSignature = std::make_unique<RootSignature>();
    m_rootSignature->Initialize(*m_graphicsDevice);

    // シェーダー読み込み & PipelineState
    {
        auto vs = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"Forward_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"Forward_PS.cso");

        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetPixelShader(ps.GetData(), ps.GetSize())
               .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
               .SetRenderTargetFormat(m_swapChain->GetFormat())
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true);

        m_pipelineState = std::make_unique<PipelineState>();
        m_pipelineState->Initialize(*m_graphicsDevice, builder);
    }

    // Camera
    m_camera = std::make_unique<Camera>();
    {
        f32 viewW = static_cast<f32>(m_window->GetWidth());
        f32 viewH = static_cast<f32>(m_window->GetHeight());
        if (!m_isGameMode) {
            viewW = (std::max)(viewW - kLeftPanelWidth, 1.0f);
            viewH = (std::max)(viewH - kToolbarHeight, 1.0f);
        }
        m_camera->SetPerspective(DirectX::XM_PIDIV4, viewW / viewH, 0.1f, 1000.0f);
    }
    m_camera->LookAt({-14.7f, 9.6f, -9.0f}, {0.0f, 0.0f, 0.0f});

    // シーン + モデル読み込み
    {
        // 暫定コマンドリストで GPU アップロード
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // ResourceManager 初期化（デフォルト白テクスチャ作成にcmdListが必要）
        m_resourceManager = std::make_unique<ResourceManager>();
        m_resourceManager->Initialize(m_graphicsDevice.get(), m_srvHeap.get(), cmdList);

        // Scene 初期化
        m_scene = std::make_unique<Scene>();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // ScriptEngine 初期化 + ゲームスクリプト実行
        m_scriptEngine = std::make_unique<ScriptEngine>();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(), std::string(ASSETS_DIR));

        // ゲームスクリプト読み込み
        {
            std::string scriptPath = std::string(SCRIPTS_DIR) + "game.lua";
            if (std::filesystem::exists(scriptPath))
            {
                m_scriptEngine->LoadScript(scriptPath);
            }
            else
            {
                Logger::Warn("Game script not found: {}", scriptPath);
            }
        }

        // Lua OnStart 実行（Entity配置等）— コマンドリストがまだ開いてる間に
        m_scriptEngine->CallOnStart();

        // エディタモード初期化時はキャプチャ解除（Luaが OnStart で capture する場合があるため）
        if (!m_isGameMode)
            m_inputSystem->SetMouseCapture(false);

        // コマンド実行 + GPU待ち
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();

        // アップロードバッファ解放
        m_resourceManager->FinishUploads();

        // スキニング PSO 作成
        {
            auto vs = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"ForwardSkinned_VS.cso");
            auto ps = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"Forward_PS.cso");

            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetPixelShader(ps.GetData(), ps.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(m_swapChain->GetFormat())
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true);

            m_skinnedPipelineState = std::make_unique<PipelineState>();
            m_skinnedPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // グリッド PSO 作成（アルファブレンド + 両面描画）
        {
            auto vs = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"ForwardGrid_VS.cso");
            auto ps = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"ForwardGrid_PS.cso");

            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetPixelShader(ps.GetData(), ps.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(m_swapChain->GetFormat())
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetAlphaBlendEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE);

            m_gridPipelineState = std::make_unique<PipelineState>();
            m_gridPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // sneakWalk アニメーションを全スケルタルEntityに追加
        {
            std::filesystem::path sneakPath = std::string(ASSETS_DIR) + "models/human/sneakWalk.gltf";
            if (std::filesystem::exists(sneakPath))
            {
                auto& reg = m_scene->GetRegistry();
                auto skelView = reg.view<SkeletalAnimation>();
                for (auto [e, skelAnim] : skelView.each())
                {
                    auto extraAnims = ModelLoader::LoadAnimationsFromFile(
                        sneakPath, *skelAnim.skeleton);
                    for (auto& a : extraAnims)
                    {
                        a->SetName("sneakWalk");
                        skelAnim.clips.push_back(std::move(a));
                    }
                }
            }
        }
    }

    // シャドウマップ作成
    {
        m_shadowDsvHeap = std::make_unique<DescriptorHeap>();
        m_shadowDsvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false);

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = m_shadowMapSize;
        shadowDesc.Height = m_shadowMapSize;
        shadowDesc.DepthOrArraySize = 1;
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc = {1, 0};
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&m_shadowMap)));

        // DSV
        m_shadowDsvHandle = m_shadowDsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_graphicsDevice->GetDevice()->CreateDepthStencilView(
            m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandle);

        // SRV
        m_shadowSrvIndex = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));

        // Shadow PSO (depth-only, no pixel shader, with depth bias)
        {
            auto vs = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"ShadowPass_VS.cso");
            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetDepthBias(8000, 2.0f);

            m_shadowPipelineState = std::make_unique<PipelineState>();
            m_shadowPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // Shadow Skinned PSO
        {
            auto vs = ShaderCompiler::LoadFromFile(std::wstring(SHADER_DIR) + L"ShadowPassSkinned_VS.cso");
            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(DXGI_FORMAT_UNKNOWN)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetDepthBias(8000, 2.0f);

            m_shadowSkinnedPipelineState = std::make_unique<PipelineState>();
            m_shadowSkinnedPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        Logger::Info("Shadow map initialized ({}x{})", m_shadowMapSize, m_shadowMapSize);
    }

    // PerFrame Constant Buffer
    struct FrameConstants {
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 proj;
        DirectX::XMFLOAT3   lightDir;
        float                time;
        DirectX::XMFLOAT3   lightColor;
        float                ambientStrength;
        DirectX::XMFLOAT4X4 lightViewProj;
    };
    m_perFrameCB = std::make_unique<ConstantBuffer>();
    m_perFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // CommandList ラッパー
    m_commandList = std::make_unique<CommandList>();

    // ImGui 初期化
    m_imguiManager = std::make_unique<ImGuiManager>();
    m_imguiManager->Initialize(
        m_window->GetHwnd(), *m_graphicsDevice, m_commandQueue->GetQueue(),
        *m_srvHeap, m_swapChain->GetFormat(), FrameResources::kFrameCount);

    m_isRunning = true;

    // ゲームモードの場合、即座にPlayモードに入る
    if (m_isGameMode)
    {
        m_pendingMode = EngineMode::Playing;
        m_modeChangeRequested = true;
    }

    Logger::Info("Application initialized successfully");
}

void Application::Run()
{
    Logger::Info("Application running...");

    // Windowsタイマー精度を1msに設定
    timeBeginPeriod(1);

    while (!m_window->ShouldClose())
    {
        m_frameStart = std::chrono::high_resolution_clock::now();

        // モード切替（前フレームのImGuiボタンから遅延実行）
        if (m_modeChangeRequested)
        {
            m_modeChangeRequested = false;
            if (m_pendingMode == EngineMode::Playing)
                EnterPlayMode();
            else
                EnterEditorMode();
        }

        // 入力状態リセット（前フレームのdeltaクリア + prevKeys保存）
        m_inputSystem->Update();

        // メッセージ処理（ここで WM_KEYDOWN/WM_MOUSEMOVE → InputSystem に蓄積）
        m_window->ProcessMessages();

        if (m_window->ShouldClose())
            break;

        // リサイズ処理
        if (m_window->WasResized())
        {
            m_window->ResetResizedFlag();
            u32 w = m_window->GetWidth();
            u32 h = m_window->GetHeight();
            if (w > 0 && h > 0)
            {
                m_commandQueue->WaitIdle();
                m_swapChain->Resize(w, h, *m_descriptorHeap);

                // デプスバッファ再作成
                m_depthBuffer.Reset();
                D3D12_RESOURCE_DESC depthDesc{};
                depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                depthDesc.Width = w;
                depthDesc.Height = h;
                depthDesc.DepthOrArraySize = 1;
                depthDesc.MipLevels = 1;
                depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
                depthDesc.SampleDesc = {1, 0};
                depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

                D3D12_CLEAR_VALUE clearValue{};
                clearValue.Format = DXGI_FORMAT_D32_FLOAT;
                clearValue.DepthStencil = {1.0f, 0};

                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

                ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE,
                    &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clearValue, IID_PPV_ARGS(&m_depthBuffer)));

                D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                m_graphicsDevice->GetDevice()->CreateDepthStencilView(
                    m_depthBuffer.Get(), &dsvDesc, m_dsvHandle);

                // カメラアスペクト比更新（エディタモードではサイドバー分引く）
                {
                    f32 viewW = static_cast<f32>(w);
                    f32 viewH = static_cast<f32>(h);
                    if (!m_isGameMode && m_engineMode == EngineMode::Editor) {
                        viewW = (std::max)(viewW - kLeftPanelWidth, 1.0f);
                        viewH = (std::max)(viewH - kToolbarHeight, 1.0f);
                    }
                    m_camera->SetPerspective(DirectX::XM_PIDIV4, viewW / viewH, 0.1f, 1000.0f);
                }

                Logger::Info("Resized to {}x{}", w, h);
            }
        }

        m_gameClock.Tick();

        // Luaホットリロード（0.5秒ごとにファイル変更チェック）
        m_scriptPollTimer += m_gameClock.GetDeltaTime();
        if (m_scriptPollTimer >= kScriptPollInterval)
        {
            m_scriptPollTimer = 0.0f;
            std::string scriptPath = std::string(SCRIPTS_DIR) + "game.lua";
            if (std::filesystem::exists(scriptPath))
            {
                auto currentTime = std::filesystem::last_write_time(scriptPath);
                if (currentTime != m_scriptLastWriteTime)
                {
                    Logger::Info("Hot-reload: game.lua changed, reloading...");
                    m_commandQueue->WaitIdle();
                    RebuildScene();
                    m_hotReloadFlash = 2.0f;
                    Logger::Info("Hot-reload complete");
                }
            }
        }

        Update();
        Render();

        // フレームレートリミッター（VSync OFF時のCPU暴走を防止）
        if (!m_useVsync)
        {
            using namespace std::chrono;
            auto targetDuration = duration_cast<high_resolution_clock::duration>(
                duration<f64>(1.0 / static_cast<f64>(kTargetFps)));
            auto elapsed = high_resolution_clock::now() - m_frameStart;
            auto remaining = targetDuration - elapsed;

            // 1ms以上余裕があればSleepで待つ（CPU負荷軽減）
            if (remaining > milliseconds(1))
            {
                std::this_thread::sleep_for(remaining - milliseconds(1));
            }
            // 残りはスピンウェイトで精密に待つ
            while (high_resolution_clock::now() - m_frameStart < targetDuration)
            {
                _mm_pause();
            }
        }
    }

    timeEndPeriod(1);

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

    // ImGui 解放
    if (m_imguiManager)
    {
        m_imguiManager->Shutdown();
        m_imguiManager.reset();
    }

    // リソース解放（逆順）
    m_inputSystem.reset();
    m_scriptEngine.reset();
    m_audioSystem.reset();
    m_shadowSkinnedPipelineState.reset();
    m_shadowPipelineState.reset();
    m_shadowMap.Reset();
    m_shadowDsvHeap.reset();
    m_gridPipelineState.reset();
    m_skinnedPipelineState.reset();
    m_scene.reset();
    m_commandList.reset();
    m_perFrameCB.reset();
    m_resourceManager.reset();
    m_srvHeap.reset();
    m_camera.reset();
    m_pipelineState.reset();
    m_rootSignature.reset();
    m_depthBuffer.Reset();
    m_dsvHeap.reset();
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
    using namespace DirectX;
    f32 dt = m_gameClock.GetDeltaTime();

    m_framesSinceStart++;

    if (m_engineMode == EngineMode::Editor)
    {
        // エディタモード: C++カメラ操作
        if (m_inputSystem->IsKeyPressed(VK_TAB))
        {
            m_inputSystem->SetMouseCapture(false);
        }
        if (m_framesSinceStart > 5 && !m_inputSystem->IsMouseCaptured() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000))
        {
            m_inputSystem->SetMouseCapture(true);
        }
        if (m_inputSystem->IsMouseCaptured() && !ImGuizmo::IsUsing())
        {
            f32 sensitivity = m_camera->GetMouseSensitivity();
            m_camera->Rotate(
                m_inputSystem->GetMouseDeltaX() * sensitivity,
                -m_inputSystem->GetMouseDeltaY() * sensitivity);

            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (GetAsyncKeyState('W') & 0x8000) m_camera->MoveForward(speed);
            if (GetAsyncKeyState('S') & 0x8000) m_camera->MoveForward(-speed);
            if (GetAsyncKeyState('D') & 0x8000) m_camera->MoveRight(speed);
            if (GetAsyncKeyState('A') & 0x8000) m_camera->MoveRight(-speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m_camera->MoveUp(-speed);
        }

        // ギズモモード切替（エディタモード時、右クリック中でない時のみ）
        if (!ImGui::GetIO().WantCaptureKeyboard && !m_inputSystem->IsMouseCaptured())
        {
            if (GetAsyncKeyState('W') & 1) m_gizmoMode = GizmoMode::Translate;
            if (GetAsyncKeyState('E') & 1) m_gizmoMode = GizmoMode::Rotate;
            if (GetAsyncKeyState('R') & 1) m_gizmoMode = GizmoMode::Scale;
            if (GetAsyncKeyState('T') & 1) m_gizmoLocalSpace = !m_gizmoLocalSpace;
        }

        // 3Dビュー上の左クリックでオブジェクト選択（レイキャスト）
        if (!ImGui::GetIO().WantCaptureMouse && !ImGuizmo::IsOver()
            && !m_inputSystem->IsMouseCaptured()
            && (GetAsyncKeyState(VK_LBUTTON) & 1))
        {
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            ScreenToClient(m_window->GetHwnd(), &cursorPos);

            // 3Dビューポート領域
            f32 vpX = kLeftPanelWidth;
            f32 vpY = kToolbarHeight;
            f32 vpW = static_cast<f32>(m_window->GetWidth()) - vpX;
            f32 vpH = static_cast<f32>(m_window->GetHeight()) - vpY;

            f32 mx = static_cast<f32>(cursorPos.x);
            f32 my = static_cast<f32>(cursorPos.y);

            // ビューポート内かチェック
            if (mx >= vpX && mx < vpX + vpW && my >= vpY && my < vpY + vpH)
            {
                // NDC座標に変換 (-1〜1)
                f32 ndcX = ((mx - vpX) / vpW) * 2.0f - 1.0f;
                f32 ndcY = 1.0f - ((my - vpY) / vpH) * 2.0f;

                // レイを生成（カメラからクリック位置へ）
                XMMATRIX invProj = XMMatrixInverse(nullptr, m_camera->GetProjectionMatrix());
                XMMATRIX invView = XMMatrixInverse(nullptr, m_camera->GetViewMatrix());

                XMVECTOR rayClip = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
                XMVECTOR rayEye = XMVector4Transform(rayClip, invProj);
                rayEye = XMVectorSetZ(rayEye, 1.0f);
                rayEye = XMVectorSetW(rayEye, 0.0f);

                XMVECTOR rayWorld = XMVector4Transform(rayEye, invView);
                XMVECTOR rayDir = XMVector3Normalize(rayWorld);
                XMFLOAT3 camPos = m_camera->GetPosition();
                XMVECTOR rayOrigin = XMLoadFloat3(&camPos);

                // 全Entityに対してバウンディングスフィアテスト
                f32 closestDist = FLT_MAX;
                entt::entity closestEntity = entt::null;

                auto& reg = m_scene->GetRegistry();
                auto view = reg.view<const Transform, const MeshRenderer>();
                for (auto [e, transform, renderer] : view.each())
                {
                    if (reg.all_of<GridPlane>(e)) continue; // グリッドは選択対象外

                    XMVECTOR entityPos = XMLoadFloat3(&transform.position);

                    // バウンディングスフィア半径を推定（スケールの最大値）
                    f32 radius = (std::max)({
                        std::abs(transform.scale.x),
                        std::abs(transform.scale.y),
                        std::abs(transform.scale.z)
                    });
                    radius = (std::max)(radius, 0.5f); // 最小半径

                    // レイ-スフィア交差テスト
                    XMVECTOR oc = XMVectorSubtract(rayOrigin, entityPos);
                    f32 b = XMVectorGetX(XMVector3Dot(oc, rayDir));
                    f32 c = XMVectorGetX(XMVector3Dot(oc, oc)) - radius * radius;
                    f32 discriminant = b * b - c;

                    if (discriminant > 0.0f)
                    {
                        f32 t = -b - std::sqrt(discriminant);
                        if (t < 0.0f) t = -b + std::sqrt(discriminant);
                        if (t > 0.0f && t < closestDist)
                        {
                            closestDist = t;
                            closestEntity = e;
                        }
                    }
                }

                m_selectedEntity = closestEntity;
            }
        }
    }
    else
    {
        // プレイモード: Luaがカメラ+ゲームロジックを制御
        m_scriptEngine->CallOnUpdate(dt);
    }

    // シーン更新（全EntityのAnimator）
    m_scene->Update(dt);
}

void Application::RebuildScene()
{
    m_selectedEntity = entt::null;
    m_scene->Clear();
    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                        m_srvHeap.get(), cmdList);

    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(), std::string(ASSETS_DIR));

    std::string scriptPath = std::string(SCRIPTS_DIR) + "game.lua";
    if (std::filesystem::exists(scriptPath))
    {
        m_scriptEngine->LoadScript(scriptPath);
    }
    m_scriptEngine->CallOnStart();

    // sneakWalk アニメーション追加
    std::filesystem::path sneakPath = std::string(ASSETS_DIR) + "models/human/sneakWalk.gltf";
    if (std::filesystem::exists(sneakPath))
    {
        auto& reg = m_scene->GetRegistry();
        auto skelView = reg.view<SkeletalAnimation>();
        for (auto [e, skelAnim] : skelView.each())
        {
            auto extraAnims = ModelLoader::LoadAnimationsFromFile(
                sneakPath, *skelAnim.skeleton);
            for (auto& a : extraAnims)
            {
                a->SetName("sneakWalk");
                skelAnim.clips.push_back(std::move(a));
            }
        }
    }

    ThrowIfFailed(cmdList->Close());
    m_commandQueue->ExecuteCommandList(cmdList);
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();

    // ホットリロード用タイムスタンプ更新
    {
        std::string reloadPath = std::string(SCRIPTS_DIR) + "game.lua";
        if (std::filesystem::exists(reloadPath))
            m_scriptLastWriteTime = std::filesystem::last_write_time(reloadPath);
    }
}

void Application::EnterPlayMode()
{
    // カメラ状態保存
    m_cameraSnapshot.position = m_camera->GetPosition();
    m_cameraSnapshot.yaw = m_camera->GetYaw();
    m_cameraSnapshot.pitch = m_camera->GetPitch();

    m_inputSystem->SetMouseCapture(false);

    // シーン再構築（Luaスクリプト再読み込み）
    RebuildScene();

    // Playモード: サイドバーなし全画面幅でアスペクト比再計算
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);

    m_engineMode = EngineMode::Playing;
    Logger::Info("Entered PLAY mode");
}

void Application::EnterEditorMode()
{
    m_inputSystem->SetMouseCapture(false);

    // カメラ復元
    m_camera->SetPosition(m_cameraSnapshot.position);
    m_camera->SetYaw(m_cameraSnapshot.yaw);
    m_camera->SetPitch(m_cameraSnapshot.pitch);

    // シーン再構築
    RebuildScene();

    // Editorモード: サイドバー分を引いたアスペクト比に再計算
    f32 viewW = static_cast<f32>(m_window->GetWidth());
    if (!m_isGameMode) viewW = (std::max)(viewW - kLeftPanelWidth, 1.0f);
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        viewW / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);

    m_inputSystem->SetMouseCapture(false);
    m_engineMode = EngineMode::Editor;
    Logger::Info("Entered EDITOR mode");
}

void Application::BuildGame()
{
    namespace fs = std::filesystem;

    // ビルド出力先
    fs::path outputDir = fs::path(ASSETS_DIR).parent_path().parent_path() / "build" / "game";

    // クリーンアップ
    if (fs::exists(outputDir))
        fs::remove_all(outputDir);
    fs::create_directories(outputDir);

    // 1. exe をコピー
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        fs::path exeSrc(exePath);
        fs::path exeDst = outputDir / "Game.exe";
        fs::copy_file(exeSrc, exeDst, fs::copy_options::overwrite_existing);
        Logger::Info("Copied exe -> {}", exeDst.string());
    }

    // 2. scripts/ をコピー
    {
        fs::path scriptsSrc = fs::path(SCRIPTS_DIR);
        fs::path scriptsDst = outputDir / "scripts";
        if (fs::exists(scriptsSrc))
        {
            fs::copy(scriptsSrc, scriptsDst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            Logger::Info("Copied scripts/");
        }
    }

    // 3. assets/ をコピー
    {
        fs::path assetsSrc = fs::path(ASSETS_DIR);
        fs::path assetsDst = outputDir / "assets";
        if (fs::exists(assetsSrc))
        {
            fs::copy(assetsSrc, assetsDst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            Logger::Info("Copied assets/");
        }
    }

    // 4. shaders/ をコピー
    {
        fs::path shadersSrc = fs::path(SHADER_DIR);
        fs::path shadersDst = outputDir / "shaders";
        if (fs::exists(shadersSrc))
        {
            fs::copy(shadersSrc, shadersDst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            Logger::Info("Copied shaders/");
        }
    }

    // 5. --game フラグ付き起動用バッチファイル
    {
        std::ofstream bat(outputDir / "Game.bat");
        bat << "@echo off\n";
        bat << "Game.exe --game\n";
        bat << "pause\n";
    }

    Logger::Info("Game build complete: {}", outputDir.string());
}

void Application::Render()
{
    using namespace DirectX;

    auto* nativeCmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_commandList->Wrap(nativeCmdList);

    u32 frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    f32 totalTime = m_gameClock.GetTotalTime();

    // シャドウマップ再作成（ImGuiで解像度変更時、前フレーム完了後に実行）
    if (m_shadowMapDirty)
    {
        m_shadowMapDirty = false;
        m_shadowMap.Reset();

        D3D12_RESOURCE_DESC shadowDesc{};
        shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shadowDesc.Width = m_shadowMapSize;
        shadowDesc.Height = m_shadowMapSize;
        shadowDesc.DepthOrArraySize = 1;
        shadowDesc.MipLevels = 1;
        shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDesc.SampleDesc = {1, 0};
        shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil = {1.0f, 0};

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        ThrowIfFailed(m_graphicsDevice->GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE,
            &shadowDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue, IID_PPV_ARGS(&m_shadowMap)));

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_graphicsDevice->GetDevice()->CreateDepthStencilView(
            m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));
    }

    // ライト方向と View/Proj 行列
    XMFLOAT3 lightDirF3 = {-0.3f, -1.0f, -0.5f};
    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirF3));
    XMVECTOR lightPos = XMVectorScale(lightDir, -30.0f);  // ライト位置（シーン中心から離す）
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    XMMATRIX lightProj = XMMatrixOrthographicLH(30.0f, 30.0f, 0.1f, 60.0f);
    XMMATRIX lightViewProj = lightView * lightProj;

    // SRV ヒープをバインド（シャドウパスでもボーンSRVが必要）
    m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
    m_commandList->SetRootSignature(*m_rootSignature);

    // ===== シャドウパス =====
    {
        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        m_commandList->ClearDepthStencil(m_shadowDsvHandle);

        // RTVなし、DSVのみ
        nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandle);

        // シャドウマップ用ビューポート
        D3D12_VIEWPORT shadowVp{};
        shadowVp.Width    = static_cast<f32>(m_shadowMapSize);
        shadowVp.Height   = static_cast<f32>(m_shadowMapSize);
        shadowVp.MinDepth = 0.0f;
        shadowVp.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = {0, 0, static_cast<LONG>(m_shadowMapSize), static_cast<LONG>(m_shadowMapSize)};
        nativeCmdList->RSSetViewports(1, &shadowVp);
        nativeCmdList->RSSetScissorRects(1, &shadowScissor);

        // シャドウパスで全Entity（グリッドは除外）を描画
        {
            auto& reg = m_scene->GetRegistry();
            auto renderView = reg.view<const Transform, const MeshRenderer>();
            for (auto [e, transform, renderer] : renderView.each())
            {
                if (reg.all_of<GridPlane>(e)) continue;

                XMMATRIX world = transform.GetWorldMatrix();

                bool isSkinned = reg.all_of<SkeletalAnimation>(e);
                if (isSkinned)
                {
                    auto& skelAnim = reg.get<SkeletalAnimation>(e);
                    skelAnim.skinningBuffer->Update(skelAnim.animator->GetSkinningMatrices(), frameIndex);
                    m_commandList->SetPipelineState(*m_shadowSkinnedPipelineState);
                    m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                        m_srvHeap->GetGpuHandle(skelAnim.skinningBuffer->GetSrvIndex(frameIndex)));
                }
                else
                {
                    m_commandList->SetPipelineState(*m_shadowPipelineState);
                }

                bool hasNodeAnim = reg.all_of<NodeAnimationComp>(e);
                for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
                {
                    const auto* mesh = renderer.meshes[mi];

                    XMMATRIX meshWorld = world;
                    if (hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
                    {
                        XMMATRIX nodeMat = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]);
                        meshWorld = nodeMat * world;
                    }

                    struct PerObjectData { XMMATRIX mvp; XMMATRIX mdl; } objData;
                    objData.mvp = XMMatrixTranspose(meshWorld * lightViewProj);
                    objData.mdl = XMMatrixTranspose(meshWorld);
                    m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

                    m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                    m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
                    m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
                }
            }
        }

        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // ===== メインパス =====
    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    auto rtv = m_swapChain->GetCurrentRTV();

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constexpr float clearColor[4] = {0.392f, 0.584f, 0.929f, 1.0f};
    m_commandList->ClearRenderTarget(rtv, clearColor);
    m_commandList->ClearDepthStencil(m_dsvHandle);
    m_commandList->SetRenderTarget(rtv, m_dsvHandle);

    // エディタモード: パネル分3Dビューをオフセット
    if (!m_isGameMode && m_engineMode == EngineMode::Editor)
    {
        u32 left = static_cast<u32>(kLeftPanelWidth);
        u32 top = static_cast<u32>(kToolbarHeight);
        u32 ww = m_window->GetWidth();
        u32 wh = m_window->GetHeight();
        u32 viewW = (ww > left) ? ww - left : 1;
        u32 viewH = (wh > top) ? wh - top : 1;
        m_commandList->SetViewportAndScissor(left, top, viewW, viewH);
    }
    else
    {
        m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
    }

    m_commandList->SetPipelineState(*m_pipelineState);

    // PerFrame CB（lightViewProj追加）
    struct FrameConstants {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT3   lightDir;
        float      time;
        XMFLOAT3   lightColor;
        float      ambientStrength;
        XMFLOAT4X4 lightVP;
    };

    FrameConstants fc{};
    XMStoreFloat4x4(&fc.view, XMMatrixTranspose(m_camera->GetViewMatrix()));
    XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(m_camera->GetProjectionMatrix()));
    fc.lightDir = lightDirF3;
    fc.time = totalTime;
    fc.lightColor = { 1.0f, 0.95f, 0.9f };
    fc.ambientStrength = 0.25f;
    XMStoreFloat4x4(&fc.lightVP, XMMatrixTranspose(lightViewProj));

    m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);
    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

    // シャドウマップSRVをバインド
    m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
        m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

    XMMATRIX viewProj = m_camera->GetViewProjMatrix();

    // 全Entityを描画
    {
        auto& reg = m_scene->GetRegistry();
        auto renderView = reg.view<const Transform, const MeshRenderer>();
        for (auto [e, transform, renderer] : renderView.each())
        {
            XMMATRIX world = transform.GetWorldMatrix();

            bool isGrid = reg.all_of<GridPlane>(e);
            bool isSkinned = reg.all_of<SkeletalAnimation>(e);

            if (isGrid)
            {
                m_commandList->SetPipelineState(*m_gridPipelineState);
            }
            else if (isSkinned)
            {
                auto& skelAnim = reg.get<SkeletalAnimation>(e);
                m_commandList->SetPipelineState(*m_skinnedPipelineState);
                m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                    m_srvHeap->GetGpuHandle(skelAnim.skinningBuffer->GetSrvIndex(frameIndex)));
            }
            else
            {
                m_commandList->SetPipelineState(*m_pipelineState);
            }

            bool hasNodeAnim = reg.all_of<NodeAnimationComp>(e);
            for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
            {
                const auto* mesh = renderer.meshes[mi];

                XMMATRIX meshWorld = world;
                if (hasNodeAnim && mi < static_cast<u32>(renderer.meshNodeTransforms.size()))
                {
                    XMMATRIX nodeMat = XMLoadFloat4x4(&renderer.meshNodeTransforms[mi]);
                    meshWorld = nodeMat * world;
                }

                struct PerObjectData { XMMATRIX mvp; XMMATRIX mdl; } objData;
                objData.mvp = XMMatrixTranspose(meshWorld * viewProj);
                objData.mdl = XMMatrixTranspose(meshWorld);
                m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

                const Material* mat = mesh->GetMaterial();
                Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable, m_srvHeap->GetGpuHandle(tex->GetSrvIndex()));

                m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
                m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
            }
        }
    }

    // ---- ImGui フレーム ----
    m_imguiManager->BeginFrame();
    ImGuizmo::BeginFrame();

    if (!m_isGameMode)
    {
    f32 displayW = ImGui::GetIO().DisplaySize.x;
    f32 displayH = ImGui::GetIO().DisplaySize.y;

    // ===== ツールバー（上部全幅） =====
    {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(displayW, kToolbarHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.140f, 0.140f, 0.140f, 1.0f));
        ImGui::Begin("##Toolbar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);

        // Play/Stop
        if (m_engineMode == EngineMode::Editor)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
            if (ImGui::Button("\xe2\x96\xb6 \xe5\x86\x8d\xe7\x94\x9f"))  // ▶ 再生
            {
                m_pendingMode = EngineMode::Playing;
                m_modeChangeRequested = true;
            }
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
            if (ImGui::Button("\xe2\x96\xa0 \xe5\x81\x9c\xe6\xad\xa2"))  // ■ 停止
            {
                m_pendingMode = EngineMode::Editor;
                m_modeChangeRequested = true;
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::SameLine(0, 12);
        if (m_engineMode == EngineMode::Playing)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
            ImGui::Text("\xe2\x97\x8f \xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe4\xb8\xad");  // ● プレイ中
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf");  // エディタ
        }

        // Luaエラー
        if (!m_scriptEngine->GetLastError().empty())
        {
            ImGui::SameLine(0, 16);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::Text("\xe2\x9a\xa0 Lua Error");  // ⚠ Lua Error
            ImGui::PopStyleColor();
        }

        // ホットリロード通知
        if (m_hotReloadFlash > 0.0f)
        {
            ImGui::SameLine(0, 12);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, m_hotReloadFlash));
            ImGui::Text("\xe2\x9c\x93 Reloaded");  // ✓ Reloaded
            ImGui::PopStyleColor();
            m_hotReloadFlash -= m_gameClock.GetDeltaTime();
        }

        // ギズモモードボタン
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0, 8);

        bool isTrans = (m_gizmoMode == GizmoMode::Translate);
        bool isRot   = (m_gizmoMode == GizmoMode::Rotate);
        bool isScl   = (m_gizmoMode == GizmoMode::Scale);

        if (isTrans) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button("W Move")) m_gizmoMode = GizmoMode::Translate;
        if (isTrans) ImGui::PopStyleColor();

        ImGui::SameLine();
        if (isRot) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button("E Rot")) m_gizmoMode = GizmoMode::Rotate;
        if (isRot) ImGui::PopStyleColor();

        ImGui::SameLine();
        if (isScl) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button("R Scl")) m_gizmoMode = GizmoMode::Scale;
        if (isScl) ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button(m_gizmoLocalSpace ? "Local" : "World"))
            m_gizmoLocalSpace = !m_gizmoLocalSpace;

        // FPS（右寄せ）
        ImGui::SameLine(displayW - 100);
        ImGui::Text("%.0f FPS", m_gameClock.GetFPS());

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ===== ヒエラルキー（左パネル） =====
    {
        ImGui::SetNextWindowPos(ImVec2(0, kToolbarHeight), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(kLeftPanelWidth, displayH - kToolbarHeight), ImGuiCond_Always);
        ImGui::Begin("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc", nullptr,  // ヒエラルキー
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        // --- ヒエラルキー ---
        ImGui::TextDisabled("\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3  (%zu)", m_scene->GetEntityCount());  // シーン
        ImGui::Separator();
        {
            auto& reg = m_scene->GetRegistry();
            auto nameView = reg.view<const NameTag>();
            for (auto [e, tag] : nameView.each())
            {
                bool selected = (e == m_selectedEntity);

                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (selected) flags |= ImGuiTreeNodeFlags_Selected;

                bool open = ImGui::TreeNodeEx(tag.name.c_str(), flags);
                if (ImGui::IsItemClicked())
                    m_selectedEntity = selected ? entt::null : e;
                if (open) ImGui::TreePop();
            }
        }

        ImGui::Separator();

        // --- 選択Entity プロパティ ---
        {
            auto& reg = m_scene->GetRegistry();
            if (m_selectedEntity != entt::null && reg.valid(m_selectedEntity))
            {
                // NameTag
                if (reg.all_of<NameTag>(m_selectedEntity))
                {
                    auto& tag = reg.get<NameTag>(m_selectedEntity);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.39f, 0.58f, 0.93f, 1.0f));
                    ImGui::Text("%s", tag.name.c_str());
                    ImGui::PopStyleColor();
                }

                ImGui::Separator();

                // Transform
                if (reg.all_of<Transform>(m_selectedEntity))
                {
                    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        auto& t = reg.get<Transform>(m_selectedEntity);
                        ImGui::DragFloat3("Position", &t.position.x, 0.1f);
                        ImGui::DragFloat3("Rotation", &t.rotation.x, 1.0f);
                        ImGui::DragFloat3("Scale",    &t.scale.x,    0.01f);
                    }
                }

                // MeshRenderer
                if (reg.all_of<MeshRenderer>(m_selectedEntity))
                {
                    if (ImGui::CollapsingHeader("MeshRenderer"))
                    {
                        auto& r = reg.get<MeshRenderer>(m_selectedEntity);
                        ImGui::Text("Meshes: %d", static_cast<int>(r.meshes.size()));
                        ImGui::Text("Materials: %d", static_cast<int>(r.materials.size()));
                    }
                }

                // SkeletalAnimation
                if (reg.all_of<SkeletalAnimation>(m_selectedEntity))
                {
                    auto& skelAnim = reg.get<SkeletalAnimation>(m_selectedEntity);
                    if (skelAnim.animator && ImGui::CollapsingHeader("SkeletalAnimation"))
                    {
                        ImGui::Text("Bones: %d",
                            static_cast<int>(skelAnim.skeleton ? skelAnim.skeleton->GetBoneCount() : 0));
                        ImGui::Text("Clips: %d", static_cast<int>(skelAnim.clips.size()));

                        for (i32 i = 0; i < static_cast<i32>(skelAnim.clips.size()); ++i)
                        {
                            const auto& clip = skelAnim.clips[i];
                            std::string label = clip->GetName().empty()
                                ? ("Clip " + std::to_string(i))
                                : clip->GetName();
                            if (ImGui::Selectable(label.c_str()))
                                skelAnim.animator->CrossFadeTo(clip.get(), 0.3f);
                        }
                    }
                }

                // NodeAnimation
                if (reg.all_of<NodeAnimationComp>(m_selectedEntity))
                {
                    auto& nodeAnim = reg.get<NodeAnimationComp>(m_selectedEntity);
                    if (nodeAnim.nodeAnimator && ImGui::CollapsingHeader("NodeAnimation"))
                    {
                        ImGui::Text("Clips: %d", static_cast<int>(nodeAnim.clips.size()));
                        for (i32 i = 0; i < static_cast<i32>(nodeAnim.clips.size()); ++i)
                        {
                            const auto& clip = nodeAnim.clips[i];
                            std::string label = clip->GetName().empty()
                                ? ("Clip " + std::to_string(i))
                                : clip->GetName();
                            if (ImGui::Selectable(label.c_str()))
                                nodeAnim.nodeAnimator->CrossFadeTo(clip.get(), 0.3f);
                        }
                    }
                }

                // GridPlane
                if (reg.all_of<GridPlane>(m_selectedEntity))
                {
                    if (ImGui::CollapsingHeader("GridPlane"))
                    {
                        auto& gp = reg.get<GridPlane>(m_selectedEntity);
                        ImGui::Checkbox("Enabled", &gp.enabled);
                    }
                }

                // PointLight
                if (reg.all_of<PointLight>(m_selectedEntity))
                {
                    if (ImGui::CollapsingHeader("PointLight"))
                    {
                        auto& pl = reg.get<PointLight>(m_selectedEntity);
                        ImGui::ColorEdit3("Color", &pl.color.x);
                        ImGui::DragFloat("Intensity", &pl.intensity, 0.1f, 0.0f, 100.0f);
                        ImGui::DragFloat("Range", &pl.range, 0.5f, 0.0f, 500.0f);
                    }
                }

                // DirectionalLight
                if (reg.all_of<DirectionalLight>(m_selectedEntity))
                {
                    if (ImGui::CollapsingHeader("DirectionalLight"))
                    {
                        auto& dl = reg.get<DirectionalLight>(m_selectedEntity);
                        ImGui::DragFloat3("Direction", &dl.direction.x, 0.01f);
                        ImGui::ColorEdit3("Color", &dl.color.x);
                        ImGui::DragFloat("Intensity", &dl.intensity, 0.1f, 0.0f, 100.0f);
                    }
                }

                // CameraComponent
                if (reg.all_of<CameraComponent>(m_selectedEntity))
                {
                    if (ImGui::CollapsingHeader("Camera"))
                    {
                        auto& cam = reg.get<CameraComponent>(m_selectedEntity);
                        ImGui::DragFloat("FOV", &cam.fovDegrees, 1.0f, 1.0f, 179.0f);
                        ImGui::DragFloat("Near", &cam.nearClip, 0.01f, 0.001f, 100.0f);
                        ImGui::DragFloat("Far", &cam.farClip, 10.0f, 1.0f, 100000.0f);
                        ImGui::Checkbox("Active", &cam.isActive);
                    }
                }
            }
        }

        ImGui::Separator();

        // --- カメラ ---
        if (ImGui::CollapsingHeader("\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9", ImGuiTreeNodeFlags_DefaultOpen))  // カメラ
        {
            auto camPos = m_camera->GetPosition();
            ImGui::Text("%.1f, %.1f, %.1f", camPos.x, camPos.y, camPos.z);
            f32 moveSpeed = m_camera->GetMoveSpeed();
            if (ImGui::SliderFloat("\xe9\x80\x9f\xe5\xba\xa6", &moveSpeed, 1.0f, 50.0f))  // 速度
                m_camera->SetMoveSpeed(moveSpeed);
        }

        // --- シャドウ品質 ---
        if (ImGui::CollapsingHeader("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x89\xe3\x82\xa6"))  // シャドウ
        {
            const char* qualities[] = {"1024 (Low)", "2048 (Medium)", "4096 (High)", "8192 (Ultra)"};
            const u32 sizes[] = {1024, 2048, 4096, 8192};
            if (ImGui::Combo("\xe8\xa7\xa3\xe5\x83\x8f\xe5\xba\xa6", &m_shadowQualityIndex, qualities, 4))  // 解像度
            {
                m_shadowMapSize = sizes[m_shadowQualityIndex];
                m_shadowMapDirty = true;  // 次フレームで再作成
            }
            ImGui::Text("%ux%u", m_shadowMapSize, m_shadowMapSize);
        }

        // --- オーディオ ---
        if (ImGui::CollapsingHeader("\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xaa"))  // オーディオ
        {
            f32 masterVol = m_audioSystem->GetMasterVolume();
            f32 bgmVol    = m_audioSystem->GetBGMVolume();
            f32 sfxVol    = m_audioSystem->GetSFXVolume();
            if (ImGui::SliderFloat("\xe3\x83\x9e\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc", &masterVol, 0.0f, 1.0f))  // マスター
                m_audioSystem->SetMasterVolume(masterVol);
            if (ImGui::SliderFloat("BGM", &bgmVol, 0.0f, 1.0f))
                m_audioSystem->SetBGMVolume(bgmVol);
            if (ImGui::SliderFloat("SE", &sfxVol, 0.0f, 1.0f))
                m_audioSystem->SetSFXVolume(sfxVol);

            const auto& bgmList = m_audioSystem->GetBGMList();
            for (const auto& bgm : bgmList)
            {
                std::string fn = std::filesystem::path(bgm).filename().string();
                ImGui::PushID(bgm.c_str());
                if (ImGui::Button("\xe2\x96\xb6")) m_audioSystem->PlayBGM(bgm);
                ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
                ImGui::PopID();
            }
            if (!bgmList.empty() && ImGui::Button("BGM \xe5\x81\x9c\xe6\xad\xa2"))
                m_audioSystem->StopBGM();

            const auto& sfxList = m_audioSystem->GetSFXList();
            for (const auto& sfx : sfxList)
            {
                std::string fn = std::filesystem::path(sfx).filename().string();
                ImGui::PushID(sfx.c_str());
                if (ImGui::Button("\xe2\x96\xb6")) m_audioSystem->PlaySFX(sfx);
                ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
                ImGui::PopID();
            }
            if (!sfxList.empty() && ImGui::Button("SE \xe5\x85\xa8\xe5\x81\x9c\xe6\xad\xa2"))
                m_audioSystem->StopAllSFX();
        }

        // --- 設定 ---
        if (ImGui::CollapsingHeader("\xe8\xa8\xad\xe5\xae\x9a"))  // 設定
        {
            ImGui::Checkbox("VSync", &m_useVsync);
        }

        // --- ビルド ---
        if (ImGui::CollapsingHeader("\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // ビルド
        {
            if (ImGui::Button("\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // ゲームビルド
            {
                BuildGame();
                m_buildCompleteFlash = 3.0f;
            }
            if (m_buildCompleteFlash > 0.0f)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
                ImGui::Text("\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86!");  // ビルド完了!
                ImGui::PopStyleColor();
                m_buildCompleteFlash -= m_gameClock.GetDeltaTime();
            }
        }

        // Luaエラー
        if (!m_scriptEngine->GetLastError().empty())
        {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", m_scriptEngine->GetLastError().c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }
    } // !m_isGameMode

    // ===== ImGuizmo ギズモ描画 =====
    if (!m_isGameMode && m_engineMode == EngineMode::Editor
        && m_selectedEntity != entt::null)
    {
        auto& reg = m_scene->GetRegistry();
        if (reg.valid(m_selectedEntity) && reg.all_of<Transform>(m_selectedEntity))
        {
            auto& transform = reg.get<Transform>(m_selectedEntity);

            XMFLOAT4X4 viewF, projF;
            XMStoreFloat4x4(&viewF, m_camera->GetViewMatrix());
            XMStoreFloat4x4(&projF, m_camera->GetProjectionMatrix());

            XMFLOAT4X4 worldF;
            XMStoreFloat4x4(&worldF, transform.GetWorldMatrix());

            // 3Dビューポート領域
            u32 vpX = static_cast<u32>(kLeftPanelWidth);
            u32 vpY = static_cast<u32>(kToolbarHeight);
            u32 vpW = (m_window->GetWidth() > vpX) ? m_window->GetWidth() - vpX : 1;
            u32 vpH = (m_window->GetHeight() > vpY) ? m_window->GetHeight() - vpY : 1;

            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
            ImGuizmo::SetRect(static_cast<float>(vpX), static_cast<float>(vpY),
                              static_cast<float>(vpW), static_cast<float>(vpH));

            ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
            if (m_gizmoMode == GizmoMode::Rotate) op = ImGuizmo::ROTATE;
            if (m_gizmoMode == GizmoMode::Scale)  op = ImGuizmo::SCALE;

            ImGuizmo::MODE mode = m_gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

            float snapValues[3] = {1.0f, 1.0f, 1.0f};
            if (m_gizmoMode == GizmoMode::Rotate)
                snapValues[0] = snapValues[1] = snapValues[2] = 15.0f;
            else if (m_gizmoMode == GizmoMode::Scale)
                snapValues[0] = snapValues[1] = snapValues[2] = 0.1f;
            bool useSnap = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

            if (ImGuizmo::Manipulate(
                    &viewF._11, &projF._11,
                    op, mode,
                    &worldF._11, nullptr,
                    useSnap ? snapValues : nullptr))
            {
                float translation[3], rotation[3], scale[3];
                ImGuizmo::DecomposeMatrixToComponents(
                    &worldF._11, translation, rotation, scale);
                transform.position = {translation[0], translation[1], translation[2]};
                transform.rotation = {rotation[0], rotation[1], rotation[2]};
                transform.scale    = {scale[0], scale[1], scale[2]};
            }
        }
    }

    m_imguiManager->EndFrame(nativeCmdList);

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(m_useVsync);
    m_frameResources->EndFrame(*m_commandQueue);
}

} // namespace dx12e
