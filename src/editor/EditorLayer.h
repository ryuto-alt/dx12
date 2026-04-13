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

class EditorLayer
{
public:
    EditorLayer();
    ~EditorLayer();

    void Initialize(EditorContext* ctx,
                    const std::string& assetsDir,
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
                f32 toolbarHeight);

    // フレーム先頭でcmdListが有効な間に呼ぶ（テクスチャサムネイルのアップロード）
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    // アセットブラウザの即時更新
    void RefreshAssetBrowser();

    // サムネイルレンダラー設定
    void SetThumbnailRenderer(class ModelThumbnailRenderer* renderer);

    // ビューポート領域（3D描画のオフセット計算用）
    ImVec2 GetViewportPos()  const { return m_viewportPos; }
    ImVec2 GetViewportSize() const { return m_viewportSize; }

private:
    void BuildDefaultLayout(ImGuiID dockspaceId, f32 toolbarHeight);

    EditorContext* m_ctx = nullptr;
    bool m_dockspaceBuilt = false;

    ImVec2 m_viewportPos  = {0, 0};
    ImVec2 m_viewportSize = {1, 1};

    std::unique_ptr<ToolbarPanel>      m_toolbar;
    std::unique_ptr<HierarchyPanel>    m_hierarchy;
    std::unique_ptr<InspectorPanel>    m_inspector;
    std::unique_ptr<SceneViewPanel>    m_sceneView;
    std::unique_ptr<AssetBrowserPanel> m_assetBrowser;
    class ModelThumbnailRenderer* m_thumbRenderer = nullptr;
};

} // namespace dx12e
