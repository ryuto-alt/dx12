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
#include "input/InputSystem.h"
#include "scene/Scene.h"
#include "scene/Entity.h"
#include "scripting/ScriptEngine.h"
#include "audio/AudioSystem.h"
#include "gui/ImGuiManager.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <thread>
#include <fstream>
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
        if (!m_isGameMode) viewW = (std::max)(viewW - kSidebarWidth, 1.0f);
        m_camera->SetPerspective(DirectX::XM_PIDIV4,
            viewW / static_cast<f32>(m_window->GetHeight()), 0.1f, 1000.0f);
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
                for (const auto& entity : m_scene->GetEntities())
                {
                    if (entity->skeleton)
                    {
                        auto extraAnims = ModelLoader::LoadAnimationsFromFile(
                            sneakPath, *entity->skeleton);
                        for (auto& a : extraAnims)
                        {
                            a->SetName("sneakWalk");
                            entity->animClips.push_back(std::move(a));
                        }
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
        shadowDesc.Width = kShadowMapSize;
        shadowDesc.Height = kShadowMapSize;
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

        Logger::Info("Shadow map initialized ({}x{})", kShadowMapSize, kShadowMapSize);
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
                    if (!m_isGameMode && m_engineMode == EngineMode::Editor)
                        viewW = (std::max)(viewW - kSidebarWidth, 1.0f);
                    m_camera->SetPerspective(DirectX::XM_PIDIV4,
                        viewW / static_cast<f32>(h), 0.1f, 1000.0f);
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
        if (m_inputSystem->IsMouseCaptured())
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
        for (const auto& entity : m_scene->GetEntities())
        {
            if (entity->skeleton)
            {
                auto extraAnims = ModelLoader::LoadAnimationsFromFile(
                    sneakPath, *entity->skeleton);
                for (auto& a : extraAnims)
                {
                    a->SetName("sneakWalk");
                    entity->animClips.push_back(std::move(a));
                }
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
    if (!m_isGameMode) viewW = (std::max)(viewW - kSidebarWidth, 1.0f);
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

    // ライト方向と View/Proj 行列
    XMFLOAT3 lightDirF3 = {0.3f, -1.0f, 0.5f};
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
        shadowVp.Width    = static_cast<f32>(kShadowMapSize);
        shadowVp.Height   = static_cast<f32>(kShadowMapSize);
        shadowVp.MinDepth = 0.0f;
        shadowVp.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = {0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
        nativeCmdList->RSSetViewports(1, &shadowVp);
        nativeCmdList->RSSetScissorRects(1, &shadowScissor);

        // シャドウパスで全Entity（グリッドは除外）を描画
        m_scene->ForEachEntity([&](const Entity& entity) {
            if (entity.useGridShader) return;  // グリッドはシャドウキャスターにしない

            XMMATRIX world = entity.transform.GetWorldMatrix();

            if (entity.hasSkeleton && entity.skinningBuffer && entity.animator)
            {
                entity.skinningBuffer->Update(entity.animator->GetSkinningMatrices(), frameIndex);
                m_commandList->SetPipelineState(*m_shadowSkinnedPipelineState);
                m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                    m_srvHeap->GetGpuHandle(entity.skinningBuffer->GetSrvIndex(frameIndex)));
            }
            else
            {
                m_commandList->SetPipelineState(*m_shadowPipelineState);
            }

            for (const auto& mesh : entity.meshes)
            {
                struct PerObjectData {
                    XMMATRIX mvp;
                    XMMATRIX mdl;
                } objData;
                objData.mvp = XMMatrixTranspose(world * lightViewProj);
                objData.mdl = XMMatrixTranspose(world);
                m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

                m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
                m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
            }
        });

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

    // エディタモード: サイドバー分3Dビューをオフセット
    if (!m_isGameMode && m_engineMode == EngineMode::Editor)
    {
        u32 sideW = static_cast<u32>(kSidebarWidth);
        u32 viewW = m_window->GetWidth() > sideW ? m_window->GetWidth() - sideW : m_window->GetWidth();
        m_commandList->SetViewportAndScissor(sideW, 0, viewW, m_window->GetHeight());
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
    fc.ambientStrength = 0.15f;
    XMStoreFloat4x4(&fc.lightVP, XMMatrixTranspose(lightViewProj));

    m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);
    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

    // シャドウマップSRVをバインド
    m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
        m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

    XMMATRIX viewProj = m_camera->GetViewProjMatrix();

    // 全Entityを描画
    m_scene->ForEachEntity([&](const Entity& entity) {
        XMMATRIX world = entity.transform.GetWorldMatrix();

        // パイプライン切り替え
        if (entity.useGridShader)
        {
            m_commandList->SetPipelineState(*m_gridPipelineState);
        }
        else if (entity.hasSkeleton && entity.skinningBuffer && entity.animator)
        {
            m_commandList->SetPipelineState(*m_skinnedPipelineState);
            m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                m_srvHeap->GetGpuHandle(entity.skinningBuffer->GetSrvIndex(frameIndex)));
        }
        else
        {
            m_commandList->SetPipelineState(*m_pipelineState);
        }

        for (const auto& mesh : entity.meshes)
        {
            struct PerObjectData {
                XMMATRIX mvp;
                XMMATRIX mdl;
            } objData;
            objData.mvp = XMMatrixTranspose(world * viewProj);
            objData.mdl = XMMatrixTranspose(world);
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

            const Material* mat = mesh->GetMaterial();
            Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
            m_commandList->SetSRVTable(RootSignature::kSlotSRVTable, m_srvHeap->GetGpuHandle(tex->GetSrvIndex()));

            m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
            m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
            m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
        }
    });

    // ---- ImGui フレーム ----
    m_imguiManager->BeginFrame();

    if (!m_isGameMode)
    {
    // ===== 左サイドバー「DX12 エンジン」 =====
    {
        f32 panelWidth = 280.0f;
        f32 displayH = ImGui::GetIO().DisplaySize.y;
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, displayH), ImGuiCond_Always);
        ImGui::Begin("DX12 \xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xb8\xe3\x83\xb3", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        // --- 再生/停止 + FPS ---
        if (m_engineMode == EngineMode::Editor)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            if (ImGui::Button("\xe5\x86\x8d\xe7\x94\x9f"))
            {
                m_pendingMode = EngineMode::Playing;
                m_modeChangeRequested = true;
            }
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("\xe5\x81\x9c\xe6\xad\xa2"))
            {
                m_pendingMode = EngineMode::Editor;
                m_modeChangeRequested = true;
            }
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::Text("%s", m_engineMode == EngineMode::Editor
            ? "\xe3\x82\xa8\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xbf"    // エディタ
            : "\xe3\x83\x97\xe3\x83\xac\xe3\x82\xa4\xe4\xb8\xad");  // プレイ中
        ImGui::SameLine(panelWidth - 80);
        ImGui::Text("%.0f FPS", m_gameClock.GetFPS());

        if (m_hotReloadFlash > 0.0f)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.4f, m_hotReloadFlash));
            ImGui::Text("\xe3\x83\xaa\xe3\x83\xad\xe3\x83\xbc\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86");  // リロード完了
            ImGui::PopStyleColor();
            m_hotReloadFlash -= m_gameClock.GetDeltaTime();
        }

        // --- Luaエラー ---
        if (!m_scriptEngine->GetLastError().empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
            ImGui::TextWrapped("Lua: %s", m_scriptEngine->GetLastError().c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // --- シーン ---
        if (ImGui::CollapsingHeader(
            "\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3",  // シーン
            ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text(
                "\xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3: %zu",  // エンティティ
                m_scene->GetEntityCount());

            for (const auto& entity : m_scene->GetEntities())
            {
                bool hasAnim = entity->animator && !entity->animClips.empty();

                if (hasAnim)
                {
                    if (ImGui::TreeNode(entity->name.c_str()))
                    {
                        auto& pos = entity->transform.position;
                        ImGui::Text("%.1f, %.1f, %.1f", pos.x, pos.y, pos.z);

                        for (i32 i = 0; i < static_cast<i32>(entity->animClips.size()); ++i)
                        {
                            const auto& clip = entity->animClips[i];
                            std::string label = clip->GetName().empty()
                                ? ("Anim " + std::to_string(i))
                                : clip->GetName();
                            if (ImGui::Selectable(label.c_str()))
                            {
                                entity->animator->CrossFadeTo(clip.get(), 0.3f);
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                else
                {
                    ImGui::BulletText("%s", entity->name.c_str());
                }
            }
        }

        // --- カメラ ---
        if (ImGui::CollapsingHeader(
            "\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9",  // カメラ
            ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto camPos = m_camera->GetPosition();
            ImGui::Text(
                "\xe4\xbd\x8d\xe7\xbd\xae: %.1f, %.1f, %.1f",  // 位置
                camPos.x, camPos.y, camPos.z);
            ImGui::Text("%s", m_engineMode == EngineMode::Editor
                ? "\xe5\x8f\xb3\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x83\xe3\x82\xaf\xe3\x81\xa7\xe6\x93\x8d\xe4\xbd\x9c"  // 右クリックで操作
                : "Lua \xe5\x88\xb6\xe5\xbe\xa1");  // Lua 制御

            f32 moveSpeed = m_camera->GetMoveSpeed();
            if (ImGui::SliderFloat(
                "\xe9\x80\x9f\xe5\xba\xa6",  // 速度
                &moveSpeed, 1.0f, 50.0f))
            {
                m_camera->SetMoveSpeed(moveSpeed);
            }
        }

        // --- オーディオ ---
        if (ImGui::CollapsingHeader(
            "\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xaa",  // オーディオ
            ImGuiTreeNodeFlags_DefaultOpen))
        {
            f32 masterVol = m_audioSystem->GetMasterVolume();
            f32 bgmVol    = m_audioSystem->GetBGMVolume();
            f32 sfxVol    = m_audioSystem->GetSFXVolume();
            if (ImGui::SliderFloat(
                "\xe3\x83\x9e\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc",  // マスター
                &masterVol, 0.0f, 1.0f))
                m_audioSystem->SetMasterVolume(masterVol);
            if (ImGui::SliderFloat("BGM", &bgmVol, 0.0f, 1.0f))
                m_audioSystem->SetBGMVolume(bgmVol);
            if (ImGui::SliderFloat("SE", &sfxVol, 0.0f, 1.0f))
                m_audioSystem->SetSFXVolume(sfxVol);

            // BGM ファイル一覧
            const auto& bgmList = m_audioSystem->GetBGMList();
            if (!bgmList.empty())
            {
                ImGui::Separator();
                ImGui::Text("BGM");
                for (const auto& bgm : bgmList)
                {
                    std::string filename = std::filesystem::path(bgm).filename().string();
                    ImGui::PushID(bgm.c_str());
                    if (ImGui::Button("\xe2\x96\xb6"))  // ▶
                    {
                        m_audioSystem->PlayBGM(bgm);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", filename.c_str());
                    ImGui::PopID();
                }
                if (ImGui::Button(
                    "BGM \xe5\x81\x9c\xe6\xad\xa2"))  // BGM 停止
                {
                    m_audioSystem->StopBGM();
                }
            }

            // SFX ファイル一覧
            const auto& sfxList = m_audioSystem->GetSFXList();
            if (!sfxList.empty())
            {
                ImGui::Separator();
                ImGui::Text("SE");
                for (const auto& sfx : sfxList)
                {
                    std::string filename = std::filesystem::path(sfx).filename().string();
                    ImGui::PushID(sfx.c_str());
                    if (ImGui::Button("\xe2\x96\xb6"))  // ▶
                    {
                        m_audioSystem->PlaySFX(sfx);
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", filename.c_str());
                    ImGui::PopID();
                }
                if (ImGui::Button(
                    "SE \xe5\x85\xa8\xe5\x81\x9c\xe6\xad\xa2"))  // SE 全停止
                {
                    m_audioSystem->StopAllSFX();
                }
            }

            if (bgmList.empty() && sfxList.empty())
            {
                ImGui::TextDisabled("assets/audio/bgm/ \xe3\x81\xa8 sfx/ \xe3\x81\xab\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\x92\xe9\x85\x8d\xe7\xbd\xae");  // assets/audio/bgm/ と sfx/ にファイルを配置
            }
        }

        // --- 設定 ---
        if (ImGui::CollapsingHeader(
            "\xe8\xa8\xad\xe5\xae\x9a"))  // 設定
        {
            ImGui::Checkbox("VSync", &m_useVsync);
        }

        // --- ビルド ---
        ImGui::Separator();
        if (ImGui::CollapsingHeader(
            "\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // ビルド
        {
            ImGui::TextWrapped(
                "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x82\x92\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe3\x81\x97\xe3\x81\xa6"  // ゲームをビルドして
                " build/game/ "
                "\xe3\x81\xab\xe5\x87\xba\xe5\x8a\x9b\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99");  // に出力します
            if (ImGui::Button(
                "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // ゲームビルド
            {
                BuildGame();
                m_buildCompleteFlash = 3.0f;
            }
            if (m_buildCompleteFlash > 0.0f)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.4f, 1.0f));
                ImGui::Text(
                    "\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86!");  // ビルド完了!
                ImGui::PopStyleColor();
                m_buildCompleteFlash -= m_gameClock.GetDeltaTime();
            }
        }

        ImGui::End();
    }
    } // !m_isGameMode

    m_imguiManager->EndFrame(nativeCmdList);

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(m_useVsync);
    m_frameResources->EndFrame(*m_commandQueue);
}

} // namespace dx12e
