#include "Application.h"
#include "Logger.h"
#include "Assert.h"
#include "PathResolver.h"

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
#include "graphics/RenderTarget.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "renderer/Camera.h"
#include "renderer/PostProcess.h"
#include "renderer/ParticleSystem.h"
#include "renderer/SpriteRenderer.h"
#include "renderer/SceneTransition.h"
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
#include "scene/SceneSerializer.h"
#include "scene/SceneFlow.h"
#include "editor/EditorContext.h"
#include "editor/EditorLayer.h"
#include "editor/EditorIconRenderer.h"
#include "editor/UndoSystem.h"
#include "editor/ModelThumbnailRenderer.h"
#include "project/Project.h"
#include "project/ProjectManager.h"
#include "project/GitIntegration.h"
#include <commdlg.h>

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
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <immintrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace dx12e
{

// オフスクリーンのシーンカラーは HDR(FP16) で描く。発光が 1.0 を超えられるので
// ブルームとトーンマップで「白熱して光る」パーティクル/エフェクトが出せる。
// scene RT / cameraPreview RT、およびそこへ描く 3D PSO 群はこの形式で揃える。
// （バックバッファ＝スワップチェインは従来どおりスワップ形式のまま）
static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// フルパスを assets ディレクトリ相対へ（シーンフロー / loadScene 用）
static std::string ToAssetRel(const std::string& full)
{
    auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
    std::string f = norm(full);
    std::string base = norm(PathResolver::AssetsDir());
    if (!base.empty() && f.rfind(base, 0) == 0)
        return f.substr(base.size());
    return f;
}

Application::Application() = default;

Application::~Application()
{
    if (m_isRunning)
    {
        Shutdown();
    }
}

void Application::Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode,
                             const ProjectInfo* /*projectInfo*/)
{
    // ロガー初期化
    Logger::Init();
    m_isGameMode = gameMode;
    m_showLauncher = !gameMode;  // ゲームモードではランチャーを表示しない
    Logger::Info("Application initializing... (mode: {})", gameMode ? "game" : "editor");

    // エディタコンテキスト初期化
    m_editorCtx = std::make_unique<EditorContext>();

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
    m_audioSystem->Initialize(PathResolver::AssetsDir());

    // Physics System
    m_physicsSystem = std::make_unique<PhysicsSystem>();
    m_physicsSystem->Initialize();

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
        auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Forward_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Forward_PS.cso");

        PipelineStateBuilder builder;
        builder.SetRootSignature(m_rootSignature->Get())
               .SetVertexShader(vs.GetData(), vs.GetSize())
               .SetPixelShader(ps.GetData(), ps.GetSize())
               .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
               .SetRenderTargetFormat(m_swapChain->GetFormat())
               .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
               .SetDepthEnabled(true)
               .SetCullMode(D3D12_CULL_MODE_NONE);  // 両面描画（片面メッシュ対応）

        m_pipelineState = std::make_unique<PipelineState>();
        m_pipelineState->Initialize(*m_graphicsDevice, builder);
    }

    // Camera
    m_camera = std::make_unique<Camera>();
    {
        f32 viewW = static_cast<f32>(m_window->GetWidth());
        f32 viewH = static_cast<f32>(m_window->GetHeight());
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

        // エディタUIアイコンを読み込み（エンジン側assets基準。プロジェクト切替前に1度）
        if (!m_isGameMode)
            LoadEditorIcons(cmdList);

        // Scene 初期化
        m_scene = std::make_unique<Scene>();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        // ScriptEngine 初期化 + ゲームスクリプト実行
        m_scriptEngine = std::make_unique<ScriptEngine>();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();

        // ゲームスクリプト読み込み
        {
            std::string scriptPath = PathResolver::ScriptsDir() + "game.lua";
            if (std::filesystem::exists(scriptPath))
            {
                m_scriptEngine->LoadScript(scriptPath);
            }
            else
            {
                Logger::Warn("Game script not found: {}", scriptPath);
            }
        }

        // 初期シーン: (配布) game.json の startScene → (エディタ) 最後に開いたシーン → default.json → クリーン状態
        {
            bool loaded = false;

            // 配布モード: exe 隣の game.json で開始シーンを指定（最優先）
            if (m_isGameMode)
            {
                ProjectInfo gi;
                if (Project::Load(PathResolver::BaseDir() + "game.json", gi) && !gi.defaultScene.empty())
                {
                    std::string startScene = PathResolver::AssetsDir() + gi.defaultScene;
                    if (std::filesystem::exists(startScene))
                    {
                        loaded = SceneSerializer::Load(*m_scene, startScene, PathResolver::AssetsDir());
                        if (loaded)
                        {
                            m_editorCtx->currentScenePath = startScene;
                            m_currentSceneRel = gi.defaultScene;
                            Logger::Info("Loaded start scene from game.json: {}", gi.defaultScene);
                        }
                    }
                }
            }

            std::string lastScene = ProjectManager::LoadLastOpenedScene();
            std::string defaultScene = PathResolver::AssetsDir() + "scenes/default.json";

            if (!loaded && !m_isGameMode && !lastScene.empty() && std::filesystem::exists(lastScene))
            {
                loaded = SceneSerializer::Load(*m_scene, lastScene, PathResolver::AssetsDir());
                if (loaded)
                    m_editorCtx->currentScenePath = lastScene;
            }
            if (!loaded && std::filesystem::exists(defaultScene))
            {
                loaded = SceneSerializer::Load(*m_scene, defaultScene, PathResolver::AssetsDir());
                if (loaded)
                    m_editorCtx->currentScenePath = defaultScene;
            }
            if (!loaded)
            {
                // クリーン初期状態: Grid + DirectionalLight + MainCamera（再生に必要な最低限）
                m_scene->SpawnPlane("Grid", {0, 0, 0}, 50.0f, true);
                auto& reg = m_scene->GetRegistry();
                auto lightE = reg.create();
                reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
                reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
                reg.emplace<DirectionalLight>(lightE);

                auto camE = reg.create();
                reg.emplace<NameTag>(camE, NameTag{"MainCamera"});
                reg.emplace<Transform>(camE, Transform{{0.0f, 6.0f, -12.0f}, {22.0f, 0.0f, 0.0f}, {1,1,1}});
                CameraComponent cam;
                cam.isActive = true;
                reg.emplace<CameraComponent>(camE, cam);
            }

            // シーンフロー / loadScene 用に現在シーンの相対パスを記録
            if (m_currentSceneRel.empty() && !m_editorCtx->currentScenePath.empty())
                m_currentSceneRel = ToAssetRel(m_editorCtx->currentScenePath);
        }

        // ホットリロード用タイムスタンプ初期化（初回の誤発火を防止）
        {
            std::string scriptPath = PathResolver::ScriptsDir() + "game.lua";
            if (std::filesystem::exists(scriptPath))
                m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);
        }

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
            auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardSkinned_VS.cso");
            auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Forward_PS.cso");

            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetPixelShader(ps.GetData(), ps.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(kSceneColorFormat)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE);

            m_skinnedPipelineState = std::make_unique<PipelineState>();
            m_skinnedPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // グリッド PSO 作成（アルファブレンド + 両面描画）
        {
            auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardGrid_VS.cso");
            auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ForwardGrid_PS.cso");

            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetPixelShader(ps.GetData(), ps.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(kSceneColorFormat)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetDepthWrite(false)        // 深度を書かない＝床など同一平面の不透明物を隠さない
                   .SetAlphaBlendEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE)
                   .SetDepthBias(-100, -1.0f);  // 深度テストではカメラ側に寄せて床の上に線を乗せる

            m_gridPipelineState = std::make_unique<PipelineState>();
            m_gridPipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // 加算発光 PSO（パーティクル用）：ライティング無視・加算合成・深度書き込みOFF
        {
            auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Emissive_VS.cso");
            auto ps = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"Emissive_PS.cso");

            PipelineStateBuilder builder;
            builder.SetRootSignature(m_rootSignature->Get())
                   .SetVertexShader(vs.GetData(), vs.GetSize())
                   .SetPixelShader(ps.GetData(), ps.GetSize())
                   .SetInputLayout(Mesh::GetInputLayout(), Mesh::GetInputLayoutCount())
                   .SetRenderTargetFormat(kSceneColorFormat)
                   .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
                   .SetDepthEnabled(true)
                   .SetDepthWrite(false)
                   .SetAdditiveBlendEnabled(true)
                   .SetCullMode(D3D12_CULL_MODE_NONE);

            m_emissivePipelineState = std::make_unique<PipelineState>();
            m_emissivePipelineState->Initialize(*m_graphicsDevice, builder);
        }

        // sneakWalk アニメーションを全スケルタルEntityに追加
        {
            std::filesystem::path sneakPath = PathResolver::AssetsDir() + "models/human/sneakWalk.gltf";
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
            auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPass_VS.cso");
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
            auto vs = ShaderCompiler::LoadFromFile(PathResolver::ShaderDirW() + L"ShadowPassSkinned_VS.cso");
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

    // PerFrame Constant Buffer（PointLight / SpotLight 各最大8灯対応）
    // レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
    static constexpr u32 kMaxPointLights = 8;
    static constexpr u32 kMaxSpotLights  = 8;
    struct PointLightGPU {
        DirectX::XMFLOAT3 position;
        float range;
        DirectX::XMFLOAT3 color;  // color * intensity
        float _pad;
    };
    struct SpotLightGPU {
        DirectX::XMFLOAT3 position;   float range;
        DirectX::XMFLOAT3 direction;  float cosInner;
        DirectX::XMFLOAT3 color;      float cosOuter;  // color * intensity
    };
    struct FrameConstants {
        DirectX::XMFLOAT4X4 view;            // 64B
        DirectX::XMFLOAT4X4 proj;            // 64B
        DirectX::XMFLOAT3   lightDir;        // 12B
        float                time;            // 4B
        DirectX::XMFLOAT3   lightColor;      // 12B
        float                ambientStrength; // 4B
        DirectX::XMFLOAT4X4 lightViewProj;   // 64B
        DirectX::XMFLOAT3   cameraPos;       // 12B
        float                _pad;            // 4B
        u32                  numPointLights;  // 4B
        u32                  numSpotLights;   // 4B
        float                _pad2[2];        // 8B → 16B boundary
        PointLightGPU        pointLights[kMaxPointLights]; // 256B
        SpotLightGPU         spotLights[kMaxSpotLights];   // 384B
    };  // total = 896B
    static_assert(sizeof(FrameConstants) == 896, "FrameConstants must be 896 bytes");
    m_perFrameCB = std::make_unique<ConstantBuffer>();
    m_perFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // カメラプレビュー用の per-frame CB（メインパスと別バッファ。同一フレーム内で
    // 別視点を描くため m_perFrameCB を上書きできない）
    m_previewFrameCB = std::make_unique<ConstantBuffer>();
    m_previewFrameCB->Initialize(*m_graphicsDevice, sizeof(FrameConstants), FrameResources::kFrameCount);

    // CommandList ラッパー
    m_commandList = std::make_unique<CommandList>();

    // ImGui 初期化
    m_imguiManager = std::make_unique<ImGuiManager>();
    m_imguiManager->Initialize(
        m_window->GetHwnd(), *m_graphicsDevice, m_commandQueue->GetQueue(),
        *m_srvHeap, m_swapChain->GetFormat(), FrameResources::kFrameCount);

    // EditorLayer 初期化
    m_editorLayer = std::make_unique<EditorLayer>();
    m_editorLayer->Initialize(m_editorCtx.get(), PathResolver::AssetsDir(),
                              PathResolver::ScriptsDir(),
                              m_resourceManager.get(), m_srvHeap.get());

    // ModelThumbnailRenderer 初期化
    m_thumbRenderer = std::make_unique<ModelThumbnailRenderer>();
    m_thumbRenderer->Initialize(m_graphicsDevice.get(), m_srvHeap.get(),
                                m_resourceManager.get(), m_rootSignature.get(),
                                m_pipelineState.get());
    m_editorLayer->SetThumbnailRenderer(m_thumbRenderer.get());

    // Physics Debug Renderer
    m_physicsDebugRenderer = std::make_unique<PhysicsDebugRenderer>();
    m_physicsDebugRenderer->Initialize(*m_graphicsDevice,
        kSceneColorFormat, DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

    m_editorIconRenderer = std::make_unique<EditorIconRenderer>();
    m_editorIconRenderer->Initialize(*m_graphicsDevice,
        m_swapChain->GetFormat(), DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());

    // オフスクリーン描画用 RT + ポストプロセス（WP3）
    {
        m_offscreenRtvHeap = std::make_unique<DescriptorHeap>();
        m_offscreenRtvHeap->Initialize(*m_graphicsDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, false);

        // シーンはスワップチェインと同一フォーマットへ描く（既存 3D PSO をそのまま使える）
        const float sceneClear[4] = {0.392f, 0.584f, 0.929f, 1.0f};
        m_sceneRT = std::make_unique<RenderTarget>();
        m_sceneRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                              m_window->GetWidth(), m_window->GetHeight(),
                              kSceneColorFormat, sceneClear);

        // カメラプレビュー RT（固定 16:9・小サイズ。選択カメラ視点をここへ描いて小窓表示）
        m_cameraPreviewRT = std::make_unique<RenderTarget>();
        m_cameraPreviewRT->Initialize(*m_graphicsDevice, m_offscreenRtvHeap.get(), m_srvHeap.get(),
                                      480, 270, kSceneColorFormat, sceneClear);

        m_postProcess = std::make_unique<PostProcess>();
        m_postProcess->Initialize(*m_graphicsDevice, m_swapChain->GetFormat(), PathResolver::ShaderDirW());

        // 2D スプライト（バックバッファ＝スワップチェイン形式へ描く）
        m_spriteRenderer = std::make_unique<SpriteRenderer>();
        m_spriteRenderer->Initialize(*m_graphicsDevice, m_srvHeap.get(),
                                     m_swapChain->GetFormat(), PathResolver::ShaderDirW());

        // 加算ビルボードパーティクル（HDR scene RT + 深度へ描く / Lua fx API）
        m_particleSystem = std::make_unique<ParticleSystem>();
        m_particleSystem->Initialize(*m_graphicsDevice, kSceneColorFormat,
                                     DXGI_FORMAT_D32_FLOAT, PathResolver::ShaderDirW());
        if (m_scriptEngine) m_scriptEngine->SetParticleSystem(m_particleSystem.get());

        // シーントランジション
        m_sceneTransition = std::make_unique<SceneTransition>();
        m_sceneTransition->Initialize(*m_graphicsDevice, m_swapChain->GetFormat(), PathResolver::ShaderDirW());
    }

    // シーンフロー（assets/sceneflow.json があれば）
    m_sceneFlow = std::make_unique<SceneFlow>();
    m_sceneFlow->Load(PathResolver::AssetsDir() + "sceneflow.json");

    m_isRunning = true;

    // ゲームモードの場合、即座にPlayモードに入る
    if (m_isGameMode)
    {
        m_pendingMode = EngineMode::Playing;
        m_modeChangeRequested = true;
    }

    // 全モデルのサムネイルを起動時にロード/レンダリング
    {
        size_t uncachedCount = m_thumbRenderer->ScanAllModels(PathResolver::AssetsDir());
        size_t cachedCount   = m_thumbRenderer->GetCachedCount();
        size_t totalModels   = uncachedCount + cachedCount;

        if (totalModels > 0)
        {
            // Phase 1: ディスクキャッシュから一括ロード（高速）
            if (cachedCount > 0)
            {
                auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
                m_thumbRenderer->LoadCachedThumbnails(cmdList);
                ThrowIfFailed(cmdList->Close());
                m_commandQueue->ExecuteCommandList(cmdList);
                m_commandQueue->WaitIdle();
                m_frameResources->EndFrame(*m_commandQueue);
                Logger::Info("[Thumbnail] Cache loaded: {} models", cachedCount);
            }

            // Phase 2: 未キャッシュのみレンダリング（進捗表示付き）
            if (uncachedCount > 0)
            {
                size_t completed = 0;

                while (m_thumbRenderer->GetPendingCount() > 0)
                {
                    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
                    m_commandList->Wrap(cmdList);

                    m_thumbRenderer->RenderNext(cmdList);
                    ++completed;

                    // ローディング画面をバックバッファに描画
                    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
                    auto rtvHandle = m_descriptorHeap->GetCpuHandle(
                        m_swapChain->GetCurrentBackBufferIndex());

                    m_commandList->TransitionResource(backBuffer,
                        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

                    float clearColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
                    cmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
                    cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                    D3D12_VIEWPORT vp = {0, 0,
                        static_cast<f32>(m_window->GetWidth()),
                        static_cast<f32>(m_window->GetHeight()), 0, 1};
                    D3D12_RECT scissor = {0, 0,
                        static_cast<LONG>(m_window->GetWidth()),
                        static_cast<LONG>(m_window->GetHeight())};
                    cmdList->RSSetViewports(1, &vp);
                    cmdList->RSSetScissorRects(1, &scissor);

                    float progress = static_cast<float>(completed) / static_cast<float>(uncachedCount);
                    m_imguiManager->BeginFrame();
                    float dispW = static_cast<float>(m_window->GetWidth());
                    float dispH = static_cast<float>(m_window->GetHeight());
                    ImGui::SetNextWindowPos(ImVec2(dispW * 0.5f, dispH * 0.5f),
                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
                    ImGui::Begin("##Loading", nullptr,
                        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
                    ImGui::Text("DX12 Engine");
                    ImGui::Separator();
                    ImGui::Text("Rendering thumbnails... (%zu / %zu)", completed, uncachedCount);
                    ImGui::ProgressBar(progress, ImVec2(-1, 24));
                    ImGui::End();
                    m_imguiManager->EndFrame(cmdList);

                    m_commandList->TransitionResource(backBuffer,
                        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                    m_commandList->Close();
                    m_commandQueue->ExecuteCommandList(cmdList);
                    m_swapChain->Present(false);
                    m_frameResources->EndFrame(*m_commandQueue);

                    m_commandQueue->WaitIdle();

                    // レンダリング結果をディスクキャッシュに保存
                    m_thumbRenderer->SavePendingCache();

                    MSG msg;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
            }

            m_resourceManager->FinishUploads();
        }
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
            try
            {
                if (m_pendingMode == EngineMode::Playing)
                    EnterPlayMode();
                else
                    EnterEditorMode();
            }
            catch (const std::exception& ex)
            {
                Logger::Error("Mode change failed: {}", ex.what());
                if (m_engineMode == EngineMode::Playing)
                    m_scriptEngine->OnPlayStop();
                m_engineMode = EngineMode::Editor;
                m_inputSystem->SetMouseCapture(false);
            }
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

                // オフスクリーン RT もウィンドウサイズへ作り直す
                if (m_sceneRT)
                    m_sceneRT->Resize(*m_graphicsDevice, w, h);

                // カメラアスペクト比更新（エディタモードではサイドバー分引く）
                m_camera->SetPerspective(DirectX::XM_PIDIV4,
                    static_cast<f32>(w) / static_cast<f32>(h), 0.1f, 1000.0f);

                Logger::Info("Resized to {}x{}", w, h);
            }
        }

        m_gameClock.Tick();

        // シーントランジション更新（WP9）
        if (m_sceneTransition)
            m_sceneTransition->Update(m_gameClock.GetDeltaTime());

        // Luaホットリロード（0.5秒ごとにファイル変更チェック）
        m_scriptPollTimer += m_gameClock.GetDeltaTime();
        if (m_scriptPollTimer >= kScriptPollInterval)
        {
            m_scriptPollTimer = 0.0f;
            std::string scriptPath = PathResolver::ScriptsDir() + "game.lua";
            if (std::filesystem::exists(scriptPath))
            {
                auto currentTime = std::filesystem::last_write_time(scriptPath);
                if (currentTime != m_scriptLastWriteTime)
                {
                    Logger::Info("Hot-reload: game.lua changed, reloading...");
                    m_commandQueue->WaitIdle();
                    RebuildScene();
                    m_editorCtx->hotReloadFlash = 2.0f;
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
            // GPU 状態をリセットして次フレームで復帰を試みる
            m_commandQueue->WaitIdle();
            if (m_engineMode == EngineMode::Playing)
            {
                if (m_engineMode == EngineMode::Playing)
                    m_scriptEngine->OnPlayStop();
                m_engineMode = EngineMode::Editor;
                m_inputSystem->SetMouseCapture(false);
                Logger::Error("Forced return to Editor mode");
            }
        }

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

    // 非同期ロードスレッドの回収
    if (m_loadThread.joinable())
        m_loadThread.join();

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
    m_editorLayer.reset();
    m_editorCtx.reset();
    m_physicsDebugRenderer.reset();
    // 新規レンダラ群（GPU リソース）をデバイス解放より前に明示破棄
    m_editorIconRenderer.reset();
    m_sceneTransition.reset();
    m_spriteRenderer.reset();
    m_postProcess.reset();
    m_cameraPreviewRT.reset();
    m_sceneRT.reset();
    m_offscreenRtvHeap.reset();
    m_sceneFlow.reset();
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
    m_previewFrameCB.reset();
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

    // 非同期プロジェクトロードの状態機械を進める
    UpdateProjectLoad(dt);

    if (m_engineMode == EngineMode::Editor)
    {
        // エディタモード: C++カメラ操作
        bool rightMouseHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        // ウィンドウが非フォーカスならカメラ操作しない
        bool isForeground = (GetForegroundWindow() == m_window->GetHwnd());

        // カーソルが 3D ビューポート上にあるか（ImGui の PassthruCentralNode の
        // WantCaptureMouse 挙動に依存せず、中央ノード矩形で直接判定）。
        // ※ パネル上で右ドラッグしてもフライが暴発しないようにするためのゲート。
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool cursorInViewport = m_editorCtx->IsCursorInViewport(mousePos.x, mousePos.y);

        if (m_framesSinceStart > 5 && rightMouseHeld && !m_inputSystem->IsMouseCaptured()
            && isForeground && cursorInViewport)
        {
            m_inputSystem->SetMouseCapture(true);
        }
        else if (!rightMouseHeld && m_inputSystem->IsMouseCaptured())
        {
            m_inputSystem->SetMouseCapture(false);
        }
        // ウィンドウが裏に行ったら強制解除
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

        // --- タッチパッド向け: キーボードフライモード（マウス/ボタン長押し不要）---
        bool kbActive = !ImGui::GetIO().WantCaptureKeyboard;  // テキスト入力中は無効
        if (kbActive && (GetAsyncKeyState(VK_OEM_3) & 1))     // ` キーでトグル
            m_editorCtx->flyMode = !m_editorCtx->flyMode;
        if (m_editorCtx->flyMode && (GetAsyncKeyState(VK_ESCAPE) & 1))
            m_editorCtx->flyMode = false;

        if (m_editorCtx->flyMode && kbActive && !m_inputSystem->IsMouseCaptured())
        {
            f32 speed = m_camera->GetMoveSpeed() * dt;
            if (GetAsyncKeyState('W') & 0x8000) m_camera->MoveForward(speed);
            if (GetAsyncKeyState('S') & 0x8000) m_camera->MoveForward(-speed);
            if (GetAsyncKeyState('D') & 0x8000) m_camera->MoveRight(speed);
            if (GetAsyncKeyState('A') & 0x8000) m_camera->MoveRight(-speed);
            if (GetAsyncKeyState('E') & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState('Q') & 0x8000) m_camera->MoveUp(-speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) m_camera->MoveUp(speed);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m_camera->MoveUp(-speed);

            // 矢印キーで視点回転（マウス不要）
            f32 rot = 1.5f * dt;  // rad/sec
            f32 yawD = 0.0f, pitchD = 0.0f;
            if (GetAsyncKeyState(VK_LEFT)  & 0x8000) yawD   -= rot;
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) yawD   += rot;
            if (GetAsyncKeyState(VK_UP)    & 0x8000) pitchD += rot;
            if (GetAsyncKeyState(VK_DOWN)  & 0x8000) pitchD -= rot;
            if (yawD != 0.0f || pitchD != 0.0f) m_camera->Rotate(yawD, pitchD);
        }

        // Ctrl+S でクイック保存
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('S') & 1))
        {
            if (m_editorCtx->currentScenePath.empty())
            {
                // パス未設定 → 名前入力ダイアログを開く（保存モード）
                m_editorCtx->showNewSceneDialog = true;
                m_editorCtx->newSceneDialogIsCreate = false;
                std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
                strncpy_s(m_editorCtx->newSceneNameBuf, "Untitled", _TRUNCATE);
            }
            else
            {
                SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
                ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
                m_editorCtx->hotReloadFlash = 1.5f;
                m_editorLayer->RefreshAssetBrowser();
            }
        }

        // Ctrl+N で新規シーン名入力ダイアログを開く
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('N') & 1))
        {
            m_editorCtx->showNewSceneDialog = true;
            m_editorCtx->newSceneDialogIsCreate = true;
            std::memset(m_editorCtx->newSceneNameBuf, 0, sizeof(m_editorCtx->newSceneNameBuf));
            strncpy_s(m_editorCtx->newSceneNameBuf, "NewScene", _TRUNCATE);
        }

        // Undo/Redo (Ctrl+Z / Ctrl+Y) + Copy/Paste/Duplicate (Ctrl+C/V/D)
        // ImGui のテキスト入力にフォーカスがある時はエンティティ操作を抑制
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && !ImGui::GetIO().WantCaptureKeyboard)
        {
            if (GetAsyncKeyState('Z') & 1)
                m_editorCtx->pendingUndo = true;
            if (GetAsyncKeyState('Y') & 1)
                m_editorCtx->pendingRedo = true;

            // コピー (Ctrl+C) — 全コンポーネントを JSON スナップショットで保持
            if (GetAsyncKeyState('C') & 1)
            {
                m_editorCtx->clipboard.clear();
                auto& reg = m_scene->GetRegistry();
                for (auto e : m_editorCtx->selectedEntities)
                {
                    if (!reg.valid(e)) continue;
                    std::string snap = SceneSerializer::SerializeEntity(
                        *m_scene, e, PathResolver::AssetsDir());
                    if (!snap.empty())
                        m_editorCtx->clipboard.push_back(std::move(snap));
                }
            }

            // ペースト (Ctrl+V) — フレーム境界（cmdList 有効時）で生成
            if ((GetAsyncKeyState('V') & 1) && !m_editorCtx->clipboard.empty())
                m_editorCtx->pendingPastes = m_editorCtx->clipboard;

            // 複製 (Ctrl+D) — 全コンポーネントのディープコピー
            if ((GetAsyncKeyState('D') & 1) && m_editorCtx->HasSelection())
            {
                for (auto e : m_editorCtx->selectedEntities)
                    m_editorCtx->pendingDuplications.push_back(e);
            }
        }

        // ギズモモード切替（右クリック中・ImGuiフォーカス中は無効）
        if (!ImGui::GetIO().WantCaptureKeyboard && !m_inputSystem->IsMouseCaptured())
        {
            // フライモード中は W/E/R/T をカメラ移動に使うのでギズモ切替は抑制
            if (!m_editorCtx->flyMode)
            {
                if (GetAsyncKeyState('W') & 1) m_editorCtx->gizmoMode = GizmoMode::Translate;
                if (GetAsyncKeyState('E') & 1) m_editorCtx->gizmoMode = GizmoMode::Rotate;
                if (GetAsyncKeyState('R') & 1) m_editorCtx->gizmoMode = GizmoMode::Scale;
                if (GetAsyncKeyState('T') & 1) m_editorCtx->gizmoLocalSpace = !m_editorCtx->gizmoLocalSpace;
            }

            // F: 選択エンティティにフォーカス（Unity 風）
            if ((GetAsyncKeyState('F') & 1) && m_editorCtx->HasSelection())
            {
                auto& reg = m_scene->GetRegistry();
                auto sel = m_editorCtx->selectedEntity;
                if (reg.valid(sel) && reg.all_of<Transform>(sel))
                {
                    const auto& t = reg.get<Transform>(sel);

                    // 対象サイズからフォーカス距離を決める
                    f32 dist = 5.0f;
                    if (reg.all_of<MeshRenderer>(sel))
                    {
                        const auto& mr = reg.get<MeshRenderer>(sel);
                        f32 maxExtent = 0.0f;
                        for (const auto* mesh : mr.meshes)
                        {
                            if (!mesh) continue;
                            auto mn = mesh->GetAABBMin();
                            auto mx = mesh->GetAABBMax();
                            maxExtent = std::max({maxExtent,
                                (mx.x - mn.x) * t.scale.x,
                                (mx.y - mn.y) * t.scale.y,
                                (mx.z - mn.z) * t.scale.z});
                        }
                        if (maxExtent > 0.0f)
                            dist = std::clamp(maxExtent * 2.0f, 2.0f, 100.0f);
                    }

                    // 親階層込みのワールド位置にフォーカス
                    DirectX::XMFLOAT3 wpos = t.position;
                    if (t.parent != entt::null && reg.valid(t.parent))
                    {
                        DirectX::XMFLOAT4X4 wf;
                        XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, sel));
                        wpos = {wf._41, wf._42, wf._43};
                    }

                    auto fwd = m_camera->GetForward();
                    m_camera->SetPosition({wpos.x - fwd.x * dist,
                                           wpos.y - fwd.y * dist,
                                           wpos.z - fwd.z * dist});
                }
            }
        }

    }
    else
    {
        // プレイモード: Luaがカメラ+ゲームロジックを制御
        m_scriptEngine->SetScreenSize(static_cast<int>(m_window->GetWidth()),
                                      static_cast<int>(m_window->GetHeight()));
        m_scriptEngine->CallOnUpdate(dt);
        m_scriptEngine->UpdateAttachedScripts(dt);

        // アクティブカメラの Transform をグローバル Camera に同期
        auto& reg = m_scene->GetRegistry();
        auto camSyncView = reg.view<const CameraComponent, const Transform>();
        for (auto [e, cam, tf] : camSyncView.each())
        {
            if (!cam.isActive) continue;
            m_camera->SetPosition(tf.position);
            m_camera->SetYaw(DirectX::XMConvertToRadians(tf.rotation.y));
            // ピッチ反転: エディタのギズモ/フラスタム(Euler)と Camera(forward.y=sin(pitch)) は符号が逆
            m_camera->SetPitch(DirectX::XMConvertToRadians(-tf.rotation.x));
            break;
        }
    }

    // シーン更新（Animator等）— エディタモードは時間を止める（ボーン行列は維持）
    m_scene->Update(m_engineMode == EngineMode::Playing ? dt : 0.0f);

    // パーティクル更新（プレイモードのみ進める）
    if (m_particleSystem)
        m_particleSystem->Update(m_engineMode == EngineMode::Playing ? dt : 0.0f);

    // 物理更新（プレイモードのみ）
    if (m_engineMode == EngineMode::Playing && m_physicsSystem->IsInitialized())
    {
        m_physicsSystem->Update(dt, m_scene->GetRegistry());
    }

    // 3D 空間オーディオ: リスナー＝カメラ、AudioSource を駆動（Playing のみ）
    if (m_engineMode == EngineMode::Playing && m_audioSystem)
    {
        auto pos = m_camera->GetPosition();
        auto fwd = m_camera->GetForward();
        m_audioSystem->SetListener(pos.x, pos.y, pos.z, fwd.x, fwd.y, fwd.z, 0.0f, 1.0f, 0.0f);

        auto& reg = m_scene->GetRegistry();
        for (auto [e, src] : reg.view<AudioSource>().each())
        {
            DirectX::XMFLOAT4X4 wf;
            DirectX::XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, e));
            const float wx = wf._41, wy = wf._42, wz = wf._43;

            if (src.playOnStart && !src.startedThisPlay && !src.clipPath.empty())
            {
                if (src.spatial)
                    src.runtimeSlot = m_audioSystem->PlaySFXSpatial(
                        src.clipPath, wx, wy, wz, src.minDistance, src.maxDistance, src.volume, src.loop);
                else
                    m_audioSystem->PlaySFX(src.clipPath, src.loop);
                src.startedThisPlay = true;
            }
            if (src.runtimeSlot >= 0 && src.spatial)
                m_audioSystem->UpdateSpatialEmitter(src.runtimeSlot, wx, wy, wz);
        }
        m_audioSystem->Update();
    }
}

void Application::RebuildScene()
{
    m_editorCtx->ClearSelection();
    m_scene->Clear();
    auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                        m_srvHeap.get(), cmdList);

    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    std::string scriptPath = PathResolver::ScriptsDir() + "game.lua";
    if (std::filesystem::exists(scriptPath))
    {
        m_scriptEngine->LoadScript(scriptPath);
    }

    ThrowIfFailed(cmdList->Close());
    m_commandQueue->ExecuteCommandList(cmdList);
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();
    m_frameResources->EndFrame(*m_commandQueue);

    // ホットリロード用タイムスタンプ更新
    {
        std::string reloadPath = PathResolver::ScriptsDir() + "game.lua";
        if (std::filesystem::exists(reloadPath))
            m_scriptLastWriteTime = std::filesystem::last_write_time(reloadPath);
    }
}

void Application::LoadEditorIcons(ID3D12GraphicsCommandList* cmdList)
{
    auto load = [&](const char* name) -> u64
    {
        std::string p = PathResolver::AssetsDir() + "editor/icons/" + name + ".png";
        if (!std::filesystem::exists(p)) return 0;
        std::wstring wp(p.begin(), p.end());
        Texture* t = m_resourceManager->GetOrLoadTexture(wp, cmdList, true);
        if (!t) return 0;
        return m_srvHeap->GetGpuHandle(t->GetSrvIndex()).ptr;
    };
    m_icons.logo        = load("logo");
    m_icons.newProject  = load("new_project");
    m_icons.openProject = load("open_project");
    m_icons.recent      = load("recent");
    m_icons.save        = load("save");
    m_icons.git         = load("git");
    m_icons.github      = load("github");
    m_icons.commit      = load("commit");
    m_icons.push        = load("push");

    // ツールバー
    m_icons.file        = load("file");
    m_icons.play        = load("play");
    m_icons.stop        = load("stop");
    m_icons.build       = load("build");
    m_icons.gizmoMove   = load("gizmo_move");
    m_icons.gizmoRotate = load("gizmo_rotate");
    m_icons.gizmoScale  = load("gizmo_scale");
    m_icons.spaceWorld  = load("space_world");
    m_icons.spaceLocal  = load("space_local");

    // エンティティ / コンポーネント種別
    m_icons.entMesh     = load("ent_mesh");
    m_icons.entLight    = load("ent_light");
    m_icons.entCamera   = load("ent_camera");
    m_icons.entAudio    = load("ent_audio");
    m_icons.entScript   = load("ent_script");
    m_icons.entPhysics  = load("ent_physics");
    m_icons.entCollider = load("ent_collider");
    m_icons.entEmpty    = load("ent_empty");

    // 各パネルが EditorContext 経由で参照できるようにポインタを配る
    if (m_editorCtx)
        m_editorCtx->icons = &m_icons;

    Logger::Info("Editor UI icons loaded");
}

void Application::BeginProjectLoad(const ProjectInfo& info, bool isNew)
{
    namespace fs = std::filesystem;
    // 直前のスレッドが残っていれば回収
    if (m_loadThread.joinable())
        m_loadThread.join();

    m_loadInfo            = info;
    m_loadIsNew           = isNew;
    m_loading             = true;
    m_showLauncher        = false;
    m_loadProjectStarted  = false;
    m_loadSceneWaitFrames = 0;
    m_loadSpinTime        = 0.0f;
    m_loadThreadDone      = false;
    m_loadStatus          = isNew ? "プロジェクトを作成中..." : "プロジェクトを読み込み中...";

    if (isNew)
    {
        // ディスク作成（フォルダ生成・テンプレ書き出し）はワーカースレッドで
        m_loadThreadRunning = true;
        ProjectInfo copy = info;
        m_loadThread = std::thread([this, copy]()
        {
            Project::CreateDefaultStructure(copy);
            std::string projPath =
                (std::filesystem::path(copy.rootDir) / (copy.name + ".dx12proj")).string();
            Project::Save(copy, projPath);
            ProjectManager::AddToRecents(copy);
            m_loadThreadDone = true;
        });
    }
    else
    {
        // 既存プロジェクト: 重い CPU 処理は無い（シーンの GPU ロードは本スレッドで）
        m_loadThreadRunning = false;
        m_loadThreadDone    = true;
    }
}

void Application::UpdateProjectLoad(f32 dt)
{
    if (!m_loading) return;
    m_loadSpinTime += dt;

    // フェーズ1: 作成スレッドの完了待ち
    if (m_loadThreadRunning)
    {
        if (!m_loadThreadDone.load()) return;  // まだ作成中（スピナー回し続ける）
        if (m_loadThread.joinable()) m_loadThread.join();
        m_loadThreadRunning = false;
    }

    // フェーズ2: LoadProject を一度だけ発火（次フレームの Render で実シーンロード）
    if (!m_loadProjectStarted)
    {
        m_loadStatus = "シーンを読み込み中...";
        LoadProject(m_loadInfo);
        m_loadProjectStarted  = true;
        m_loadSceneWaitFrames = 2;  // pending* が Render で消化されるまで猶予
        return;
    }

    // フェーズ3: シーンロード（pending*）が消化されたら完了
    bool pendingScene = !m_editorCtx->pendingLoadPath.empty() || m_editorCtx->pendingNewScene;
    if (m_loadSceneWaitFrames > 0) --m_loadSceneWaitFrames;
    if (m_loadSceneWaitFrames == 0 && !pendingScene)
    {
        m_loading            = false;
        m_loadProjectStarted = false;
        m_editorCtx->buildCompleteFlash = 1.5f;
    }
}

void Application::RenderLoadingOverlay()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.09f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##LoadingOverlay", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    // 回転スピナー（円弧をぐるぐる）
    const float r = 34.0f;
    const int   segs = 28;
    float t = m_loadSpinTime * 3.2f;
    for (int i = 0; i < segs; ++i)
    {
        float a0 = t + (float)i / segs * 6.2831853f;
        float a1 = t + (float)(i + 1) / segs * 6.2831853f;
        float alpha = (float)i / segs;  // フェードする尾
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.39f, 0.58f, 0.93f, alpha));
        dl->AddLine(ImVec2(c.x + cosf(a0) * r, c.y + sinf(a0) * r),
                    ImVec2(c.x + cosf(a1) * r, c.y + sinf(a1) * r), col, 5.0f);
    }

    // ステータステキスト（中央寄せ）
    const char* msg = m_loadStatus.c_str();
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r + 24.0f),
                IM_COL32(235, 235, 235, 255), msg);

    if (!m_loadInfo.name.empty())
    {
        ImVec2 ns = ImGui::CalcTextSize(m_loadInfo.name.c_str());
        dl->AddText(ImVec2(c.x - ns.x * 0.5f, c.y + r + 48.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), m_loadInfo.name.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Application::LoadProject(const ProjectInfo& info)
{
    namespace fs = std::filesystem;
    m_projectInfo = info;

    // git 状態をプロジェクトごとに再評価
    m_gitChecked   = false;
    m_gitOutput.clear();

    // 「スキップ（デフォルト）」= 組み込みパスのまま。すでに既定シーンが読み込まれている。
    if (info.rootDir.empty())
    {
        Logger::Info("Using built-in default project (no project root)");
        if (m_window) m_window->SetTitle(L"DX12 Engine");
        return;
    }

    // 1) パスをプロジェクト配下へ再ポイント（shaders はエンジン側を維持）
    PathResolver::SetProjectRoot(info.rootDir);

    // 2) パス依存サブシステムを更新
    m_audioSystem->SetAssetsDir(PathResolver::AssetsDir());
    m_scriptEngine->SetAssetsDir(PathResolver::AssetsDir());
    if (m_editorLayer)
        m_editorLayer->SetAssetRoots(PathResolver::AssetsDir(), PathResolver::ScriptsDir());

    // 3) プロジェクトの game.lua を読み込み直す
    {
        std::string scriptPath = PathResolver::ScriptsDir() + "game.lua";
        if (fs::exists(scriptPath))
        {
            m_scriptEngine->LoadScript(scriptPath);
            m_scriptLastWriteTime = fs::last_write_time(scriptPath);
        }
    }

    // 4) 開始シーンを決定してロード（フレーム境界で実行）
    std::string sceneRel = info.defaultScene.empty() ? "scenes/default.json" : info.defaultScene;
    std::string sceneFull = PathResolver::AssetsDir() + sceneRel;
    m_currentSceneRel = sceneRel;

    if (fs::exists(sceneFull))
    {
        // 既存シーンを次フレームで安全にロード
        m_editorCtx->pendingLoadPath = sceneFull;
    }
    else
    {
        // 新規プロジェクト: グリッド + 平行光源のスターターシーンを生成して保存
        fs::create_directories(fs::path(sceneFull).parent_path());
        m_editorCtx->pendingNewScenePath = sceneFull;
        m_editorCtx->pendingNewScene = true;
    }

    // 5) ウィンドウタイトルにプロジェクト名
    if (m_window)
    {
        std::wstring title = L"DX12 Engine - ";
        title += std::wstring(info.name.begin(), info.name.end());
        m_window->SetTitle(title);
    }

    Logger::Info("Project loaded: {} ({})", info.name, info.rootDir);
}

void Application::SaveCurrentProject()
{
    namespace fs = std::filesystem;

    // 現在シーンを保存
    if (!m_editorCtx->currentScenePath.empty())
    {
        SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
        ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
    }

    // .dx12proj を保存（プロジェクトを開いている場合のみ）
    if (!m_projectInfo.rootDir.empty())
    {
        m_projectInfo.defaultScene    = m_currentSceneRel.empty() ? m_projectInfo.defaultScene : m_currentSceneRel;
        m_projectInfo.lastOpenedScene = m_currentSceneRel;
        std::string projPath = (fs::path(m_projectInfo.rootDir) / (m_projectInfo.name + ".dx12proj")).string();
        Project::Save(m_projectInfo, projPath);
    }
    m_editorCtx->buildCompleteFlash = 2.0f;
    Logger::Info("Project saved");
}

void Application::RenderProjectWindow()
{
    ImGui::Begin("Project");

    if (m_projectInfo.rootDir.empty())
    {
        ImGui::TextDisabled("(組み込みデフォルトプロジェクト)");
    }
    else
    {
        ImGui::Text("名前: %s", m_projectInfo.name.c_str());
        ImGui::TextWrapped("場所: %s", m_projectInfo.rootDir.c_str());
        ImGui::TextWrapped("シーン: %s", m_currentSceneRel.c_str());
    }
    ImGui::Separator();

    auto icon = [](u64 h, float s) { if (h) { ImGui::Image(static_cast<ImTextureID>(h), ImVec2(s, s)); ImGui::SameLine(); } };

    icon(m_icons.save, 22);
    if (ImGui::Button("プロジェクトを保存", ImVec2(-1, 30)))
        SaveCurrentProject();

    icon(m_icons.newProject, 22);
    if (ImGui::Button("新規プロジェクト...", ImVec2(-1, 0)))
    {
        ProjectInfo created;
        if (ProjectManager::NewProjectDialog(created, m_window->GetHwnd()))
            BeginProjectLoad(created, /*isNew=*/true);
    }
    icon(m_icons.openProject, 22);
    if (ImGui::Button("プロジェクトを開く...", ImVec2(-1, 0)))
    {
        ProjectInfo opened;
        if (ProjectManager::OpenProjectDialog(opened, m_window->GetHwnd()))
            BeginProjectLoad(opened, /*isNew=*/false);
    }
    icon(m_icons.recent, 22);
    if (ImGui::Button("ランチャーに戻る", ImVec2(-1, 0)))
        m_showLauncher = true;

    ImGui::End();
}

void Application::RenderVersionControlWindow()
{
    // 非表示タブ/折りたたみ時は中身を一切実行しない。
    // ここでは git/gh の外部プロセスを起動するため、毎フレーム走ると
    // メインスレッドがブロックされてエディタ全体がもたつく原因になる。
    if (!ImGui::Begin("Version Control (Git)"))
    {
        ImGui::End();
        return;
    }

    // git/gh の存在チェック（1 度だけ）
    if (!m_gitChecked)
    {
        m_gitAvailable = GitIntegration::IsGitAvailable();
        m_ghAvailable  = GitIntegration::IsGhAvailable();
        m_gitChecked   = true;
        if (m_gitCommitMsgBuf[0] == '\0')
        {
            const char* def = "Update";
            std::copy(def, def + 7, m_gitCommitMsgBuf.begin());
        }
    }

    if (m_projectInfo.rootDir.empty())
    {
        ImGui::TextWrapped("プロジェクトを開く/作成すると Git を使用できます。");
        ImGui::End();
        return;
    }
    if (!m_gitAvailable)
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "git が見つかりません。インストールして PATH を通してください。");
        ImGui::End();
        return;
    }

    const std::string& root = m_projectInfo.rootDir;

    // git 状態（repo/branch/remote/branches）は外部プロセスで重い。
    // タブが開かれた瞬間(IsWindowAppearing)と、操作直後(m_gitForceRefresh)だけ取得する。
    // → VC タブを表示したまま見てるだけの間は一切 git を叩かないので、定期ヒッチが出ない。
    if (ImGui::IsWindowAppearing() || m_gitForceRefresh)
    {
        m_gitForceRefresh = false;
        m_gitRepoCache = GitIntegration::IsRepo(root);
        if (m_gitRepoCache)
        {
            m_gitBranchCache = GitIntegration::CurrentBranch(root);
            m_gitRemoteCache = GitIntegration::RemoteUrl(root);
            m_gitBranches    = GitIntegration::ListBranches(root);
        }
        else { m_gitBranchCache.clear(); m_gitRemoteCache.clear(); m_gitBranches.clear(); }
    }

    auto icon = [](u64 h, float s) { if (h) { ImGui::Image(static_cast<ImTextureID>(h), ImVec2(s, s)); ImGui::SameLine(); } };

    if (!m_gitRepoCache)
    {
        ImGui::TextWrapped("このプロジェクトはまだ Git リポジトリではありません。");
        icon(m_icons.git, 22);
        if (ImGui::Button("Git リポジトリを初期化", ImVec2(-1, 30)))
        {
            auto r = GitIntegration::Init(root);
            m_gitOutput = r.output.empty() ? "git init 完了" : r.output;
            m_gitForceRefresh = true;  // 即再取得
        }
        ImGui::End();
        return;
    }

    // ---- リポジトリ操作 ----
    const std::string& branch = m_gitBranchCache;
    const std::string& remote = m_gitRemoteCache;
    icon(m_icons.git, 18);
    ImGui::Text("ブランチ: %s", branch.empty() ? "(未コミット)" : branch.c_str());
    ImGui::TextWrapped("リモート: %s", remote.empty() ? "(未設定)" : remote.c_str());
    if (m_ghAvailable && !m_ghUser.empty())
    { ImGui::SameLine(); ImGui::TextDisabled("(@%s)", m_ghUser.c_str()); }
    ImGui::Separator();

    // ---- GitHub ログイン状態 ----
    if (m_ghAvailable)
    {
        if (!m_ghUserChecked)
        {
            m_ghUser = GitIntegration::GitHubUser();
            m_ghUserChecked = true;
        }
        icon(m_icons.github, 18);
        if (m_ghUser.empty())
        {
            ImGui::Text("GitHub: 未ログイン");
            ImGui::SameLine();
            if (ImGui::SmallButton("ログイン"))
            {
                GitIntegration::LaunchLogin();
                m_gitOutput = "別ウィンドウでログインを進めてください。完了後「再確認」を押してや。";
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("再確認")) m_ghUserChecked = false;
        }
        else
        {
            ImGui::Text("GitHub: @%s", m_ghUser.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("再確認")) m_ghUserChecked = false;
        }
    }

    // ---- ブランチ ----
    ImGui::SeparatorText("ブランチ");
    {
        const char* curBr = branch.empty() ? "(未コミット)" : branch.c_str();
        ImGui::SetNextItemWidth(-80);
        if (ImGui::BeginCombo("切替##branch", curBr))
        {
            for (const auto& b : m_gitBranches)
                if (ImGui::Selectable(b.c_str(), b == branch) && b != branch)
                {
                    auto r = GitIntegration::CheckoutBranch(root, b);
                    m_gitOutput = r.ok() ? ("ブランチを " + b + " に切替") : r.output;
                    m_gitForceRefresh = true;
                }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(-80);
        ImGui::InputTextWithHint("##newbranch", "新しいブランチ名",
                                 m_gitNewBranchBuf.data(), m_gitNewBranchBuf.size());
        ImGui::SameLine();
        if (ImGui::Button("作成切替") && m_gitNewBranchBuf[0] != '\0')
        {
            auto r = GitIntegration::CreateBranch(root, m_gitNewBranchBuf.data());
            m_gitOutput = r.ok() ? (std::string("ブランチ ") + m_gitNewBranchBuf.data() + " を作成") : r.output;
            m_gitNewBranchBuf.fill('\0');
            m_gitForceRefresh = true;
        }
    }

    // ---- リモート（origin 未設定時のみ手動 URL を出す）----
    if (remote.empty())
    {
        ImGui::SeparatorText("リモート (origin 未設定)");
        ImGui::SetNextItemWidth(-60);
        ImGui::InputTextWithHint("##remote", "https://github.com/owner/repo.git",
                                 m_gitRemoteBuf.data(), m_gitRemoteBuf.size());
        ImGui::SameLine();
        if (ImGui::Button("設定") && m_gitRemoteBuf[0] != '\0')
        {
            auto r = GitIntegration::AddRemote(root, m_gitRemoteBuf.data());
            m_gitOutput = r.ok() ? "リモートを設定しました" : r.output;
            m_gitForceRefresh = true;
        }
        ImGui::TextDisabled("クローンした場合は自動設定済みです");
    }

    ImGui::SeparatorText("コミット");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##commitmsg", "コミットメッセージ",
                             m_gitCommitMsgBuf.data(), m_gitCommitMsgBuf.size());

    icon(m_icons.commit, 22);
    if (ImGui::Button("コミット", ImVec2(130, 30)))
    {
        // シーン/プロジェクトを保存してからコミット
        SaveCurrentProject();
        auto r = GitIntegration::CommitAll(root, m_gitCommitMsgBuf.data());
        m_gitOutput = r.output.empty() ? "コミット完了" : r.output;
    }
    ImGui::SameLine();
    icon(m_icons.push, 22);
    if (ImGui::Button("プッシュ", ImVec2(130, 30)))
    {
        auto r = GitIntegration::Push(root, true);
        m_gitOutput = r.output.empty() ? "プッシュ完了" : r.output;
    }

    if (ImGui::Button("コミット & プッシュ", ImVec2(-1, 30)))
    {
        SaveCurrentProject();
        auto c = GitIntegration::CommitAll(root, m_gitCommitMsgBuf.data());
        auto p = GitIntegration::Push(root, true);
        m_gitOutput = c.output + "\n" + p.output;
        m_gitForceRefresh = true;
    }

    // ---- プル / フェッチ（リモートの変更を取り込む）----
    if (ImGui::Button("プル (pull)", ImVec2(130, 26)))
    {
        auto r = GitIntegration::Pull(root);
        m_gitOutput = r.output.empty() ? "プル完了" : r.output;
        m_gitForceRefresh = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("フェッチ (fetch)", ImVec2(150, 26)))
    {
        auto r = GitIntegration::Fetch(root);
        m_gitOutput = r.output.empty() ? "フェッチ完了" : r.output;
        m_gitForceRefresh = true;
    }

    // ---- GitHub 連携（gh CLI）----
    ImGui::SeparatorText("GitHub");
    if (m_ghAvailable)
    {
        if (remote.empty())
        {
            icon(m_icons.github, 22);
            ImGui::TextWrapped("gh で GitHub リポジトリを作成して公開できます。");
            if (ImGui::Button("GitHub リポジトリ作成 (private) & push", ImVec2(-1, 0)))
            {
                SaveCurrentProject();
                GitIntegration::CommitAll(root, m_gitCommitMsgBuf.data());
                auto r = GitIntegration::CreateGitHubRepo(root, m_projectInfo.name, true);
                m_gitOutput = r.output;
            }
            if (ImGui::Button("GitHub リポジトリ作成 (public) & push", ImVec2(-1, 0)))
            {
                SaveCurrentProject();
                GitIntegration::CommitAll(root, m_gitCommitMsgBuf.data());
                auto r = GitIntegration::CreateGitHubRepo(root, m_projectInfo.name, false);
                m_gitOutput = r.output;
            }
        }
        else
        {
            ImGui::TextDisabled("リモート設定済み。コミット & プッシュを使用してください。");
        }
    }
    else
    {
        ImGui::TextDisabled("gh CLI が無いため、リモート URL を手動設定してプッシュしてください。");
    }

    // ---- 出力ログ ----
    if (ImGui::Button("状態を取得 (git status)"))
    {
        auto r = GitIntegration::Status(root);
        m_gitOutput = r.output;
    }
    if (!m_gitOutput.empty())
    {
        ImGui::SeparatorText("出力");
        ImGui::BeginChild("##gitout", ImVec2(0, 120), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(m_gitOutput.c_str());
        ImGui::EndChild();
    }

    ImGui::End();
}

void Application::WireScriptCallbacks()
{
    if (!m_scriptEngine) return;

    m_scriptEngine->SetLoadSceneCallback(
        [this](const std::string& rel) { m_editorCtx->pendingGameLoadPath = rel; });

    m_scriptEngine->SetNextSceneCallback(
        [this]() {
            if (m_sceneFlow)
            {
                std::string n = m_sceneFlow->Next(m_currentSceneRel);
                if (!n.empty()) m_editorCtx->pendingGameLoadPath = n;
            }
        });

    m_scriptEngine->SetQuitCallback(
        [this]() { if (m_window) PostMessageW(m_window->GetHwnd(), WM_CLOSE, 0, 0); });

    m_scriptEngine->SetTransitionCallback(
        [this](const std::string& rel, int type, float dur) {
            if (!m_sceneTransition) return;
            m_transitionTargetScene = rel;
            m_sceneTransition->Start(static_cast<TransitionType>(type), dur);
        });

    // ゲーム内 UI（即時モード）コールバック
    m_scriptEngine->SetUiCallbacks(
        [this](float x, float y, const std::string& text, float size, float r, float g, float b, float a) {
            UICommand c; c.type = UICommand::Type::Text;
            c.x = x; c.y = y; c.size = size; c.text = text;
            c.r = r; c.g = g; c.b = b; c.a = a;
            m_uiCommands.push_back(std::move(c));
        },
        [this](float x, float y, float w, float h, const std::string& label) -> bool {
            UICommand c; c.type = UICommand::Type::Button;
            c.x = x; c.y = y; c.w = w; c.h = h; c.text = label;
            m_uiCommands.push_back(std::move(c));
            // 前フレームに押されたか
            return m_pressedButtons.count(label) > 0;
        },
        [this](float x, float y, float w, float h, const std::string& path) {
            UICommand c; c.type = UICommand::Type::Image;
            c.x = x; c.y = y; c.w = w; c.h = h; c.text = path;
            m_uiCommands.push_back(std::move(c));
        });
}

void Application::SyncActiveCameraToGlobal()
{
    auto& reg = m_scene->GetRegistry();
    auto camView = reg.view<const CameraComponent, const Transform>();
    for (auto [e, cam, tf] : camView.each())
    {
        if (!cam.isActive) continue;
        m_camera->SetPosition(tf.position);
        m_camera->SetYaw(DirectX::XMConvertToRadians(tf.rotation.y));
        // ピッチ反転: エディタのギズモ/フラスタム(Euler)と Camera(forward.y=sin(pitch)) は符号が逆
        m_camera->SetPitch(DirectX::XMConvertToRadians(-tf.rotation.x));
        m_camera->SetPerspective(
            DirectX::XMConvertToRadians(cam.fovDegrees),
            static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
            cam.nearClip, cam.farClip);
        break;
    }
}

void Application::DoRuntimeSceneLoad(const std::string& rel, ID3D12GraphicsCommandList* cmdList)
{
    std::string full = PathResolver::AssetsDir() + rel;
    if (!std::filesystem::exists(full))
    {
        Logger::Warn("loadScene: scene not found: {}", full);
        return;
    }

    // 物理リセット
    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_physicsSystem->Shutdown();
    m_physicsSystem->Initialize();

    // シーン再構築
    m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(), m_srvHeap.get(), cmdList);
    if (!SceneSerializer::Load(*m_scene, full, PathResolver::AssetsDir()))
    {
        Logger::Warn("loadScene: failed to load {}", full);
        return;
    }
    m_editorCtx->currentScenePath = full;
    m_currentSceneRel = rel;

    // ScriptEngine 作り直し（コールバック再注入）
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(), m_camera.get(),
                               m_audioSystem.get(), m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();
    std::string gs = PathResolver::ScriptsDir() + "game.lua";
    if (std::filesystem::exists(gs)) m_scriptEngine->LoadScript(gs);

    // アクティブカメラがなければ最初のものを有効化
    {
        auto& reg = m_scene->GetRegistry();
        auto camView = reg.view<CameraComponent>();
        bool hasActive = false;
        for (auto [e, c] : camView.each()) if (c.isActive) { hasActive = true; break; }
        if (!hasActive && !camView.empty())
            reg.get<CameraComponent>(*camView.begin()).isActive = true;
    }

    m_scriptEngine->OnPlayStart();
    if (m_particleSystem) m_particleSystem->Clear();  // シーン切替時に前シーンの粒子を消す
    SyncActiveCameraToGlobal();

    // 新シーンの RigidBody を物理登録
    {
        auto& reg = m_scene->GetRegistry();
        for (auto [e, rb] : reg.view<RigidBody>().each())
        {
            if (rb.bodyId != kInvalidBodyId) m_physicsSystem->UnregisterBody(reg, e);
            m_physicsSystem->RegisterBody(reg, e);
        }
    }
    m_physicsSystem->ResetAccumulator();

    Logger::Info("Runtime scene loaded: {}", rel);
}

void Application::EnterPlayMode()
{
    // カメラ設置チェック
    {
        auto& reg = m_scene->GetRegistry();
        auto camCheck = reg.view<const CameraComponent>();
        if (camCheck.empty())
        {
            m_editorCtx->errorMessage = "シーンに Camera が配置されていません。\nHierarchy 右クリック → Camera で追加してください。";
            m_editorCtx->errorFlash = 1.0f;
            Logger::Warn("Play cancelled: no CameraComponent found");
            return;
        }
        // isActive なカメラがなければ最初のカメラを自動で有効化
        bool hasActive = false;
        for (auto [e, cam] : camCheck.each())
            if (cam.isActive) { hasActive = true; break; }
        if (!hasActive)
        {
            auto first = *camCheck.begin();
            reg.get<CameraComponent>(first).isActive = true;
            Logger::Info("Auto-activated first CameraComponent for play mode");
        }
    }

    // GPU を待機してコマンドリスト状態を安全にする
    m_commandQueue->WaitIdle();

    // カメラ状態保存
    m_cameraSnapshot.position = m_camera->GetPosition();
    m_cameraSnapshot.yaw = m_camera->GetYaw();
    m_cameraSnapshot.pitch = m_camera->GetPitch();

    // Lua が触る前のエディタ状態を Stop 時の完全復元用に保存
    m_playSceneJson = SceneSerializer::SaveToString(*m_scene, PathResolver::AssetsDir());

    // ゲーム用カメラ: アクティブな CameraComponent をグローバル Camera に同期
    SyncActiveCameraToGlobal();

    // OnPlayStart 直後に Lua が変えた値を打ち消すため、Transform / RigidBody / Material PBR を覚えておく
    m_editorSnapshots.clear();
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<NameTag, Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            EntitySnapshot snap;
            snap.position      = transform.position;
            snap.rotation      = transform.rotation;
            snap.scale         = transform.scale;
            snap.quaternion    = transform.quaternion;
            snap.useQuaternion = transform.useQuaternion;

            snap.hasRigidBody = reg.all_of<RigidBody>(entity);
            if (snap.hasRigidBody)
                snap.rigidBodyData = reg.get<RigidBody>(entity);

            if (reg.all_of<MeshRenderer>(entity))
            {
                const auto& mr = reg.get<MeshRenderer>(entity);
                if (!mr.meshes.empty() && mr.meshes[0] && mr.meshes[0]->GetMaterial())
                {
                    snap.materialMetallic  = (mr.overrideMetallic  >= 0.0f) ? mr.overrideMetallic
                                           : mr.meshes[0]->GetMaterial()->defaultMetallic;
                    snap.materialRoughness = (mr.overrideRoughness >= 0.0f) ? mr.overrideRoughness
                                           : mr.meshes[0]->GetMaterial()->defaultRoughness;
                }
            }

            m_editorSnapshots[name.name] = snap;
        }
    }

    m_inputSystem->SetMouseCapture(false);

    // スクリプトエンジン初期化（エンティティにアタッチされた LuaScript 用）
    // ※ グローバルな game.lua の OnStart は呼ばない（エディタ配置のみで Play する）
    m_scriptEngine->Shutdown();
    m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                               m_camera.get(), m_audioSystem.get(),
                               m_physicsSystem.get(), PathResolver::AssetsDir());
    WireScriptCallbacks();

    std::string scriptPath = PathResolver::ScriptsDir() + "game.lua";
    if (std::filesystem::exists(scriptPath))
    {
        m_scriptEngine->LoadScript(scriptPath);
    }
    m_scriptEngine->OnPlayStart();
    if (m_particleSystem) m_particleSystem->Clear();  // Play 開始時に粒子をリセット

    // エディタのスナップショットで上書き（Luaが勝手に変えた状態をエディタの状態に戻す）
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<NameTag, Transform>();
        for (auto [entity, name, transform] : view.each())
        {
            auto it = m_editorSnapshots.find(name.name);
            if (it == m_editorSnapshots.end()) continue;
            const auto& snap = it->second;

            // Transform 復元
            transform.position      = snap.position;
            transform.rotation      = snap.rotation;
            transform.scale         = snap.scale;
            transform.quaternion    = snap.quaternion;
            transform.useQuaternion = snap.useQuaternion;

            // Physics: エディタで外してたら Lua が付けたものを削除
            if (!snap.hasRigidBody)
            {
                reg.remove<RigidBody>(entity);
                reg.remove<ConvexHullCollider>(entity);
                reg.remove<BoxCollider>(entity);
                reg.remove<SphereCollider>(entity);
                reg.remove<CapsuleCollider>(entity);
            }
            else
            {
                // エディタのパラメータで上書き（Luaのデフォルト値ではなくエディタの設定値を使う）
                auto rb = snap.rigidBodyData;
                rb.bodyId = kInvalidBodyId;
                reg.emplace_or_replace<RigidBody>(entity, rb);
            }

            // Material PBR 復元（オーバーライド値に設定）
            if (reg.all_of<MeshRenderer>(entity))
            {
                auto& mr = reg.get<MeshRenderer>(entity);
                mr.overrideMetallic  = snap.materialMetallic;
                mr.overrideRoughness = snap.materialRoughness;
            }
        }
    }

    // Playモード: サイドバーなし全画面幅でアスペクト比再計算
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);

    // 物理のタイムステップ蓄積をリセット
    m_physicsSystem->ResetAccumulator();

    // 全 RigidBody を物理エンジンに登録（エディタで復元した状態で）
    {
        auto& reg = m_scene->GetRegistry();
        auto view = reg.view<RigidBody>();
        for (auto [entity, rb] : view.each())
        {
            if (rb.bodyId != kInvalidBodyId)
                m_physicsSystem->UnregisterBody(reg, entity);
            m_physicsSystem->RegisterBody(reg, entity);
        }
    }

    // ホットリロード用タイムスタンプ更新
    if (std::filesystem::exists(scriptPath))
        m_scriptLastWriteTime = std::filesystem::last_write_time(scriptPath);

    m_engineMode = EngineMode::Playing;
    Logger::Info("Entered PLAY mode");
}

void Application::EnterEditorMode()
{
    DX_ASSERT(!m_playSceneJson.empty(),
              "EnterEditorMode requires a prior EnterPlayMode snapshot");

    m_commandQueue->WaitIdle();

    // Play 中に鳴っていた SE（空間含む）を停止
    if (m_audioSystem) m_audioSystem->StopAllSFX();

    // OnPlayStop は ScriptEngine::Shutdown より前に呼ぶ（Shutdown で Lua state が消える）
    if (m_engineMode == EngineMode::Playing)
        m_scriptEngine->OnPlayStop();

    m_physicsSystem->UnregisterAllBodies(m_scene->GetRegistry());
    m_physicsSystem->Shutdown();
    m_physicsSystem->Initialize();

    m_inputSystem->SetMouseCapture(false);

    m_camera->SetPosition(m_cameraSnapshot.position);
    m_camera->SetYaw(m_cameraSnapshot.yaw);
    m_camera->SetPitch(m_cameraSnapshot.pitch);

    m_editorCtx->ClearSelection();

    // JSON スナップショットからシーン全体を完全復元
    {
        auto* cmdList = m_frameResources->BeginFrame(*m_commandQueue);

        // Scene::Spawn は内部で m_cmdList を使うので最新の cmdList で更新する
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), cmdList);

        SceneSerializer::LoadFromString(*m_scene, m_playSceneJson, PathResolver::AssetsDir());

        m_scriptEngine->Shutdown();
        m_scriptEngine->Initialize(m_scene.get(), m_inputSystem.get(),
                                   m_camera.get(), m_audioSystem.get(),
                                   m_physicsSystem.get(), PathResolver::AssetsDir());
        WireScriptCallbacks();

        ThrowIfFailed(cmdList->Close());
        m_commandQueue->ExecuteCommandList(cmdList);
        m_commandQueue->WaitIdle();
        m_resourceManager->FinishUploads();
        m_frameResources->EndFrame(*m_commandQueue);
    }

    // 古い JSON で誤復元しないよう、消費後はクリアする
    m_playSceneJson.clear();
    m_playSceneJson.shrink_to_fit();

    // シーン再構築でエンティティ ID が変わり Undo スタックの参照が無効になるためクリア
    m_editorCtx->undoSystem.Clear();
    m_editorCtx->ClearSelection();

    // Play 中に CameraComponent の FOV を採用してた可能性があるためエディタ用に戻す
    m_camera->SetPerspective(DirectX::XM_PIDIV4,
        static_cast<f32>(m_window->GetWidth()) / static_cast<f32>(m_window->GetHeight()),
        0.1f, 1000.0f);

    m_inputSystem->SetMouseCapture(false);
    m_engineMode = EngineMode::Editor;
    Logger::Info("Entered EDITOR mode");
}

void Application::BuildGameStandalone()
{
    // 開始シーンを title.json に（あれば）。無ければ現在の currentScenePath を使う。
    std::string title = PathResolver::AssetsDir() + "scenes/title.json";
    if (std::filesystem::exists(title))
        m_editorCtx->currentScenePath = title;
    BuildGame();
}

void Application::BuildGame()
{
    namespace fs = std::filesystem;

    // ビルド出力先
    fs::path outputDir = fs::path(PathResolver::BaseDir()) / "build" / "game";

    // クリーンアップ
    if (fs::exists(outputDir))
        fs::remove_all(outputDir);
    fs::create_directories(outputDir);

    // 1. exe をコピー（+ exe 隣の DLL も全部コピー: D3D12MA.dll / DirectXTex.dll 等）
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        fs::path exeSrc(exePath);
        fs::path exeDst = outputDir / "Game.exe";
        fs::copy_file(exeSrc, exeDst, fs::copy_options::overwrite_existing);
        Logger::Info("Copied exe -> {}", exeDst.string());

        // 同じフォルダの .dll をすべて配布フォルダへ
        std::error_code ec;
        for (auto& entry : fs::directory_iterator(exeSrc.parent_path(), ec))
        {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext == ".dll" || ext == ".DLL")
            {
                fs::copy_file(entry.path(), outputDir / entry.path().filename(),
                              fs::copy_options::overwrite_existing, ec);
                Logger::Info("Copied dll -> {}", entry.path().filename().string());
            }
        }
    }

    // 2. scripts/ をコピー
    {
        fs::path scriptsSrc = fs::path(PathResolver::ScriptsDir());
        fs::path scriptsDst = outputDir / "scripts";
        if (fs::exists(scriptsSrc))
        {
            fs::copy(scriptsSrc, scriptsDst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            Logger::Info("Copied scripts/");
        }
    }

    // 3. assets/ をコピー
    {
        fs::path assetsSrc = fs::path(PathResolver::AssetsDir());
        fs::path assetsDst = outputDir / "assets";
        if (fs::exists(assetsSrc))
        {
            fs::copy(assetsSrc, assetsDst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            Logger::Info("Copied assets/");
        }
    }

    // 4. shaders/ をコピー
    {
        fs::path shadersSrc = fs::path(PathResolver::ShaderDirW());
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

    // 6. 配布設定 game.json（開始シーン = 現在編集中シーンの assets 相対パス）
    {
        std::string startSceneRel = "scenes/default.json";
        if (!m_editorCtx->currentScenePath.empty())
        {
            auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
            std::string full = norm(m_editorCtx->currentScenePath);
            std::string base = norm(PathResolver::AssetsDir());
            if (!base.empty() && full.rfind(base, 0) == 0)
                startSceneRel = full.substr(base.size());
            else
                startSceneRel = fs::path(full).lexically_relative(fs::path(base)).generic_string();

            if (startSceneRel.empty() || startSceneRel.rfind("..", 0) == 0)
            {
                Logger::Warn("Current scene is outside assets/; start scene may not be bundled: {}",
                             m_editorCtx->currentScenePath);
                startSceneRel = "scenes/default.json";
            }
        }

        std::ofstream gj(outputDir / "game.json");
        gj << "{\n";
        gj << "  \"title\": \"Game\",\n";
        gj << "  \"startScene\": \"" << startSceneRel << "\",\n";
        gj << "  \"windowWidth\": 1280,\n";
        gj << "  \"windowHeight\": 720\n";
        gj << "}\n";
        Logger::Info("Wrote game.json (startScene = {})", startSceneRel);
    }

    Logger::Info("Game build complete: {}", outputDir.string());
}

void Application::RenderSceneMeshes(ID3D12GraphicsCommandList* nativeCmdList, u32 frameIndex,
                                   DirectX::XMMATRIX viewProj, bool isGameView)
{
    using namespace DirectX;
    auto& reg = m_scene->GetRegistry();
    auto renderView = reg.view<const Transform, const MeshRenderer>();

    // パーティクル判定（名前が "Pfx" で始まる＝加算発光で描く）
    auto isPfx = [&](entt::entity e) -> bool {
        const auto* nt = reg.try_get<NameTag>(e);
        return nt && nt->name.rfind("Pfx", 0) == 0;
    };

    // 1エンティティ分の描画（パイプライン選択 + メッシュ描画）
    auto drawEntity = [&](entt::entity e, const Transform& transform, const MeshRenderer& renderer)
    {
        XMMATRIX world = (transform.parent != entt::null)
            ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

        bool isGrid = reg.all_of<GridPlane>(e);
        bool isSkinned = reg.all_of<SkeletalAnimation>(e);

        if (isPfx(e))
        {
            m_commandList->SetPipelineState(*m_emissivePipelineState);
        }
        else if (isGrid)
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

            // PBR テクスチャ SRV ブロックをバインド
            if (mat && mat->srvBlockIndex != 0xFFFFFFFF)
            {
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                    m_srvHeap->GetGpuHandle(mat->srvBlockIndex));
            }
            else
            {
                Texture* tex = (mat && mat->albedoTexture) ? mat->albedoTexture : m_resourceManager->GetDefaultWhiteTexture();
                m_commandList->SetSRVTable(RootSignature::kSlotSRVTable,
                    m_srvHeap->GetGpuHandle(tex->GetSrvIndex()));
            }

            // PBR Material Constants (Slot 5)
            struct { float metallic; float roughness; u32 flags; float pad; } pbrParams;
            // MeshRenderer のオーバーライド値を優先、なければ Material の値
            pbrParams.metallic  = (renderer.overrideMetallic  >= 0.0f) ? renderer.overrideMetallic
                                : (mat ? mat->defaultMetallic : 0.0f);
            pbrParams.roughness = (renderer.overrideRoughness >= 0.0f) ? renderer.overrideRoughness
                                : (mat ? mat->defaultRoughness : 0.5f);
            pbrParams.flags     = 0;
            if (mat && mat->normalMapTexture) pbrParams.flags |= 1u;
            // overrideが有効な場合、metalRoughnessテクスチャのスケーリングを無効化
            bool hasOverride = (renderer.overrideMetallic >= 0.0f || renderer.overrideRoughness >= 0.0f);
            if (!hasOverride && mat && mat->metalRoughnessTexture) pbrParams.flags |= 2u;
            pbrParams.pad = 0;
            nativeCmdList->SetGraphicsRoot32BitConstants(RootSignature::kSlotPBRMaterial, 4, &pbrParams, 0);

            m_commandList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_commandList->SetVertexBuffer(mesh->GetVertexBuffer().GetView());
            m_commandList->SetIndexBuffer(mesh->GetIndexBuffer().GetView());
            m_commandList->DrawIndexedInstanced(mesh->GetIndexCount());
        }
    };

    // パス1: 不透明（グリッド・パーティクル以外）を先に描く＝深度を確定
    for (auto [e, transform, renderer] : renderView.each())
    {
        if (reg.all_of<GridPlane>(e)) continue;
        if (isPfx(e)) continue;
        drawEntity(e, transform, renderer);
    }

    // パス2: エディタ用グリッド（半透明・深度書き込みOFF）。ゲームビューでは描かない。
    if (!isGameView)
    {
        for (auto [e, transform, renderer] : renderView.each())
        {
            if (!reg.all_of<GridPlane>(e)) continue;
            drawEntity(e, transform, renderer);
        }
    }

    // パス3: 発光パーティクル（加算合成）を最後に重ねる＝不透明物に隠れず光る
    for (auto [e, transform, renderer] : renderView.each())
    {
        if (isPfx(e)) drawEntity(e, transform, renderer);
    }
}

void Application::Render()
{
    using namespace DirectX;

    auto* nativeCmdList = m_frameResources->BeginFrame(*m_commandQueue);
    m_commandList->Wrap(nativeCmdList);

    // Deferred: new scene（描画前に処理しないと GPU リソース解放でクラッシュする）
    if (m_editorCtx->pendingNewScene && m_engineMode == EngineMode::Editor)
    {
        m_editorCtx->pendingNewScene = false;
        // プロジェクト新規作成の初期シーンなら保存先が指定されている
        std::string starterPath = std::move(m_editorCtx->pendingNewScenePath);
        m_editorCtx->pendingNewScenePath.clear();
        m_editorCtx->ClearSelection();
        m_editorCtx->undoSystem.Clear();
        m_editorCtx->currentScenePath = starterPath;  // 空なら未保存の新規シーン
        m_scene->Clear();
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        // ---- 再生に必要な最低限のデフォルト配置 ----
        m_scene->SpawnPlane("Grid", {0, 0, 0}, 50.0f, true);   // エディタ用グリッド
        m_scene->SpawnPlane("Ground", {0, 0, 0}, 20.0f, false); // 実体のある床
        m_scene->SpawnBox("Cube", {0, 0.5f, 0}, {0, 0, 0}, {1, 1, 1}); // サンプルオブジェクト
        {
            auto& reg = m_scene->GetRegistry();
            // 平行光源
            auto lightE = reg.create();
            reg.emplace<NameTag>(lightE, NameTag{"DirectionalLight"});
            reg.emplace<Transform>(lightE, Transform{{0, 10, 0}, {-45, -30, 0}, {1,1,1}});
            reg.emplace<DirectionalLight>(lightE);

            // メインカメラ（CameraComponent が無いと Play で映らないため必須）
            auto camE = reg.create();
            reg.emplace<NameTag>(camE, NameTag{"MainCamera"});
            reg.emplace<Transform>(camE, Transform{{0.0f, 6.0f, -12.0f}, {22.0f, 0.0f, 0.0f}, {1,1,1}});
            CameraComponent cam;
            cam.isActive = true;
            reg.emplace<CameraComponent>(camE, cam);
        }
        if (!starterPath.empty())
            m_currentSceneRel = ToAssetRel(starterPath);
        // 作成と同時にシーンファイルを保存
        if (!m_editorCtx->currentScenePath.empty())
        {
            SceneSerializer::Save(*m_scene, m_editorCtx->currentScenePath, PathResolver::AssetsDir());
            ProjectManager::SaveLastOpenedScene(m_editorCtx->currentScenePath);
            m_editorCtx->hotReloadFlash = 1.5f;
        }
        m_editorLayer->RefreshAssetBrowser();
        Logger::Info("New scene created");
    }

    // Deferred: scene load（描画前に処理）
    if (!m_editorCtx->pendingLoadPath.empty() && m_engineMode == EngineMode::Editor)
    {
        std::string loadPath = std::move(m_editorCtx->pendingLoadPath);
        m_editorCtx->pendingLoadPath.clear();
        m_editorCtx->ClearSelection();
        m_editorCtx->undoSystem.Clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        if (SceneSerializer::Load(*m_scene, loadPath, PathResolver::AssetsDir()))
        {
            m_editorCtx->currentScenePath = loadPath;
            m_currentSceneRel = ToAssetRel(loadPath);
            ProjectManager::SaveLastOpenedScene(loadPath);
            m_editorCtx->hotReloadFlash = 1.5f;
            m_editorLayer->RefreshAssetBrowser();
            Logger::Info("Scene loaded: {}", loadPath);
        }
    }

    // Play 中のシーン切替（Lua loadScene/nextScene、またはトランジション中間点）
    {
        // トランジションが中間点に達したら、保留中のターゲットをロード対象にする
        if (m_sceneTransition && m_sceneTransition->ConsumeHalfway() && !m_transitionTargetScene.empty())
        {
            m_editorCtx->pendingGameLoadPath = m_transitionTargetScene;
            m_transitionTargetScene.clear();
        }

        if (!m_editorCtx->pendingGameLoadPath.empty() && m_engineMode == EngineMode::Playing)
        {
            std::string rel = std::move(m_editorCtx->pendingGameLoadPath);
            m_editorCtx->pendingGameLoadPath.clear();
            DoRuntimeSceneLoad(rel, nativeCmdList);
        }
        else if (!m_editorCtx->pendingGameLoadPath.empty())
        {
            // Play 中でなければ無視（誤発火防止）
            m_editorCtx->pendingGameLoadPath.clear();
        }
    }

    // エンティティ生成（前フレームのドラッグ&ドロップ等から遅延実行）
    if (!m_editorCtx->pendingSpawns.empty())
    {
        char dbgBuf[128];
        snprintf(dbgBuf, sizeof(dbgBuf), "[PendingSpawns] count=%zu mode=%s\n",
            m_editorCtx->pendingSpawns.size(),
            m_engineMode == EngineMode::Editor ? "Editor" : "Playing");
        OutputDebugStringA(dbgBuf);
    }
    if (!m_editorCtx->pendingSpawns.empty() && m_engineMode == EngineMode::Editor)
    {
        auto spawns = std::move(m_editorCtx->pendingSpawns);
        m_editorCtx->pendingSpawns.clear();

        // Scene の内部 cmdList を今フレームのものに更新
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        for (auto& req : spawns)
        {
            std::string name = std::filesystem::path(req.modelPath).stem().string();
            entt::entity spawnedEntity = entt::null;

            if (req.modelPath == "__primitive_box__")
            {
                auto e = m_scene->SpawnBox(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__primitive_sphere__")
            {
                auto e = m_scene->SpawnSphere(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__primitive_plane__")
            {
                auto e = m_scene->SpawnPlane(name, req.position);
                spawnedEntity = e.GetHandle();
            }
            else if (req.modelPath == "__empty__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"Empty"});
                reg.emplace<Transform>(e);
                spawnedEntity = e;
            }
            else if (req.modelPath == "__camera__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"Camera"});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                // 他にアクティブカメラがなければ自動で isActive=true
                bool hasActive = false;
                for (auto [oe, oc] : reg.view<const CameraComponent>().each())
                    if (oc.isActive) { hasActive = true; break; }
                reg.emplace<CameraComponent>(e, CameraComponent{60.0f, 0.1f, 1000.0f, !hasActive});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__directional_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"DirectionalLight"});
                reg.emplace<Transform>(e, Transform{req.position, {-30.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<DirectionalLight>(e, DirectionalLight{{0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1.0f});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__point_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"PointLight"});
                reg.emplace<Transform>(e, Transform{req.position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<PointLight>(e, PointLight{{1.0f, 1.0f, 1.0f}, 1.0f, 10.0f});
                spawnedEntity = e;
            }
            else if (req.modelPath == "__spot_light__")
            {
                auto& reg = m_scene->GetRegistry();
                auto e = reg.create();
                reg.emplace<NameTag>(e, NameTag{"SpotLight"});
                reg.emplace<Transform>(e, Transform{req.position, {-60.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
                reg.emplace<SpotLight>(e, SpotLight{});
                spawnedEntity = e;
            }
            else
            {
                auto entity = m_scene->Spawn(name, req.modelPath, req.position);

                // D&D時のみ: AABBから自動スケーリング
                bool valid = entity.IsValid();
                bool hasMR = valid && entity.HasComponent<MeshRenderer>();
                {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "[D&D Scale] valid=%d hasMR=%d\n", valid, hasMR);
                    OutputDebugStringA(buf);
                }

                if (hasMR)
                {
                    auto& mr = entity.GetComponent<MeshRenderer>();
                    f32 maxExtent = 0.0f;
                    for (const auto* mesh : mr.meshes)
                    {
                        if (!mesh) continue;
                        auto mn = mesh->GetAABBMin();
                        auto mx = mesh->GetAABBMax();
                        f32 dx = mx.x - mn.x;
                        f32 dy = mx.y - mn.y;
                        f32 dz = mx.z - mn.z;
                        if (dx > maxExtent) maxExtent = dx;
                        if (dy > maxExtent) maxExtent = dy;
                        if (dz > maxExtent) maxExtent = dz;
                    }
                    // 既存Luaモデルと同じ見た目サイズにスケーリング
                    constexpr f32 kDefaultScale = 0.01f;
                    auto& t = entity.GetComponent<Transform>();
                    t.scale = {kDefaultScale, kDefaultScale, kDefaultScale};

                    // glTF/glb はZ-upなのでX軸90度回転で立たせる
                    std::string ext = std::filesystem::path(req.modelPath).extension().string();
                    if (ext == ".gltf" || ext == ".glb")
                        t.rotation.x = 90.0f;
                }
                spawnedEntity = entity.GetHandle();
            }

            // Undo に Spawn コマンドを積む
            if (spawnedEntity != entt::null)
            {
                m_editorCtx->undoSystem.PushCommand(
                    std::make_unique<SpawnEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(), spawnedEntity));
            }
            Logger::Info("Spawned: {}", name);
        }
    }

    // Undo/Redo（エンティティ復元がモデル再ロードを伴うため cmdList 有効時に実行）
    if ((m_editorCtx->pendingUndo || m_editorCtx->pendingRedo)
        && m_engineMode == EngineMode::Editor)
    {
        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);
        if (m_editorCtx->pendingUndo) m_editorCtx->undoSystem.Undo();
        if (m_editorCtx->pendingRedo) m_editorCtx->undoSystem.Redo();
        m_editorCtx->pendingUndo = false;
        m_editorCtx->pendingRedo = false;
    }

    // エンティティ複製（Ctrl+D / 右クリック複製。全コンポーネントのディープコピー）
    if (!m_editorCtx->pendingDuplications.empty() && m_engineMode == EngineMode::Editor)
    {
        auto sources = std::move(m_editorCtx->pendingDuplications);
        m_editorCtx->pendingDuplications.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        m_editorCtx->ClearSelection();
        for (auto src : sources)
        {
            entt::entity copy = SceneSerializer::DuplicateEntity(
                *m_scene, src, PathResolver::AssetsDir());
            if (copy == entt::null) continue;

            m_editorCtx->AddToSelection(copy);
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnEntityCommand>(
                    m_scene.get(), PathResolver::AssetsDir(), copy));
            Logger::Info("Duplicated entity: {}",
                         m_scene->GetRegistry().get<NameTag>(copy).name);
        }
    }

    // エンティティペースト（Ctrl+V。コピー時の JSON スナップショットから生成）
    if (!m_editorCtx->pendingPastes.empty() && m_engineMode == EngineMode::Editor)
    {
        auto pastes = std::move(m_editorCtx->pendingPastes);
        m_editorCtx->pendingPastes.clear();

        m_scene->Initialize(m_resourceManager.get(), m_graphicsDevice.get(),
                            m_srvHeap.get(), nativeCmdList);

        m_editorCtx->ClearSelection();
        for (const auto& snap : pastes)
        {
            entt::entity e = SceneSerializer::InstantiateEntity(
                *m_scene, snap, PathResolver::AssetsDir());
            if (e == entt::null) continue;

            auto& reg = m_scene->GetRegistry();
            // 元と重ならないよう少しずらして配置
            if (reg.all_of<Transform>(e))
                reg.get<Transform>(e).position.x += 1.0f;

            m_editorCtx->AddToSelection(e);
            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<SpawnEntityCommand>(
                    m_scene.get(), PathResolver::AssetsDir(), e));
            Logger::Info("Pasted entity: {}", reg.get<NameTag>(e).name);
        }
    }

    // スクリプトアタッチ遅延処理
    if (!m_editorCtx->pendingScriptAttachments.empty())
    {
        auto attachments = std::move(m_editorCtx->pendingScriptAttachments);
        m_editorCtx->pendingScriptAttachments.clear();

        char dbgBuf[128];
        snprintf(dbgBuf, sizeof(dbgBuf),
            "[PendingScriptAttachments] processing %zu\n", attachments.size());
        OutputDebugStringA(dbgBuf);

        auto& reg = m_scene->GetRegistry();
        for (const auto& req : attachments)
        {
            if (!reg.valid(req.entity))
            {
                OutputDebugStringA("[PendingScriptAttachments] SKIP invalid entity\n");
                continue;
            }

            // Undo 用に現状を保存
            bool        hadBefore  = reg.all_of<LuaScript>(req.entity);
            std::string oldPath;
            bool        oldEnabled = true;
            if (hadBefore)
            {
                const auto& cur = reg.get<LuaScript>(req.entity);
                oldPath    = cur.scriptPath;
                oldEnabled = cur.enabled;
            }

            m_scriptEngine->AttachScriptToEntity(req.entity, req.scriptPath);

            m_editorCtx->undoSystem.PushCommand(
                std::make_unique<AttachScriptCommand>(
                    &reg, req.entity,
                    hadBefore, oldPath, oldEnabled,
                    req.scriptPath));
        }
    }

    // サムネイルテクスチャのロード（描画コマンドの前に実行）
    m_editorLayer->LoadPendingThumbnails(nativeCmdList);

    // モデルサムネイルのオフスクリーンレンダリング
    m_thumbRenderer->RenderPending(nativeCmdList, m_swapChain->GetCurrentBackBufferIndex());

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

    // ライトの向きを Transform 回転に追従させる（回転の変化分=デルタを direction に適用）。
    // これでインスペクターの Transform 回転でもギズモ回転でも光の向きが変わる。
    // direction を真実とし回転はデルタのみ与えるので、Direction 欄の直接編集とも共存できる。
    {
        auto& reg = m_scene->GetRegistry();
        auto eq = [](const XMFLOAT3& r) {
            return XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(r.x), XMConvertToRadians(r.y), XMConvertToRadians(r.z));
        };
        auto applyDelta = [&](XMFLOAT3& dir, XMFLOAT3& prevRot, bool& init, const XMFLOAT3& rot) {
            if (!init) { prevRot = rot; init = true; return; }
            if (rot.x == prevRot.x && rot.y == prevRot.y && rot.z == prevRot.z) return;
            XMVECTOR delta = XMQuaternionMultiply(XMQuaternionInverse(eq(prevRot)), eq(rot));
            XMVECTOR nd = XMVector3Normalize(XMVector3Rotate(XMLoadFloat3(&dir), delta));
            XMStoreFloat3(&dir, nd);
            prevRot = rot;
        };
        for (auto [e, dl, tf] : reg.view<dx12e::DirectionalLight, const Transform>().each())
            applyDelta(dl.direction, dl._prevRot, dl._prevRotInit, tf.rotation);
        for (auto [e, sl, tf] : reg.view<dx12e::SpotLight, const Transform>().each())
            applyDelta(sl.direction, sl._prevRot, sl._prevRotInit, tf.rotation);
    }

    // ライト方向/色: ECS の DirectionalLight から取得。
    // ★ DirectionalLight が無い場合は「太陽光なし」(色=黒) にする。
    //   以前はフル強度の既定太陽にフォールバックしていたため、ライトを消しても
    //   明るいまま＆既定方向の影が出る、という分かりにくい挙動だった。
    //   ambient だけ残すので真っ暗にはならない。
    XMFLOAT3 lightDirF3   = {-0.3f, -1.0f, -0.5f};  // 影の方向（色が黒なら影は出ない）
    XMFLOAT3 lightColorF3 = {0.0f, 0.0f, 0.0f};      // 既定=太陽なし
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
            lightAmbient = dl.ambient;
        }
    }
    XMVECTOR lightDir = XMVector3Normalize(XMLoadFloat3(&lightDirF3));
    XMStoreFloat3(&lightDirF3, lightDir);  // 正規化した値を書き戻す
    // シャドウ投影はシーン全体（広めのアリーナ）を覆うサイズに。
    // 以前は 30×30(±15) しかなく、±19 の壁などが範囲外でクリップされ影が崩れていた。
    XMVECTOR lightPos = XMVectorScale(lightDir, -45.0f);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    XMMATRIX lightProj = XMMatrixOrthographicLH(56.0f, 56.0f, 0.1f, 90.0f);
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

                XMMATRIX world = (transform.parent != entt::null)
                    ? ComputeWorldMatrix(reg, e) : transform.GetWorldMatrix();

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

    // ===== メインパス（オフスクリーン RT へ描画）=====
    // 3D ビューポート矩形（エディタは中央ノード、ゲーム/Play全画面は全体）
    u32 vpLeft, vpTop, vpW, vpH;
    {
        auto vp = m_editorLayer->GetViewportPos();
        auto vs = m_editorLayer->GetViewportSize();
        vpLeft = static_cast<u32>(vp.x);
        vpTop  = static_cast<u32>(vp.y);
        vpW    = static_cast<u32>(vs.x);
        vpH    = static_cast<u32>(vs.y);
        if (vpW < 1) vpW = 1;
        if (vpH < 1) vpH = 1;
        if (m_isGameMode || m_engineMode != EngineMode::Editor)
        {
            vpLeft = 0; vpTop = 0;
            vpW = m_window->GetWidth();
            vpH = m_window->GetHeight();
        }
    }

    m_sceneRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);

    constexpr float clearColor[4] = {0.392f, 0.584f, 0.929f, 1.0f};
    m_commandList->ClearRenderTarget(m_sceneRT->GetRtv(), clearColor);
    m_commandList->ClearDepthStencil(m_dsvHandle);
    m_commandList->SetRenderTarget(m_sceneRT->GetRtv(), m_dsvHandle);
    m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

    // カメラのアスペクト比をビューポートに合わせる
    if (!m_isGameMode && m_engineMode == EngineMode::Editor)
        m_camera->SetPerspective(DirectX::XM_PIDIV4,
            static_cast<f32>(vpW) / static_cast<f32>(vpH), 0.1f, 1000.0f);

    m_commandList->SetPipelineState(*m_pipelineState);

    // PerFrame CB（PointLight / SpotLight 各最大8灯対応）
    // レイアウトは shaders/forward/Lighting.hlsli の PerFrameConstants と完全一致させること。
    static constexpr u32 kMaxPointLightsR = 8;
    static constexpr u32 kMaxSpotLightsR  = 8;
    struct PointLightGPU {
        XMFLOAT3 position;
        float range;
        XMFLOAT3 color;
        float _pad;
    };
    struct SpotLightGPU {
        XMFLOAT3 position;   float range;
        XMFLOAT3 direction;  float cosInner;
        XMFLOAT3 color;      float cosOuter;
    };
    struct FrameConstants {
        XMFLOAT4X4 view;
        XMFLOAT4X4 proj;
        XMFLOAT3   lightDir;
        float      time;
        XMFLOAT3   lightColor;
        float      ambientStrength;
        XMFLOAT4X4 lightVP;
        XMFLOAT3   cameraPos;
        float      _pad;
        u32        numPointLights;
        u32        numSpotLights;
        float      _pad2[2];
        PointLightGPU pointLights[kMaxPointLightsR];
        SpotLightGPU  spotLights[kMaxSpotLightsR];
    };

    FrameConstants fc{};
    XMStoreFloat4x4(&fc.view, XMMatrixTranspose(m_camera->GetViewMatrix()));
    XMStoreFloat4x4(&fc.proj, XMMatrixTranspose(m_camera->GetProjectionMatrix()));
    fc.lightDir = lightDirF3;
    fc.time = totalTime;
    fc.lightColor = lightColorF3;
    fc.ambientStrength = lightAmbient;
    XMStoreFloat4x4(&fc.lightVP, XMMatrixTranspose(lightViewProj));
    fc.cameraPos = m_camera->GetPosition();

    // PointLight を ECS から収集
    fc.numPointLights = 0;
    {
        auto& reg = m_scene->GetRegistry();
        auto plView = reg.view<const dx12e::PointLight, const Transform>();
        for (auto [e, pl, tf] : plView.each())
        {
            if (fc.numPointLights >= kMaxPointLightsR) break;
            auto& pld = fc.pointLights[fc.numPointLights];
            pld.position = tf.position;
            pld.range = pl.range;
            pld.color = {pl.color.x * pl.intensity,
                         pl.color.y * pl.intensity,
                         pl.color.z * pl.intensity};
            pld._pad = 0.0f;
            fc.numPointLights++;
        }
    }

    // SpotLight を ECS から収集（位置=Transform、軸=direction、内外コーン角を cos へ）
    fc.numSpotLights = 0;
    {
        auto& reg = m_scene->GetRegistry();
        auto slView = reg.view<const dx12e::SpotLight, const Transform>();
        for (auto [e, sl, tf] : slView.each())
        {
            if (fc.numSpotLights >= kMaxSpotLightsR) break;
            auto& sld = fc.spotLights[fc.numSpotLights];
            sld.position = tf.position;
            sld.range    = sl.range;

            XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&sl.direction));
            XMStoreFloat3(&sld.direction, dir);

            // outer >= inner を保証してから cos 化（cos は単調減少なので inner の cos の方が大きい）
            float outerDeg = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            sld.cosInner = std::cos(XMConvertToRadians(sl.innerConeDeg));
            sld.cosOuter = std::cos(XMConvertToRadians(outerDeg));

            sld.color = {sl.color.x * sl.intensity,
                         sl.color.y * sl.intensity,
                         sl.color.z * sl.intensity};
            fc.numSpotLights++;
        }
    }

    m_perFrameCB->Update(&fc, sizeof(fc), frameIndex);
    m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame, m_perFrameCB->GetGpuAddress(frameIndex));

    // シャドウマップSRVをバインド
    m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
        m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

    XMMATRIX viewProj = m_camera->GetViewProjMatrix();

    // 全Entityを描画（メインパス: 編集カメラ視点）
    RenderSceneMeshes(nativeCmdList, frameIndex, viewProj,
                      (m_isGameMode || m_engineMode == EngineMode::Playing));

    // ---- Physics Debug Draw（オフスクリーン RT へ）----
    if (m_physicsDebugDraw && m_physicsDebugRenderer->IsEnabled())
    {
        m_physicsDebugRenderer->BeginFrame();
        m_physicsDebugRenderer->CollectFromRegistry(m_scene->GetRegistry());

        XMFLOAT4X4 vp;
        XMStoreFloat4x4(&vp, XMMatrixTranspose(m_camera->GetViewProjMatrix()));
        m_physicsDebugRenderer->Render(nativeCmdList, vp);
    }

    // ---- パーティクル（加算ビルボード）: ゲームビューのみ、HDR scene RT + 深度へ ----
    if (m_particleSystem && (m_isGameMode || m_engineMode == EngineMode::Playing))
    {
        m_commandList->SetRenderTarget(m_sceneRT->GetRtv(), m_dsvHandle);
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
        XMMATRIX invView = XMMatrixInverse(nullptr, m_camera->GetViewMatrix());
        XMFLOAT3 camRight, camUp;
        XMStoreFloat3(&camRight, invView.r[0]);
        XMStoreFloat3(&camUp,    invView.r[1]);
        m_particleSystem->Render(nativeCmdList, m_camera->GetViewProjMatrix(), camRight, camUp);
    }

    // ===== ポストプロセス: オフスクリーン RT → バックバッファ =====
    auto* backBuffer = m_swapChain->GetCurrentBackBuffer();
    auto  rtv        = m_swapChain->GetCurrentRTV();

    m_sceneRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_commandList->TransitionResource(backBuffer,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    {
        constexpr float bbClear[4] = {0.05f, 0.05f, 0.06f, 1.0f};
        m_commandList->ClearRenderTarget(rtv, bbClear);
        nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);  // 深度なし
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);
        m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());

        const f32 fullW = static_cast<f32>(m_sceneRT->GetWidth());
        const f32 fullH = static_cast<f32>(m_sceneRT->GetHeight());

        // ポストエフェクトはゲームビュー（ゲームモード or Play 中）のみに適用する。
        // エディタのシーンビューでは素通し（enabled=false）にして編集中の見た目を素のままにする。
        // ただし Post Process ウィンドウの "Preview in Scene View" を ON にすればプレビュー可。
        PostProcessSettings ppApplied = m_scene->GetPostSettings();
        const bool isGameView = (m_isGameMode || m_engineMode == EngineMode::Playing);
        if (!isGameView && !ppApplied.previewInEditor)
            ppApplied.enabled = false;

        // ヒット時の画面インパクト（fx:pulse）: クロマ + 放射ブラーを瞬間的に上乗せ
        if (isGameView && m_particleSystem)
        {
            float pulse = m_particleSystem->GetPulse();
            if (pulse > 0.001f)
            {
                ppApplied.chromaticOn = true;
                ppApplied.chromatic   = (std::max)(ppApplied.chromatic, pulse * 1.2f);
                ppApplied.radialOn    = true;
                ppApplied.radial      = (std::max)(ppApplied.radial, pulse * 0.8f);
            }
        }

        m_postProcess->Apply(nativeCmdList,
            m_srvHeap->GetGpuHandle(m_sceneRT->GetSrvIndex()),
            ppApplied,
            static_cast<f32>(vpLeft) / fullW, static_cast<f32>(vpTop) / fullH,
            static_cast<f32>(vpW)    / fullW, static_cast<f32>(vpH)   / fullH,
            1.0f / fullW, 1.0f / fullH, totalTime);
    }

    // ---- Editor Icon Draw（ポスト後のバックバッファへ, エディタモードのみ）----
    if (m_engineMode == EngineMode::Editor && !m_isGameMode)
    {
        m_commandList->SetRenderTarget(rtv, m_dsvHandle);
        m_commandList->SetViewportAndScissor(vpLeft, vpTop, vpW, vpH);

        m_editorIconRenderer->BeginFrame();
        m_editorIconRenderer->CollectFromRegistry(m_scene->GetRegistry(), *m_editorCtx);

        XMFLOAT4X4 vpIcon;
        XMStoreFloat4x4(&vpIcon, XMMatrixTranspose(m_camera->GetViewProjMatrix()));
        m_editorIconRenderer->Render(nativeCmdList, vpIcon, vpW, vpH);
    }

    // ---- 2D スプライト / ゲーム内 UI 画像（バックバッファ全面へ）----
    if (m_spriteRenderer && (m_isGameMode || m_engineMode == EngineMode::Playing))
    {
        m_spriteRenderer->BeginFrame();
        // Lua の ui:image() コマンドをテクスチャ読み込み＋サブミット
        for (const auto& c : m_uiCommands)
        {
            if (c.type != UICommand::Type::Image || c.text.empty()) continue;
            std::wstring wpath(c.text.begin(), c.text.end());  // ASCII パス想定
            Texture* tex = m_resourceManager->GetOrLoadTexture(wpath, nativeCmdList);
            if (!tex) continue;
            SpriteDesc s;
            s.pos      = {c.x, c.y};
            s.size     = {c.w, c.h};
            s.color    = {c.r, c.g, c.b, c.a};
            s.srvIndex = tex->GetSrvIndex();
            m_spriteRenderer->Submit(s);
        }

        if (m_spriteRenderer->HasAny())
        {
            nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
            m_spriteRenderer->Render(nativeCmdList, m_window->GetWidth(), m_window->GetHeight());
        }
    }

    // ===== カメラプレビュー（選択カメラ視点を小窓へ）=====
    // 選択中エンティティにカメラがあれば、その視点でシーンを専用 RT に再描画。
    // EditorLayer がシーンビュー隅に小窓表示する（Play を押さずに見える）。
    m_editorCtx->cameraPreviewTexHandle = 0;
    if (m_engineMode == EngineMode::Editor && !m_isGameMode && m_cameraPreviewRT)
    {
        auto& reg = m_scene->GetRegistry();
        entt::entity camEnt = entt::null;
        for (auto e : m_editorCtx->selectedEntities)
            if (reg.valid(e) && reg.all_of<CameraComponent, Transform>(e)) { camEnt = e; break; }

        if (camEnt != entt::null)
        {
            const auto& tf  = reg.get<Transform>(camEnt);
            const auto& cam = reg.get<CameraComponent>(camEnt);

            // 回転（quat）からビュー行列を構築（カメラアイコン/フラスタムと同じ向き）
            XMFLOAT4 q;
            if (tf.useQuaternion)
                q = tf.quaternion;
            else
                XMStoreFloat4(&q, XMQuaternionRotationRollPitchYaw(
                    XMConvertToRadians(tf.rotation.x),
                    XMConvertToRadians(tf.rotation.y),
                    XMConvertToRadians(tf.rotation.z)));
            XMMATRIX rot  = XMMatrixRotationQuaternion(XMLoadFloat4(&q));
            XMVECTOR eye  = XMLoadFloat3(&tf.position);
            XMMATRIX view = XMMatrixLookToLH(eye, rot.r[2], rot.r[1]);

            const u32 pw = m_cameraPreviewRT->GetWidth();
            const u32 ph = m_cameraPreviewRT->GetHeight();
            XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(cam.fovDegrees),
                static_cast<f32>(pw) / static_cast<f32>(ph), cam.nearClip, cam.farClip);
            XMMATRIX camViewProj = view * proj;

            // メインパスの fc（ライト等）を流用し、視点だけ差し替えて専用 CB へ
            FrameConstants fcp = fc;
            XMStoreFloat4x4(&fcp.view, XMMatrixTranspose(view));
            XMStoreFloat4x4(&fcp.proj, XMMatrixTranspose(proj));
            fcp.cameraPos = tf.position;
            m_previewFrameCB->Update(&fcp, sizeof(fcp), frameIndex);

            m_cameraPreviewRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr float pvClear[4] = {0.392f, 0.584f, 0.929f, 1.0f};
            m_commandList->ClearRenderTarget(m_cameraPreviewRT->GetRtv(), pvClear);
            m_commandList->ClearDepthStencil(m_dsvHandle);
            m_commandList->SetRenderTarget(m_cameraPreviewRT->GetRtv(), m_dsvHandle);
            m_commandList->SetViewportAndScissor(pw, ph);

            m_commandList->SetDescriptorHeap(m_srvHeap->GetHeap());
            m_commandList->SetRootSignature(*m_rootSignature);
            m_commandList->SetPerFrameCBV(RootSignature::kSlotPerFrame,
                m_previewFrameCB->GetGpuAddress(frameIndex));
            m_commandList->SetSRVTable(RootSignature::kSlotShadowSRV,
                m_srvHeap->GetGpuHandle(m_shadowSrvIndex));

            // グリッドは出さない＝isGameView=true
            RenderSceneMeshes(nativeCmdList, frameIndex, camViewProj, true);

            m_cameraPreviewRT->Transition(*m_commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_editorCtx->cameraPreviewTexHandle =
                m_srvHeap->GetGpuHandle(m_cameraPreviewRT->GetSrvIndex()).ptr;

            // プレビュー描画でRT/ビューポートを切り替えたので、バックバッファへ戻す。
            // これをしないと直後の ImGui がプレビューRTへ描かれ、画面に出なくなる。
            m_commandList->SetRenderTarget(rtv, m_dsvHandle);
            m_commandList->SetViewportAndScissor(m_window->GetWidth(), m_window->GetHeight());
        }
    }

    // ---- ImGui フレーム ----
    m_imguiManager->BeginFrame();
    ImGuizmo::BeginFrame();

    if (!m_isGameMode && m_loading)
    {
        // ---- ローディングオーバーレイ（プロジェクト作成/読込中）----
        RenderLoadingOverlay();
    }
    else if (!m_isGameMode && m_showLauncher)
    {
        // ---- プロジェクトランチャー（起動直後 / 「ランチャーに戻る」選択時）----
        LauncherIcons li;
        li.logo        = m_icons.logo;
        li.newProject  = m_icons.newProject;
        li.openProject = m_icons.openProject;
        li.recent      = m_icons.recent;

        ProjectInfo selected;
        LauncherAction action = ProjectManager::RenderLauncher(selected, m_window->GetHwnd(), li);
        if (action == LauncherAction::CreateNew)
            BeginProjectLoad(selected, /*isNew=*/true);
        else if (action == LauncherAction::OpenExisting)
            BeginProjectLoad(selected, /*isNew=*/false);
        else if (action == LauncherAction::Skip)
        {
            LoadProject(selected);
            m_showLauncher = false;
        }
    }
    else if (!m_isGameMode)
    {
        bool pendingPlayMode = false;
        m_editorLayer->Render(
            m_engineMode == EngineMode::Playing,
            m_scene.get(), m_camera.get(), m_window.get(),
            m_scriptEngine.get(), m_audioSystem.get(),
            m_physicsDebugRenderer.get(), m_physicsDebugDraw,
            m_useVsync, m_shadowQualityIndex, m_shadowMapSize,
            m_shadowMapDirty, &m_gameClock,
            m_modeChangeRequested, pendingPlayMode,
            PathResolver::AssetsDir(), kLeftPanelWidth, kToolbarHeight);

        if (m_modeChangeRequested)
            m_pendingMode = pendingPlayMode ? EngineMode::Playing : EngineMode::Editor;

        // ---- ポストプロセス: ON/OFF ウィンドウ と パラメータ ウィンドウ ----
        {
            auto& pp = m_scene->GetPostSettings();

            // 全エフェクトのメタ情報（トグルとパラメータ描画を一元定義）
            struct PostFx {
                const char* cat;                 // カテゴリ見出し
                const char* label;               // 表示名
                const char* help;                // 説明（null可）
                bool*       on;                  // 有効フラグ
                std::function<void()> params;    // パラメータ描画
            };
            const std::vector<PostFx> fx = {
                {"カラー", "露出 Exposure", "明るさを乗算で調整", &pp.exposureOn,
                    [&]{ ImGui::SliderFloat("値##exposure", &pp.exposure, 0.1f, 4.0f, "%.2f"); }},
                {"カラー", "コントラスト Contrast", nullptr, &pp.contrastOn,
                    [&]{ ImGui::SliderFloat("値##contrast", &pp.contrast, 0.0f, 2.0f, "%.2f"); }},
                {"カラー", "明るさ Brightness", "加算で明暗を調整", &pp.brightnessOn,
                    [&]{ ImGui::SliderFloat("値##brightness", &pp.brightness, -0.5f, 0.5f, "%.2f"); }},
                {"カラー", "彩度 Saturation", nullptr, &pp.saturationOn,
                    [&]{ ImGui::SliderFloat("値##saturation", &pp.saturation, 0.0f, 2.0f, "%.2f"); }},
                {"カラー", "色温度 Warmth", "+で暖色、-で寒色", &pp.warmthOn,
                    [&]{ ImGui::SliderFloat("値##warmth", &pp.warmth, -1.0f, 1.0f, "%.2f"); }},
                {"カラー", "色相回転 Hue", "色相を回す（度）", &pp.hueOn,
                    [&]{ ImGui::SliderFloat("角度##hue", &pp.hueShift, 0.0f, 360.0f, "%.0f°"); }},
                {"カラー", "色味 Tint", "RGB を乗算", &pp.tintOn,
                    [&]{ ImGui::ColorEdit3("色##tint", &pp.tint.x); }},

                {"ブルーム/ビネット", "ブルーム Bloom", "明部がにじむ", &pp.bloomOn,
                    [&]{ ImGui::SliderFloat("強度##bloom", &pp.bloom, 0.0f, 1.0f, "%.2f");
                         ImGui::SliderFloat("しきい値##bloomth", &pp.bloomThreshold, 0.0f, 1.0f, "%.2f"); }},
                {"ブルーム/ビネット", "ビネット Vignette", "周辺減光", &pp.vignetteOn,
                    [&]{ ImGui::SliderFloat("強度##vig", &pp.vignette, 0.0f, 1.0f, "%.2f"); }},

                {"スタイライズ", "色収差 Chromatic", "画面端でRGBがズレる", &pp.chromaticOn,
                    [&]{ ImGui::SliderFloat("強度##chroma", &pp.chromatic, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "ピクセル化 Pixelize", "ブロック状にモザイク", &pp.pixelizeOn,
                    [&]{ ImGui::SliderFloat("ブロックpx##pix", &pp.pixelSize, 1.0f, 64.0f, "%.0f"); }},
                {"スタイライズ", "ポスタライズ Posterize", "色数を段階化", &pp.posterizeOn,
                    [&]{ ImGui::SliderInt("階調##post", &pp.posterize, 2, 16); }},
                {"スタイライズ", "ディザ Dither", "順序ディザで階調化", &pp.ditherOn,
                    [&]{ ImGui::SliderInt("階調##dither", &pp.ditherLevels, 2, 8); }},
                {"スタイライズ", "CRT走査線 Scanline", "走査線＋画面湾曲", &pp.scanlineOn,
                    [&]{ ImGui::SliderFloat("強度##scan", &pp.scanline, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "シャープ Sharpen", "輪郭を強調", &pp.sharpenOn,
                    [&]{ ImGui::SliderFloat("強度##sharp", &pp.sharpen, 0.0f, 1.0f, "%.2f"); }},
                {"スタイライズ", "フィルムグレイン Grain", "ザラつきノイズ", &pp.grainOn,
                    [&]{ ImGui::SliderFloat("強度##grain", &pp.grain, 0.0f, 1.0f, "%.2f"); }},

                {"カラー操作", "色反転 Invert", nullptr, &pp.invertOn,
                    [&]{ ImGui::SliderFloat("強度##inv", &pp.invert, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "セピア Sepia", nullptr, &pp.sepiaOn,
                    [&]{ ImGui::SliderFloat("強度##sepia", &pp.sepia, 0.0f, 1.0f, "%.2f"); }},
                {"カラー操作", "グレースケール Grayscale", nullptr, &pp.grayscaleOn,
                    [&]{ ImGui::SliderFloat("強度##gray", &pp.grayscale, 0.0f, 1.0f, "%.2f"); }},

                {"歪み", "レンズ歪み Lens", "バレル/魚眼", &pp.lensOn,
                    [&]{ ImGui::SliderFloat("強度##lens", &pp.lens, -1.0f, 1.0f, "%.2f"); }},
                {"歪み", "波ゆらぎ Wave", "水中/陽炎のゆれ", &pp.waveOn,
                    [&]{ ImGui::SliderFloat("振幅##wamp", &pp.waveAmp, 0.0f, 0.05f, "%.3f");
                         ImGui::SliderFloat("周波数##wfreq", &pp.waveFreq, 1.0f, 40.0f, "%.1f");
                         ImGui::SliderFloat("速度##wspd", &pp.waveSpeed, 0.0f, 8.0f, "%.1f"); }},
                {"歪み", "放射ブラー Radial", "中心へズームブラー", &pp.radialOn,
                    [&]{ ImGui::SliderFloat("強度##rad", &pp.radial, 0.0f, 1.0f, "%.2f"); }},
                {"歪み", "グリッチ Glitch", "デジタル乱れ", &pp.glitchOn,
                    [&]{ ImGui::SliderFloat("強度##glitch", &pp.glitch, 0.0f, 1.0f, "%.2f"); }},

                {"輪郭", "輪郭線 Outline", "Sobelエッジ検出", &pp.outlineOn,
                    [&]{ ImGui::SliderFloat("強度##outl", &pp.outline, 0.0f, 4.0f, "%.2f");
                         ImGui::ColorEdit3("線の色##outlc", &pp.outlineColor.x); }},

                {"アンチエイリアス", "FXAA", "簡易アンチエイリアス", &pp.fxaaOn, {}},
            };

            // ===== ウィンドウ1: ON/OFF チェックリスト =====
            ImGui::Begin("Post Process");
            ImGui::Checkbox("有効（マスター）", &pp.enabled);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::Checkbox("シーンビューでもプレビュー", &pp.previewInEditor);
            ImGui::TextDisabled("通常はゲームビューのみ適用 / 各効果のパラメータは「Post Process パラメータ」窓で");
            ImGui::Separator();

            ImGui::BeginDisabled(!pp.enabled);
            const char* curCat = nullptr;
            int enabledCount = 0;
            for (const auto& f : fx)
            {
                if (curCat == nullptr || std::strcmp(curCat, f.cat) != 0)
                {
                    curCat = f.cat;
                    ImGui::SeparatorText(curCat);
                }
                ImGui::Checkbox(f.label, f.on);
                if (f.help)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::BeginItemTooltip())
                    { ImGui::TextUnformatted(f.help); ImGui::EndTooltip(); }
                }
                if (*f.on) ++enabledCount;
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::Button("すべてOFF"))
                for (const auto& f : fx) *f.on = false;
            ImGui::SameLine();
            if (ImGui::Button("初期値に戻す"))
            { bool prev = pp.previewInEditor; pp = PostProcessSettings{}; pp.previewInEditor = prev; }
            ImGui::SameLine();
            ImGui::TextDisabled("有効中: %d", enabledCount);
            ImGui::End();

            // ===== ウィンドウ2: 有効なエフェクトのパラメータ =====
            ImGui::Begin("Post Process パラメータ");
            if (!pp.enabled)
                ImGui::TextDisabled("マスターが OFF です（Post Process 窓で有効化）");
            else if (enabledCount == 0)
                ImGui::TextDisabled("エフェクトを有効にすると、ここに調整項目が出ます");
            else
            {
                ImGui::PushItemWidth(-120.0f);
                curCat = nullptr;
                for (const auto& f : fx)
                {
                    if (!*f.on || !f.params) continue;
                    ImGui::SeparatorText(f.label);
                    ImGui::PushID(f.label);
                    f.params();
                    ImGui::PopID();
                }
                ImGui::PopItemWidth();
            }
            ImGui::End();
        }

        // ---- Scene Flow 設定ウィンドウ（シーンの流れ）----
        if (m_sceneFlow)
        {
            namespace fs = std::filesystem;
            std::vector<std::string> scenes;
            std::string scenesDir = PathResolver::AssetsDir() + "scenes";
            if (fs::exists(scenesDir))
            {
                for (auto& e : fs::directory_iterator(scenesDir))
                    if (e.is_regular_file() && e.path().extension() == ".json")
                        scenes.push_back("scenes/" + e.path().filename().string());
            }

            ImGui::Begin("Scene Flow");
            ImGui::TextWrapped("ゲーム開始シーンと、各シーンの次シーンを設定する。");

            std::string start = m_sceneFlow->Start();
            if (ImGui::BeginCombo("Start Scene", start.empty() ? "(none)" : start.c_str()))
            {
                for (auto& s : scenes)
                    if (ImGui::Selectable(s.c_str(), s == start))
                        m_sceneFlow->SetStart(s);
                ImGui::EndCombo();
            }

            ImGui::SeparatorText("Next scene");
            for (auto& s : scenes)
            {
                std::string nx = m_sceneFlow->Next(s);
                ImGui::PushID(s.c_str());
                if (ImGui::BeginCombo(s.c_str(), nx.empty() ? "(none)" : nx.c_str()))
                {
                    if (ImGui::Selectable("(none)", nx.empty()))
                        m_sceneFlow->SetNext(s, "");
                    for (auto& t : scenes)
                        if (ImGui::Selectable(t.c_str(), t == nx))
                            m_sceneFlow->SetNext(s, t);
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Save sceneflow.json"))
                m_sceneFlow->Save(PathResolver::AssetsDir() + "sceneflow.json");
            ImGui::End();
        }

        // Deferred: game build
        if (m_editorCtx->pendingBuildGame)
        {
            m_editorCtx->pendingBuildGame = false;
            BuildGame();
            m_editorCtx->buildCompleteFlash = 3.0f;
        }

        // Deferred: entity deletion
        if (!m_editorCtx->pendingDeletions.empty())
        {
            auto deletions = std::move(m_editorCtx->pendingDeletions);
            m_editorCtx->pendingDeletions.clear();
            for (auto root : deletions)
            {
                auto& reg = m_scene->GetRegistry();
                if (!reg.valid(root)) continue;  // 先行削除のサブツリーに含まれていた場合

                // サブツリー収集（親→子の順。BFS）
                std::vector<entt::entity> subtree{root};
                for (size_t i = 0; i < subtree.size(); ++i)
                {
                    for (auto [c, t] : reg.view<const Transform>().each())
                    {
                        if (t.parent == subtree[i])
                            subtree.push_back(c);
                    }
                }

                // Undo 用スナップショット（全コンポーネント + ローカル親インデックス）
                std::vector<DeletedEntityRecord> records;
                records.reserve(subtree.size());
                entt::entity externalParent = reg.all_of<Transform>(root)
                    ? reg.get<Transform>(root).parent : entt::null;
                for (auto e : subtree)
                {
                    DeletedEntityRecord rec;
                    rec.snapshot = SceneSerializer::SerializeEntity(
                        *m_scene, e, PathResolver::AssetsDir());
                    if (reg.all_of<Transform>(e))
                    {
                        auto parent = reg.get<Transform>(e).parent;
                        auto it = std::find(subtree.begin(), subtree.end(), parent);
                        if (it != subtree.end())
                            rec.parentLocalIndex = static_cast<int>(it - subtree.begin());
                    }
                    records.push_back(std::move(rec));
                }

                m_editorCtx->undoSystem.PushCommand(
                    std::make_unique<DeleteEntityCommand>(
                        m_scene.get(), PathResolver::AssetsDir(),
                        std::move(records), subtree, externalParent));

                // 子から順に削除
                for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
                {
                    if (reg.valid(*it))
                        m_scene->Remove(Entity(*it, &reg));
                }
            }

            // 削除で無効になった選択をクリーンアップ
            {
                auto& reg = m_scene->GetRegistry();
                auto& sel = m_editorCtx->selectedEntities;
                sel.erase(std::remove_if(sel.begin(), sel.end(),
                          [&](entt::entity e) { return !reg.valid(e); }),
                          sel.end());
                if (m_editorCtx->selectedEntity != entt::null
                    && !reg.valid(m_editorCtx->selectedEntity))
                {
                    m_editorCtx->selectedEntity =
                        sel.empty() ? entt::null : sel.back();
                }
            }
        }

        // ---- プロジェクト / バージョン管理(Git) ウィンドウ ----
        RenderProjectWindow();
        RenderVersionControlWindow();
    }

    // ---- ゲーム内 UI: テキスト/ボタン（ImGui オーバーレイ・ゲーム/Play 中のみ）----
    if (m_isGameMode || m_engineMode == EngineMode::Playing)
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##GameUI", nullptr, flags);

        auto* dl = ImGui::GetWindowDrawList();
        std::unordered_set<std::string> nowPressed;
        for (const auto& c : m_uiCommands)
        {
            if (c.type == UICommand::Type::Text)
            {
                ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a));
                dl->AddText(ImGui::GetFont(), c.size, ImVec2(c.x, c.y), col, c.text.c_str());
            }
            else if (c.type == UICommand::Type::Button)
            {
                ImGui::SetCursorPos(ImVec2(c.x, c.y));
                if (ImGui::Button(c.text.c_str(), ImVec2(c.w, c.h)))
                    nowPressed.insert(c.text);
            }
        }
        ImGui::End();
        m_pressedButtons = std::move(nowPressed);
    }
    else
    {
        m_pressedButtons.clear();
    }
    m_uiCommands.clear();

    m_imguiManager->EndFrame(nativeCmdList);

    // ---- シーントランジション オーバーレイ（ImGui の上に被せる）----
    // フェードは 3D ビューにだけ適用する:
    //   エディタ/Play 中 … 中央ビューポート矩形だけにスシザーを絞り、周りの
    //                      エディタUI（パネル/ツールバー）には掛からないようにする。
    //   ゲーム単体       … ウィンドウ全体。
    if (m_sceneTransition && m_sceneTransition->IsActive())
    {
        u32 tLeft = 0, tTop = 0;
        u32 tW = m_window->GetWidth(), tH = m_window->GetHeight();
        if (!m_isGameMode && m_editorLayer)
        {
            auto vpos  = m_editorLayer->GetViewportPos();
            auto vsize = m_editorLayer->GetViewportSize();
            tLeft = static_cast<u32>(vpos.x);
            tTop  = static_cast<u32>(vpos.y);
            tW    = static_cast<u32>(vsize.x);
            tH    = static_cast<u32>(vsize.y);
            if (tW < 1) tW = 1;
            if (tH < 1) tH = 1;
        }

        nativeCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_commandList->SetViewportAndScissor(tLeft, tTop, tW, tH);
        float aspect = (tH > 0) ? static_cast<f32>(tW) / static_cast<f32>(tH) : 1.0f;
        m_sceneTransition->Render(nativeCmdList, aspect);
    }

    m_commandList->TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->Close();

    m_commandQueue->ExecuteCommandList(nativeCmdList);
    m_swapChain->Present(m_useVsync);
    m_frameResources->EndFrame(*m_commandQueue);

    // GPU がこのフレームのコピーを完了してからアップロードバッファを解放する
    // (モデル/テクスチャスポーンで積んだ CopyResource が in-flight のうちに
    //  Reset すると OBJECT_DELETED_WHILE_STILL_IN_USE で落ちるため)
    m_commandQueue->WaitIdle();
    m_resourceManager->FinishUploads();
}

} // namespace dx12e
