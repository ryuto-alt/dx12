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
#include "core/GameClock.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <DirectXMath.h>
#include <cmath>
#include <cctype>
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
                m_ctx->showNetworkStatus =
                m_ctx->showVfxEditor = false;
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
            // フォールバック（全画面）
            nodePos  = ImVec2(0, toolbarHeight);
            nodeSize = ImGui::GetIO().DisplaySize;
            nodeSize.y -= toolbarHeight;
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

    // ===== ビューポート HUD オーバーレイ（Nebula 風の半透明ピル）=====
    // 左下に FPS / オブジェクト数、下中央に選択オブジェクトのラベルを重ねる。
    // NoInputs でカメラ操作・ピッキングを一切ブロックしない。
    if (!isPlaying && m_viewportSize.x > 1.0f)
    {
        const ImGuiWindowFlags ovFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs;

        // ---- 左下: 統計（FPS + オブジェクト数）----
        size_t objCount = 0;
        for (auto [e, tag] : reg.view<const NameTag>().each())
        {
            (void)tag;
            if (!reg.all_of<GridPlane>(e)) ++objCount;
        }

        // 右上に配置（左下/中央/右下は既存のカメラヒント・プレビュー・選択ラベルで使用済み）
        ImGui::SetNextWindowPos(
            ImVec2(m_viewportPos.x + m_viewportSize.x - 10.0f, m_viewportPos.y + 10.0f),
            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.78f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::AppBg);
        ImGui::PushStyleColor(ImGuiCol_Border,   theme::Border);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.0f);
        if (ImGui::Begin("##VPStats", nullptr, ovFlags))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Good);
            ImGui::Text("%.0f", clock->GetFPS());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 4); ImGui::TextDisabled("FPS");
            ImGui::SameLine(0, 14); ImGui::TextUnformatted(std::to_string(objCount).c_str());
            ImGui::SameLine(0, 4); ImGui::TextDisabled("objects");
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        // ---- 下中央: 選択オブジェクトのラベル ----
        if (m_ctx->HasSelection() && reg.valid(m_ctx->selectedEntity)
            && reg.all_of<NameTag>(m_ctx->selectedEntity))
        {
            const std::string& selName = reg.get<NameTag>(m_ctx->selectedEntity).name;
            ImGui::SetNextWindowPos(
                ImVec2(m_viewportPos.x + m_viewportSize.x * 0.5f,
                       m_viewportPos.y + 10.0f),
                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.82f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::AppBg);
            ImGui::PushStyleColor(ImGuiCol_Border,   theme::AccentDim2);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 5));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 13.0f);
            if (ImGui::Begin("##VPSelLabel", nullptr, ovFlags))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentLight);
                ImGui::TextUnformatted(selName.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 6);
                ImGui::TextDisabled("selected");
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }

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
                // Unity 準拠: シーンビューへのドロップは常に「配置」
                // (画像はスプライト、モデル/プレハブはそのまま生成。Application 側で振り分け)。
                // メッシュへのテクスチャ貼り付けは Inspector のテクスチャスロット D&D が専用操作。
                const char* droppedPath = static_cast<const char*>(payload->Data);

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
            ImGui::EndDragDropTarget();
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ===== 3D ビューポート操作（カメラナビ + ピッキング + ギズモ + 削除）=====
    if (!isPlaying)
    {
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

} // namespace dx12e
