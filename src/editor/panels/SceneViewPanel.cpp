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
#include <cstring>

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

    // 親階層込みのワールド行列でギズモを表示
    bool hasParent = (transform.parent != entt::null && reg.valid(transform.parent));
    XMFLOAT4X4 worldF;
    XMStoreFloat4x4(&worldF, hasParent ? ComputeWorldMatrix(reg, ctx.selectedEntity)
                                       : transform.GetWorldMatrix());

    ImGuizmo::SetOrthographic(ctx.view2D);   // 2D ビューモードは正射ギズモ
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

        // 親がいる場合: 操作後のワールド行列を親の逆行列でローカルに戻す
        XMFLOAT4X4 localF = worldF;
        if (hasParent)
        {
            XMMATRIX parentWorld = ComputeWorldMatrix(reg, transform.parent);
            XMMATRIX local = XMLoadFloat4x4(&worldF)
                           * XMMatrixInverse(nullptr, parentWorld);
            XMStoreFloat4x4(&localF, local);
        }

        float translation[3], rotation[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(
            &localF._11, translation, rotation, scale);
        transform.position = {translation[0], translation[1], translation[2]};
        transform.rotation = {rotation[0], rotation[1], rotation[2]};

        // Scale が 0 以下になると行列が壊れてギズモが消えるので最小値でクランプ
        constexpr float kMinScale = 0.001f;
        scale[0] = (std::max)(scale[0], kMinScale);
        scale[1] = (std::max)(scale[1], kMinScale);
        scale[2] = (std::max)(scale[2], kMinScale);
        transform.scale = {scale[0], scale[1], scale[2]};

        // ライトの向きは Transform 回転に追従（Application 側で direction に反映）するので
        // ここでは特別扱い不要。回転ギズモを回せば光の向きも変わる。

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
    // ※ WantCaptureMouse には依存しない（PassthruCentralNode の挙動次第で
    //   中央ノード上でも true になり得るため）。クリックがビュー矩形内かは下で判定。
    //
    // ギズモによるブロックは「選択がある＝ギズモが実際に描画されている」ときだけ有効にする。
    // 選択が無いと RenderGizmo が Manipulate を呼ばず、ImGuizmo::IsOver() が直前に選択して
    // いたエンティティ位置の当たり判定を保持（ステイル）するため、同じ場所を再クリックしても
    // IsOver()==true で弾かれて再選択できない不具合になる（選択→解除→同じ物を再選択できない）。
    bool gizmoBlocking = ctx.HasSelection() && (ImGuizmo::IsUsing() || ImGuizmo::IsOver());
    if (ImGui::GetIO().KeyAlt                 // Alt+左ドラッグはオービット操作なのでピッキングしない
        || gizmoBlocking
        || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return;

    ImVec2 mousePos = ImGui::GetIO().MousePos;

    if (mousePos.x < vpX || mousePos.x >= vpX + vpW
        || mousePos.y < vpY || mousePos.y >= vpY + vpH)
        return;

    // NDC
    f32 ndcX = ((mousePos.x - vpX) / vpW) * 2.0f - 1.0f;
    f32 ndcY = 1.0f - ((mousePos.y - vpY) / vpH) * 2.0f;

    XMMATRIX view = camera->GetViewMatrix();
    XMMATRIX proj = camera->GetProjectionMatrix();
    XMMATRIX invView     = XMMatrixInverse(nullptr, view);
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);
    const XMVECTOR camRightV = invView.r[0];   // ビルボードスプライトのピッキング展開用
    const XMVECTOR camUpV    = invView.r[1];

    // クリップ空間の near/far をワールドへアンプロジェクトしてレイを作る。
    // 透視でも正射でも正しい（正射はカメラ位置基準だと平行レイにならず破綻する）。
    XMVECTOR pNear = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
    XMVECTOR pFar  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
    pNear = XMVectorScale(pNear, 1.0f / XMVectorGetW(pNear));
    pFar  = XMVectorScale(pFar,  1.0f / XMVectorGetW(pFar));
    XMVECTOR rayOrigin = pNear;
    XMVECTOR rayDir    = XMVector3Normalize(XMVectorSubtract(pFar, pNear));

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

    // レイ vs 三角形（Möller–Trumbore, 両面）。ヒットで t(>0)、外れ/背面平行で -1。
    auto rayTestTri = [&](XMVECTOR v0, XMVECTOR v1, XMVECTOR v2) -> f32 {
        XMVECTOR e1 = XMVectorSubtract(v1, v0);
        XMVECTOR e2 = XMVectorSubtract(v2, v0);
        XMVECTOR p  = XMVector3Cross(rayDir, e2);
        f32 det = XMVectorGetX(XMVector3Dot(e1, p));
        if (std::abs(det) < 1e-8f) return -1.0f;          // レイが三角形と平行
        f32 inv = 1.0f / det;
        XMVECTOR tv = XMVectorSubtract(rayOrigin, v0);
        f32 u = XMVectorGetX(XMVector3Dot(tv, p)) * inv;
        if (u < 0.0f || u > 1.0f) return -1.0f;
        XMVECTOR q = XMVector3Cross(tv, e1);
        f32 v = XMVectorGetX(XMVector3Dot(rayDir, q)) * inv;
        if (v < 0.0f || u + v > 1.0f) return -1.0f;
        f32 t = XMVectorGetX(XMVector3Dot(e2, q)) * inv;
        return t > 0.0f ? t : -1.0f;
    };
    // ワールド空間スプライトのクアッド（描画と同じ 4 隅）でレイ判定。絵の全面がクリック対象になる。
    auto rayTestSprite = [&](const Sprite2D& sp, const XMMATRIX& world) -> f32 {
        const f32 hx = sp.size.x * 0.5f, hy = sp.size.y * 0.5f;
        XMVECTOR tl, tr, br, bl;
        if (sp.billboard)
        {
            const XMVECTOR ctr = world.r[3];
            const f32 sx = XMVectorGetX(XMVector3Length(world.r[0]));
            const f32 sy = XMVectorGetX(XMVector3Length(world.r[1]));
            const XMVECTOR R = XMVectorScale(camRightV, hx * sx);
            const XMVECTOR U = XMVectorScale(camUpV,    hy * sy);
            tl = XMVectorSubtract(XMVectorAdd(ctr, U), R);
            tr = XMVectorAdd(XMVectorAdd(ctr, U), R);
            br = XMVectorAdd(XMVectorSubtract(ctr, U), R);
            bl = XMVectorSubtract(XMVectorSubtract(ctr, U), R);
        }
        else
        {
            tl = XMVector3Transform(XMVectorSet(-hx,  hy, 0.0f, 1.0f), world);
            tr = XMVector3Transform(XMVectorSet( hx,  hy, 0.0f, 1.0f), world);
            br = XMVector3Transform(XMVectorSet( hx, -hy, 0.0f, 1.0f), world);
            bl = XMVector3Transform(XMVectorSet(-hx, -hy, 0.0f, 1.0f), world);
        }
        f32 t0 = rayTestTri(tl, tr, br);
        f32 t1 = rayTestTri(tl, br, bl);
        if (t0 < 0.0f) return t1;
        if (t1 < 0.0f) return t0;
        return (std::min)(t0, t1);
    };

    // 全 Transform 持ちエンティティをピッキング対象にする
    auto pickView = reg.view<const Transform>();
    for (auto [e, transform] : pickView.each())
    {
        if (reg.all_of<GridPlane>(e)) continue;

        // ワールド空間スプライト: 実際のクアッド全面でレイ判定（固定 AABB だと中央しか反応しない）
        if (reg.all_of<Sprite2D>(e))
        {
            const auto& sp = reg.get<Sprite2D>(e);
            if (sp.worldSpace && !sp.texturePath.empty())
            {
                XMMATRIX world = ComputeWorldMatrix(reg, e);
                f32 t = rayTestSprite(sp, world);
                if (t > 0.0f && t < closestDist)
                {
                    closestDist = t;
                    closestEntity = e;
                }
                continue;   // AABB 判定はスキップ
            }
        }

        // 親階層込みのワールド位置/スケールで判定
        XMFLOAT3 wpos   = transform.position;
        XMFLOAT3 wscale = transform.scale;
        if (transform.parent != entt::null && reg.valid(transform.parent))
        {
            XMMATRIX wm = ComputeWorldMatrix(reg, e);
            XMFLOAT4X4 wf;
            XMStoreFloat4x4(&wf, wm);
            wpos = {wf._41, wf._42, wf._43};
            wscale = {
                XMVectorGetX(XMVector3Length(wm.r[0])),
                XMVectorGetX(XMVector3Length(wm.r[1])),
                XMVectorGetX(XMVector3Length(wm.r[2]))};
        }

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
                wpos.x + aabbMin.x * wscale.x,
                wpos.y + aabbMin.y * wscale.y,
                wpos.z + aabbMin.z * wscale.z
            };
            worldMax = {
                wpos.x + aabbMax.x * wscale.x,
                wpos.y + aabbMax.y * wscale.y,
                wpos.z + aabbMax.z * wscale.z
            };
        }
        else
        {
            // MeshRenderer なし (Camera/Light/Empty): 固定サイズ AABB
            constexpr f32 kIconHalf = 0.5f;
            worldMin = {
                wpos.x - kIconHalf,
                wpos.y - kIconHalf,
                wpos.z - kIconHalf
            };
            worldMax = {
                wpos.x + kIconHalf,
                wpos.y + kIconHalf,
                wpos.z + kIconHalf
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

void SceneViewPanel::HandleCameraNavigation(entt::registry& reg,
                                            EditorContext& ctx,
                                            Camera* camera,
                                            f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 m = io.MousePos;
    bool inViewport = m.x >= vpX && m.x < vpX + vpW
                   && m.y >= vpY && m.y < vpY + vpH;

    // 操作ヒントをビュー左下に薄く表示（発見性のため）。/utf-8 でビルドするので
    // 日本語リテラルはそのまま UTF-8 として ImGui に渡せる。
    ImGui::GetForegroundDrawList()->AddText(
        ImVec2(vpX + 12.0f, vpY + vpH - 44.0f),
        IM_COL32(235, 235, 235, 140),
        "Touchpad 2-finger: vert=fwd/back  horiz=left/right  SHIFT=up/down  Ctrl/pinch=zoom(to cursor)");

    ImGui::GetForegroundDrawList()->AddText(
        ImVec2(vpX + 12.0f, vpY + vpH - 26.0f),
        IM_COL32(235, 235, 235, 160),
        "\xe5\x8f\xb3\xe3\x83\x89\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xb0:\xe8\xa6\x96\xe7\x82\xb9  "
        "WASD/Space/Shift:\xe7\xa7\xbb\xe5\x8b\x95  "
        "\xe3\x83\x9b\xe3\x82\xa4\xe3\x83\xbc\xe3\x83\xab:\xe3\x82\xba\xe3\x83\xbc\xe3\x83\xa0  "
        "\xe4\xb8\xad\xe3\x83\x89\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xb0:\xe3\x83\x91\xe3\x83\xb3  "
        "Alt+\xe5\xb7\xa6:\xe5\x91\xa8\xe5\x9b\x9e  F:\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xbc\xe3\x82\xab\xe3\x82\xb9");

    // タッチパッド向け: キーボードフライモードの状態表示（ASCII で確実に出す）
    if (ctx.flyMode)
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(vpX + 12.0f, vpY + 12.0f), IM_COL32(120, 230, 120, 255),
            "FLY MODE  |  WASD: move   Q/E: down/up   Arrows: look   [ ` ] or ESC: exit");
    else
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(vpX + 12.0f, vpY + 12.0f), IM_COL32(210, 210, 210, 130),
            "[ ` ] keyboard fly mode (no mouse needed)");

    // ギズモ操作中、またはカーソルがビュー外なら以降のカメラ操作はしない
    if (ImGuizmo::IsUsing() || !inViewport)
        return;

    // --- 2D ビューモード: パン（中ドラッグ）＋ズーム（ホイール）のみ。回転/ドリーなし ---
    // カメラの正射化・向き固定は Application 側で毎フレーム適用。ここでは x/y パンとズーム量だけ動かす。
    if (ctx.view2D)
    {
        // ホイール: 正射サイズを増減（上スクロールでズームイン）。
        if (io.MouseWheel != 0.0f)
        {
            f32 factor = (io.MouseWheel > 0.0f) ? 0.9f : (1.0f / 0.9f);
            ctx.view2DZoom = std::clamp(ctx.view2DZoom * factor, 0.2f, 1000.0f);
        }
        // 中ドラッグ: パン。回転は 0 固定なので MoveRight/MoveUp はワールド X/Y 平行移動になる。
        // 画面ピクセル → 世界量は「ビュー縦 = 2*zoom」をビュー高で割って換算。
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
        {
            f32 worldPerPixel = (2.0f * ctx.view2DZoom) / (std::max)(1.0f, vpH);
            camera->MoveRight(-io.MouseDelta.x * worldPerPixel);
            camera->MoveUp(io.MouseDelta.y * worldPerPixel);
        }
        return;
    }

    // --- タッチパッド/ホイール ナビゲーション ---
    //   縦スワイプ(二本指)        : 前後
    //   SHIFT + 縦スワイプ          : 上下
    //   Ctrl + 縦スワイプ / ピンチ  : ズーム
    //   横スワイプ(二本指)        : 左右ストレイフ
    //   右ドラッグ中の縦スワイプ    : フライ移動速度の増減
    {
        const f32 step = (std::max)(0.5f, m_orbitDistance * 0.15f);

        // マウスカーソルを通るレイ方向へカメラをドリーする＝カーソル位置を中心にズーム。
        // ピッキングと同じ NDC→ワールドのレイ計算を流用。amount 正でズームイン（カーソルへ前進）。
        auto zoomTowardCursor = [&](f32 amount)
        {
            f32 ndcX = ((m.x - vpX) / vpW) * 2.0f - 1.0f;
            f32 ndcY = 1.0f - ((m.y - vpY) / vpH) * 2.0f;

            XMMATRIX invProj = XMMatrixInverse(nullptr, camera->GetProjectionMatrix());
            XMMATRIX invView = XMMatrixInverse(nullptr, camera->GetViewMatrix());

            XMVECTOR rayClip = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
            XMVECTOR rayEye  = XMVector4Transform(rayClip, invProj);
            rayEye = XMVectorSetZ(rayEye, 1.0f);
            rayEye = XMVectorSetW(rayEye, 0.0f);
            XMVECTOR rayDir  = XMVector3Normalize(XMVector4Transform(rayEye, invView));

            XMFLOAT3 camPosF = camera->GetPosition();
            XMVECTOR pos = XMVectorAdd(XMLoadFloat3(&camPosF), XMVectorScale(rayDir, amount));
            XMFLOAT3 outPos;
            XMStoreFloat3(&outPos, pos);
            camera->SetPosition(outPos);
        };

        if (io.MouseWheel != 0.0f)
        {
            if (io.MouseDown[ImGuiMouseButton_Right])
            {
                f32 speed = camera->GetMoveSpeed();
                speed *= (io.MouseWheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
                camera->SetMoveSpeed(std::clamp(speed, 0.2f, 200.0f));
            }
            else if (io.KeyShift)
            {
                camera->MoveUp(io.MouseWheel * step);              // 上下
            }
            else if (io.KeyCtrl)
            {
                zoomTowardCursor(io.MouseWheel * step * 1.6f);     // ズーム（ピンチ・カーソル中心）
            }
            else
            {
                camera->MoveForward(io.MouseWheel * step);         // 前後（移動）
            }
        }

        // 横スワイプ（二本指 左右）: ストレイフ。右ドラッグ中は速度調整を優先して無効。
        if (io.MouseWheelH != 0.0f && !io.MouseDown[ImGuiMouseButton_Right])
        {
            camera->MoveRight(-io.MouseWheelH * step);             // 左右（符号反転）
        }
    }

    // --- 中ボタンドラッグ: パン ---
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
    {
        f32 panScale = 0.0015f * (std::max)(1.0f, m_orbitDistance);
        camera->MoveRight(-io.MouseDelta.x * panScale);
        camera->MoveUp(io.MouseDelta.y * panScale);
    }

    // --- Alt + 左ドラッグ: 選択物（無ければ前方点）を中心にオービット ---
    if (io.KeyAlt && io.MouseDown[ImGuiMouseButton_Left])
    {
        // ドラッグ開始フレームで pivot と距離を確定
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            XMFLOAT3 camPos = camera->GetPosition();
            XMFLOAT3 fwd    = camera->GetForward();
            XMFLOAT3 pivot  = {camPos.x + fwd.x * 10.0f,
                               camPos.y + fwd.y * 10.0f,
                               camPos.z + fwd.z * 10.0f};

            if (ctx.HasSelection() && reg.valid(ctx.selectedEntity)
                && reg.all_of<Transform>(ctx.selectedEntity))
            {
                const auto& t = reg.get<Transform>(ctx.selectedEntity);
                pivot = t.position;
                if (t.parent != entt::null && reg.valid(t.parent))
                {
                    XMFLOAT4X4 wf;
                    XMStoreFloat4x4(&wf, ComputeWorldMatrix(reg, ctx.selectedEntity));
                    pivot = {wf._41, wf._42, wf._43};
                }
            }

            m_orbitPivot = pivot;
            XMVECTOR p = XMLoadFloat3(&pivot);
            XMVECTOR c = XMLoadFloat3(&camPos);
            m_orbitDistance = (std::max)(0.001f, XMVectorGetX(XMVector3Length(c - p)));
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            f32 sens = camera->GetMouseSensitivity();
            camera->Rotate(io.MouseDelta.x * sens, -io.MouseDelta.y * sens);

            // pivot から距離を保って再配置（forward は Rotate で更新済み）
            XMFLOAT3 fwd = camera->GetForward();
            XMVECTOR p = XMLoadFloat3(&m_orbitPivot);
            XMVECTOR f = XMLoadFloat3(&fwd);
            XMFLOAT3 np;
            XMStoreFloat3(&np, p - f * m_orbitDistance);
            camera->SetPosition(np);
        }
    }
}

void SceneViewPanel::HandleDeleteKey(entt::registry& reg,
                                     EditorContext& ctx,
                                     Scene* /*scene*/,
                                     f32 /*vpX*/, f32 /*vpY*/, f32 /*vpW*/, f32 /*vpH*/)
{
    if (!ctx.HasSelection()) return;
    if (ImGui::GetIO().WantCaptureKeyboard) return;

    // ビューポートの右クリックはフライカメラ専用にしたので、削除は Del キーで行う
    // （右クリック削除は Hierarchy パネルのコンテキストメニューで担保）。
    bool deletePressed = (GetAsyncKeyState(VK_DELETE) & 1) != 0;

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
