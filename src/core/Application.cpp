#include "Application.h"
#include "Logger.h"
#include "Assert.h"

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
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "scripting/ScriptEngine.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsSystem.h"
#include "physics/PhysicsDebugRenderer.h"
#include "gui/ImGuiManager.h"

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <thread>
#include <immintrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace dx12e
{

namespace
{
constexpr u32 kMaxPointLightsCB = 8;

struct PointLightGPU {
    DirectX::XMFLOAT3 position;
    float             range;
    DirectX::XMFLOAT3 color;
    float             _pad;
};

struct FrameConstants {
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT3   lightDir;
    float                time;
    DirectX::XMFLOAT3   lightColor;
    float                ambientStrength;
    DirectX::XMFLOAT4X4 lightViewProj;
    DirectX::XMFLOAT3   cameraPos;
    float                _pad;
    u32                  numPointLights;
    float                _pad2[3];
    PointLightGPU        pointLights[kMaxPointLightsCB];
};
static_assert(sizeof(FrameConstants) == 512, "FrameConstants must be 512 bytes");
}  // namespace

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

    m_gameClock.Reset();

    // Input System
    m_inputSystem = std::make_unique<InputSystem>();
    m_inputSystem->Initialize(m_window->GetHwnd());
    m_window->SetInputSystem(m_inputSystem.get());

    // Audio System
    m_audioSystem = std::make_unique<AudioSystem>();
    m_audioSystem->Initialize(std::string(ASSETS_DIR));

    // Physics System
    m_physicsSystem = std::make_unique<PhysicsSystem>();
    m_physicsSystem->Initialize();

    // Shader Visible SRV ヒープ
    m_srvHeap = std::make_unique<DescriptorHeap>();
    m_srvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, true);

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

    // シェーダー & PipelineState (forward)
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
               .SetDepthEnabled(true)
               .SetCullMode(D3D12_CULL_MODE_NONE);

        m_pipelineState = std::make_unique<PipelineState>();
        m_pipelineState->Initialize(*m_graphicsDevice, builder);
    }

    // Camera (free WASD/mouse cam, fallback when no CameraComponent active)
    m_camera = std::make_unique<Camera>();
    {
        f32 viewW = static_cast<f32>(m_window->GetWidth());
        f32 viewH = static_cast<f32>(m_window->GetHeight());
        m_camera->SetPerspective(kDefaultFovYRad, viewW / viewH, kDefaultNearZ, kDefaultFarZ);
    }
    m_camera->LookAt({-14.7f, 9.6f, -9.0f}, {0.0f, 0.0f, 0.0f});

    // シーン + Lua 初期化
    {
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        m_resourceManager = std::make_unique<ResourceManager>();
        m_resourceManager->Initialize(m_graphicsDevice.get(), m_srvHeap.get(), cmdList);

        m_scene = std::make_unique<Scene>();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // ScriptEngine 初期化
        m_scriptEngine = std::make_unique<ScriptEngine>();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), std::string(ASSETS_DIR));

        // game.lua 読み込み → OnStart 呼び出し
        std::string scriptPath = std::string(SCRIPTS_DIR) + "game.lua";
        if (std::filesystem::exists(scriptPath))
        {
            m_scriptEngine->LoadScript(scriptPath);
            m_scriptEngine->CallOnStart();
            m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
        }
        else
        {
            Logger::Warn("Game script not found: {}", scriptPath);
        }

        // game.lua が何も生成しなかった場合のデフォルトシーン (Grid + DirectionalLight)
        if (m_scene->GetEntityCount() == 0)
        {
            m_scene->SpawnPlane("Grid", {0, 0, 0}, 50.0f, true);
            auto& reg = m_scene->GetRegistry();
            auto lightE = reg.create();
            reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
            reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
            reg.emplace<DirectionalLight>(lightE);
        }

        // エンティティアタッチ済みの LuaScript を初期化
        m_scriptEngine->OnPlayStart();

        // 全 RigidBody を物理エンジンに登録
        {
            auto& reg = m_scene->GetRegistry();
            auto rbView = reg.view<RigidBody>();
            for (auto [entity, rb] : rbView.each())
            {
                if (rb.bodyId == kInvalidBodyId)
                    m_physicsSystem->RegisterBody(reg, entity);
            }
        }

        // コマンド実行 + GPU 待ち
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();
        m_resourceManager->FinishUploads();

        // スキニング PSO
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
                   .SetDepthEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE);

            m_skinnedPipelineState = std::make_unique<PipelineState>();
            m_skinnedPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // グリッド PSO
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
                   .SetCullMode(D3D12_CULL_MODE_NONE)
                   .SetDepthBias(-100, -1.0f);

            m_gridPipelineState = std::make_unique<PipelineState>();
            m_gridPipelineState->Initialize(*m_graphicsDevice, builder);
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

        m_shadowDsvHandle = m_shadowDsvHeap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_graphicsDevice->GetDevice()->CreateDepthStencilView(
            m_shadowMap.Get(), &dsvDesc, m_shadowDsvHandle);

        m_shadowSrvIndex = m_srvHeap->AllocateIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_graphicsDevice->GetDevice()->CreateShaderResourceView(
            m_shadowMap.Get(), &srvDesc, m_srvHeap->GetCpuHandle(m_shadowSrvIndex));

        // Shadow PSO
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
    m_perFrameCB = std::make_unique<ConstantBuffer>();
    m_perFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // CommandList ラッパー
    m_commandList = std::make_unique<CommandList>();

    // ImGui (将来のデバッグオーバーレイ用に保持: 現在は空フレーム)
    m_imguiManager = std::make_unique<ImGuiManager>();
    m_imguiManager->Initialize(
        m_window->GetHwnd(), *m_graphicsDevice, m_commandQueue->GetQueue(),
        *m_srvHeap, m_swapChain->GetFormat(), FrameResources::kFrameCount);

    // Physics Debug Renderer (デフォルト OFF だが、Lua/C++ から制御可能)
    m_physicsDebugRenderer = std::make_unique<PhysicsDebugRenderer>();
    m_physicsDebugRenderer->Initialize(*m_graphicsDevice,
        m_swapChain->GetFormat(), DXGI_FORMAT_D32_FLOAT, SHADER_DIR);

    m_isRunning = true;
    Logger::Info("Application initialized successfully");
}

void Application::Run()
{
    Logger::Info("Application running...");
    timeBeginPeriod(1);

    while (!m_window->ShouldClose())
    {
        m_frameStart = std::chrono::high_resolution_clock::now();

        m_inputSystem->Update();
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

                m_camera->SetPerspective(kDefaultFovYRad,
                    static_cast<f32>(w) / static_cast<f32>(h), kDefaultNearZ, kDefaultFarZ);

                Logger::Info("Resized to {}x{}", w, h);
            }
        }

        m_gameClock.Tick();

        // Lua ホットリロード (game.lua の変更を 0.5 秒ごとにチェック)
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
                    Logger::Info("Hot-reload complete");
                }
            }
        }

        try
        {
            Update();
            Render();
        }
        catch (const std::exception& ex)
        {
            Logger::Error("Frame error: {}", ex.what());
            m_commandQueue->WaitIdle();
        }

        // フレームレートリミッター
        if (!m_useVsync)
        {
            using namespace std::chrono;
            auto targetDuration = duration_cast<high_resolution_clock::duration>(
                duration<f64>(1.0 / static_cast<f64>(kTargetFps)));
            auto elapsed = high_resolution_clock::now() - m_frameStart;
            auto remaining = targetDuration - elapsed;

            if (remaining > milliseconds(1))
            {
                std::this_thread::sleep_for(remaining - milliseconds(1));
            }
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

    if (m_commandQueue)
        m_commandQueue->WaitIdle();

    if (m_scriptEngine)
        m_scriptEngine->OnPlayStop();

    if (m_imguiManager)
    {
        m_imguiManager->Shutdown();
        m_imguiManager.reset();
    }

    m_physicsDebugRenderer.reset();
    if (m_physicsSystem)
    {
        m_physicsSystem->Shutdown();
        m_physicsSystem.reset();
    }
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

    // 自由視点カメラ操作 (右クリック中のみ): debug fly / fallback view
    {
        bool rightMouseHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool isForeground   = (GetForegroundWindow() == m_window->GetHwnd());

        if (m_framesSinceStart > 5 && rightMouseHeld && !m_inputSystem->IsMouseCaptured() && isForeground)
        {
            m_inputSystem->SetMouseCapture(true);
        }
        else if (!rightMouseHeld && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
        }
        if (!isForeground && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
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

    // Lua ロジック
    m_scriptEngine->CallOnUpdate(dt);
    m_scriptEngine->UpdateAttachedScripts(dt);

    // シーン更新 (Animator など)
    m_scene->Update(dt);

    // 物理更新
    if (m_physicsSystem->IsInitialized())
    {
        m_physicsSystem->Update(dt, m_scene->GetRegistry());
    }
}

void Application::Render()
{
    using namespace DirectX;

    auto* nativeCmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_commandList->Wrap(nativeCmdList);

    u32 frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    f32 totalTime = m_gameClock.GetTotalTime();

    // === アクティブカメラ算出: free cam をデフォルトに、CameraComponent(isActive=true) があれば上書き ===
    XMMATRIX viewMat   = m_camera->GetViewMatrix();
    XMMATRIX projMat   = m_camera->GetProjectionMatrix();
    XMFLOAT3 cameraPos = m_camera->GetPosition();
    {
        auto& reg = m_scene->GetRegistry();
        auto camView = reg.view<const CameraComponent, const Transform>();
        for (auto [e, cam, tf] : camView.each())
        {
            if (!cam.isActive) continue;

            f32 yawRad   = XMConvertToRadians(tf.rotation.y);
            f32 pitchRad = XMConvertToRadians(tf.rotation.x);
            XMVECTOR forward = XMVectorSet(
                std::sin(yawRad) * std::cos(pitchRad),
                std::sin(pitchRad),
                std::cos(yawRad) * std::cos(pitchRad), 0.0f);
            XMVECTOR up    = XMVectorSet(0, 1, 0, 0);
            XMVECTOR pos   = XMLoadFloat3(&tf.position);
            viewMat = XMMatrixLookToLH(pos, forward, up);

            f32 safeNear = (cam.nearClip > 0.0001f) ? cam.nearClip : 0.0001f;
            f32 safeFar  = (cam.farClip  > safeNear + 0.01f) ? cam.farClip : safeNear + 0.01f;
            f32 aspect   = static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight());
            projMat = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(cam.fovDegrees), aspect, safeNear, safeFar);

            cameraPos = tf.position;
            break;
        }
    }

    // === ライティング情報 (DirectionalLight から) ===
    XMFLOAT3 lightDirF3 = {-0.3f, -1.0f, -0.5f};
    XMFLOAT3 lightColorF3 = {1.0f, 0.95f, 0.9f};
    float    lightAmbient = 0.25f;
    {
        auto& reg = m_scene->GetRegistry();
        auto dlView = reg.view<const dx12e::DirectionalLight>();
        if (!dlView.empty())
        {
            auto first = *dlView.begin();
            const auto& dl = dlView.get<const dx12e::DirectionalLight>(first);
            lightDirF3 = dl.direction;
            lightColorF3 = {dl.color.x * dl.intensity,
                            dl.color.y * dl.intensity,
                            dl.color.z * dl.intensity};
        }
    }
    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirF3));
    XMStoreFloat3(&lightDirF3, lightDir);
    XMVECTOR lightPos = XMVectorScale(lightDir, -30.0f);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    XMMATRIX lightProj = XMMatrixOrthographicLH(30.0f, 30.0f, 0.1f, 60.0f);
    XMMATRIX lightViewProj = lightView * lightProj;
    XMMATRIX viewProj      = viewMat * projMat;

    // SRV ヒープをバインド
    m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
    m_commandList->SetRootSignature(*m_rootSignature);

    // === シャドウパス ===
    {
        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);

        m_commandList->ClearDepthStencil(m_shadowDsvHandle);
        nativeCmdList->OMSetRenderTargets(0, nullptr, FALSE, &m_shadowDsvHandle);

        D3D12_VIEWPORT shadowVp{};
        shadowVp.Width    = static_cast<f32>(kShadowMapSize);
        shadowVp.Height   = static_cast<f32>(kShadowMapSize);
        shadowVp.MinDepth = 0.0f;
        shadowVp.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor = {0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
        nativeCmdList->RSSetViewports(1, &shadowVp);
        nativeCmdList->RSSetScissorRects(1, &shadowScissor);

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

        m_commandList->TransitionResource(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // === バックバッファ準備 ===
    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    auto rtv = m_swapChain->GetCurrentRTV();

    m_commandList->TransitionResource(backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constexpr float clearColor[4] = {0.07f, 0.07f, 0.10f, 1.0f};
    m_commandList->ClearRenderTarget(rtv, clearColor);
    m_commandList->ClearDepthStencil(m_dsvHandle);
    m_commandList->SetRenderTarget(rtv, m_dsvHandle);
    m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());

    // === メインパス: PerFrame CB 更新 → 全 Entity 描画 ===
    {
        FrameConstants fc{};
        XMStoreFloat4x4(&fc.view, XMMatrixTranspose(viewMat));
        XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(projMat));
        fc.lightDir        = lightDirF3;
        fc.time            = totalTime;
        fc.lightColor      = lightColorF3;
        fc.ambientStrength = lightAmbient;
        XMStoreFloat4x4(&fc.lightViewProj, XMMatrixTranspose(lightViewProj));
        fc.cameraPos = cameraPos;

        fc.numPointLights = 0;
        {
            auto& reg = m_scene->GetRegistry();
            auto plView = reg.view<const dx12e::PointLight, const Transform>();
            for (auto [e, pl, tf] : plView.each())
            {
                if (fc.numPointLights >= kMaxPointLightsCB) break;
                auto& pld = fc.pointLights[fc.numPointLights];
                pld.position = tf.position;
                pld.range    = pl.range;
                pld.color    = {pl.color.x * pl.intensity,
                                pl.color.y * pl.intensity,
                                pl.color.z * pl.intensity};
                pld._pad     = 0.0f;
                fc.numPointLights++;
            }
        }

        m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);
        m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

        m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
            m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

        auto& reg = m_scene->GetRegistry();
        auto renderView = reg.view<const Transform, const MeshRenderer>();
        for (auto [e, transform, renderer] : renderView.each())
        {
            XMMATRIX world = transform.GetWorldMatrix();
            bool isGrid    = reg.all_of<GridPlane>(e);
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
                if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
                {
                    m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                        m_srvHeap->GetGpuHandle(mat->srvBlockIndex));
                }
                else
                {
                    Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture
                                                                : m_resourceManager->GetDefaultWhiteTexture();
                    m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                        m_srvHeap->GetGpuHandle(tex->GetSrvIndex()));
                }

                struct { float metallic; float roughness; u32 flags; float pad; } pbrParams;
                pbrParams.metallic  = (renderer.overrideMetallic  >= 0.0f) ? renderer.overrideMetallic
                                    : (mat ? mat->defaultMetallic : 0.0f);
                pbrParams.roughness = (renderer.overrideRoughness >= 0.0f) ? renderer.overrideRoughness
                                    : (mat ? mat->defaultRoughness : 0.5f);
                pbrParams.flags     = 0;
                if (mat && mat->normalMapTexture) pbrParams.flags |= 1u;
                bool hasOverride = (renderer.overrideMetallic >= 0.0f || renderer.overrideRoughness >= 0.0f);
                if (!hasOverride && mat && mat->metalRoughnessTexture) pbrParams.flags |= 2u;
                pbrParams.pad = 0;
                nativeCmdList->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 4, &pbrParams, 0);

                m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
                m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
                m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
            }
        }
    }

    // Physics Debug Draw (有効時のみ)
    if (m_physicsDebugRenderer->IsEnabled())
    {
        m_physicsDebugRenderer->BeginFrame();
        m_physicsDebugRenderer->CollectFromRegistry(m_scene->GetRegistry());

        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixTranspose(viewProj));
        m_physicsDebugRenderer->Render(nativeCmdList, vp);
    }

    // ImGui フレーム (将来のデバッグオーバーレイ用フック: 現状は空)
    m_imguiManager->BeginFrame();
    m_imguiManager->EndFrame(nativeCmdList);

    m_commandList->TransitionResource(backBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(m_useVsync);
    m_frameResources->EndFrame(*m_commandQueue);

    // GPU がコピーを完了してからアップロードバッファを解放
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();
}

void Application::RebuildScene()
{
    // game.lua ホットリロード: シーンクリア + ScriptEngine 再初期化 + OnStart 再呼び出し
    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_scene->Clear();

    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                        m_srvHeap.get(), cmdList);

    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), std::string(ASSETS_DIR));

    std::string scriptPath = std::string(SCRIPTS_DIR) + "game.lua";
    if (std::filesystem::exists(scriptPath))
    {
        m_scriptEngine->LoadScript(scriptPath);
        m_scriptEngine->CallOnStart();
        m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
    }

    // デフォルトシーン (game.lua が何もしなかった場合)
    if (m_scene->GetEntityCount() == 0)
    {
        m_scene->SpawnPlane("Grid", {0, 0, 0}, 50.0f, true);
        auto& reg = m_scene->GetRegistry();
        auto lightE = reg.create();
        reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
        reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
        reg.emplace<DirectionalLight>(lightE);
    }

    m_scriptEngine->OnPlayStart();

    {
        auto& reg = m_scene->GetRegistry();
        auto rbView = reg.view<RigidBody>();
        for (auto [entity, rb] : rbView.each())
        {
            if (rb.bodyId == kInvalidBodyId)
                m_physicsSystem->RegisterBody(reg, entity);
        }
    }

    ThrowIfFailed(cmdList->Close());
    m_commandQueue->ExecuteCommandList(cmdList);
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();
    m_frameResources->EndFrame(*m_commandQueue);
}

} // namespace dx12e
