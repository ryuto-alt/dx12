#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "editor/UndoSystem.h"
#include "editor/EditorIcons.h"

namespace dx12e
{

enum class GizmoMode { Translate, Rotate, Scale };

struct PendingSpawnRequest
{
    std::string modelPath;
    DirectX::XMFLOAT3 position{};
};

struct PendingScriptAttach
{
    entt::entity entity = entt::null;
    std::string  scriptPath;   // assets 相対パス
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

    // メニュー「表示 > レイアウトをリセット」で立てるフラグ。
    // EditorLayer が検知してデフォルトドックレイアウトを再構築する。
    bool resetLayout = false;

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

    bool IsCursorInViewport(f32 mx, f32 my) const
    {
        return viewportW > 0.0f && viewportH > 0.0f
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

    // エラー通知（Play 不可等）
    std::string errorMessage;
    f32 errorFlash = 0.0f;

    // 遅延処理キュー
    std::vector<PendingSpawnRequest> pendingSpawns;
    std::vector<entt::entity>        pendingDuplications;  // Ctrl+D / 右クリック複製
    std::vector<std::string>         pendingPastes;        // Ctrl+V (エンティティJSON)
    std::vector<entt::entity>        pendingDeletions;
    // Undo/Redo はエンティティ復元（モデル再ロード）を伴う場合があるため
    // cmdList が有効なフレーム境界まで遅延する
    bool pendingUndo = false;
    bool pendingRedo = false;
    std::vector<PendingScriptAttach> pendingScriptAttachments;
    std::string pendingLoadPath;
    std::string pendingGameLoadPath;  // Play 中の loadScene()（assets 相対）。フレーム境界で安全にロード
    bool pendingBuildGame = false;
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

    // Undo/Redo
    UndoSystem undoSystem;

    // クリップボード（Ctrl+C/V 用。エンティティの JSON スナップショット）
    std::vector<std::string> clipboard;

    // エディタ UI アイコン（Application が所有・populate。null/0 ならテキスト表示にフォールバック）
    const EditorUiIcons* icons = nullptr;
};

} // namespace dx12e
