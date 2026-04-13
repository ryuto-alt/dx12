#include "editor/EditorLayer.h"
#include "editor/EditorContext.h"
#include "editor/panels/ToolbarPanel.h"
#include "editor/panels/HierarchyPanel.h"
#include "editor/panels/InspectorPanel.h"
#include "editor/panels/SceneViewPanel.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "scene/Scene.h"
#include "core/GameClock.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

namespace dx12e
{

EditorLayer::EditorLayer() = default;
EditorLayer::~EditorLayer() = default;

void EditorLayer::Initialize(EditorContext* ctx,
                             const std::string& assetsDir,
                             ResourceManager* resourceManager,
                             DescriptorHeap* srvHeap)
{
    m_ctx = ctx;

    m_toolbar      = std::make_unique<ToolbarPanel>();
    m_hierarchy    = std::make_unique<HierarchyPanel>();
    m_inspector    = std::make_unique<InspectorPanel>();
    m_sceneView    = std::make_unique<SceneViewPanel>();
    m_assetBrowser = std::make_unique<AssetBrowserPanel>();

    m_assetBrowser->Initialize(assetsDir, resourceManager, srvHeap);
}

void EditorLayer::BuildDefaultLayout(ImGuiID dockspaceId, f32 /*toolbarHeight*/)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // 左(20%): ヒエラルキー | 残り
    ImGuiID dockLeft = 0;
    ImGuiID dockRemaining = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.18f, &dockLeft, &dockRemaining);

    // 残り → 右(22%): インスペクター | センター
    ImGuiID dockRight = 0;
    ImGuiID dockCenter = 0;
    ImGui::DockBuilderSplitNode(dockRemaining, ImGuiDir_Right, 0.22f, &dockRight, &dockCenter);

    // センター → 下(25%): アセットブラウザ | ビューポート(中央)
    ImGuiID dockBottom = 0;
    ImGuiID dockViewport = 0;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.25f, &dockBottom, &dockViewport);

    ImGui::DockBuilderDockWindow(
        "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc", dockLeft);
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc", dockRight);
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6", dockBottom);

    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorLayer::Render(bool isPlaying,
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
                         f32 /*leftPanelWidth*/,
                         f32 toolbarHeight)
{
    auto& reg = scene->GetRegistry();

    // ===== ツールバー（画面上部、DockSpace の外に固定） =====
    m_toolbar->Render(isPlaying, *m_ctx, outModeChangeRequested, outPendingPlayMode,
                      scriptEngine, clock, scene, window, audioSystem, assetsDir, toolbarHeight);

    // ===== DockSpace（ツールバーの下に全画面） =====
    ImGuiID dockspaceId = 0;
    {
        f32 displayW = ImGui::GetIO().DisplaySize.x;
        f32 displayH = ImGui::GetIO().DisplaySize.y;

        ImGui::SetNextWindowPos(ImVec2(0, toolbarHeight), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(displayW, displayH - toolbarHeight), ImGuiCond_Always);

        ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##DockHost", nullptr, hostFlags);
        ImGui::PopStyleVar(3);

        dockspaceId = ImGui::GetID("EditorDockSpace");

        if (!m_dockspaceBuilt)
        {
            m_dockspaceBuilt = true;
            BuildDefaultLayout(dockspaceId, toolbarHeight);
        }

        // PassthruCentralNode: 中央ノードの背景を描画しない → 3Dが見える
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0),
            ImGuiDockNodeFlags_PassthruCentralNode);

        ImGui::End();
    }

    // ===== 各パネル（ドッキング対応ウィンドウ） =====
    m_hierarchy->Render(reg, *m_ctx);

    m_inspector->Render(reg, *m_ctx, camera, audioSystem, physicsDebugRenderer,
                        physicsDebugDraw, useVsync, shadowQualityIndex, shadowMapSize,
                        shadowMapDirty, clock, scene);

    m_assetBrowser->Render(*m_ctx, clock->GetDeltaTime());

    // ===== 中央ノードの領域を取得（3Dビューポート座標） =====
    {
        ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId);
        if (centralNode)
        {
            m_viewportPos  = centralNode->Pos;
            m_viewportSize = centralNode->Size;
        }
        else
        {
            // フォールバック
            m_viewportPos  = ImVec2(0, toolbarHeight);
            m_viewportSize = ImGui::GetIO().DisplaySize;
            m_viewportSize.y -= toolbarHeight;
        }

        if (m_viewportSize.x < 1.0f) m_viewportSize.x = 1.0f;
        if (m_viewportSize.y < 1.0f) m_viewportSize.y = 1.0f;
    }

    // ===== シーンビューポート ドロップターゲット =====
    // ドラッグ中のみ表示（通常時はマウスイベントをブロックしない）
    if (const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload();
        dragPayload && dragPayload->IsDataType(AssetBrowserPanel::kDragDropPayloadType))
    {
        ImGui::SetNextWindowPos(m_viewportPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(m_viewportSize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.2f, 0.4f, 0.8f, 0.1f));
        ImGui::Begin("##SceneDropTarget", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoDocking);

        ImGui::InvisibleButton("##SceneDrop", m_viewportSize,
            ImGuiButtonFlags_None);

        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    AssetBrowserPanel::kDragDropPayloadType))
            {
                const char* droppedPath = static_cast<const char*>(payload->Data);
                PendingSpawnRequest req;
                req.modelPath = droppedPath;
                req.position = {0.0f, 0.0f, 0.0f};
                m_ctx->pendingSpawns.push_back(req);
                Logger::Info("Dropped to scene: {}", droppedPath);
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ===== 3D ピッキング + ギズモ =====
    if (!isPlaying)
    {
        m_sceneView->HandlePicking(reg, *m_ctx, camera,
                                   m_viewportPos.x, m_viewportPos.y,
                                   m_viewportSize.x, m_viewportSize.y);
        m_sceneView->RenderGizmo(reg, *m_ctx, camera,
                                 m_viewportPos.x, m_viewportPos.y,
                                 m_viewportSize.x, m_viewportSize.y);
    }
}

void EditorLayer::LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    m_assetBrowser->LoadPendingThumbnails(cmdList);
}

} // namespace dx12e
