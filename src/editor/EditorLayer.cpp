#include "editor/EditorLayer.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "ecs/Components.h"
#include "editor/panels/ToolbarPanel.h"
#include "editor/panels/HierarchyPanel.h"
#include "editor/panels/InspectorPanel.h"
#include "editor/panels/SceneViewPanel.h"
#include "editor/panels/AssetBrowserPanel.h"
#include "editor/panels/ConsolePanel.h"
#include "editor/ModelThumbnailRenderer.h"
#include "scene/Scene.h"
#include "renderer/Camera.h"
#include "renderer/Mesh.h"
#include "resource/MaterialAssetIO.h"
#include "core/GameClock.h"
#include "core/Logger.h"
// シーンビューのライティング編集（太陽ドラッグ / ライトのハンドル / ライティング・パネル）
#include "editor/LightHandles.h"
#include "editor/panels/LightingPanel.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <DirectXMath.h>
#include <cmath>
#include <cctype>
#include <fstream>
#include <string>
#include <filesystem>

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
    m_console      = std::make_unique<ConsolePanel>();

    m_hierarchy->SetAssetsDir(assetsDir);
    m_assetBrowser->Initialize(assetsDir, scriptsDir, resourceManager, srvHeap);
    m_inspector->SetAssetBrowser(m_assetBrowser.get());
}

void EditorLayer::BuildDefaultLayout(ImGuiID dockspaceId, f32 /*toolbarHeight*/)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // ── レイアウト方針（中核4窓は常時固定。ツール窓は開いた時だけ右下に出す）──
    //  左            : ヒエラルキー（全高）
    //  中央上        : ビューポート（3D）
    //  中央下        : アセットブラウザ（横長・単独）
    //  右            : インスペクター（全高）。ツール窓が1個でも開くと上半分に縮み、
    //                  下半分にツール系タブ（Post Process / IBL / SSAO / 設定 / Flow / Project / VC）が出る。

    // 左(18%): ヒエラルキー | 残り
    ImGuiID dockLeft = 0, dockRemaining = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.18f, &dockLeft, &dockRemaining);

    // 残り → 右(24%): 右カラム | センター
    ImGuiID dockRightCol = 0, dockCenter = 0;
    ImGui::DockBuilderSplitNode(dockRemaining, ImGuiDir_Right, 0.24f, &dockRightCol, &dockCenter);

    // センター → 下(33%): アセットブラウザ | ビューポート(中央)
    ImGuiID dockBottom = 0, dockViewport = 0;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.33f, &dockBottom, &dockViewport);

    // 左: ヒエラルキー
    ImGui::DockBuilderDockWindow(
        "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc", dockLeft);
    // 中央下: コンソールを先にドック → アセットブラウザを後にドック
    // （同ノードのタブ。アセットブラウザが既定のアクティブタブになる）
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xb3\xe3\x83\xb3\xe3\x82\xbd\xe3\x83\xbc\xe3\x83\xab", dockBottom);  // コンソール
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6", dockBottom);

    const char* kInspector =
        "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc";
    const bool anyTool = (m_ctx && m_ctx->AnyToolWindowOpen());
    if (anyTool)
    {
        // 右カラム → 上=インスペクター / 下=ツール系タブ（下 42%）
        ImGuiID dockRightTop = 0, dockRightBottom = 0;
        ImGui::DockBuilderSplitNode(dockRightCol, ImGuiDir_Down, 0.42f, &dockRightBottom, &dockRightTop);
        ImGui::DockBuilderDockWindow(kInspector, dockRightTop);

        // 開いているかに関わらず全ツール窓をここへドック付け（後で開いた窓も同じタブ群に入る）。
        ImGui::DockBuilderDockWindow("Post Process",            dockRightBottom);
        ImGui::DockBuilderDockWindow("Post Process パラメータ", dockRightBottom);
        ImGui::DockBuilderDockWindow("Skybox / IBL",            dockRightBottom);
        ImGui::DockBuilderDockWindow("SSAO",                    dockRightBottom);
        ImGui::DockBuilderDockWindow("エンジン設定",            dockRightBottom);
        ImGui::DockBuilderDockWindow("ビルド設定",              dockRightBottom);
        ImGui::DockBuilderDockWindow("Scene Flow",              dockRightBottom);
        ImGui::DockBuilderDockWindow("Project",                 dockRightBottom);
        ImGui::DockBuilderDockWindow("Version Control (Git)",   dockRightBottom);
        ImGui::DockBuilderDockWindow("Network",                 dockRightBottom);
        ImGui::DockBuilderDockWindow("Network 設定",            dockRightBottom);
    }
    else
    {
        // ツール窓が全部OFF: インスペクターが右カラム全高（スッキリ4窓）。
        ImGui::DockBuilderDockWindow(kInspector, dockRightCol);
    }

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
                         f32& cascadeSplitLambda,
                         f32& cascadeBlendBand,
                         bool& showCascadeDebug,
                         GameClock* clock,
                         bool& outModeChangeRequested,
                         bool& outPendingPlayMode,
                         const std::string& assetsDir,
                         f32 /*leftPanelWidth*/,
                         f32 toolbarHeight,
                         ResourceManager* resourceManager,
                         DescriptorHeap* srvHeap,
                         ID3D12GraphicsCommandList* cmdList)
{
    auto& reg = scene->GetRegistry();

    // ===== ツールバー（画面上部、DockSpace の外に固定） =====
    m_toolbar->Render(isPlaying, *m_ctx, outModeChangeRequested, outPendingPlayMode,
                      scriptEngine, clock, scene, window, audioSystem, assetsDir, toolbarHeight);

    // ===== DockSpace（ツールバーの下に全画面） =====
    ImGuiID dockspaceId = 0;
    {
        // multi-viewport有効時、ImGui座標はスクリーン座標になるためメインビューポート原点基準で置く
        const ImGuiViewport* mainVp = ImGui::GetMainViewport();
        f32 displayW = mainVp->Size.x;
        f32 displayH = mainVp->Size.y;

        ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x, mainVp->Pos.y + toolbarHeight), ImGuiCond_Always);
        // 下端はステータスバーぶん空ける（ドックのパネルが帯に潜り込まないように）
        ImGui::SetNextWindowSize(ImVec2(displayW, displayH - toolbarHeight - kStatusBarHeight),
                                 ImGuiCond_Always);
        ImGui::SetNextWindowViewport(mainVp->ID);

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

        // ツール窓の開閉が「空 ⇔ 非空」を跨いだらレイアウトを作り直す。
        // （右下タブ領域を出す / 畳んで Inspector を右カラム全高へ戻す）
        const bool anyToolNow = m_ctx->AnyToolWindowOpen();
        if (anyToolNow != m_prevAnyToolShown)
        {
            m_prevAnyToolShown = anyToolNow;
            m_dockspaceBuilt = false;
        }

        // メニュー「表示 > レイアウトをリセット」要求でデフォルト配置を作り直す
        if (m_ctx->resetLayout)
        {
            // リセット時はツール窓を全部畳んでスッキリ中核4窓へ戻す
            m_ctx->showPostProcess = m_ctx->showPostParams = m_ctx->showSkybox =
                m_ctx->showSSAO = m_ctx->showEngineSettings = m_ctx->showSceneFlow =
                m_ctx->showProject = m_ctx->showVersionControl =
                m_ctx->showMcpBridge = m_ctx->showBuildSettings =
                m_ctx->showNetworkStatus = m_ctx->showNetworkSettings =
                m_ctx->showVfxEditor = m_ctx->showUiEditor =
                m_ctx->showAnimEditor = m_ctx->showSpriteSheetEditor = false;
            m_prevAnyToolShown = false;
            m_dockspaceBuilt = false;
            m_ctx->resetLayout = false;
        }

        if (!m_dockspaceBuilt)
        {
            m_dockspaceBuilt = true;
            BuildDefaultLayout(dockspaceId, toolbarHeight);
        }

        // エディタUIとしてレイアウトを固定する:
        //   PassthruCentralNode … 中央ノードの背景を描かず3Dを見せる
        //   NoDockingSplit      … ノードの分割（再ドッキング）を禁止
        //   NoUndocking         … タブを引き剥がして浮かせるのを禁止
        // ※ NoResize は付けない＝スプリッタでパネル（アセットブラウザ等）の
        //   上下/左右サイズをドラッグ調整できる。構造は固定のまま大きさだけ可変。
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0),
            ImGuiDockNodeFlags_PassthruCentralNode |
            ImGuiDockNodeFlags_NoDockingSplit |
            ImGuiDockNodeFlags_NoUndocking);

        ImGui::End();
    }

    // ===== 各パネル（ドッキング対応ウィンドウ） =====
    m_hierarchy->Render(reg, *m_ctx);

    m_inspector->SetScriptEngine(scriptEngine);
    m_inspector->SetAssetsDir(assetsDir);
    m_inspector->Render(reg, *m_ctx, scene);
    // グローバルなエンジン設定は Inspector から分離した独立ウィンドウ（ツール窓・トグル式）。
    if (m_ctx->showEngineSettings)
        m_inspector->RenderEngineSettings(*m_ctx, camera, audioSystem, physicsDebugRenderer,
                            physicsDebugDraw, useVsync, shadowQualityIndex, shadowMapSize,
                            shadowMapDirty, cascadeSplitLambda, cascadeBlendBand, showCascadeDebug,
                            clock);

    // ライティング・パネル（シーンの光を1画面で詰める独立フローティング窓）。
    // 影/CSM の実体は Application が持つのでここで参照を渡す（「エンジン設定」窓と同じ値を触る）。
    RenderLightingPanel(scene, *m_ctx, shadowQualityIndex, shadowMapSize, shadowMapDirty,
                        cascadeSplitLambda, cascadeBlendBand, showCascadeDebug);

    m_assetBrowser->Render(*m_ctx, clock->GetDeltaTime());

    // コンソール（アセットブラウザの隣タブに常設。全ログ + Lua 即時実行）
    m_console->Render(scriptEngine, isPlaying);

    // ===== 中央ノードの領域を取得し、シーンビューを 16:9 にレターボックス =====
    {
        ImVec2 nodePos, nodeSize;
        if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId))
        {
            nodePos  = centralNode->Pos;
            nodeSize = centralNode->Size;
        }
        else
        {
            // フォールバック（全画面。multi-viewport有効時はスクリーン座標なのでビューポート原点基準）
            const ImGuiViewport* mainVp = ImGui::GetMainViewport();
            nodePos  = ImVec2(mainVp->Pos.x, mainVp->Pos.y + toolbarHeight);
            nodeSize = mainVp->Size;
            nodeSize.y -= toolbarHeight + kStatusBarHeight;
        }
        if (nodeSize.x < 1.0f) nodeSize.x = 1.0f;
        if (nodeSize.y < 1.0f) nodeSize.y = 1.0f;

        // 16:9 固定。中央ノードに収まる最大の 16:9 矩形を中央寄せにする。
        // 余白部分はバックバッファのクリア色（暗色）がそのまま見えてレターボックスになる。
        const float kTargetAspect = 16.0f / 9.0f;
        ImVec2 vpSize = nodeSize;
        if (nodeSize.x / nodeSize.y > kTargetAspect)
            vpSize.x = nodeSize.y * kTargetAspect;   // 横が余る → 左右に帯
        else
            vpSize.y = nodeSize.x / kTargetAspect;   // 縦が余る → 上下に帯

        m_viewportPos  = ImVec2(nodePos.x + (nodeSize.x - vpSize.x) * 0.5f,
                                nodePos.y + (nodeSize.y - vpSize.y) * 0.5f);
        m_viewportSize = vpSize;

        // Application のフライカメラ発動判定で使うため EditorContext に矩形を共有
        m_ctx->viewportX = m_viewportPos.x;
        m_ctx->viewportY = m_viewportPos.y;
        m_ctx->viewportW = m_viewportSize.x;
        m_ctx->viewportH = m_viewportSize.y;
    }

    // ※ ビューポートに重ねていた HUD（FPS/objects/draws/culled のピルと選択名ラベル）は
    //    下部ステータスバーへ移設した。3D ビューの上には何も置かない
    //    ＝ゲーム UI の見た目を作るとき、エディタの表示物が絵に混ざらない。

    // ===== カメラプレビュー小窓（選択カメラ視点。Application が描画してハンドル共有）=====
    // カメラを選択すると、その視点の映像をシーンビュー右下に表示（Play 不要）。
    if (m_ctx->cameraPreviewTexHandle != 0)
    {
        const float pw  = 320.0f;
        const float ph  = pw * 9.0f / 16.0f;   // 16:9
        const float pad = 12.0f;
        ImGui::SetNextWindowPos(
            ImVec2(m_viewportPos.x + m_viewportSize.x - pw - pad,
                   m_viewportPos.y + m_viewportSize.y - ph - pad - 26.0f),  // タイトルバー分
            ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        if (ImGui::Begin("Camera Preview", nullptr,
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav))
        {
            ImGui::Image(static_cast<ImTextureID>(m_ctx->cameraPreviewTexHandle), ImVec2(pw, ph));

            // カメラ視点の「中央」を示すガイド: 中心十字＋枠＋三分割線。
            // どこが画面中央に来ているか（=センタリングできているか）を確認しやすくする。
            const ImVec2 imgMin = ImGui::GetItemRectMin();
            const ImVec2 imgMax = ImGui::GetItemRectMax();
            const ImVec2 ctr((imgMin.x + imgMax.x) * 0.5f, (imgMin.y + imgMax.y) * 0.5f);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 cFrame  = IM_COL32(255, 255, 255, 70);
            const ImU32 cThird  = IM_COL32(255, 255, 255, 45);
            const ImU32 cCross  = IM_COL32(255, 220, 60, 200);
            // 枠
            dl->AddRect(imgMin, imgMax, cFrame);
            // 三分割線
            for (int i = 1; i <= 2; ++i)
            {
                float fx = imgMin.x + (imgMax.x - imgMin.x) * (i / 3.0f);
                float fy = imgMin.y + (imgMax.y - imgMin.y) * (i / 3.0f);
                dl->AddLine(ImVec2(fx, imgMin.y), ImVec2(fx, imgMax.y), cThird);
                dl->AddLine(ImVec2(imgMin.x, fy), ImVec2(imgMax.x, fy), cThird);
            }
            // 中心十字（中央が一目で分かる）
            const float r = 9.0f;
            dl->AddLine(ImVec2(ctr.x - r, ctr.y), ImVec2(ctr.x + r, ctr.y), cCross, 1.5f);
            dl->AddLine(ImVec2(ctr.x, ctr.y - r), ImVec2(ctr.x, ctr.y + r), cCross, 1.5f);
            dl->AddCircle(ctr, 2.5f, cCross);
        }
        ImGui::End();
        ImGui::PopStyleVar();
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

                std::string ext = std::filesystem::path(droppedPath).extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Material)
                {
                    // .dxmat は「配置」ではなく、ドロップ先メッシュ(サブメッシュ単位)へのマテリアル適用
                    // (Unity/Unreal と同じ操作感。Inspector のマテリアルスロット D&D と同じ経路)。
                    SubmeshPickResult pick = m_sceneView->PickEntityAndSubmesh(
                        reg, *m_ctx, camera, m_viewportPos.x, m_viewportPos.y,
                        m_viewportSize.x, m_viewportSize.y);
                    if (pick.entity != entt::null && reg.all_of<MeshRenderer>(pick.entity))
                    {
                        auto& mr = reg.get<MeshRenderer>(pick.entity);
                        MeshRenderer before = mr;

                        // 絶対パス → assets 相対(InspectorPanel の D&D と同じ変換)
                        std::string abs  = std::filesystem::path(droppedPath).lexically_normal().string();
                        std::string base = std::filesystem::path(assetsDir).lexically_normal().string();
                        std::replace(abs.begin(), abs.end(), '\\', '/');
                        std::replace(base.begin(), base.end(), '\\', '/');
                        std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

                        MeshRenderer::SetOverride(mr.materialAsset, pick.submeshIndex, rel);

                        // UVタイリングが既定値(1.0)のままなら .dxmat の値を初期値としてコピー
                        if (mr.uvScaleU == 1.0f && mr.uvScaleV == 1.0f)
                        {
                            std::ifstream ifs(assetsDir + rel, std::ios::binary);
                            if (ifs)
                            {
                                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                                                            std::istreambuf_iterator<char>());
                                MaterialAssetData data;
                                if (ParseMaterialAsset(bytes, data) &&
                                    pick.submeshIndex < mr.meshes.size() && mr.meshes[pick.submeshIndex])
                                {
                                    mr.uvScaleU = data.uvTilingU;
                                    mr.uvScaleV = data.uvTilingV;
                                    if (scene)
                                        for (auto* mesh : mr.meshes)
                                            if (mesh) mesh->ApplyUVScale(*scene->GetDevice(), mr.uvScaleU, mr.uvScaleV);
                                }
                            }
                        }

                        m_ctx->undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                            &reg, pick.entity, before, mr, "マテリアル割当(D&D)"));
                        Logger::Info("マテリアルD&D適用: entity={} submesh={} -> {}",
                            static_cast<u32>(pick.entity), pick.submeshIndex, rel);
                    }
                    else
                    {
                        Logger::Info("マテリアルD&D: ドロップ先にメッシュがないため何もしません");
                    }
                }
                else
                {
                    // Unity 準拠: シーンビューへのドロップは常に「配置」
                    // (画像はスプライト、モデル/プレハブはそのまま生成。Application 側で振り分け)。
                    // メッシュへのテクスチャ貼り付けは Inspector のテクスチャスロット D&D が専用操作。
                    PendingSpawnRequest req;
                    req.modelPath = droppedPath;

                    // マウス座標からワールド座標を計算（Y=0 平面との交点）
                    req.position = ScreenToWorldOnGroundPlane(
                        camera, ImGui::GetIO().MousePos,
                        m_viewportPos, m_viewportSize);

                    m_ctx->pendingSpawns.push_back(req);
                    Logger::Info("Dropped to scene at ({:.1f}, {:.1f}, {:.1f}): {}",
                        req.position.x, req.position.y, req.position.z, droppedPath);
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ===== 3D ビューポート操作（カメラナビ + ピッキング + ギズモ + 削除）=====
    if (!isPlaying)
    {
        // ゲーム内 UI のプレビュー（UI編集モード ON のとき）。背景 DrawList に描くため
        // ギズモ（RenderGizmo）より先に呼ぶ＝ギズモやハンドルが UI の手前に重なる。
        // Play 中/ゲームモードは Application の ##GameUI 経路が描くのでここでは呼ばない。
        m_sceneView->RenderUiPreview(reg, *m_ctx,
                                     m_viewportPos.x, m_viewportPos.y,
                                     m_viewportSize.x, m_viewportSize.y,
                                     resourceManager, srvHeap, cmdList);
        m_sceneView->HandleCameraNavigation(reg, *m_ctx, camera,
                                            m_viewportPos.x, m_viewportPos.y,
                                            m_viewportSize.x, m_viewportSize.y);
        // ギズモ(Manipulate)を先に回す → ピッキングを後に回す。
        // 理由: HandlePicking は「ギズモ上か」を ImGuizmo::IsOver()/IsUsing() で判定するが、
        //   Manipulate を呼ぶ前だと ImGuizmo の内部当たり判定が「前フレームのマウス位置・行列」で
        //   止まっている(ステイル)。回転ギズモは輪が細いので、このズレでクリックがピッキングに
        //   横取りされ「見えてる輪とズレた所で反応する」状態になる。先に Manipulate を回せば
        //   現在フレームのマウスで掴み判定が確定し、IsUsing() が立つので HandlePicking が正しく弾く。
        m_sceneView->RenderGizmo(reg, *m_ctx, camera,
                                 m_viewportPos.x, m_viewportPos.y,
                                 m_viewportSize.x, m_viewportSize.y);
        // ライトの直接操作（L+マウスで太陽を回す / コーン角・range・向きのハンドル）と
        // 影響範囲ワイヤの描画。RunViewportTools より前に呼ぶこと：当たり判定とドラッグ処理を
        // 「今フレームのマウス位置」で先に済ませ、その結果を viewportToolHandlers 経由で
        // 「このクリックは食った」として伝えるため（ギズモを先に回すのと同じ理由）。
        LightHandlesFrame(reg, *m_ctx, camera);
        // ビューポートツール（地形ブラシ / ライトのハンドル操作など）の受け口。
        // ギズモの次・UI 編集/3D ピッキングの前に回し、消費されたら以降の編集操作をしない。
        m_sceneView->RunViewportTools(*m_ctx, camera,
                                      m_viewportPos.x, m_viewportPos.y,
                                      m_viewportSize.x, m_viewportSize.y);
        // UI 編集モードの UI 要素編集（選択/移動/リサイズ）はギズモ確定後・3D ピッキング前。
        // UI 要素にヒットしたクリックはここで消費され、HandlePicking へは渡らない。
        m_sceneView->HandleUiEditing(reg, *m_ctx,
                                     m_viewportPos.x, m_viewportPos.y,
                                     m_viewportSize.x, m_viewportSize.y);
        m_sceneView->HandlePicking(reg, *m_ctx, camera,
                                   m_viewportPos.x, m_viewportPos.y,
                                   m_viewportSize.x, m_viewportSize.y);
        m_sceneView->HandleDeleteKey(reg, *m_ctx, scene,
                                     m_viewportPos.x, m_viewportPos.y,
                                     m_viewportSize.x, m_viewportSize.y);
        m_sceneView->HandleTextureContextMenu(reg, *m_ctx, camera,
                                              m_viewportPos.x, m_viewportPos.y,
                                              m_viewportSize.x, m_viewportSize.y);
    }

    // ===== 最下部のステータスバー（3D ビューに重ねない情報の置き場）=====
    RenderStatusBar(scene, camera, clock, isPlaying);
}

void EditorLayer::RenderStatusBar(Scene* scene, Camera* camera, GameClock* clock, bool isPlaying)
{
    auto& reg = scene->GetRegistry();

    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x, mainVp->Pos.y + mainVp->Size.y - kStatusBarHeight),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, kStatusBarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    // パネルより一段暗くして「窓の外の帯」に見せる（同色だとパネルの続きに見えて読みにくい）
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::AppBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // グローバルの FramePadding はツールバー用に縦 9px と大きい。帯の中のボタンが
    // そのままだと帯より高くなるので、ここだけ小さくする。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 2));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav       | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 上辺の区切り線（ドックのパネルとの境目）
    {
        const ImVec2 p = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(
            p, ImVec2(p.x + ImGui::GetWindowSize().x, p.y),
            ImGui::GetColorU32(theme::Border));
    }

    // 行を帯の中央に置く。AlignTextToFramePadding はグローバルの FramePadding(縦9px)を
    // 足すので帯からはみ出す＝文字の下が切れる。ここは自前で中央に寄せる。
    ImGui::SetCursorPosY((kStatusBarHeight - ImGui::GetTextLineHeight()) * 0.5f);

    // ---- 左: 選択中の名前（複数選択なら件数）/ シーンのロード進捗 ----
    if (m_ctx->sceneLoadProgress >= 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentLight);
        ImGui::Text("シーン読み込み中 %.0f%%", m_ctx->sceneLoadProgress * 100.0f);
        ImGui::PopStyleColor();
    }
    else if (m_ctx->HasSelection() && reg.valid(m_ctx->selectedEntity)
             && reg.all_of<NameTag>(m_ctx->selectedEntity))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentLight);
        ImGui::TextUnformatted(reg.get<NameTag>(m_ctx->selectedEntity).name.c_str());
        ImGui::PopStyleColor();
        if (m_ctx->selectedEntities.size() > 1)
        {
            ImGui::SameLine(0, 6);
            ImGui::TextDisabled("+%zu 件", m_ctx->selectedEntities.size() - 1);
        }
    }
    else
    {
        ImGui::TextDisabled("選択なし");
    }

    // ---- 右: カメラ速度 → 描画統計。右端から逆算して配置 ----
    size_t objCount = 0;
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        (void)tag;
        if (!reg.all_of<GridPlane>(e)) ++objCount;
    }

    char stats[128];
    snprintf(stats, sizeof(stats), "%.0f FPS    %zu obj    %u draws    %u culled",
             clock->GetFPS(), objCount, m_ctx->statDraws, m_ctx->statCulled);
    char camLabel[64];
    snprintf(camLabel, sizeof(camLabel), "カメラ速度 %.1f", camera->GetMoveSpeed());

    const float statsW = ImGui::CalcTextSize(stats).x;
    const float camW   = ImGui::CalcTextSize(camLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - statsW - camW - 20.0f);

    // カメラ設定（速度・感度・グリッド表示）。ツール窓を開かず1クリックで触れる場所に置く。
    if (ImGui::SmallButton(camLabel))
        ImGui::OpenPopup("##CamQuickSettings");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("カメラの移動速度（右ドラッグ中にホイールでも変えられます）");
    if (ImGui::BeginPopup("##CamQuickSettings"))
    {
        ImGui::TextDisabled("カメラ");
        f32 speed = camera->GetMoveSpeed();
        ImGui::SetNextItemWidth(200);
        // 対数目盛り: 遅い側(1〜10)を細かく、速い側(〜200)をざっくり動かせる
        if (ImGui::SliderFloat("移動速度", &speed, 0.2f, 200.0f, "%.1f m/s",
                               ImGuiSliderFlags_Logarithmic))
            camera->SetMoveSpeed(speed);

        struct Preset { const char* label; f32 speed; };
        static const Preset presets[] = {
            {"精密 1",  1.0f}, {"標準 5",  5.0f}, {"広域 20", 20.0f}, {"最速 80", 80.0f}};
        for (int i = 0; i < 4; ++i)
        {
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(presets[i].label)) camera->SetMoveSpeed(presets[i].speed);
        }

        f32 sens = camera->GetMouseSensitivity();
        ImGui::SetNextItemWidth(200);
        if (ImGui::SliderFloat("マウス感度", &sens, 0.0005f, 0.02f, "%.4f"))
            camera->SetMouseSensitivity(sens);

        // グリッド床の表示/非表示。UI やライティングを見るとき邪魔になるので手元に置く。
        ImGui::Separator();
        for (auto [e, gp] : reg.view<GridPlane>().each())
        {
            ImGui::Checkbox("グリッドを表示", &gp.enabled);
            break;   // グリッドは1枚だけ
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine(0, 20);
    ImGui::PushStyleColor(ImGuiCol_Text, isPlaying ? theme::TextDim : theme::TextFaint);
    ImGui::TextUnformatted(stats);
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void EditorLayer::LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList)
{
    m_assetBrowser->LoadPendingThumbnails(cmdList);
}

void EditorLayer::RefreshAssetBrowser()
{
    m_assetBrowser->ForceRefresh();
}

void EditorLayer::SetAssetRoots(const std::string& assetsDir, const std::string& scriptsDir)
{
    m_assetBrowser->SetRoots(assetsDir, scriptsDir);
}

void EditorLayer::SetThumbnailRenderer(ModelThumbnailRenderer* renderer)
{
    m_thumbRenderer = renderer;
    m_assetBrowser->SetThumbnailRenderer(renderer);
}

void EditorLayer::SetMaterialPreviewRenderer(MaterialPreviewRenderer* renderer)
{
    m_assetBrowser->SetMaterialPreviewRenderer(renderer);
}

} // namespace dx12e
