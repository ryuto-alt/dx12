#include "editor/EditorLayer.h"
#include "editor/EditorContext.h"
#include "editor/panels/ToolbarPanel.h"
#include "editor/panels/HierarchyPanel.h"
#include "editor/panels/InspectorPanel.h"
#include "editor/panels/SceneViewPanel.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "editor/panels/GameViewPanel.h"
#include "editor/ModelThumbnailRenderer.h"
#include "scene/Scene.h"
#include "renderer/Camera.h"
#include "core/GameClock.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <DirectXMath.h>
#include <cmath>

namespace dx12e
{

// マウス座標から Y=0 平面上のワールド座標を計算
static DirectX::XMFLOAT3 ScreenToWorldOnGroundPlane(
    Camera* camera, ImVec2 mousePos, ImVec2 vpPos, ImVec2 vpSize)
{
    using namespace DirectX;

    // NDC
    float ndcX = ((mousePos.x - vpPos.x) / vpSize.x) * 2.0f - 1.0f;
    float ndcY = 1.0f - ((mousePos.y - vpPos.y) / vpSize.y) * 2.0f;

    XMMATRIX invProj = XMMatrixInverse(nullptr, camera->GetProjectionMatrix());
    XMMATRIX invView = XMMatrixInverse(nullptr, camera->GetViewMatrix());

    XMVECTOR rayClip = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
    XMVECTOR rayEye = XMVector4Transform(rayClip, invProj);
    rayEye = XMVectorSetZ(rayEye, 1.0f);
    rayEye = XMVectorSetW(rayEye, 0.0f);
    XMVECTOR rayDir = XMVector3Normalize(XMVector4Transform(rayEye, invView));

    XMFLOAT3 camPosF = camera->GetPosition();
    XMVECTOR rayOrigin = XMLoadFloat3(&camPosF);

    // Y=0 平面との交点: t = -origin.y / dir.y
    XMFLOAT3 dir;
    XMStoreFloat3(&dir, rayDir);

    if (std::abs(dir.y) > 1e-6f)
    {
        float t = -camPosF.y / dir.y;
        if (t > 0.0f)
        {
            // 交点を計算
            XMFLOAT3 result;
            XMStoreFloat3(&result, XMVectorAdd(rayOrigin, XMVectorScale(rayDir, t)));
            result.y = 0.0f;  // 浮動小数誤差防止
            return result;
        }
    }

    // Y=0 に交差しない場合（カメラが上を向いてる等）→ カメラの前方 10m に配置
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVectorAdd(rayOrigin, XMVectorScale(rayDir, 10.0f)));
    return result;
}

EditorLayer::EditorLayer() = default;
EditorLayer::~EditorLayer() = default;

bool EditorLayer::IsSceneViewHovered() const
{
    return m_sceneView ? m_sceneView->IsHovered() : false;
}

void EditorLayer::Initialize(EditorContext* ctx,
                             const std::string& assetsDir,
                             const std::string& scriptsDir,
                             ResourceManager* resourceManager,
                             DescriptorHeap* srvHeap)
{
    m_ctx = ctx;

    m_toolbar      = std::make_unique<ToolbarPanel>();
    m_hierarchy    = std::make_unique<HierarchyPanel>();
    m_inspector    = std::make_unique<InspectorPanel>();
    m_sceneView    = std::make_unique<SceneViewPanel>();
    m_assetBrowser = std::make_unique<AssetBrowserPanel>();
    m_gameView     = std::make_unique<GameViewPanel>();

    m_hierarchy->SetAssetsDir(assetsDir);
    m_assetBrowser->Initialize(assetsDir, scriptsDir, resourceManager, srvHeap);
}

void EditorLayer::BuildDefaultLayout(ImGuiID dockspaceId, f32 /*toolbarHeight*/)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // Blender 流の上下構成:
    // 上 (75%): Scene | Game の左右分割 (3D ビューポートを大きく)
    // 下 (25%): Hierarchy | AssetBrowser | Inspector の3列
    ImGuiID dockTop = 0, dockBottom = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.25f, &dockBottom, &dockTop);

    // 上 → 左半分(シーンタブ) | 右半分(ゲームタブ)
    ImGuiID dockSceneView = 0, dockGameView = 0;
    ImGui::DockBuilderSplitNode(dockTop, ImGuiDir_Right, 0.50f,
                                 &dockGameView, &dockSceneView);

    // 下を3列に分割: ヒエラルキー(15%) | アセットブラウザ(中央) | インスペクター(20%)
    ImGuiID dockBottomLeft = 0, dockBottomRest = 0;
    ImGui::DockBuilderSplitNode(dockBottom, ImGuiDir_Left, 0.15f,
                                 &dockBottomLeft, &dockBottomRest);
    ImGuiID dockBottomCenter = 0, dockBottomRight = 0;
    ImGui::DockBuilderSplitNode(dockBottomRest, ImGuiDir_Right, 0.235f,
                                 &dockBottomRight, &dockBottomCenter);

    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xb7\xe3\x83\xbc\xe3\x83\xb3", dockSceneView);
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0", dockGameView);
    ImGui::DockBuilderDockWindow(
        "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc", dockBottomLeft);
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6", dockBottomCenter);
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc", dockBottomRight);

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
                         f32 toolbarHeight,
                         u64 sceneViewTextureId,
                         u64 gameViewTextureId)
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

        // SceneView も GameView も RT を Image 表示するパネルになったので透過は不要
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None);

        ImGui::End();
    }

    // ===== 各パネル（ドッキング対応ウィンドウ） =====
    m_hierarchy->Render(reg, *m_ctx);

    m_inspector->SetScriptEngine(scriptEngine);
    m_inspector->SetAssetsDir(assetsDir);
    m_inspector->Render(reg, *m_ctx, camera, audioSystem, physicsDebugRenderer,
                        physicsDebugDraw, useVsync, shadowQualityIndex, shadowMapSize,
                        shadowMapDirty, clock, scene);

    m_assetBrowser->Render(*m_ctx, clock->GetDeltaTime());

    // ===== シーン / ゲームタブ: ViewMode に応じて表示制御 =====
    // Both = 両方タブ表示 / Scene = シーンのみ / Game = ゲームのみ
    const bool showScene = (m_ctx->viewMode != ViewMode::Game);
    const bool showGame  = (m_ctx->viewMode != ViewMode::Scene);

    if (showScene)
    {
        m_sceneView->Render(isPlaying, sceneViewTextureId);
        m_sceneViewSize = m_sceneView->GetContentSize();
    }
    if (showGame)
    {
        m_gameView->Render(isPlaying, gameViewTextureId);
        m_gameViewSize    = m_gameView->GetContentSize();
        m_gameViewHovered = m_gameView->IsHovered();
    }

    // ===== シーンビューポートのドロップターゲット (シーンタブの Image 矩形上に重ねる) =====
    // ドラッグ中のみ表示（通常時はマウスイベントをブロックしない）
    if (showScene)
    {
        const ImVec2 imgMin  = m_sceneView->GetImageMin();
        const ImVec2 imgSize = m_sceneView->GetImageSize();
        if (const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload();
            dragPayload && dragPayload->IsDataType(AssetBrowserPanel::kDragDropPayloadType))
        {
            ImGui::SetNextWindowPos(imgMin, ImGuiCond_Always);
            ImGui::SetNextWindowSize(imgSize, ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.2f, 0.4f, 0.8f, 0.1f));
            ImGui::Begin("##SceneDropTarget", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoDocking);

            ImGui::InvisibleButton("##SceneDrop", imgSize, ImGuiButtonFlags_None);

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                        AssetBrowserPanel::kDragDropPayloadType))
                {
                    const char* droppedPath = static_cast<const char*>(payload->Data);
                    PendingSpawnRequest req;
                    req.modelPath = droppedPath;
                    // マウス座標 → ワールド座標 (Y=0 平面との交点)。基準は Image 矩形 (16:9)
                    req.position = ScreenToWorldOnGroundPlane(
                        camera, ImGui::GetIO().MousePos, imgMin, imgSize);
                    m_ctx->pendingSpawns.push_back(req);
                    Logger::Info("Dropped to scene at ({:.1f}, {:.1f}, {:.1f}): {}",
                        req.position.x, req.position.y, req.position.z, droppedPath);
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
    }

    // ===== 3D ピッキング + ギズモ + 削除 (シーンタブが表示されている時のみ) =====
    if (!isPlaying && showScene)
    {
        const ImVec2 imgMin  = m_sceneView->GetImageMin();
        const ImVec2 imgSize = m_sceneView->GetImageSize();
        m_sceneView->HandlePicking(reg, *m_ctx, camera,
                                   imgMin.x, imgMin.y, imgSize.x, imgSize.y);
        m_sceneView->RenderGizmo(reg, *m_ctx, camera,
                                 imgMin.x, imgMin.y, imgSize.x, imgSize.y);
        m_sceneView->HandleDeleteKey(reg, *m_ctx, scene,
                                     imgMin.x, imgMin.y, imgSize.x, imgSize.y);
    }
}

void EditorLayer::LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    m_assetBrowser->LoadPendingThumbnails(cmdList);
}

void EditorLayer::RefreshAssetBrowser()
{
    m_assetBrowser->ForceRefresh();
}

void EditorLayer::SetThumbnailRenderer(ModelThumbnailRenderer* renderer)
{
    m_thumbRenderer = renderer;
    m_assetBrowser->SetThumbnailRenderer(renderer);
}

} // namespace dx12e
