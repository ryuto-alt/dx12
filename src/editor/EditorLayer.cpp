#include "editor/EditorLayer.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/EditorCommands.h"   // ショートカット処理（コマンド表が唯一の正）
#include "editor/CommandPalette.h"   // Ctrl+K / Ctrl+P
#include "editor/ToolWindows.h"      // ツール窓レジストリ（既定のドック先・すべて閉じる）
#include "editor/Toast.h"            // 右下トースト
#include "gui/FloatingGuard.h"   // フローティング窓の収容領域
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
#include "editor/AiDebugOverlay.h"
#include "editor/panels/LightingPanel.h"
#include "editor/panels/AudioMixerPanel.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <DirectXMath.h>
#include <cmath>
#include <cctype>
#include <cstring>
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
    m_palette      = std::make_unique<CommandPalette>();

    m_hierarchy->SetAssetsDir(assetsDir);
    m_assetBrowser->Initialize(assetsDir, scriptsDir, resourceManager, srvHeap);
    m_inspector->SetAssetBrowser(m_assetBrowser.get());
}

f32 EditorLayer::StatusBarHeight() { return theme::Px(kStatusBarHeight); }

// 既定レイアウトを作る。★呼ぶのは起動時と「レイアウトをリセット」だけ（窓の開閉では呼ばない）。
//
//   左            : ヒエラルキー（全高）
//   中央上        : ビューポート（3D）
//   中央下        : アセットブラウザ + コンソール（タブ）
//   右            : インスペクター（全高）。ツール窓（Post Process / Skybox / エンジン設定 / Git …）は
//                   ここに**タブとして追加される**（開くと最前面のタブになる）。分割は変えない。
//
// 以前はツール窓が 1 個でも開くと右カラムを縦に割り（右下にツールタブ領域）、閉じると割りを畳む方式で、
// 開閉のたびにドックを丸ごと作り直していた。実ノードの寸法から比を吸い上げて再現していたが、
// 丸め誤差（分割バー・タブ帯）が毎回積もり、開閉を繰り返すほどビューポートが縮んだ。
// 「作り直さない」が最も確実なので、ツール窓は最初から全部インスペクターのタブ群へ入れておく
// （未表示の窓は Begin されないだけでタブも出ない）。
void EditorLayer::BuildDefaultLayout(ImGuiID dockspaceId, f32 /*toolbarHeight*/)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // 左(18%): ヒエラルキー | 残り
    ImGuiID dockLeft = 0, dockRemaining = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, kRatioLeft, &dockLeft, &dockRemaining);

    // 残り → 右(24%): 右カラム | センター
    ImGuiID dockRightCol = 0, dockCenter = 0;
    ImGui::DockBuilderSplitNode(dockRemaining, ImGuiDir_Right, kRatioRight, &dockRightCol, &dockCenter);

    // センター → 下(33%): アセットブラウザ | ビューポート(中央)
    ImGuiID dockBottom = 0, dockViewport = 0;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, kRatioBottom, &dockBottom, &dockViewport);

    // 左: ヒエラルキー
    ImGui::DockBuilderDockWindow(
        "\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc", dockLeft);
    // 中央下: コンソールを先にドック → アセットブラウザを後にドック
    // （同ノードのタブ。アセットブラウザが既定のアクティブタブになる）
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xb3\xe3\x83\xb3\xe3\x82\xbd\xe3\x83\xbc\xe3\x83\xab", dockBottom);  // コンソール
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6", dockBottom);

    // 右: ツール窓（レジストリの RightTab 全部）→ 最後にインスペクター。
    // 後からドックした窓が既定のアクティブタブになるので、インスペクターを最後にして初期表示にする。
    for (const tools::Desc& t : tools::kAll)
        if (t.slot == tools::DockSlot::RightTab && t.imguiName && t.imguiName[0])
            ImGui::DockBuilderDockWindow(t.imguiName, dockRightCol);
    ImGui::DockBuilderDockWindow(
        "\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc", dockRightCol);  // インスペクター

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
    m_ctx->isPlaying = isPlaying;   // 各パネルが「Play 中は押せない」を判定するのに使う
    theme::g_paintChromeFn = &ui::deco::PaintChrome;   // テーマ・バリアントの窓外装飾（Gui は Editor に依存できないので口を渡す）
    toolbarHeight = ui::Px(toolbarHeight);   // Application::kToolbarHeight は論理 px（100% 表示）。以降は物理 px

    auto& reg = scene->GetRegistry();

    // フォーカスのあるパネルを確定する（Del / F2 が「押した先のパネルだけ」で効くように）。
    // ImGui の NavWindow（最後にクリック/フォーカスされた窓）の名前で判定する。
    // どの窓でもない（ビューポート / ツールバー / ステータスバー / トースト）は None = ビューポート扱い。
    {
        using P = EditorContext::Panel;
        P p = P::None;
        if (ImGuiContext* g = ImGui::GetCurrentContext(); g && g->NavWindow)
        {
            const char* n = g->NavWindow->RootWindow ? g->NavWindow->RootWindow->Name : g->NavWindow->Name;
            auto is = [n](const char* w) { return std::strcmp(n, w) == 0; };
            auto starts = [n](const char* w) { return std::strncmp(n, w, std::strlen(w)) == 0; };
            if (is("\xe3\x83\x92\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xab\xe3\x82\xad\xe3\x83\xbc"))          p = P::Hierarchy;     // ヒエラルキー
            else if (is("\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc")) p = P::Inspector; // インスペクター
            else if (is("\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6")) p = P::AssetBrowser; // アセットブラウザ
            else if (is("\xe3\x82\xb3\xe3\x83\xb3\xe3\x82\xbd\xe3\x83\xbc\xe3\x83\xab"))                 p = P::Console;       // コンソール
            else if (starts("##DockHost") || starts("##StatusBar") || starts("##Toolbar") || starts("##Toast")
                     || starts("##SceneDropTarget") || is("gizmo") || is("Camera Preview"))
                p = P::None;
            else
                p = P::Other;   // フローティングのツール窓 / ポップアップなど
        }
        m_ctx->focusedPanel = p;
    }

    // エディタカメラの位置と向き（「カメラの前にエンティティを作る」に使う）
    if (camera)
    {
        m_ctx->camPos = camera->GetPosition();
        m_ctx->camFwd = camera->GetForward();
        m_ctx->camValid = true;
    }

    // ===== ショートカット（コマンド表が唯一の正）=====
    // 表のキーが押されたら Execute。Application に散っていた処理の移設先。
    const cmd::Env cmdEnv{scene, assetsDir};
    cmd::ProcessShortcuts(*m_ctx, cmdEnv);

    // Play / Stop の要求（F5 / Shift+F5 / Esc(一時停止中) / パレット）をモード切替へ
    if (m_ctx->pendingPlayRequest != 0)
    {
        if (m_ctx->pendingPlayRequest == 1 && !isPlaying)
        {
            outPendingPlayMode = true;
            outModeChangeRequested = true;
        }
        else if (m_ctx->pendingPlayRequest == 2 && isPlaying)
        {
            outPendingPlayMode = false;
            outModeChangeRequested = true;
        }
        m_ctx->pendingPlayRequest = 0;
    }

    // ===== ツールバー（画面上部、DockSpace の外に固定） =====
    theme::RestoreMainScale();   // DPI: 別倍率のモニターの窓を描いた後でもメイン窓の座標計算は主倍率で
    m_toolbar->Render(isPlaying, *m_ctx, outModeChangeRequested, outPendingPlayMode,
                      scriptEngine, clock, scene, window, audioSystem, assetsDir, toolbarHeight);

    // ===== DockSpace（ツールバーの下に全画面） =====
    theme::RestoreMainScale();
    ImGuiID dockspaceId = 0;
    {
        // multi-viewport有効時、ImGui座標はスクリーン座標になるためメインビューポート原点基準で置く
        const ImGuiViewport* mainVp = ImGui::GetMainViewport();
        f32 displayW = mainVp->Size.x;
        f32 displayH = mainVp->Size.y;

        ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x, mainVp->Pos.y + toolbarHeight), ImGuiCond_Always);
        // 下端はステータスバーぶん空ける（ドックのパネルが帯に潜り込まないように）
        ImGui::SetNextWindowSize(ImVec2(displayW, displayH - toolbarHeight - StatusBarHeight()),
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

        // メニュー「表示 > レイアウトをリセット」要求でデフォルト配置を作り直す。
        // ドックを作り直すのはここと起動時だけ（ツール窓の開閉では作り直さない＝ビューポートが縮まない）。
        if (m_ctx->resetLayout)
        {
            // ツール窓を全部閉じてスッキリ中核 4 窓へ戻す（レジストリの全窓。取りこぼしなし）。
            tools::CloseAll(*m_ctx);
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
        // タブの文字だけ太字（Semibold 相当）にする。タブ帯はこの DockSpace 呼び出し中に描かれるので、
        // ここで Push/Pop するだけで全パネルのタブに効く（各パネルの中身は既定の本文フォントのまま）。
        ui::PushBold();
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0),
            ImGuiDockNodeFlags_PassthruCentralNode |
            ImGuiDockNodeFlags_NoDockingSplit |
            ImGuiDockNodeFlags_NoUndocking);
        ui::PopBold();

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

    // オーディオミキサー（中で showAudioMixer を見て早期 return する）
    RenderAudioMixerPanel(audioSystem, *m_ctx);

    m_assetBrowser->Render(*m_ctx, clock->GetDeltaTime());

    // コンソール（アセットブラウザの隣タブに常設。全ログ + Lua 即時実行）
    // ctx を渡すのは、シェーダーのエラー行クリックで Inspector へ飛ばすため
    // （EditorContext::revealShaderIssue を次フレームの InspectorPanel が拾う）。
    m_console->Render(scriptEngine, isPlaying, m_ctx);

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
            nodeSize.y -= toolbarHeight + StatusBarHeight();
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
        const float pw  = ui::Px(320.0f);
        const float ph  = pw * 9.0f / 16.0f;   // 16:9
        const float pad = ui::Px(12.0f);
        ImGui::SetNextWindowPos(
            ImVec2(m_viewportPos.x + m_viewportSize.x - pw - pad,
                   m_viewportPos.y + m_viewportSize.y - ph - pad - ui::Px(26.0f)),  // タイトルバー分
            ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(6.0f, 6.0f));
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
            const float r = ui::Px(9.0f);
            dl->AddLine(ImVec2(ctr.x - r, ctr.y), ImVec2(ctr.x + r, ctr.y), cCross, ui::PxF(1.5f));
            dl->AddLine(ImVec2(ctr.x, ctr.y - r), ImVec2(ctr.x, ctr.y + r), cCross, ui::PxF(1.5f));
            dl->AddCircle(ctr, ui::PxF(2.5f), cCross);
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
                        // ★ここは頂点バッファへ焼く（ApplyUVScale）ので、値を戻すだけの
                        //   ComponentEditCommand では **絵が戻らない**。焼き直しまで面倒を見る
                        //   MeshRendererLookCommand を併せて積む（UndoSystem.h の注記どおり）。
                        //   しかも mr.meshes は同じモデルの全インスタンスで共有されるので、
                        //   焼いたままだと他のインスタンスまでタイル表示が残る。
                        const MeshRendererLook lookBefore = MeshRendererLook::From(mr);
                        bool bakedUv = false;
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
                                    bakedUv = true;
                                }
                            }
                        }

                        if (bakedUv)
                        {
                            // Undo は逆順なので Look（焼き直し）→ ComponentEdit（値の復元）の順に走る。
                            auto batch = std::make_unique<CompositeCommand>("マテリアル割当(D&D)");
                            batch->Add(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                                &reg, pick.entity, before, mr, "マテリアル割当(D&D)"));
                            batch->Add(std::make_unique<MeshRendererLookCommand>(
                                scene, &reg, pick.entity, lookBefore, MeshRendererLook::From(mr)));
                            m_ctx->undoSystem.PushCommand(std::move(batch));
                        }
                        else
                        {
                            m_ctx->undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                                &reg, pick.entity, before, mr, "マテリアル割当(D&D)"));
                        }
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

    // 選択中の Brain の知覚・行動（Play 中・一時停止中。エディタの編集操作には関わらない表示だけ）
    AiDebugOverlayFrame(reg, *m_ctx, camera);

    // ===== 最下部のステータスバー（3D ビューに重ねない情報の置き場）=====
    theme::RestoreMainScale();
    RenderStatusBar(scene, camera, clock, isPlaying);

    // ===== コマンドパレット (Ctrl+K) / クイックオープン (Ctrl+P) と、右下のトースト通知 =====
    // どちらも最後に描く＝他のパネルより手前。トーストはステータスバーの上に積む。
    theme::RestoreMainScale();
    m_palette->Render(*m_ctx, cmdEnv, reg, m_assetBrowser.get());
    ui::RenderToasts(ImGui::GetIO().DeltaTime, StatusBarHeight());

    // ===== フローティング窓をメインウィンドウ内へ収める領域を渡す =====
    // ImGui は保存位置の無い新規窓を (60,60) に開くのでツールバーに被っていた。ツールバーの下〜
    // ステータスバーの上へ収める。実際のクランプは ImGuiManager::EndFrame（全パネルの描画後）が行う
    // （Application 直属のツール窓もあるので、ここではまだ全部は出そろっていない）。
    {
        const ImGuiViewport* mvp = ImGui::GetMainViewport();
        floatguard::SetArea(
            ImVec2(mvp->Pos.x, mvp->Pos.y + toolbarHeight),
            ImVec2(mvp->Pos.x + mvp->Size.x, mvp->Pos.y + mvp->Size.y - StatusBarHeight()));
    }
}

void EditorLayer::RenderStatusBar(Scene* scene, Camera* camera, GameClock* clock, bool isPlaying)
{
    auto& reg = scene->GetRegistry();

    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x, mainVp->Pos.y + mainVp->Size.y - StatusBarHeight()),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, StatusBarHeight()), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    // 最深の面（Bg0）＝窓の外の帯。パネルと同色だと続きに見えて読みにくい。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::Bg0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(12.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ui::Px(8.0f, 2.0f));
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav       | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float  ww = ImGui::GetWindowSize().x;
    const float  midY = StatusBarHeight() * 0.5f;
    const float  lineH = ImGui::GetTextLineHeight();

    // 上辺の区切り線（ドックのパネルとの境目）
    dl->AddLine(wp, ImVec2(wp.x + ww, wp.y), ImGui::GetColorU32(theme::Border));

    // 行を帯の中央に置く（AlignTextToFramePadding は FramePadding を足すので使わない）。
    ImGui::SetCursorPosY(std::floor(midY - lineH * 0.5f));

    // ---- 左: 選択中の名前（複数選択なら件数）/ シーンのロード進捗 ----
    if (m_ctx->sceneLoadProgress >= 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::AccentHover);
        ImGui::Text("シーン読み込み中 %.0f%%", m_ctx->sceneLoadProgress * 100.0f);
        ImGui::PopStyleColor();
    }
    else if (m_ctx->HasSelection() && reg.valid(m_ctx->selectedEntity)
             && reg.all_of<NameTag>(m_ctx->selectedEntity))
    {
        // 選択名は通常色（青にしない。アクセントは選択の面とフォーカスだけに使う）
        ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
        ImGui::TextUnformatted(ICON_POINTER);
        ImGui::PopStyleColor();
        ImGui::SameLine(0, ui::Px(6.0f));
        ImGui::TextUnformatted(reg.get<NameTag>(m_ctx->selectedEntity).name.c_str());
        if (m_ctx->selectedEntities.size() > 1)
        {
            ImGui::SameLine(0, ui::Px(6.0f));
            ImGui::TextDisabled("+%zu 件", m_ctx->selectedEntities.size() - 1);
        }
    }
    else
    {
        ImGui::TextDisabled("選択なし");
    }

    // ---- 右: 描画統計（等幅・区切り線つき）→ その左にカメラ速度 ----
    size_t objCount = 0;
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        (void)tag;
        if (!reg.all_of<GridPlane>(e)) ++objCount;
    }

    char seg[4][32];
    snprintf(seg[0], sizeof(seg[0]), "%3.0f FPS",    clock->GetFPS());
    snprintf(seg[1], sizeof(seg[1]), "%4zu obj",     objCount);
    snprintf(seg[2], sizeof(seg[2]), "%5u draws",    m_ctx->statDraws);
    snprintf(seg[3], sizeof(seg[3]), "%5u culled",   m_ctx->statCulled);

    ui::PushMono();
    float segW[4];
    float total = 0.0f;
    const float kGap = ui::Px(12.0f);   // 区切り線の左右の余白
    for (int i = 0; i < 4; ++i)
    {
        segW[i] = ImGui::CalcTextSize(seg[i]).x;
        total += segW[i] + (i ? kGap * 2.0f + ui::Px(1.0f) : 0.0f);
    }
    float x = wp.x + ww - ui::Px(12.0f) - total;
    const float ty = std::floor(wp.y + midY - lineH * 0.5f);
    const ImU32 statCol = ImGui::GetColorU32(isPlaying ? theme::Text : theme::TextDim);
    for (int i = 0; i < 4; ++i)
    {
        if (i)
        {
            x += kGap;
            dl->AddLine(ImVec2(x, wp.y + midY - ui::Px(6.0f)), ImVec2(x, wp.y + midY + ui::Px(6.0f)),
                        ImGui::GetColorU32(theme::BorderStrong), ui::Px(1.0f));
            x += ui::Px(1.0f) + kGap;
        }
        dl->AddText(ImVec2(x, ty), statCol, seg[i]);
        x += segW[i];
    }
    ui::PopMono();
    const float statsLeft = wp.x + ww - ui::Px(12.0f) - total;

    // カメラ設定（速度・感度・グリッド表示）。ツール窓を開かず1クリックで触れる場所に置く。
    char camLabel[64];
    snprintf(camLabel, sizeof(camLabel), "カメラ速度 %.1f", camera->GetMoveSpeed());
    const float camW = ImGui::CalcTextSize(camLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(statsLeft - ui::Px(18.0f) - camW, wp.y + std::floor((StatusBarHeight() - (lineH + ui::Px(4.0f))) * 0.5f)));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ui::Px(8.0f, 2.0f));
    const bool camClicked = ImGui::Button(camLabel);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (camClicked)
        ImGui::OpenPopup("##CamQuickSettings");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("カメラの移動速度（右ドラッグ中にホイールでも変えられます）");
    if (ImGui::BeginPopup("##CamQuickSettings"))
    {
        ImGui::TextDisabled("カメラ");
        f32 speed = camera->GetMoveSpeed();
        ImGui::SetNextItemWidth(ui::Px(200.0f));
        // 対数目盛り: 遅い側(1〜10)を細かく、速い側(〜200)をざっくり動かせる
        if (ui::SliderFloat("移動速度", &speed, 0.2f, 200.0f, "%.1f m/s",
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
        ImGui::SetNextItemWidth(ui::Px(200.0f));
        if (ui::SliderFloat("マウス感度", &sens, 0.0005f, 0.02f, "%.4f"))
            camera->SetMouseSensitivity(sens);

        // グリッド床の表示/非表示。UI やライティングを見るとき邪魔になるので手元に置く。
        ImGui::Separator();
        for (auto [e, gp] : reg.view<GridPlane>().each())
        {
            ui::Checkbox("グリッドを表示", &gp.enabled);
            break;   // グリッドは1枚だけ
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleVar(4);
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
