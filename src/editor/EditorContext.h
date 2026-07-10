#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "core/mcp/McpDeferred.h"
#include "editor/UndoSystem.h"
#include "editor/EditorIcons.h"

namespace dx12e
{

enum class GizmoMode { Translate, Rotate, Scale };

// ツールバーのPlayドロップダウンで選ぶマルチプレイのテストロール(フェーズ⑨)。
// EnterPlayModeがこれを見てnet:host()/net:join()相当を自動実行する(Lua無しでの手早い動作確認用)。
enum class NetTestRole { Offline, Host, Client };

struct PendingSpawnRequest
{
    std::string modelPath;
    DirectX::XMFLOAT3 position{};
    std::string name;   // 空ならデフォルト名。MCP の create_entity から任意名を付けるのに使う。
    McpDeferred mcp;    // MCP 由来なら相関情報。生成後に本物の entityId を送り返す。client=0 で無効。
};

// MCP 由来のエンティティ削除要求。フレーム境界でサブツリー削除後に deletedCount を送り返す。
// エディタ UI からの削除は従来どおり pendingDeletions(下) を使い、応答は送らない。
struct McpPendingDelete
{
    entt::entity entity = entt::null;
    McpDeferred  mcp;
};

struct PendingScriptAttach
{
    entt::entity entity = entt::null;
    std::string  scriptPath;   // assets 相対パス
};

// アセットブラウザからテクスチャをドラッグ&ドロップしてマテリアルに割り当てる要求
// (SceneView 上のメッシュへドロップ、または Inspector のマテリアルスロットへドロップ)。
// テクスチャロード(GetOrLoadTexture)が cmdList を要するためフレーム境界で処理する。
enum class MaterialTextureSlot { Albedo, Normal, MetalRoughness };
struct PendingMaterialTextureDrop
{
    entt::entity entity = entt::null;
    u32 submeshIndex = 0;              // MeshRenderer::meshes[] のインデックス
    MaterialTextureSlot slot = MaterialTextureSlot::Albedo;
    std::string texturePath;           // 絶対パス(処理側で assets 相対に正規化)
};


class EditorContext
{
public:
    // ---- マルチ選択 ----
    entt::entity selectedEntity = entt::null;   // プライマリ（後方互換）
    std::vector<entt::entity> selectedEntities; // 全選択リスト

    bool IsSelected(entt::entity e) const
    {
        return std::find(selectedEntities.begin(), selectedEntities.end(), e)
               != selectedEntities.end();
    }

    void Select(entt::entity e)
    {
        selectedEntities.clear();
        selectedEntity = e;
        if (e != entt::null)
            selectedEntities.push_back(e);
    }

    void AddToSelection(entt::entity e)
    {
        if (e == entt::null) return;
        if (!IsSelected(e))
            selectedEntities.push_back(e);
        selectedEntity = e;
    }

    void ToggleSelection(entt::entity e)
    {
        if (e == entt::null) return;
        auto it = std::find(selectedEntities.begin(), selectedEntities.end(), e);
        if (it != selectedEntities.end())
        {
            selectedEntities.erase(it);
            selectedEntity = selectedEntities.empty() ? entt::null : selectedEntities.back();
        }
        else
        {
            selectedEntities.push_back(e);
            selectedEntity = e;
        }
    }

    void ClearSelection()
    {
        selectedEntity = entt::null;
        selectedEntities.clear();
    }

    bool HasSelection() const { return !selectedEntities.empty(); }

    // ギズモ
    GizmoMode gizmoMode      = GizmoMode::Translate;
    bool      gizmoLocalSpace = false;

    // 2D ビューモード（Unity の 2D ボタン相当）。ON 中はエディタカメラを正射＋XY平面正対に固定し、
    // 回転/ドリーを禁止（中ドラッグでパン、ホイールでズーム）。ギズモも正射モードで表示する。
    bool  view2D     = false;
    float view2DZoom = 6.0f;   // 正射の縦半分（世界単位）。小さいほどズームイン。

    // UI 編集モード（ツールバーの「UI」トグル）。ON 中は非 Play でも SceneView に
    // ゲーム内 UI（UICanvas ツリー）をプレビュー描画し、UI 要素のクリック選択・
    // ドラッグ編集を 3D ピッキングより優先する。Play 中/ゲームモードは従来どおり
    // ##GameUI 経路のみが描く（二重描画しない）。
    bool uiEditMode = false;

    // メニュー「表示 > レイアウトをリセット」で立てるフラグ。
    // EditorLayer が検知してデフォルトドックレイアウトを再構築する。
    bool resetLayout = false;

    // ---- ツール窓の表示トグル（既定OFF=スッキリ中核4窓。ツールバー「窓」/メニュー「ウィンドウ」から開閉）----
    // ON の窓だけ Inspector の下（右カラム下部）にタブで現れる。全OFFなら Inspector が右カラム全高。
    bool showPostProcess    = false;
    bool showPostParams     = false;
    bool showSkybox         = false;   // Skybox / IBL
    bool showSSAO           = false;
    bool showEngineSettings = false;
    bool showSceneFlow      = false;
    bool showProject        = false;
    bool showVersionControl = false;
    bool showMcpBridge      = false;   // MCP / AI Bridge モニタ窓
    bool showBuildSettings  = false;   // ビルド設定（Unity の Build Settings 相当）
    bool showNetworkStatus  = false;   // マルチプレイ状態モニタ窓（接続一覧/tick/帯域、フェーズ⑧）
    bool showNetworkSettings = false;  // マルチプレイ設定窓（tickRate/port等、assets/network.json、フェーズ⑨）

    // ---- マルチプレイのローカルテストループ（ツールバーPlayドロップダウン、フェーズ⑨）----
    NetTestRole netTestRole = NetTestRole::Offline;   // 次にPlayした時に自動でHost/Joinするか
    std::string netTestJoinAddress = "127.0.0.1";     // Client選択時の接続先IP
    u16 netTestJoinPort = 0;                          // 0=NetworkConfig.defaultPortを使う。--net-client CLI 起動時のみ明示指定
    bool netTestLaunchClientRequested = false;        // 「テストクライアント起動」ボタン押下(フレーム境界でCreateProcess)
    // パーティクルエディタ（VFXアセットの作成・編集）。プレビュー枠+アセット一覧+パラメータで
    // 幅を食うため、右下タブ群（右カラム24%のさらに下42%）には収まらない。他の設定系ツール窓と
    // 違い右下タブにはドックせず独立したフローティング窓として開く＝ AnyToolWindowOpen には含めない
    // （含めると右下タブ領域だけ確保されて中身が無い＝Inspectorが無駄に縮む）。
    bool showVfxEditor      = false;
    // マテリアルエディタ/マテリアルライブラリ(Poly Havenダウンローダー)も VfxEditor 同様、
    // 独立フローティング窓として開く(AnyToolWindowOpen には含めない)。
    bool showMaterialEditor  = false;
    bool showMaterialLibrary = false;
    // AssetBrowser で .dxmat をダブルクリックした時に立つ。MaterialEditorPanel が消費して開く。
    std::string pendingOpenMaterialPath;

    bool AnyToolWindowOpen() const
    {
        return showPostProcess || showPostParams || showSkybox || showSSAO
            || showEngineSettings || showSceneFlow || showProject || showVersionControl
            || showMcpBridge || showBuildSettings || showNetworkStatus || showNetworkSettings;
    }

    // タッチパッド向けキーボードフライモード（` キーでトグル）。
    // ON 中は WASD 移動 / Q・E 上下 / 矢印キーで視点回転（マウス・ボタン長押し不要）。
    // ON 中はギズモの W/E/R 切替ショートカットを抑制する。
    bool flyMode = false;

    // 3D ビューポート（ドックスペース中央ノード）の矩形（ImGui 座標）。
    // EditorLayer が毎フレーム更新。Application のフライカメラ発動判定
    // （カーソルが 3D ビュー上にあるか）に使う。ImGui の PassthruCentralNode の
    // WantCaptureMouse 挙動に依存せず確実にビュー上判定するための値。
    f32 viewportX = 0.0f;
    f32 viewportY = 0.0f;
    f32 viewportW = 0.0f;
    f32 viewportH = 0.0f;

    // フローティングツール窓(マテリアルエディタ/マテリアルライブラリ/パーティクルエディタ等、
    // ImGuiWindowFlags_NoDockingで浮かぶ独立窓)がホバーされているか。
    // これらの窓は 3D ビューポート矩形の上に重なって浮かぶことがあるため、IsCursorInViewport が
    // 矩形だけで判定すると「窓の上でドラッグ/ホイール操作した」のがそのままシーンカメラにも
    // 効いてしまう(窓に隠れているだけの3Dビューを触っている扱いになる)。ここで弾く。
    //
    // 2値ラッチにしている理由: 各パネルが ThisFrame を立てるのは ImGui パスの「後半」
    // (Application::Render の EditorLayer::Render より後)なので、同一フレーム内のリセット+読み取り
    // だと HandleCameraNavigation は常に false を読んでしまう。フレーム冒頭で前フレームの
    // ThisFrame を hovered へ確定コピーし、読む側は常に「前フレームの確定値」を見る(1フレーム遅延)。
    bool floatingToolWindowHovered = false;           // 読み取り用(前フレームの確定値)
    bool floatingToolWindowHoveredThisFrame = false;  // 書き込み用(各パネルが今フレーム立てる)

    bool IsCursorInViewport(f32 mx, f32 my) const
    {
        return !floatingToolWindowHovered
            && viewportW > 0.0f && viewportH > 0.0f
            && mx >= viewportX && mx < viewportX + viewportW
            && my >= viewportY && my < viewportY + viewportH;
    }

    // カメラプレビュー用テクスチャの GPU ハンドル（ImTextureID）。
    // Application が選択カメラ視点を描画してセット。0 ならプレビュー非表示。
    // EditorLayer がシーンビュー隅に ImGui::Image でオーバーレイ表示する。
    u64 cameraPreviewTexHandle = 0;

    // シーンパス
    std::string currentScenePath;

    // 通知フラッシュ
    f32 hotReloadFlash    = 0.0f;
    f32 buildCompleteFlash = 0.0f;
    f32 buildErrorFlash    = 0.0f;  // ビルド失敗表示（>0 の間 赤で「✗ ビルド失敗」）
    std::string buildErrorMsg;      // ビルド失敗の具体理由（空なら汎用メッセージ）

    // エラー通知（Play 不可等）
    std::string errorMessage;
    f32 errorFlash = 0.0f;

    // 遅延処理キュー
    std::vector<PendingSpawnRequest> pendingSpawns;
    std::vector<entt::entity>        pendingDuplications;  // Ctrl+D / 右クリック複製
    std::vector<std::string>         pendingPastes;        // Ctrl+V (エンティティJSON)
    std::vector<entt::entity>        pendingDeletions;
    std::vector<McpPendingDelete>    mcpDeletions;         // MCP 由来削除（応答に deletedCount を返す）
    std::vector<McpPendingDelete>    mcpDuplications;      // MCP 由来複製（.entity=複製元。応答に複製先 id を返す）
    // Undo/Redo はエンティティ復元（モデル再ロード）を伴う場合があるため
    // cmdList が有効なフレーム境界まで遅延する
    bool pendingUndo = false;
    bool pendingRedo = false;
    std::vector<PendingScriptAttach> pendingScriptAttachments;
    std::vector<PendingMaterialTextureDrop> pendingMaterialTextureDrops;
    // 選択エンティティ（+子孫）を .prefab として書き出す要求。Application がフレーム境界で処理。
    entt::entity pendingCreatePrefab = entt::null;
    std::string pendingLoadPath;
    std::string pendingGameLoadPath;  // Play 中の loadScene()（assets 相対）。フレーム境界で安全にロード
    bool pendingBuildGame = false;     // ビルド設定パネルの「ビルド」で立つ＝実行要求
    // ファイルメニュー「プロジェクトを閉じる」で立つ。Application がフレーム境界で
    // ランチャーへ戻す（削除等のファイル操作は一切不要＝現在の状態はそのままメモリに残す）。
    bool pendingCloseProject = false;
    // 直近ビルドの成果物フォルダ（完了後に Explorer で開くために保持）。
    std::string lastBuildDir;

    // ===== ビルド設定（Unity の Build Settings / Unreal の Packaging に相当）=====
    // ビルド設定ウィンドウ（showBuildSettings）で編集し、BuildGame() が参照する。
    struct BuildConfig
    {
        char title[128] = "Game";           // 製品名（game.pak マニフェストの title・ゲーム窓のタイトル）
        int  width  = 1280;                 // ゲームウィンドウ幅
        int  height = 720;                  // ゲームウィンドウ高
        std::string startScene;             // 開始シーン（assets 相対。空=現在開いているシーン）
        std::string outputDir;              // 配置先の親フォルダ（空=ビルド時に選択）
        bool openFolderAfterBuild = true;   // 完了後に成果物フォルダを開く
    } buildConfig;

    bool pendingNewScene  = false;
    // 空でなければ pendingNewScene 実行時にこのパスへスターターシーンを保存する
    // （プロジェクト新規作成の初期シーン用。通常の「新規シーン」は空のまま）
    std::string pendingNewScenePath;
    bool showNewSceneDialog = false;
    bool newSceneDialogIsCreate = true;  // true=新規作成, false=名前を付けて保存
    char newSceneNameBuf[128] = {};

    // スクリプト作成ダイアログ
    bool showNewScriptDialog = false;
    char newScriptNameBuf[128] = {};

    // カスタムシェーダー作成ダイアログ
    bool showNewShaderDialog = false;
    char newShaderNameBuf[128] = {};

    // Undo/Redo
    UndoSystem undoSystem;

    // クリップボード（Ctrl+C/V 用。エンティティの JSON スナップショット）
    std::vector<std::string> clipboard;

    // エディタ UI アイコン（Application が所有・populate。null/0 ならテキスト表示にフォールバック）
    const EditorUiIcons* icons = nullptr;
};

} // namespace dx12e
