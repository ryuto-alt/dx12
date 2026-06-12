#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "editor/UndoSystem.h"

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
    bool pendingBuildGame = false;
    bool pendingNewScene  = false;
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
};

} // namespace dx12e
