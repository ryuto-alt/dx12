#pragma once

#include <memory>
#include <string>
#include "core/Types.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class EditorContext;
class Scene;
class Camera;
class Window;
class ScriptEngine;
class AudioSystem;
class PhysicsDebugRenderer;
class GameClock;
class ResourceManager;
class DescriptorHeap;

class ToolbarPanel;
class HierarchyPanel;
class InspectorPanel;
class SceneViewPanel;
class AssetBrowserPanel;
class GameViewPanel;

class EditorLayer
{
public:
    EditorLayer();
    ~EditorLayer();

    void Initialize(EditorContext* ctx,
                    const std::string& assetsDir,
                    const std::string& scriptsDir,
                    ResourceManager* resourceManager,
                    DescriptorHeap* srvHeap);

    void Render(bool isPlaying,
                Scene* scene,
                Camera* camera,
                Window* window,
                ScriptEngine* scriptEngine,
                AudioSystem* audioSystem,
                PhysicsDebugRenderer* physicsDebugRenderer,
                bool& physicsDebugDraw,
                bool& useVsync,
                i32& shadowQualityIndex,
                u32& shadowMapSize,
                bool& shadowMapDirty,
                GameClock* clock,
                bool& outModeChangeRequested,
                bool& outPendingPlayMode,
                const std::string& assetsDir,
                f32 leftPanelWidth,
                f32 toolbarHeight,
                u64 sceneViewTextureId,
                u64 gameViewTextureId);

    // フレーム先頭でcmdListが有効な間に呼ぶ（テクスチャサムネイルのアップロード）
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    // アセットブラウザの即時更新
    void RefreshAssetBrowser();

    // サムネイルレンダラー設定
    void SetThumbnailRenderer(class ModelThumbnailRenderer* renderer);

    // SceneView パネルのコンテンツサイズ（Application が SceneView RT のリサイズ判定に使用）
    ImVec2 GetSceneViewSize() const { return m_sceneViewSize; }
    // SceneView タブがホバーされてるか (Application::Update がカメラ右ドラッグ判定で使う)
    bool   IsSceneViewHovered() const;

    // GameView パネルのコンテンツサイズ（Application が GameView RT のリサイズ判定に使用）
    ImVec2 GetGameViewSize() const { return m_gameViewSize; }
    bool   IsGameViewHovered() const { return m_gameViewHovered; }

private:
    void BuildDefaultLayout(ImGuiID dockspaceId, f32 toolbarHeight);

    EditorContext* m_ctx = nullptr;
    bool m_dockspaceBuilt = false;

    std::unique_ptr<ToolbarPanel>      m_toolbar;
    std::unique_ptr<HierarchyPanel>    m_hierarchy;
    std::unique_ptr<InspectorPanel>    m_inspector;
    std::unique_ptr<SceneViewPanel>    m_sceneView;
    std::unique_ptr<AssetBrowserPanel> m_assetBrowser;
    std::unique_ptr<GameViewPanel>     m_gameView;
    class ModelThumbnailRenderer* m_thumbRenderer = nullptr;

    // SceneView / GameView 状態（Application が RT リサイズ判定に使う）
    ImVec2 m_sceneViewSize  = {1, 1};
    ImVec2 m_gameViewSize   = {1, 1};
    bool   m_gameViewHovered = false;
};

} // namespace dx12e
