#pragma once

#include "Types.h"
#include "Window.h"
#include "GameClock.h"

#include <memory>
#include <chrono>
#include <filesystem>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "ecs/Components.h"

namespace dx12e
{
    class GraphicsDevice;
    class CommandQueue;
    class SwapChain;
    class FrameResources;
    class DescriptorHeap;
    class RootSignature;
    class PipelineState;
    class CommandList;
    class ConstantBuffer;
    class Camera;
    class ResourceManager;
    class InputSystem;
    class ImGuiManager;
    class Scene;
    class ScriptEngine;
    class AudioSystem;
    class PhysicsSystem;
    class PhysicsDebugRenderer;
}

namespace dx12e
{

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize(HINSTANCE hInstance, int nCmdShow);
    void Run();
    void Shutdown();

private:
    void Update();
    void Render();
    void RebuildScene();  // game.lua ホットリロード時にシーンを再構築

    // Window / DX12 core
    std::unique_ptr<Window>            m_window;
    std::unique_ptr<GraphicsDevice>    m_graphicsDevice;
    std::unique_ptr<CommandQueue>      m_commandQueue;
    std::unique_ptr<SwapChain>         m_swapChain;
    std::unique_ptr<FrameResources>    m_frameResources;
    std::unique_ptr<DescriptorHeap>    m_descriptorHeap;
    std::unique_ptr<DescriptorHeap>    m_dsvHeap;
    std::unique_ptr<DescriptorHeap>    m_srvHeap;
    std::unique_ptr<CommandList>       m_commandList;

    // Pipeline
    std::unique_ptr<RootSignature>     m_rootSignature;
    std::unique_ptr<PipelineState>     m_pipelineState;
    std::unique_ptr<PipelineState>     m_skinnedPipelineState;
    std::unique_ptr<PipelineState>     m_gridPipelineState;
    std::unique_ptr<PipelineState>     m_shadowPipelineState;
    std::unique_ptr<PipelineState>     m_shadowSkinnedPipelineState;

    // Depth buffer (バックバッファ用)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_dsvHandle{};

    // Shadow map
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    std::unique_ptr<DescriptorHeap>    m_shadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_shadowDsvHandle{};
    u32                                m_shadowSrvIndex = 0;
    static constexpr u32               kShadowMapSize = 4096;

    // ConstantBuffers
    std::unique_ptr<ConstantBuffer>    m_perFrameCB;

    // Resources
    std::unique_ptr<ResourceManager>   m_resourceManager;

    // Camera (free WASD/mouse cam, used as fallback when no CameraComponent(isActive=true))
    std::unique_ptr<Camera>            m_camera;

    // Game systems
    std::unique_ptr<InputSystem>       m_inputSystem;
    std::unique_ptr<Scene>             m_scene;
    std::unique_ptr<ScriptEngine>      m_scriptEngine;
    std::unique_ptr<AudioSystem>       m_audioSystem;
    std::unique_ptr<PhysicsSystem>     m_physicsSystem;
    std::unique_ptr<PhysicsDebugRenderer> m_physicsDebugRenderer;

    // ImGui (将来のデバッグオーバーレイ用に保持。現在は init/begin/end の薄いラッパー)
    std::unique_ptr<ImGuiManager>      m_imguiManager;

    // クロック・状態
    GameClock                          m_gameClock;
    bool                               m_isRunning = false;
    u32                                m_framesSinceStart = 0;

    // カメラのデフォルトパラメータ
    static constexpr f32 kDefaultFovYRad = DirectX::XM_PIDIV4;
    static constexpr f32 kDefaultNearZ   = 0.1f;
    static constexpr f32 kDefaultFarZ    = 1000.0f;

    // Lua ホットリロード
    std::filesystem::file_time_type m_scriptLastWriteTime{};
    f32 m_scriptPollTimer = 0.0f;
    static constexpr f32 kScriptPollInterval = 0.5f;

    // フレームレートリミッター
    static constexpr f32 kTargetFps = 144.0f;
    bool m_useVsync = false;
    std::chrono::high_resolution_clock::time_point m_frameStart{};
};

} // namespace dx12e
