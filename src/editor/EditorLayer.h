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
class CommandPalette;   // Ctrl+K / Ctrl+P（editor/CommandPalette.h）
class ViewportToolbar;  // ビューポート上端の専用ツールバー + ビューキューブ（editor/panels/ViewportToolbar.h）
class WorkspaceManager; // ドックの構築 / レイアウト保存 / ワークスペース / 下部ドック（editor/WorkspaceManager.h）

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

    // ワークスペース / レイアウト管理（UI 自動テストと ToolbarPanel が状態を読む）。
    WorkspaceManager* GetWorkspace() const { return m_workspace.get(); }

    // 下部ステータスバーの高さ。DockSpace はこのぶん短くする（重なると下端のパネルが隠れる）。
    static constexpr f32 kStatusBarHeight = 28.0f;   // 論理 px（100% 表示）
    static f32 StatusBarHeight();                    // 現在の倍率での物理 px（Px(kStatusBarHeight)）

private:
    void BuildDefaultLayout(ImGuiID dockspaceId, f32 toolbarHeight);
    // 画面最下部の細い帯。FPS/描画統計・選択中の名前・カメラ速度をここへ集約する
    //（3D ビューに重ねるとゲーム UI を作るとき邪魔になるため）。
    void RenderStatusBar(Scene* scene, Camera* camera, GameClock* clock, bool isPlaying);

    EditorContext* m_ctx = nullptr;
    bool m_dockspaceBuilt = false;

    // ドックの既定の分割比。★レイアウトは起動時と「レイアウトをリセット」でしか作らない。
    //   以前はツール窓の開閉のたびにドックを壊して作り直しており、実ノードから吸い上げた比の
    //   丸め誤差が積もって、開閉するほどビューポートが縮んでいった（1145x645 → 320x180）。
    //   今はツール窓が右カラムの「インスペクター」のタブとして入るだけで、分割は一切変わらない。
    static constexpr f32 kRatioLeft   = 0.18f;   // 左カラム(ヒエラルキー) / ドックスペース全体
    static constexpr f32 kRatioRight  = 0.24f;   // 右カラム / (全体 - 左)
    static constexpr f32 kRatioBottom = 0.33f;   // 中央下(アセットブラウザ) / センター

    ImVec2 m_viewportPos  = {0, 0};
    ImVec2 m_viewportSize = {1, 1};
    // ビューポート専用ツールバーの帯（中央ノードの上端。3D の矩形のすぐ上）
    ImVec2 m_viewportBarPos = {0, 0};
    float  m_viewportBarW = 0.0f;
    float  m_viewportBarH = 0.0f;

    std::unique_ptr<ToolbarPanel>      m_toolbar;
    std::unique_ptr<HierarchyPanel>    m_hierarchy;
    std::unique_ptr<InspectorPanel>    m_inspector;
    std::unique_ptr<SceneViewPanel>    m_sceneView;
    std::unique_ptr<AssetBrowserPanel> m_assetBrowser;
    std::unique_ptr<ConsolePanel>      m_console;
    std::unique_ptr<CommandPalette>    m_palette;
    std::unique_ptr<ViewportToolbar>   m_viewportBar;
    std::unique_ptr<WorkspaceManager>  m_workspace;
    class ModelThumbnailRenderer* m_thumbRenderer = nullptr;
};

} // namespace dx12e
