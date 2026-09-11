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
class ConsolePanel;

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

    // resourceManager/srvHeap/cmdList はゲーム内 UI プレビュー（UI編集モード時の
    // SceneView 描画）の UIImage テクスチャ遅延ロード用。cmdList は記録中の
    // コマンドリスト（Application::Render の nativeCmdList）を渡す。
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
                f32& cascadeSplitLambda,
                f32& cascadeBlendBand,
                bool& showCascadeDebug,
                GameClock* clock,
                bool& outModeChangeRequested,
                bool& outPendingPlayMode,
                const std::string& assetsDir,
                f32 leftPanelWidth,
                f32 toolbarHeight,
                ResourceManager* resourceManager,
                DescriptorHeap* srvHeap,
                ID3D12GraphicsCommandList* cmdList);

    // フレーム先頭でcmdListが有効な間に呼ぶ（テクスチャサムネイルのアップロード）
    void LoadPendingThumbnails(ID3D12GraphicsCommandList* cmdList);

    // アセットブラウザの即時更新
    void RefreshAssetBrowser();

    // プロジェクト切替時にアセット/スクリプトのルートを差し替える
    void SetAssetRoots(const std::string& assetsDir, const std::string& scriptsDir);

    // サムネイルレンダラー設定
    void SetThumbnailRenderer(class ModelThumbnailRenderer* renderer);

    // マテリアル球体サムネイル(アセットブラウザへ転送するだけ)
    void SetMaterialPreviewRenderer(class MaterialPreviewRenderer* renderer);

    // ビューポート領域（3D描画のオフセット計算用）。
    // 戻り値は「メインウィンドウのクライアント領域基準」のピクセル座標。multi-viewport有効時は
    // ImGui座標(=スクリーン座標)からメインビューポート原点を引いて変換する
    // (スワップチェインの D3D12 ビューポート矩形にそのまま使えるようにするため)。
    ImVec2 GetViewportPos()  const
    {
        const ImVec2 vp = ImGui::GetMainViewport()->Pos;
        return ImVec2(m_viewportPos.x - vp.x, m_viewportPos.y - vp.y);
    }
    ImVec2 GetViewportSize() const { return m_viewportSize; }

    // マテリアルエディタ/ライブラリ等、Application 直属のフローティングツール窓が
    // サムネイルキャッシュを使い回すためのアクセサ(InspectorPanelと同じ理由)。
    AssetBrowserPanel* GetAssetBrowser() const { return m_assetBrowser.get(); }

    // 下部ステータスバーの高さ。DockSpace はこのぶん短くする（重なると下端のパネルが隠れる）。
    static constexpr f32 kStatusBarHeight = 30.0f;

private:
    void BuildDefaultLayout(ImGuiID dockspaceId, f32 toolbarHeight);
    // 画面最下部の細い帯。FPS/描画統計・選択中の名前・カメラ速度をここへ集約する
    //（3D ビューに重ねるとゲーム UI を作るとき邪魔になるため）。
    void RenderStatusBar(Scene* scene, Camera* camera, GameClock* clock, bool isPlaying);

    EditorContext* m_ctx = nullptr;
    bool m_dockspaceBuilt = false;
    // ツール窓が「1個でも開いているか」の前フレーム状態。空⇔非空に変わった時だけ
    // レイアウトを作り直す（右下タブ領域を出す/畳んで Inspector を全高に戻す）。
    bool m_prevAnyToolShown = false;

    // ★ユーザーがドラッグで決めた分割比。BuildDefaultLayout は DockBuilderRemoveNode で
    //   ドックツリーを丸ごと壊すので、既定値を焼き込んだままだと**ツール窓を 1 個開閉する
    //   たびに調整した幅が既定へ戻る**（毎日使うと効いてくる種類の不便）。
    //   作り直す前に実ノードの寸法から比を吸い上げ、作り直しでそれを使う。
    //   ＝「ツール窓の開閉でレイアウトを作り直す」という元の意図は保ったまま、
    //     ユーザーの調整だけを引き継ぐ。
    f32 m_ratioLeft        = 0.18f;   // 左カラム(ヒエラルキー) / ドックスペース全体
    f32 m_ratioRight       = 0.24f;   // 右カラム / (全体 - 左)
    f32 m_ratioBottom      = 0.33f;   // 中央下(アセットブラウザ) / センター
    f32 m_ratioRightBottom = 0.42f;   // 右下(ツールタブ) / 右カラム
    // 比を吸い上げるためのノード id（BuildDefaultLayout が毎回書く。未構築は 0）。
    ImGuiID m_nodeLeft = 0, m_nodeRightCol = 0, m_nodeBottom = 0, m_nodeRightBottom = 0;
    void CaptureDockRatios();

    ImVec2 m_viewportPos  = {0, 0};
    ImVec2 m_viewportSize = {1, 1};

    std::unique_ptr<ToolbarPanel>      m_toolbar;
    std::unique_ptr<HierarchyPanel>    m_hierarchy;
    std::unique_ptr<InspectorPanel>    m_inspector;
    std::unique_ptr<SceneViewPanel>    m_sceneView;
    std::unique_ptr<AssetBrowserPanel> m_assetBrowser;
    std::unique_ptr<ConsolePanel>      m_console;
    class ModelThumbnailRenderer* m_thumbRenderer = nullptr;
};

} // namespace dx12e
