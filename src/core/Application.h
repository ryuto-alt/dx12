#pragma once

#include "Types.h"
#include "Window.h"
#include "GameClock.h"

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <filesystem>
#include <wrl/client.h>
#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include <array>
#include <thread>
#include <atomic>
#include "ecs/Components.h"
#include "project/Project.h"
#include "editor/EditorIcons.h"

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
    class RenderTarget;
    class PostProcess;
    class ParticleSystem;
    class SpriteRenderer;
    class SceneTransition;
    class SceneFlow;
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
    struct Material;
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

    // ヘッドレスでゲームをビルド（--build CLI 用）。開始シーンは title.json があればそれ。
    void BuildGameStandalone();

    enum class EngineMode { Editor, Playing };

private:
    void Update();
    void Render();
    // シーン内の全メッシュを指定 viewProj で描画（メインパスとカメラプレビューで共用）。
    // isGameView=true でエディタ用グリッドを除外。per-frame CB / シャドウSRV /
    // ルートシグネチャ / RT / ビューポートは呼び出し側で設定済みとする。
    void RenderSceneMeshes(ID3D12GraphicsCommandList* nativeCmdList, u32 frameIndex,
                           DirectX::XMMATRIX viewProj, bool isGameView);
    void RebuildScene();
    // ランチャーで選んだ/作成したプロジェクトを実行時に読み込む（パス再ポイント + シーンロード）
    void LoadProject(const ProjectInfo& info);
    // エディタUIアイコン(PNG)をSRVへ読み込む（起動時に1度。エンジン側assets基準）
    void LoadEditorIcons(ID3D12GraphicsCommandList* cmdList);
    // 非同期プロジェクトロード: 作成/読込のCPU処理をワーカーで回しローディング表示
    void BeginProjectLoad(const ProjectInfo& info, bool isNew);
    void UpdateProjectLoad(f32 dt);   // 毎フレーム状態機械を進める（!m_loading なら何もしない）
    void RenderLoadingOverlay();      // ローディングオーバーレイ描画
    // エディタの「Project」「Version Control」ウィンドウ描画（ランチャー閉後）
    void RenderProjectWindow();
    void RenderVersionControlWindow();
    // 現在のプロジェクトを保存（.dx12proj + 現在シーン）
    void SaveCurrentProject();
    void EnterPlayMode();
    void EnterEditorMode();
    void BuildGame();

    // Lua の loadScene/nextScene/quit/ui コールバックを ScriptEngine に注入（再生成のたび呼ぶ）
    void WireScriptCallbacks();
    // アクティブな CameraComponent をグローバル Camera に同期（Play 開始 / loadScene 後）
    void SyncActiveCameraToGlobal();
    // Play 中のシーン切替（フレーム境界で安全に実行）
    void DoRuntimeSceneLoad(const std::string& assetsRelPath, ID3D12GraphicsCommandList* cmdList);

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
    std::unique_ptr<PipelineState>     m_emissivePipelineState;  // 加算発光パーティクル用
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
    static constexpr f32 kToolbarHeight   = 60.0f;  // メニューバー + アイコン列の2段
    bool m_isGameMode = false;
    std::unique_ptr<EditorContext> m_editorCtx;
    std::unique_ptr<EditorLayer>   m_editorLayer;
    std::unique_ptr<ModelThumbnailRenderer> m_thumbRenderer;
    bool m_showLauncher = true;  // プロジェクトランチャー表示フラグ

    // ---- プロジェクト管理 / バージョン管理(Git) ----
    ProjectInfo m_projectInfo;            // 現在開いているプロジェクト（rootDir 空 = 組み込みパス）
    bool        m_gitChecked   = false;   // git/gh の存在チェック済みか
    bool        m_gitAvailable = false;
    bool        m_ghAvailable  = false;
    std::array<char, 512>  m_gitRemoteBuf{};    // リモート URL 入力
    std::array<char, 256>  m_gitCommitMsgBuf{}; // コミットメッセージ入力
    std::string m_gitOutput;              // 直近コマンドの出力ログ
    bool        m_gitRepoCache = false;   // IsRepo の簡易キャッシュ
    std::string m_gitBranchCache;         // ブランチ名キャッシュ
    std::string m_gitRemoteCache;         // origin URL キャッシュ
    bool        m_gitForceRefresh = false; // 操作直後に git 状態を再取得するワンショットフラグ

    // GitHub ログイン状態（gh）
    std::string m_ghUser;                 // gh ログインユーザー（空=未ログイン）
    bool        m_ghUserChecked = false;  // ログイン状態を取得済みか（重いので一度だけ）

    // ブランチ操作
    std::vector<std::string> m_gitBranches;     // ローカルブランチ一覧
    std::array<char, 128>    m_gitNewBranchBuf{}; // 新規ブランチ名入力

    // ---- エディタUIアイコン（ImTextureID=ImU64。0=未読込。EditorContext::icons から参照される）----
    EditorUiIcons m_icons;

    // ---- 非同期プロジェクトロード ----
    std::thread        m_loadThread;
    std::atomic<bool>  m_loadThreadDone{false};
    bool               m_loadThreadRunning = false;
    bool               m_loading           = false;  // ローディングオーバーレイ表示中
    bool               m_loadProjectStarted = false; // LoadProject 発火済みか
    bool               m_loadIsNew         = false;
    int                m_loadSceneWaitFrames = 0;
    f32                m_loadSpinTime      = 0.0f;
    std::string        m_loadStatus;
    ProjectInfo        m_loadInfo;
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

    // オフスクリーン描画 + ポストプロセス（WP3）
    std::unique_ptr<DescriptorHeap> m_offscreenRtvHeap;
    std::unique_ptr<RenderTarget>   m_sceneRT;
    std::unique_ptr<PostProcess>    m_postProcess;
    std::unique_ptr<ParticleSystem> m_particleSystem;  // 加算ビルボードパーティクル（Lua fx API）

    // カメラプレビュー（選択カメラ視点を小窓表示）。専用 RT + 専用 per-frame CB。
    std::unique_ptr<RenderTarget>   m_cameraPreviewRT;
    std::unique_ptr<ConstantBuffer> m_previewFrameCB;

    // 2D スプライト / ゲーム内 UI 描画（WP4 / WP7）
    std::unique_ptr<SpriteRenderer> m_spriteRenderer;

    // ゲーム内 UI（Lua 即時モード）コマンドバッファ（WP7）
    struct UICommand
    {
        enum class Type { Text, Button, Image, Rect } type = Type::Text;
        float x = 0, y = 0, w = 0, h = 0, size = 24.0f;  // Rect では size=角丸半径
        float r = 1, g = 1, b = 1, a = 1;
        std::string text;  // text 内容 / button ラベル / image パス
    };
    std::vector<UICommand>          m_uiCommands;     // 今フレームの UI 描画要求
    std::unordered_set<std::string> m_pressedButtons; // 前フレームに押されたボタンのラベル

    // シーントランジション（WP9）
    std::unique_ptr<SceneTransition> m_sceneTransition;
    std::string                      m_transitionTargetScene;  // 中間点でロードする assets 相対パス（空=ロード無し）

    // シーンフロー（WP6）
    std::unique_ptr<SceneFlow> m_sceneFlow;
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

    // 現在ロード中シーンの assets 相対パス（シーンフロー / loadScene 用）
    std::string m_currentSceneRel;

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
