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

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>

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

    // モデル読み込み（Box の代わりにモデルファイルがあればロード）
    {
        // 暫定コマンドリストで GPU アップロード
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // ResourceManager 初期化（デフォルト白テクスチャ作成にcmdListが必要）
        m_resourceManager = std::make_unique<ResourceManager>();
        m_resourceManager->Initialize(m_graphicsDevice.get(), m_srvHeap.get(), cmdList);

        std::filesystem::path modelPath = std::string(ASSETS_DIR) + "models/human/walk.gltf";
        if (std::filesystem::exists(modelPath))
        {
            auto modelData = ModelLoader::LoadFromFile(
                *m_graphicsDevice, cmdList, modelPath, *m_resourceManager);

            for (u32 i = 0; i < modelData.meshes.size(); ++i)
            {
                if (i < modelData.materials.size() && modelData.materials[i])
                {
                    modelData.meshes[i]->SetMaterial(modelData.materials[i].get());
                    m_modelMaterials.push_back(std::move(modelData.materials[i]));
                }
                m_modelMeshes.push_back(std::move(modelData.meshes[i]));
            }

            // スケルトン・アニメーション取得
            m_skeleton = std::move(modelData.skeleton);
            m_animClip = std::move(modelData.animClip);
        }
        else
        {
            // フォールバック: Boxメッシュ
            Logger::Warn("Model not found, using fallback box");
            auto box = std::make_unique<Mesh>();
            box->InitializeAsBox(*m_graphicsDevice);
            m_modelMeshes.push_back(std::move(box));
        }

        // コマンド実行 + GPU待ち
        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();

        // アップロードバッファ解放
        for (auto& mesh : m_modelMeshes)
            mesh->FinishUpload();
        m_resourceManager->FinishUploads();

        // スキニング PSO 作成（スケルトンがある場合）
        if (m_skeleton)
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

            // Animator
            m_animator = std::make_unique<Animator>();
            m_animator->Initialize(m_skeleton.get(), m_animClip.get());
            m_animator->SetLooping(true);

            // SkinningBuffer
            m_skinningBuffer = std::make_unique<SkinningBuffer>();
            m_skinningBuffer->Initialize(*m_graphicsDevice, *m_srvHeap,
                                         Skeleton::kMaxBones, FrameResources::kFrameCount);

            Logger::Info("Skeletal animation initialized ({} bones)", m_skeleton->GetBoneCount());
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
    m_skinnedPipelineState.reset();
    m_skinningBuffer.reset();
    m_animator.reset();
    m_animClip.reset();
    m_skeleton.reset();
    m_commandList.reset();
    m_perFrameCB.reset();
    m_modelMaterials.clear();
    m_modelMeshes.clear();
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
    if (m_animator)
    {
        m_animator->Update(m_gameClock.GetDeltaTime());
    }
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

    // ボーン行列 GPU 転送
    if (m_animator && m_skinningBuffer)
    {
        m_skinningBuffer->Update(m_animator->GetSkinningMatrices(), frameIndex);
    }

    // 各メッシュを描画
    // スケール縮小 + X軸-90度回転(モデル起こす) + Y軸回転(ターンテーブル)
    XMMATRIX model = XMMatrixScaling(0.02f, 0.02f, 0.02f)
                   * XMMatrixRotationX(XM_PIDIV2)
                   * XMMatrixTranslation(0.0f, -1.0f, 0.0f)
                   * XMMatrixRotationY(totalTime);
    XMMATRIX viewProj = m_camera->GetViewProjMatrix();

    for (const auto& mesh : m_modelMeshes)
    {
        // スケルトンがあればスキニングPSOを使用
        if (m_skeleton && m_skinnedPipelineState)
        {
            m_commandList->SetPipelineState(*m_skinnedPipelineState);
            m_commandList->SetSRVTable(RootSignature::kSlotBonesSRV,
                m_srvHeap->GetGpuHandle(m_skinningBuffer->GetSrvIndex(frameIndex)));
        }
        else
        {
            m_commandList->SetPipelineState(*m_pipelineState);
        }

        // MVP + Model (各16 DWORD = 合計 32 DWORD)
        struct PerObjectData {
            XMMATRIX mvp;
            XMMATRIX mdl;
        } objData;
        objData.mvp = XMMatrixTranspose(model * viewProj);
        objData.mdl = XMMatrixTranspose(model);
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

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(true);
    m_frameResources->EndFrame(*m_commandQueue);
}

} // namespace dx12e
