#include "editor/panels/SceneViewPanel.h"
#include "editor/EditorContext.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Mesh.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"
#include <DirectXMath.h>
#include <algorithm>

namespace dx12e
{

using namespace DirectX;

void SceneViewPanel::RenderGizmo(entt::registry& reg,
                                 EditorContext& ctx,
                                 Camera* camera,
                                 f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    if (ctx.selectedEntity == entt::null)
        return;

    if (!reg.valid(ctx.selectedEntity) || !reg.all_of<Transform>(ctx.selectedEntity))
        return;

    auto& transform = reg.get<Transform>(ctx.selectedEntity);

    XMFLOAT4X4 viewF, projF;
    XMStoreFloat4x4(&viewF, camera->GetViewMatrix());
    XMStoreFloat4x4(&projF, camera->GetProjectionMatrix());

    XMFLOAT4X4 worldF;
    XMStoreFloat4x4(&worldF, transform.GetWorldMatrix());

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(vpX, vpY, vpW, vpH);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == GizmoMode::Rotate) op = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == GizmoMode::Scale)  op = ImGuizmo::SCALE;

    ImGuizmo::MODE mode = ctx.gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    float snapValues[3] = {1.0f, 1.0f, 1.0f};
    if (ctx.gizmoMode == GizmoMode::Rotate)
        snapValues[0] = snapValues[1] = snapValues[2] = 15.0f;
    else if (ctx.gizmoMode == GizmoMode::Scale)
        snapValues[0] = snapValues[1] = snapValues[2] = 0.1f;
    bool useSnap = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    // ギズモ操作開始を検出して Transform をスナップショット
    bool isUsing = ImGuizmo::IsUsing();
    if (isUsing && !m_gizmoWasUsing)
    {
        // ドラッグ開始: 全選択エンティティの変更前 Transform を保存
        m_gizmoStartTransform = transform;
        m_gizmoStartGroup.clear();
        for (auto e : ctx.selectedEntities)
        {
            if (reg.valid(e) && reg.all_of<Transform>(e))
                m_gizmoStartGroup.push_back({e, reg.get<Transform>(e)});
        }
    }

    if (ImGuizmo::Manipulate(
            &viewF._11, &projF._11,
            op, mode,
            &worldF._11, nullptr,
            useSnap ? snapValues : nullptr))
    {
        XMFLOAT3 oldPos = transform.position;

        float translation[3], rotation[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(
            &worldF._11, translation, rotation, scale);
        transform.position = {translation[0], translation[1], translation[2]};
        transform.rotation = {rotation[0], rotation[1], rotation[2]};

        // Scale が 0 以下になると行列が壊れてギズモが消えるので最小値でクランプ
        constexpr float kMinScale = 0.001f;
        scale[0] = (std::max)(scale[0], kMinScale);
        scale[1] = (std::max)(scale[1], kMinScale);
        scale[2] = (std::max)(scale[2], kMinScale);
        transform.scale = {scale[0], scale[1], scale[2]};

        // マルチ選択時: 移動デルタを他の選択エンティティにも適用
        if (ctx.gizmoMode == GizmoMode::Translate && ctx.selectedEntities.size() > 1)
        {
            XMFLOAT3 delta = {transform.position.x - oldPos.x,
                              transform.position.y - oldPos.y,
                              transform.position.z - oldPos.z};
            for (auto e : ctx.selectedEntities)
            {
                if (e == ctx.selectedEntity) continue;
                if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
                auto& t = reg.get<Transform>(e);
                t.position.x += delta.x;
                t.position.y += delta.y;
                t.position.z += delta.z;
            }
        }
    }

    // ギズモ操作終了を検出して Undo コマンドを push（全選択を 1 コマンドに集約）
    if (!isUsing && m_gizmoWasUsing)
    {
        auto composite = std::make_unique<CompositeCommand>("Transform");
        for (auto& [e, before] : m_gizmoStartGroup)
        {
            if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
            const auto& after = reg.get<Transform>(e);
            bool changed =
                before.position.x != after.position.x ||
                before.position.y != after.position.y ||
                before.position.z != after.position.z ||
                before.rotation.x != after.rotation.x ||
                before.rotation.y != after.rotation.y ||
                before.rotation.z != after.rotation.z ||
                before.scale.x    != after.scale.x ||
                before.scale.y    != after.scale.y ||
                before.scale.z    != after.scale.z;
            if (changed)
                composite->Add(std::make_unique<TransformCommand>(&reg, e, before, after));
        }
        if (!composite->Empty())
            ctx.undoSystem.PushCommand(std::move(composite));
        m_gizmoStartGroup.clear();
    }
    m_gizmoWasUsing = isUsing;
}

void SceneViewPanel::HandlePicking(entt::registry& reg,
                                   EditorContext& ctx,
                                   Camera* camera,
                                   f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    if (ImGui::GetIO().WantCaptureMouse
        || ImGuizmo::IsUsing() || ImGuizmo::IsOver()
        || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return;

    ImVec2 mousePos = ImGui::GetIO().MousePos;

    if (mousePos.x < vpX || mousePos.x >= vpX + vpW
        || mousePos.y < vpY || mousePos.y >= vpY + vpH)
        return;

    // NDC
    f32 ndcX = ((mousePos.x - vpX) / vpW) * 2.0f - 1.0f;
    f32 ndcY = 1.0f - ((mousePos.y - vpY) / vpH) * 2.0f;

    XMMATRIX invProj = XMMatrixInverse(nullptr, camera->GetProjectionMatrix());
    XMMATRIX invView = XMMatrixInverse(nullptr, camera->GetViewMatrix());

    XMVECTOR rayClip = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
    XMVECTOR rayEye = XMVector4Transform(rayClip, invProj);
    rayEye = XMVectorSetZ(rayEye, 1.0f);
    rayEye = XMVectorSetW(rayEye, 0.0f);
    XMVECTOR rayDir = XMVector3Normalize(XMVector4Transform(rayEye, invView));
    XMFLOAT3 camPosF = camera->GetPosition();
    XMVECTOR rayOrigin = XMLoadFloat3(&camPosF);

    f32 closestDist = FLT_MAX;
    entt::entity closestEntity = entt::null;

    XMFLOAT3 orig, dir;
    XMStoreFloat3(&orig, rayOrigin);
    XMStoreFloat3(&dir, rayDir);

    // AABB レイキャスト関数
    auto rayTestAABB = [&](XMFLOAT3 worldMin, XMFLOAT3 worldMax) -> f32 {
        if (worldMin.x > worldMax.x) std::swap(worldMin.x, worldMax.x);
        if (worldMin.y > worldMax.y) std::swap(worldMin.y, worldMax.y);
        if (worldMin.z > worldMax.z) std::swap(worldMin.z, worldMax.z);

        f32 tmin = -FLT_MAX, tmax = FLT_MAX;
        auto slabTest = [&](f32 o, f32 d, f32 bmin, f32 bmax) -> bool {
            if (std::abs(d) < 1e-8f)
                return (o >= bmin && o <= bmax);
            f32 t1 = (bmin - o) / d;
            f32 t2 = (bmax - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = (std::max)(tmin, t1);
            tmax = (std::min)(tmax, t2);
            return tmin <= tmax;
        };
        if (slabTest(orig.x, dir.x, worldMin.x, worldMax.x)
            && slabTest(orig.y, dir.y, worldMin.y, worldMax.y)
            && slabTest(orig.z, dir.z, worldMin.z, worldMax.z)
            && tmax > 0.0f)
        {
            f32 t = tmin > 0.0f ? tmin : tmax;
            return t > 0.0f ? t : -1.0f;
        }
        return -1.0f;
    };

    // 全 Transform 持ちエンティティをピッキング対象にする
    auto pickView = reg.view<const Transform>();
    for (auto [e, transform] : pickView.each())
    {
        if (reg.all_of<GridPlane>(e)) continue;

        XMFLOAT3 worldMin, worldMax;

        if (reg.all_of<MeshRenderer>(e))
        {
            // MeshRenderer あり: メッシュ AABB を使う
            const auto& renderer = reg.get<MeshRenderer>(e);
            XMFLOAT3 aabbMin = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            XMFLOAT3 aabbMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (const auto* mesh : renderer.meshes)
            {
                if (!mesh) continue;
                auto meshMin = mesh->GetAABBMin();
                auto meshMax = mesh->GetAABBMax();
                aabbMin.x = (std::min)(aabbMin.x, meshMin.x);
                aabbMin.y = (std::min)(aabbMin.y, meshMin.y);
                aabbMin.z = (std::min)(aabbMin.z, meshMin.z);
                aabbMax.x = (std::max)(aabbMax.x, meshMax.x);
                aabbMax.y = (std::max)(aabbMax.y, meshMax.y);
                aabbMax.z = (std::max)(aabbMax.z, meshMax.z);
            }
            worldMin = {
                transform.position.x + aabbMin.x * transform.scale.x,
                transform.position.y + aabbMin.y * transform.scale.y,
                transform.position.z + aabbMin.z * transform.scale.z
            };
            worldMax = {
                transform.position.x + aabbMax.x * transform.scale.x,
                transform.position.y + aabbMax.y * transform.scale.y,
                transform.position.z + aabbMax.z * transform.scale.z
            };
        }
        else
        {
            // MeshRenderer なし (Camera/Light/Empty): 固定サイズ AABB
            constexpr f32 kIconHalf = 0.5f;
            worldMin = {
                transform.position.x - kIconHalf,
                transform.position.y - kIconHalf,
                transform.position.z - kIconHalf
            };
            worldMax = {
                transform.position.x + kIconHalf,
                transform.position.y + kIconHalf,
                transform.position.z + kIconHalf
            };
        }

        f32 t = rayTestAABB(worldMin, worldMax);
        if (t > 0.0f && t < closestDist)
        {
            closestDist = t;
            closestEntity = e;
        }
    }

    // Ctrl+クリックでマルチ選択
    bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl)
    {
        if (closestEntity != entt::null)
            ctx.ToggleSelection(closestEntity);
    }
    else
    {
        ctx.Select(closestEntity);
    }
}

void SceneViewPanel::HandleDeleteKey(entt::registry& reg,
                                     EditorContext& ctx,
                                     Scene* /*scene*/,
                                     f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    if (!ctx.HasSelection()) return;
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    bool deletePressed = (GetAsyncKeyState(VK_DELETE) & 1) != 0;

    // 右クリックコンテキストメニュー（ビューポート内のみ）
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    bool inViewport = mousePos.x >= vpX && mousePos.x < vpX + vpW
                   && mousePos.y >= vpY && mousePos.y < vpY + vpH;

    if (inViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && !ImGui::GetIO().WantCaptureMouse)
    {
        ImGui::OpenPopup("##SceneContextMenu");
    }

    if (ImGui::BeginPopup("##SceneContextMenu"))
    {
        if (ImGui::MenuItem("\xe5\x89\x8a\xe9\x99\xa4 (Del)"))
            deletePressed = true;
        ImGui::EndPopup();
    }

    if (deletePressed)
    {
        // マルチ選択の全エンティティを削除
        // Undo コマンドは Application の遅延削除処理で積まれる
        for (auto e : ctx.selectedEntities)
        {
            if (!reg.valid(e)) continue;
            ctx.pendingDeletions.push_back(e);
        }
        ctx.ClearSelection();
    }
}

} // namespace dx12e
