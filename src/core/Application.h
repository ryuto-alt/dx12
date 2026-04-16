#pragma once

#include "Types.h"
#include "Window.h"
#include "GameClock.h"

#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "ecs/Components.h"

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
    class PhysicsSystem;
    class PhysicsDebugRenderer;
    class EditorIconRenderer;
    class EditorContext;
    class EditorLayer;
    class ModelThumbnailRenderer;
    class RenderTarget;
    class GameViewPanel;
    struct Material;
    struct ProjectInfo;
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

    void Initialize(HINSTANCE hInstance, int nCmdShow, bool gameMode = false,
                    const ProjectInfo* projectInfo = nullptr);
    void Run();
    void Shutdown();

    enum class EngineMode { Editor, Playing };

private:
    // 描画パスごとのオプションフラグ。
    // デフォルトは「最終ゲーム画面」相当の最小描画 (GameView 用)。
    // SceneView では各 true を明示的に有効化する。
    struct SceneRenderFlags {
        bool drawGrid          = false;
        bool drawIcons         = false;
        bool drawPhysicsDebug  = false;
    };

    // CameraComponent(isActive=true) から毎フレーム算出される視点情報
    struct GameCameraView {
        DirectX::XMFLOAT3   position{};
        DirectX::XMFLOAT4X4 view{};       // row-major (転置前)
        DirectX::XMFLOAT4X4 proj{};       // row-major (転置前)
        bool valid = false;
    };

    void Update();
    void Render();
    void RebuildScene();
    void EnterPlayMode();
    void EnterEditorMode();
    void BuildGame();

    // SceneView と GameView の両方が呼ぶ共通描画メソッド
    void RenderSceneToTarget(ID3D12GraphicsCommandList* nativeCmdList,
                             const DirectX::XMMATRIX& viewMat,
                             const DirectX::XMMATRIX& projMat,
                             const DirectX::XMFLOAT3& cameraPos,
                             ConstantBuffer* perFrameCB,
                             u32 frameIndex,
                             const DirectX::XMMATRIX& lightViewProj,
                             const DirectX::XMFLOAT3& lightDir,
                             const DirectX::XMFLOAT3& lightColor,
                             f32 lightAmbient,
                             f32 totalTime,
                             SceneRenderFlags flags);

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

    // エディタ/フォールバック用カメラのデフォルトパラメータ
    // (CameraComponent を持たない GameView は本値で投影行列を作る)
    static constexpr f32 kDefaultFovYRad = DirectX::XM_PIDIV4;  // 45度
    static constexpr f32 kDefaultNearZ   = 0.1f;
    static constexpr f32 kDefaultFarZ    = 1000.0f;
    bool m_isGameMode = false;
    std::unique_ptr<EditorContext> m_editorCtx;
    std::unique_ptr<EditorLayer>   m_editorLayer;
    std::unique_ptr<ModelThumbnailRenderer> m_thumbRenderer;

    // GameView: シーンに置かれた CameraComponent(isActive=true) の視点を描画する別 RT
    std::unique_ptr<RenderTarget>   m_gameViewRT;
    std::unique_ptr<ConstantBuffer> m_gameViewPerFrameCB;
    GameCameraView                  m_gameCameraView{};
    bool m_showLauncher = true;  // プロジェクトランチャー表示フラグ
    std::unique_ptr<Camera>            m_camera;
    std::unique_ptr<ConstantBuffer>    m_perFrameCB;
    std::unique_ptr<CommandList>       m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE        m_dsvHandle{};
    std::unique_ptr<InputSystem>       m_inputSystem;
    std::unique_ptr<Scene>             m_scene;
    std::unique_ptr<ScriptEngine>      m_scriptEngine;
    std::unique_ptr<AudioSystem>       m_audioSystem;
    std::unique_ptr<PhysicsSystem>     m_physicsSystem;
    std::unique_ptr<PhysicsDebugRenderer> m_physicsDebugRenderer;
    std::unique_ptr<EditorIconRenderer>   m_editorIconRenderer;
    bool                               m_physicsDebugDraw = false;
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

    // OnPlayStart 直後に Lua が変更した値をエディタ配置値で打ち消すための即時上書き用スナップショット。
    // Stop 時の完全復元には使わない（そちらは m_playSceneJson 経由）
    struct EntitySnapshot {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 rotation;
        DirectX::XMFLOAT3 scale;
        DirectX::XMFLOAT4 quaternion;
        bool useQuaternion;

        bool      hasRigidBody = false;
        RigidBody rigidBodyData;

        float materialMetallic  = 1.0f;
        float materialRoughness = 1.0f;
    };
    std::unordered_map<std::string, EntitySnapshot> m_editorSnapshots;

    // Play 開始時のシーン全体スナップショット（Stop 時の復元用）
    std::string m_playSceneJson;

    // Luaホットリロード
    std::filesystem::file_time_type m_scriptLastWriteTime{};
    f32 m_scriptPollTimer = 0.0f;
    static constexpr f32 kScriptPollInterval = 0.5f;

    // フレームレートリミッター
    static constexpr f32 kTargetFps = 144.0f;
    bool m_useVsync = false;
    std::chrono::high_resolution_clock::time_point m_frameStart{};
};

} // namespace dx12e
