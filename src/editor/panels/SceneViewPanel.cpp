#include "editor/panels/SceneViewPanel.h"
#include "editor/EditorContext.h"
#include "editor/UiEditUtil.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Mesh.h"
#include "scene/Scene.h"
#include "ui/UISystem.h"

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

namespace
{
    // ImGuizmo の DecomposeMatrixToComponents は回転を Rx*Ry*Rz 順で分解するが、
    // このエンジンの Transform::GetWorldMatrix は XMMatrixRotationRollPitchYaw
    // (= Rz*Rx*Ry 順) で行列を組み立てる。順序が食い違うため、ギズモが作った行列を
    // ImGuizmo 分解→オイラー保存→次フレームに RollPitchYaw で再構築すると回転が
    // 毎フレーム壊れていた（マウスと回転が合わない・オブジェクトごとに挙動が変わる原因）。
    // ここでは GetWorldMatrix と完全に逆対応する分解を行い、ラウンドトリップを無損失にする。
    // (Rz*Rx*Ry を展開した行列成分から閉形式で抽出。20万ケースのランダム検証で誤差ゼロ確認済み)
    void DecomposeWorldToRPY(const XMFLOAT4X4& w, float t[3], float eulerDeg[3], float s[3])
    {
        constexpr float kRad2Deg = 57.2957795130823f;

        // スケール = 各基底行の長さ
        const float sx = sqrtf(w.m[0][0]*w.m[0][0] + w.m[0][1]*w.m[0][1] + w.m[0][2]*w.m[0][2]);
        const float sy = sqrtf(w.m[1][0]*w.m[1][0] + w.m[1][1]*w.m[1][1] + w.m[1][2]*w.m[1][2]);
        const float sz = sqrtf(w.m[2][0]*w.m[2][0] + w.m[2][1]*w.m[2][1] + w.m[2][2]*w.m[2][2]);
        s[0] = sx; s[1] = sy; s[2] = sz;

        // 平行移動 = 第4行
        t[0] = w.m[3][0]; t[1] = w.m[3][1]; t[2] = w.m[3][2];

        // 回転行を正規化（スケール除去）
        const float i0 = sx > 1e-8f ? 1.0f / sx : 0.0f;
        const float i1 = sy > 1e-8f ? 1.0f / sy : 0.0f;
        const float i2 = sz > 1e-8f ? 1.0f / sz : 0.0f;
        float m[3][3];
        for (int j = 0; j < 3; ++j)
        {
            m[0][j] = w.m[0][j] * i0;
            m[1][j] = w.m[1][j] * i1;
            m[2][j] = w.m[2][j] * i2;
        }

        // R = Rz(z)*Rx(x)*Ry(y) の成分: m[2][1]=-sin x, m[2][0]=cx*sy, m[2][2]=cx*cy,
        //                              m[0][1]=sz*cx, m[1][1]=cz*cx
        float sinx = -m[2][1];
        sinx = sinx > 1.0f ? 1.0f : (sinx < -1.0f ? -1.0f : sinx);
        const float cosx = sqrtf(m[2][0]*m[2][0] + m[2][2]*m[2][2]); // = |cos x|
        const float x = asinf(sinx);
        float y, z;
        if (cosx > 1e-6f)
        {
            y = atan2f(m[2][0], m[2][2]);
            z = atan2f(m[0][1], m[1][1]);
        }
        else
        {
            // ジンバルロック (cos x ~ 0): 行列は (y - sgn*z) のみに依存。z=0 に固定。
            const float sgn = (sinx >= 0.0f) ? 1.0f : -1.0f;
            y = atan2f(sgn * m[1][0], m[0][0]);
            z = 0.0f;
        }
        eulerDeg[0] = x * kRad2Deg;
        eulerDeg[1] = y * kRad2Deg;
        eulerDeg[2] = z * kRad2Deg;
    }

    // ---- UI 編集モード（HandleUiEditing）----
    // m_uiDragEdges の値: リサイズ中はエッジ bit の組み合わせ（四隅は 2bit）
    constexpr int kUiDragNone = -1;   // 非ドラッグ
    constexpr int kUiDragMove = 0;    // 移動ドラッグ
    constexpr int kUiEdgeL = 1, kUiEdgeR = 2, kUiEdgeT = 4, kUiEdgeB = 8;

    constexpr f32 kUiHandleHalf    = 4.0f;   // リサイズハンドル矩形の半径（描画、スクリーン px）
    constexpr f32 kUiHandleHitHalf = 6.0f;   // ハンドルの当たり判定半径（掴みやすく描画より広め）

    // 選択アウトライン / ハンドルの色（テーマの Accent 0x4c8dff に合わせる）
    constexpr ImU32 kUiAccentCol    = IM_COL32(76, 141, 255, 255);
    constexpr ImU32 kUiHandleCol    = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kUiHandleHotCol = IM_COL32(255, 204, 26, 255);   // ImGuizmo の SELECTION 相当
}

void SceneViewPanel::RenderUiPreview(entt::registry& reg,
                                     EditorContext& ctx,
                                     f32 vpX, f32 vpY, f32 vpW, f32 vpH,
                                     ResourceManager* resources,
                                     DescriptorHeap* srvHeap,
                                     ID3D12GraphicsCommandList* cmdList)
{
    if (!ctx.uiEditMode || vpW <= 0.0f || vpH <= 0.0f)
        return;

    // ギズモ（RenderGizmo）と同じ背景 DrawList に描く: 全 ImGui ウィンドウより先に
    // 描かれるため、バックバッファ直描きの 3D シーンの上・各パネルの下に自然に重なる。
    // レターボックス帯や UICanvas 外へのはみ出しがビューポート外に掛からないよう、
    // ##GameUI 経路の PushClipRect と同様にビューポート矩形でクリップする。
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);
    UISystem::RenderPreview(reg, dl, vpX, vpY, vpW, vpH, resources, srvHeap, cmdList);
    dl->PopClipRect();
}

void SceneViewPanel::RenderGizmo(entt::registry& reg,
                                 EditorContext& ctx,
                                 Camera* camera,
                                 f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    if (ctx.selectedEntity == entt::null)
        return;

    if (!reg.valid(ctx.selectedEntity) || !reg.all_of<Transform>(ctx.selectedEntity))
        return;

    // UI 編集モード中は UIRect 持ちの選択に 3D ギズモを出さない
    // （HandleUiEditing の矩形アウトライン + 8 ハンドルが移動/リサイズを担う）。
    if (ctx.uiEditMode && reg.all_of<UIRect>(ctx.selectedEntity))
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

    // ImGuizmo は既定で「カメラに近い側」に軸を反転表示する(AllowAxisFlip)。
    // カメラを動かすたびに軸の向きが入れ替わって見えて操作方向が分からなくなるため無効化し、
    // 軸の向きをカメラ位置に関係なく常に固定する。
    ImGuizmo::AllowAxisFlip(false);

    // 軸が奥を向くと ImGuizmo は軸を反転(flip)させ、その印として黒い破線(ハッチング)を
    // 矢印上に重ねて描く。これが X/Z 矢印に黒い線が走るように見える原因なので、太さ 0 にして
    // 描画を抑止する（反転自体は掴みやすさのため残す）。GetStyle はグローバル状態なので毎フレーム設定で良い。
    ImGuizmo::GetStyle().HatchedAxisLineThickness = 0.0f;

    // 回転ギズモが「細くて暗くて小さい」と、どの輪を掴んでるか分からず変な軸で回してしまう。
    // 線を太く・色を鮮やかに・サイズを大きくして、どの軸の輪か一目で分かるようにする。
    {
        ImGuizmo::Style& gz = ImGuizmo::GetStyle();
        gz.RotationLineThickness      = 4.0f;   // 軸リング（既定2.0）
        gz.RotationOuterLineThickness = 3.0f;   // 外周のスクリーン回転円
        gz.TranslationLineThickness   = 4.0f;
        gz.Colors[ImGuizmo::DIRECTION_X] = ImVec4(0.95f, 0.22f, 0.22f, 1.0f); // 鮮やかな赤=X
        gz.Colors[ImGuizmo::DIRECTION_Y] = ImVec4(0.30f, 0.90f, 0.30f, 1.0f); // 鮮やかな緑=Y
        gz.Colors[ImGuizmo::DIRECTION_Z] = ImVec4(0.25f, 0.55f, 1.00f, 1.0f); // 鮮やかな青=Z
        gz.Colors[ImGuizmo::SELECTION]   = ImVec4(1.00f, 0.80f, 0.10f, 0.90f); // ホバー時の黄色をくっきり
    }
    ImGuizmo::SetGizmoSizeClipSpace(0.15f);   // 既定0.1より大きく＝掴みやすく・見やすく

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == GizmoMode::Rotate) op = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == GizmoMode::Scale)  op = ImGuizmo::SCALE;

    ImGuizmo::MODE mode = ctx.gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    // 回転ギズモは常にワールド軸（まっすぐ）で表示・操作する。
    // ローカルにするとオブジェクトの現在の回転にリングが追従して傾き、
    // 「オブジェクトごとに向きが変わる」状態になるため、回転はワールド固定にする。
    if (ctx.gizmoMode == GizmoMode::Rotate)
        mode = ImGuizmo::WORLD;

    float snapValues[3] = {1.0f, 1.0f, 1.0f};
    if (ctx.gizmoMode == GizmoMode::Rotate)
        snapValues[0] = snapValues[1] = snapValues[2] = 15.0f;
    else if (ctx.gizmoMode == GizmoMode::Scale)
        snapValues[0] = snapValues[1] = snapValues[2] = 0.1f;
    bool useSnap = ImGui::GetIO().KeyCtrl;   // ImGui 経由＝ウィンドウがフォーカスされている時だけ

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
        // ImGuizmo::DecomposeMatrixToComponents は回転順が GetWorldMatrix と食い違うので使わない。
        // エンジンの RollPitchYaw と完全に逆対応する自前分解で無損失ラウンドトリップにする。
        DecomposeWorldToRPY(localF, translation, rotation, scale);
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

// UI 編集モードの UI 要素編集（クリック選択 + ドラッグ移動 + 8 ハンドルリサイズ + Undo）。
// EditorLayer が RenderGizmo（ImGuizmo の当たり判定確定）の後・HandlePicking の前に呼ぶ。
// UI 要素にヒットしたクリックは m_uiClickConsumed に記録し、HandlePicking が 3D ピッキングを
// スキップする（ヒット無しなら従来どおり 3D ピッキングへフォールスルー）。
void SceneViewPanel::HandleUiEditing(entt::registry& reg,
                                     EditorContext& ctx,
                                     f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    m_uiClickConsumed = false;

    if (!ctx.uiEditMode || vpW <= 0.0f || vpH <= 0.0f)
    {
        m_uiDragEdges = kUiDragNone;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mousePos = io.MousePos;
    const bool inViewport = !ctx.floatingToolWindowHovered
        && mousePos.x >= vpX && mousePos.x < vpX + vpW
        && mousePos.y >= vpY && mousePos.y < vpY + vpH;

    // ---- ドラッグ継続 / 終了（開始済みドラッグはビュー外に出てもボタンを離すまで追従）----
    if (m_uiDragEdges != kUiDragNone)
    {
        m_uiClickConsumed = true;   // ドラッグ中は 3D ピッキングへ一切渡さない

        if (!reg.valid(m_uiDragEntity) || !reg.all_of<UIRect>(m_uiDragEntity))
        {
            m_uiDragEdges = kUiDragNone;   // ドラッグ中に対象が消えた（削除等）
        }
        else if (!io.MouseDown[ImGuiMouseButton_Left])
        {
            // ドラッグ終了: 変化があれば Undo コマンドを push（ギズモドラッグと同じ定型）
            const UIRect& before = m_uiDragStartRect;
            const UIRect& after  = reg.get<UIRect>(m_uiDragEntity);
            bool changed =
                before.offsetMin.x != after.offsetMin.x ||
                before.offsetMin.y != after.offsetMin.y ||
                before.offsetMax.x != after.offsetMax.x ||
                before.offsetMax.y != after.offsetMax.y;
            if (changed)
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<UIRect>>(
                    &reg, m_uiDragEntity, before, after, "UI Rect"));
            m_uiDragEdges = kUiDragNone;
        }
        else
        {
            // 「開始スナップショット + 総移動量」の絶対値方式で更新（増分方式のドリフト防止）。
            // スクリーンΔ → キャンバス px は canvasScale で割るだけ（Inspector と同じ換算）。
            const f32 scale = (m_uiDragCanvasScale > 1e-6f) ? m_uiDragCanvasScale : 1.0f;
            const f32 dxC = (mousePos.x - m_uiDragStartMouse.x) / scale;
            const f32 dyC = (mousePos.y - m_uiDragStartMouse.y) / scale;

            auto& rect = reg.get<UIRect>(m_uiDragEntity);
            rect = m_uiDragStartRect;

            if (m_uiDragEdges == kUiDragMove)
            {
                rect.offsetMin.x += dxC; rect.offsetMax.x += dxC;
                rect.offsetMin.y += dyC; rect.offsetMax.y += dyC;
            }
            else
            {
                // エッジ移動 = 該当 offset に Δcanvas を加算。解決式
                // （rectMin = parentMin + parentSize*anchorMin + offsetMin）が offset に対して
                // 線形・係数 1 なので、アンカー一致（pos/size 意味論）でもストレッチ
                // （余白 px 意味論）でも同じ式で成立し、Inspector の表示と一貫する。
                // 最小サイズガード: 幅/高さをキャンバス空間 1px 未満・反転にしない。
                constexpr f32 kMinSizePx = 1.0f;
                const f32 startW = (m_uiDragStartMax.x - m_uiDragStartMin.x) / scale;
                const f32 startH = (m_uiDragStartMax.y - m_uiDragStartMin.y) / scale;
                if (m_uiDragEdges & kUiEdgeL) rect.offsetMin.x += (std::min)(dxC, startW - kMinSizePx);
                if (m_uiDragEdges & kUiEdgeR) rect.offsetMax.x += (std::max)(dxC, kMinSizePx - startW);
                if (m_uiDragEdges & kUiEdgeT) rect.offsetMin.y += (std::min)(dyC, startH - kMinSizePx);
                if (m_uiDragEdges & kUiEdgeB) rect.offsetMax.y += (std::max)(dyC, kMinSizePx - startH);
            }
        }
    }

    // ---- 解決済み UI 矩形（描画順 = 奥→手前）。ドラッグ反映後に解決するので今フレームの位置 ----
    std::vector<UiResolvedRect> rects;
    UISystem::ResolveRects(reg, vpX, vpY, vpW, vpH, rects);

    auto findRect = [&rects](entt::entity e) -> const UiResolvedRect* {
        if (e == entt::null) return nullptr;
        for (const auto& rr : rects)
            if (rr.e == e) return &rr;
        return nullptr;
    };

    // 8 ハンドル（四隅 + 四辺中点）。配列順 = 当たり判定の優先順（角が先）。
    struct UiHandle { f32 x, y; int edges; ImGuiMouseCursor cursor; };
    auto makeHandles = [](const UiResolvedRect& rr, UiHandle out[8]) {
        const f32 midX = (rr.min.x + rr.max.x) * 0.5f;
        const f32 midY = (rr.min.y + rr.max.y) * 0.5f;
        out[0] = {rr.min.x, rr.min.y, kUiEdgeL | kUiEdgeT, ImGuiMouseCursor_ResizeNWSE};
        out[1] = {rr.max.x, rr.min.y, kUiEdgeR | kUiEdgeT, ImGuiMouseCursor_ResizeNESW};
        out[2] = {rr.min.x, rr.max.y, kUiEdgeL | kUiEdgeB, ImGuiMouseCursor_ResizeNESW};
        out[3] = {rr.max.x, rr.max.y, kUiEdgeR | kUiEdgeB, ImGuiMouseCursor_ResizeNWSE};
        out[4] = {midX,     rr.min.y, kUiEdgeT,            ImGuiMouseCursor_ResizeNS};
        out[5] = {midX,     rr.max.y, kUiEdgeB,            ImGuiMouseCursor_ResizeNS};
        out[6] = {rr.min.x, midY,     kUiEdgeL,            ImGuiMouseCursor_ResizeEW};
        out[7] = {rr.max.x, midY,     kUiEdgeR,            ImGuiMouseCursor_ResizeEW};
    };

    // 選択中エンティティの解決済み矩形（UIRect 持ちで可視ツリー内なら見つかる）。
    // 回転/スキュー中（hasXform）はリサイズハンドルを出さない（軸平行前提のため）。
    const UiResolvedRect* selRect = findRect(ctx.selectedEntity);
    UiHandle handles[8] = {};
    if (selRect && !selRect->hasXform)
        makeHandles(*selRect, handles);

    // ホバー中のハンドル（非ドラッグ時のみ。カーソル形状と色変化に使う）
    int hoveredHandle = -1;
    if (selRect && !selRect->hasXform && m_uiDragEdges == kUiDragNone && inViewport && !io.KeyAlt)
    {
        for (int i = 0; i < 8; ++i)
        {
            if (std::abs(mousePos.x - handles[i].x) <= kUiHandleHitHalf
                && std::abs(mousePos.y - handles[i].y) <= kUiHandleHitHalf)
            {
                hoveredHandle = i;
                break;
            }
        }
    }

    // ---- カーソル形状（ハンドル = リサイズ向き / 矩形内 = 移動）----
    if (m_uiDragEdges > kUiDragMove)
    {
        for (int i = 0; selRect && i < 8; ++i)
            if (handles[i].edges == m_uiDragEdges) { ImGui::SetMouseCursor(handles[i].cursor); break; }
    }
    else if (m_uiDragEdges == kUiDragMove)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    else if (hoveredHandle >= 0)
    {
        ImGui::SetMouseCursor(handles[hoveredHandle].cursor);
    }
    else if (selRect && inViewport && !io.KeyAlt
             && uiedit::ResolvedRectContains(*selRect, ImVec2(mousePos.x, mousePos.y)))
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    // ---- クリック: ハンドル > UI 要素（手前優先） > 3D ピッキングへフォールスルー ----
    // gizmoBlocking は HandlePicking と同条件。ただし UIRect 持ちを選択中は RenderGizmo が
    // ギズモを抑制しており、Manipulate が呼ばれず ImGuizmo::IsOver() がステイルになるため除外。
    const bool uiRectSelected = ctx.selectedEntity != entt::null
        && reg.valid(ctx.selectedEntity) && reg.all_of<UIRect>(ctx.selectedEntity);
    const bool gizmoBlocking = ctx.HasSelection() && !uiRectSelected
        && (ImGuizmo::IsUsing() || ImGuizmo::IsOver());
    if (m_uiDragEdges == kUiDragNone
        && inViewport && !io.KeyAlt && !gizmoBlocking
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (hoveredHandle >= 0)
        {
            // リサイズ開始（選択中エンティティ。selRect が今フレーム解決済み = UIRect 保有保証）
            m_uiDragEntity      = ctx.selectedEntity;
            m_uiDragStartRect   = reg.get<UIRect>(ctx.selectedEntity);
            m_uiDragStartMouse  = {mousePos.x, mousePos.y};
            m_uiDragStartMin    = {selRect->min.x, selRect->min.y};
            m_uiDragStartMax    = {selRect->max.x, selRect->max.y};
            m_uiDragCanvasScale = selRect->canvasScale;
            m_uiDragEdges       = handles[hoveredHandle].edges;
            m_uiClickConsumed   = true;
        }
        else
        {
            // 後方走査 = 描画順の逆 = 最前面優先で UI 要素をヒットテスト
            // （回転/スキュー要素は逆写像で判定）
            const UiResolvedRect* hit = nullptr;
            for (auto it = rects.rbegin(); it != rects.rend(); ++it)
            {
                if (uiedit::ResolvedRectContains(*it, ImVec2(mousePos.x, mousePos.y)))
                {
                    hit = &(*it);
                    break;
                }
            }
            if (hit)
            {
                // クリック = 最外殻のウィジェット（ボタン本体など）を選択、ダブルクリックで 1 段
                // 深掘り（Figma / UMG 方式。ラベル(子 UIText)だけ掴んで「文字だけ動く」のを防ぐ）
                const bool dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                const entt::entity target =
                    uiedit::ResolveClickTarget(reg, hit->e, ctx.selectedEntity, dbl);
                if (target != entt::null)
                {
                    if (io.KeyCtrl)
                    {
                        ctx.ToggleSelection(target);   // 3D ピッキングと同じマルチ選択
                    }
                    else
                    {
                        ctx.Select(target);
                        // 選択と同時に移動ドラッグ開始（Unity 同様、掴んでそのまま動かせる）
                        if (const UiResolvedRect* tr = findRect(target))
                        {
                            m_uiDragEntity      = target;
                            m_uiDragStartRect   = reg.get<UIRect>(target);
                            m_uiDragStartMouse  = {mousePos.x, mousePos.y};
                            m_uiDragStartMin    = {tr->min.x, tr->min.y};
                            m_uiDragStartMax    = {tr->max.x, tr->max.y};
                            m_uiDragCanvasScale = tr->canvasScale;
                            m_uiDragEdges       = kUiDragMove;
                        }
                    }
                }
                m_uiClickConsumed = true;
                // 選択が変わったので描画用に引き直す（ホバー状態は旧選択のものなので破棄）
                selRect = findRect(ctx.selectedEntity);
                if (selRect && !selRect->hasXform)
                    makeHandles(*selRect, handles);
                hoveredHandle = -1;
            }
            // ヒット無し → 消費しない = HandlePicking の 3D ピッキングへフォールスルー
        }
    }

    // ---- 選択中 UI 要素のアウトライン + 8 ハンドル描画（プレビューと同じ背景 DrawList）----
    if (selRect)
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);
        uiedit::DrawResolvedOutline(dl, *selRect, kUiAccentCol, 2.0f);
        for (int i = 0; !selRect->hasXform && i < 8; ++i)
        {
            const bool hot = (m_uiDragEdges > kUiDragMove)
                ? (handles[i].edges == m_uiDragEdges)
                : (i == hoveredHandle);
            const ImVec2 hmin(handles[i].x - kUiHandleHalf, handles[i].y - kUiHandleHalf);
            const ImVec2 hmax(handles[i].x + kUiHandleHalf, handles[i].y + kUiHandleHalf);
            dl->AddRectFilled(hmin, hmax, hot ? kUiHandleHotCol : kUiHandleCol);
            dl->AddRect(hmin, hmax, kUiAccentCol);
        }
        dl->PopClipRect();
    }
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
    // UI 編集モードで UIRect 持ちを選択中も同じ理屈（RenderGizmo がギズモを抑制して
    // Manipulate が呼ばれない）で IsOver() がステイルになるため、ブロック判定から除外する。
    bool uiRectSelected = ctx.uiEditMode && ctx.selectedEntity != entt::null
        && reg.valid(ctx.selectedEntity) && reg.all_of<UIRect>(ctx.selectedEntity);
    bool gizmoBlocking = ctx.HasSelection() && !uiRectSelected
        && (ImGuizmo::IsUsing() || ImGuizmo::IsOver());
    if (ImGui::GetIO().KeyAlt                 // Alt+左ドラッグはオービット操作なのでピッキングしない
        || gizmoBlocking
        || m_uiClickConsumed                  // UI 編集（HandleUiEditing）がこのクリックを消費済み
        || ctx.floatingToolWindowHovered      // フローティングツール窓の上のクリックは背後のシーンを選択しない
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

        XMFLOAT3 worldMin, worldMax;

        if (reg.all_of<MeshRenderer>(e))
        {
            // MeshRenderer あり: 統合ローカルAABBの8頂点をワールド行列(回転込み)で変換して
            // ワールドAABBを再構築する。旧実装は平行移動+スケールのみで回転を無視しており、
            // 回転させたメッシュで見た目とヒット判定がズレてクリック選択が外れていた
            // (PickEntityAndSubmesh と同じ修正)。
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
            const XMMATRIX wm = ComputeWorldMatrix(reg, e);
            worldMin = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            worldMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (int ci = 0; ci < 8; ++ci)
            {
                XMVECTOR corner = XMVectorSet(
                    (ci & 1) ? aabbMax.x : aabbMin.x,
                    (ci & 2) ? aabbMax.y : aabbMin.y,
                    (ci & 4) ? aabbMax.z : aabbMin.z, 1.0f);
                XMFLOAT3 c;
                XMStoreFloat3(&c, XMVector3TransformCoord(corner, wm));
                worldMin.x = (std::min)(worldMin.x, c.x);
                worldMin.y = (std::min)(worldMin.y, c.y);
                worldMin.z = (std::min)(worldMin.z, c.z);
                worldMax.x = (std::max)(worldMax.x, c.x);
                worldMax.y = (std::max)(worldMax.y, c.y);
                worldMax.z = (std::max)(worldMax.z, c.z);
            }
        }
        else
        {
            // MeshRenderer なし (Camera/Light/Empty): ワールド位置中心の固定サイズ AABB
            XMFLOAT3 wpos = transform.position;
            if (transform.parent != entt::null && reg.valid(transform.parent))
            {
                XMMATRIX wm = ComputeWorldMatrix(reg, e);
                XMFLOAT4X4 wf;
                XMStoreFloat4x4(&wf, wm);
                wpos = {wf._41, wf._42, wf._43};
            }
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

SubmeshPickResult SceneViewPanel::PickEntityAndSubmesh(entt::registry& reg,
                                                       Camera* camera,
                                                       f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    SubmeshPickResult result;

    ImVec2 mousePos = ImGui::GetIO().MousePos;
    if (mousePos.x < vpX || mousePos.x >= vpX + vpW
        || mousePos.y < vpY || mousePos.y >= vpY + vpH)
        return result;

    // NDC → ワールドレイ。HandlePicking と同じ組み立て方（透視/正射どちらでも正しい）。
    f32 ndcX = ((mousePos.x - vpX) / vpW) * 2.0f - 1.0f;
    f32 ndcY = 1.0f - ((mousePos.y - vpY) / vpH) * 2.0f;

    XMMATRIX view = camera->GetViewMatrix();
    XMMATRIX proj = camera->GetProjectionMatrix();
    XMMATRIX invViewProj = XMMatrixInverse(nullptr, view * proj);

    XMVECTOR pNear = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invViewProj);
    XMVECTOR pFar  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invViewProj);
    pNear = XMVectorScale(pNear, 1.0f / XMVectorGetW(pNear));
    pFar  = XMVectorScale(pFar,  1.0f / XMVectorGetW(pFar));
    XMVECTOR rayOrigin = pNear;
    XMVECTOR rayDir    = XMVector3Normalize(XMVectorSubtract(pFar, pNear));

    XMFLOAT3 orig, dir;
    XMStoreFloat3(&orig, rayOrigin);
    XMStoreFloat3(&dir, rayDir);

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

    f32 closestDist = FLT_MAX;

    // MeshRenderer 持ちエンティティのみ対象。HandlePicking と違い、サブメッシュ単位で
    // 個別に AABB を作りレイテストする(マージした全体AABBだと「どのサブメッシュか」が分からない)。
    auto meshView = reg.view<const Transform, const MeshRenderer>();
    for (auto [e, transform, renderer] : meshView.each())
    {
        // 床グリッドはマテリアル適用対象外(HandlePicking と同じ扱い)。これが無いと
        // 巨大な床平面が最近ヒットを奪い、D&Dしたマテリアルがグリッドに吸われて
        // 「適用したのに何も変わらない」状態になる(グリッドは専用PSOでmaterialAssetを無視する)。
        if (reg.all_of<GridPlane>(e)) continue;

        // ローカルAABBの8頂点をワールド行列(回転込み)で変換してワールドAABBを再構築する。
        // 旧実装は平行移動+スケールのみで回転を無視しており、回転させたメッシュで見た目と
        // ヒット判定がズレてドロップが外れていた。
        const XMMATRIX wm = ComputeWorldMatrix(reg, e);

        for (u32 mi = 0; mi < static_cast<u32>(renderer.meshes.size()); ++mi)
        {
            const auto* mesh = renderer.meshes[mi];
            if (!mesh) continue;
            auto meshMin = mesh->GetAABBMin();
            auto meshMax = mesh->GetAABBMax();

            XMFLOAT3 worldMin = { FLT_MAX,  FLT_MAX,  FLT_MAX};
            XMFLOAT3 worldMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (int ci = 0; ci < 8; ++ci)
            {
                XMVECTOR corner = XMVectorSet(
                    (ci & 1) ? meshMax.x : meshMin.x,
                    (ci & 2) ? meshMax.y : meshMin.y,
                    (ci & 4) ? meshMax.z : meshMin.z, 1.0f);
                XMFLOAT3 c;
                XMStoreFloat3(&c, XMVector3TransformCoord(corner, wm));
                worldMin.x = (std::min)(worldMin.x, c.x);
                worldMin.y = (std::min)(worldMin.y, c.y);
                worldMin.z = (std::min)(worldMin.z, c.z);
                worldMax.x = (std::max)(worldMax.x, c.x);
                worldMax.y = (std::max)(worldMax.y, c.y);
                worldMax.z = (std::max)(worldMax.z, c.z);
            }

            f32 t = rayTestAABB(worldMin, worldMax);
            if (t > 0.0f && t < closestDist)
            {
                closestDist = t;
                result.entity = e;
                result.submeshIndex = mi;
            }
        }
    }

    return result;
}

void SceneViewPanel::HandleTextureContextMenu(entt::registry& reg,
                                              EditorContext& ctx,
                                              Camera* camera,
                                              f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    ImGuiIO& io = ImGui::GetIO();

    // 押下位置からの移動量がこれ未満なら「ドラッグでない普通のクリック」とみなす。
    // フライカメラ(右クリック長押し+視点回転)は Application::Update が GetAsyncKeyState で
    // 独自に処理しており、ここでの ImGui 側クリック/ドラッグ判定とは完全に独立(同じ物理入力を
    // 両方が見ているだけなので、実際に視点を動かした操作はこの距離チェックで自然に弾かれる)。
    constexpr float kClickDragThresholdSq = 6.0f * 6.0f;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        ImVec2 mp = io.MousePos;
        bool inViewport = !ctx.floatingToolWindowHovered
            && mp.x >= vpX && mp.x < vpX + vpW && mp.y >= vpY && mp.y < vpY + vpH;
        m_textureCtxTarget = inViewport ? PickEntityAndSubmesh(reg, camera, vpX, vpY, vpW, vpH)
                                        : SubmeshPickResult{};
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && m_textureCtxTarget.entity != entt::null)
    {
        ImVec2 downPos = io.MouseClickedPos[ImGuiMouseButton_Right];
        ImVec2 upPos = io.MousePos;
        float dx = upPos.x - downPos.x, dy = upPos.y - downPos.y;
        if (dx * dx + dy * dy <= kClickDragThresholdSq)
            ImGui::OpenPopup("SceneViewTextureCtxMenu");
    }

    if (ImGui::BeginPopup("SceneViewTextureCtxMenu"))
    {
        entt::entity e = m_textureCtxTarget.entity;
        u32 smi = m_textureCtxTarget.submeshIndex;
        bool valid = e != entt::null && reg.valid(e) && reg.all_of<MeshRenderer>(e);
        bool hasOverride = valid && reg.get<MeshRenderer>(e).HasAnyTextureOverride(smi);

        if (!valid)
        {
            ImGui::TextDisabled("(no mesh)");
        }
        else
        {
            ImGui::BeginDisabled(!hasOverride);
            if (ImGui::MenuItem("\xe3\x83\x86\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x81\xe3\x83\xa3\xe3\x82\x92\xe5\xa4\x96\xe3\x81\x99"))
            {
                auto& mr = reg.get<MeshRenderer>(e);
                MeshRenderer before = mr;
                MeshRenderer::SetOverride(mr.overrideAlbedoTexture, smi, "");
                MeshRenderer::SetOverride(mr.overrideNormalTexture, smi, "");
                MeshRenderer::SetOverride(mr.overrideMetalRoughnessTexture, smi, "");
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                    &reg, e, before, mr, "Material Texture"));
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }
}

void SceneViewPanel::HandleCameraNavigation(entt::registry& reg,
                                            EditorContext& ctx,
                                            Camera* camera,
                                            f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 m = io.MousePos;
    bool inViewport = !ctx.floatingToolWindowHovered
                   && m.x >= vpX && m.x < vpX + vpW
                   && m.y >= vpY && m.y < vpY + vpH;

    // ギズモ操作中、またはカーソルがビュー外なら以降のカメラ操作はしない
    if (ImGuizmo::IsUsing() || !inViewport)
        return;

    // --- 2D ビューモード: パン（中ドラッグ）＋ズーム（ホイール）のみ。回転/ドリーなし ---
    // カメラの正射化・向き固定は Application 側で毎フレーム適用。ここでは x/y パンとズーム量だけ動かす。
    if (ctx.view2D)
    {
        // 回転は 0 固定なので MoveRight/MoveUp はそのままワールド X/Y 平行移動になる。
        // タッチパッド二本指（中ボタンの無い端末向け）: 横=左右パン, SHIFT+縦=上下パン, 縦=ズーム。
        // 上のヒント表示と一致させる。パン量はズーム（=見えてる世界サイズ）に比例。
        f32 panStep = (std::max)(0.5f, ctx.view2DZoom) * 0.2f;

        if (io.MouseWheel != 0.0f)
        {
            if (io.KeyShift)
                camera->MoveUp(io.MouseWheel * panStep);          // SHIFT+縦: 上下パン
            else
            {
                f32 factor = (io.MouseWheel > 0.0f) ? 0.9f : (1.0f / 0.9f);
                ctx.view2DZoom = std::clamp(ctx.view2DZoom * factor, 0.2f, 1000.0f);  // 縦: ズーム
            }
        }
        if (io.MouseWheelH != 0.0f)
            camera->MoveRight(-io.MouseWheelH * panStep);         // 横: 左右パン（符号は 3D 側と統一）

        // 中ドラッグ: パン（マウス用）。画面ピクセル → 世界量は「ビュー縦 = 2*zoom」をビュー高で割って換算。
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
    // ImGui::IsKeyPressed は WndProc 経由＝ウィンドウがフォーカスされている時だけ true になるので、
    // 別アプリ作業中の Del がエンティティを消すのを防げる（GetAsyncKeyState はフォーカス無関係）。
    bool deletePressed = ImGui::IsKeyPressed(ImGuiKey_Delete, false);

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
