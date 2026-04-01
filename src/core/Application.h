#pragma once

#include "Types.h"
#include "Window.h"
#include "GameClock.h"

#include <memory>
#include <vector>
#include <chrono>
#include <filesystem>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>

// Forward declarations for graphics module
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

    void Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode = false);
    void Run();
    void Shutdown();

    enum class EngineMode { Editor, Playing };
    enum class GizmoMode { Translate, Rotate, Scale };

private:
    void Update();
    void Render();
    void RebuildScene();
    void EnterPlayMode();
    void EnterEditorMode();
    void BuildGame();

    std::unique_ptr<Window>         m_window;
    std::unique_ptr<GraphicsDevice> m_graphicsDevice;
    std::unique_ptr<CommandQueue>   m_commandQueue;
    std::unique_ptr<SwapChain>      m_swapChain;
    std::unique_ptr<FrameResources> m_frameResources;
    std::unique_ptr<DescriptorHeap>    m_descriptorHeap;
    std::unique_ptr<DescriptorHeap>    m_dsvHeap;
    std::unique_ptr<RootSignature>     m_rootSignature;
    std::unique_ptr<PipelineState>     m_pipelineState;
    std::unique_ptr<DescriptorHeap>    m_srvHeap;
    std::unique_ptr<ResourceManager>   m_resourceManager;
    std::unique_ptr<ImGuiManager>      m_imguiManager;
    std::unique_ptr<PipelineState>     m_skinnedPipelineState;
    std::unique_ptr<PipelineState>     m_gridPipelineState;
    std::unique_ptr<PipelineState>     m_shadowPipelineState;
    std::unique_ptr<PipelineState>     m_shadowSkinnedPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    std::unique_ptr<DescriptorHeap>    m_shadowDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_shadowDsvHandle{};
    u32                                m_shadowSrvIndex = 0;
    u32                                m_shadowMapSize = 4096;
    i32                                m_shadowQualityIndex = 2;  // 0:1024, 1:2048, 2:4096, 3:8192
    bool                               m_shadowMapDirty = false;
    // エディタレイアウト
    static constexpr f32 kLeftPanelWidth  = 280.0f;
    static constexpr f32 kToolbarHeight   = 36.0f;
    entt::entity m_selectedEntity = entt::null;
    GizmoMode m_gizmoMode = GizmoMode::Translate;
    bool m_gizmoLocalSpace = false;
    bool m_isGameMode = false;
    std::unique_ptr<Camera>            m_camera;
    std::unique_ptr<ConstantBuffer>    m_perFrameCB;
    std::unique_ptr<CommandList>       m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_dsvHandle{};
    std::unique_ptr<InputSystem>       m_inputSystem;
    std::unique_ptr<Scene>             m_scene;
    std::unique_ptr<ScriptEngine>      m_scriptEngine;
    std::unique_ptr<AudioSystem>       m_audioSystem;
    GameClock                          m_gameClock;
    bool                               m_isRunning = false;
    u32                                m_framesSinceStart = 0;

    // エディタ/プレイモード
    EngineMode m_engineMode = EngineMode::Editor;
    EngineMode m_pendingMode = EngineMode::Editor;
    bool m_modeChangeRequested = false;
    struct CameraSnapshot {
        DirectX::XMFLOAT3 position;
        f32 yaw;
        f32 pitch;
    } m_cameraSnapshot{};

    // Luaホットリロード
    std::filesystem::file_time_type m_scriptLastWriteTime{};
    f32 m_scriptPollTimer = 0.0f;
    f32 m_hotReloadFlash = 0.0f;
    f32 m_buildCompleteFlash = 0.0f;
    static constexpr f32 kScriptPollInterval = 0.5f;

    // フレームレートリミッター
    static constexpr f32 kTargetFps = 144.0f;
    bool m_useVsync = false;
    std::chrono::high_resolution_clock::time_point m_frameStart{};
};

} // namespace dx12e
