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
#include "gui/ImGuiManager.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <thread>
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

    // Input System
    m_inputSystem = std::make_unique<InputSystem>();
    m_inputSystem->Initialize(m_window->GetHwnd());
    m_window->SetInputSystem(m_inputSystem.get());

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
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);
    m_camera->LookAt({0.0f, 2.0f, -5.0f}, {0.0f, 0.0f, 0.0f});

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

        // Entity配置
        std::string humanModel = std::string(ASSETS_DIR) + "models/human/walk.gltf";
        m_scene->Spawn("human1", humanModel, {0.0f, -1.0f, 0.0f}, {90.0f, 0.0f, 0.0f}, {0.02f, 0.02f, 0.02f});
        m_scene->Spawn("human2", humanModel, {3.0f, -1.0f, 0.0f}, {90.0f, 0.0f, 0.0f}, {0.02f, 0.02f, 0.02f});
        m_scene->Spawn("human3", humanModel, {-3.0f, -1.0f, 0.0f}, {90.0f, 180.0f, 0.0f}, {0.02f, 0.02f, 0.02f});

        // グリッド床
        m_scene->SpawnPlane("grid_floor", {0.0f, -1.0f, 0.0f}, 50.0f, true);

        // プリミティブオブジェクト
        m_scene->SpawnBox("box1", {6.0f, -0.5f, 0.0f});
        m_scene->SpawnSphere("sphere1", {-6.0f, -0.5f, 0.0f});

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

    // PerFrame Constant Buffer
    struct FrameConstants {
        DirectX::XMFLOAT4X4 view;
        DirectX::XMFLOAT4X4 proj;
        DirectX::XMFLOAT3   lightDir;
        float                time;
        DirectX::XMFLOAT3   lightColor;
        float                ambientStrength;
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

                // カメラアスペクト比更新
                m_camera->SetPerspective(DirectX::XM_PIDIV4,
                    static_cast<f32>(w) / static_cast<f32>(h), 0.1f, 1000.0f);

                Logger::Info("Resized to {}x{}", w, h);
            }
        }

        m_gameClock.Tick();
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

    // Escape でマウスキャプチャ解除、右クリックでキャプチャ開始
    if (m_inputSystem->IsKeyPressed(VK_TAB))
    {
        m_inputSystem->SetMouseCapture(false);
    }
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)
    {
        if (!m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(true);
        }
    }

    // カメラ操作（マウスキャプチャ中のみ）
    if (m_inputSystem->IsMouseCaptured())
    {
        // マウス回転
        f32 sensitivity = m_camera->GetMouseSensitivity();
        m_camera->Rotate(
            m_inputSystem->GetMouseDeltaX() * sensitivity,
            -m_inputSystem->GetMouseDeltaY() * sensitivity);

        // WASD移動
        f32 speed = m_camera->GetMoveSpeed() * dt;
        if (m_inputSystem->IsKeyDown('W')) m_camera->MoveForward(speed);
        if (m_inputSystem->IsKeyDown('S')) m_camera->MoveForward(-speed);
        if (m_inputSystem->IsKeyDown('D')) m_camera->MoveRight(speed);
        if (m_inputSystem->IsKeyDown('A')) m_camera->MoveRight(-speed);
        if (m_inputSystem->IsKeyDown(VK_SPACE)) m_camera->MoveUp(speed);
        if (m_inputSystem->IsKeyDown(VK_SHIFT)) m_camera->MoveUp(-speed);
    }

    // シーン更新（全EntityのAnimator）
    m_scene->Update(dt);
}

void Application::Render()
{
    using namespace DirectX;

    auto* nativeCmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_commandList->Wrap(nativeCmdList);

    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    auto rtv = m_swapChain->GetCurrentRTV();

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constexpr float clearColor[4] = {0.392f, 0.584f, 0.929f, 1.0f};
    m_commandList->ClearRenderTarget(rtv, clearColor);
    m_commandList->ClearDepthStencil(m_dsvHandle);
    m_commandList->SetRenderTarget(rtv, m_dsvHandle);
    m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());

    m_commandList->SetRootSignature(*m_rootSignature);
    m_commandList->SetPipelineState(*m_pipelineState);

    // SRV ヒープをバインド
    m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

    // PerFrame CB
    f32 totalTime = m_gameClock.GetTotalTime();
    u32 frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    struct FrameConstants {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT3   lightDir;
        float      time;
        XMFLOAT3   lightColor;
        float      ambientStrength;
    };

    FrameConstants fc{};
    XMStoreFloat4x4(&fc.view, XMMatrixTranspose(m_camera->GetViewMatrix()));
    XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(m_camera->GetProjectionMatrix()));
    fc.lightDir = { 0.3f, -1.0f, 0.5f };  // 斜め上からのライト
    fc.time = totalTime;
    fc.lightColor = { 1.0f, 0.95f, 0.9f };  // 暖色系の白
    fc.ambientStrength = 0.15f;

    m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);
    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

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
            entity.skinningBuffer->Update(entity.animator->GetSkinningMatrices(), frameIndex);
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
            // MVP + Model (各16 DWORD = 合計 32 DWORD)
            struct PerObjectData {
                XMMATRIX mvp;
                XMMATRIX mdl;
            } objData;
            objData.mvp = XMMatrixTranspose(world * viewProj);
            objData.mdl = XMMatrixTranspose(world);
            m_commandList->SetPerObjectConstants(RootSignature::kSlotPerObject, 32, &objData);

            // SRV テーブルをセット（テクスチャが無ければデフォルト白）
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

    ImGui::Begin("Animation Controller");
    ImGui::Text("FPS: %.1f (target: %.0f)", m_gameClock.GetFPS(), kTargetFps);
    ImGui::Checkbox("VSync", &m_useVsync);
    ImGui::Separator();

    // Scene情報
    ImGui::Text("Entities: %zu", m_scene->GetEntityCount());
    ImGui::Separator();

    // 各EntityのAnimator操作
    for (const auto& entity : m_scene->GetEntities())
    {
        if (!entity->animator || entity->animClips.empty()) continue;

        if (ImGui::TreeNode(entity->name.c_str()))
        {
            auto& pos = entity->transform.position;
            ImGui::Text("Pos: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);

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

    ImGui::Separator();
    ImGui::Text("Camera");
    auto camPos = m_camera->GetPosition();
    ImGui::Text("  Pos: %.1f, %.1f, %.1f", camPos.x, camPos.y, camPos.z);
    ImGui::Text("  %s", m_inputSystem->IsMouseCaptured() ? "Right-click captured (TAB to release)" : "Right-click to look around");

    f32 moveSpeed = m_camera->GetMoveSpeed();
    if (ImGui::SliderFloat("Move Speed", &moveSpeed, 1.0f, 50.0f))
    {
        m_camera->SetMoveSpeed(moveSpeed);
    }
    ImGui::End();

    m_imguiManager->EndFrame(nativeCmdList);

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(m_useVsync);
    m_frameResources->EndFrame(*m_commandQueue);
}

} // namespace dx12e
