#pragma once

#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

enum class GizmoMode { Translate, Rotate, Scale };

struct PendingSpawnRequest
{
    std::string modelPath;
    DirectX::XMFLOAT3 position{};
};

class EditorContext
{
public:
    // 選択エンティティ
    entt::entity selectedEntity = entt::null;

    // ギズモ
    GizmoMode gizmoMode      = GizmoMode::Translate;
    bool      gizmoLocalSpace = false;

    // シーンパス
    std::string currentScenePath;

    // 通知フラッシュ
    f32 hotReloadFlash    = 0.0f;
    f32 buildCompleteFlash = 0.0f;

    // 遅延処理キュー
    std::vector<PendingSpawnRequest> pendingSpawns;
    std::vector<entt::entity>        pendingDeletions;
    std::string pendingLoadPath;  // Load ボタンで選択されたシーンパス
    bool pendingBuildGame = false; // Build ボタンが押された

    // 選択解除
    void ClearSelection() { selectedEntity = entt::null; }
};

} // namespace dx12e
