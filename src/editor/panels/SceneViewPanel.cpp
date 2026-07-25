#include "editor/panels/SceneViewPanel.h"
#include "editor/EditorContext.h"
#include "editor/ScenePick.h"
#include "editor/UiEditUtil.h"
#include "editor/UndoSystem.h"
#include "core/CpuScope.h"
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
#include <cmath>
#include <cstdio>
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

    // 行列（ローカル or 親を外した後の行列）を Transform の TRS へ書き戻す。
    // DecomposeWorldToRPY と対で「GetWorldMatrix と無損失にラウンドトリップする」のが要点。
    void ApplyMatrixToTransform(Transform& t, const XMMATRIX& m)
    {
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, m);
        float translation[3], rotation[3], scale[3];
        DecomposeWorldToRPY(f, translation, rotation, scale);
        t.position = {translation[0], translation[1], translation[2]};
        t.rotation = {rotation[0], rotation[1], rotation[2]};
        // Scale が 0 以下になると行列が壊れてギズモが消えるので最小値でクランプ
        constexpr float kMinScale = 0.001f;
        t.scale = {(std::max)(scale[0], kMinScale),
                   (std::max)(scale[1], kMinScale),
                   (std::max)(scale[2], kMinScale)};
    }

    // ギズモのドラッグ増分をマウス脇に小さく表示する。
    // 注: 引数なし GetBackgroundDrawList() は「カレントウィンドウのビューポート」の
    // 背景リストを返すため、マルチビューポートで別 OS ウィンドウ側に描かれて見えなくなる。
    // 必ずメインビューポートを明示すること（RenderUiPreview と同じ理由）。
    void DrawGizmoDeltaOverlay(const char* text)
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const ImVec2 pos(mp.x + 18.0f, mp.y + 18.0f);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 bmin(pos.x - 6.0f, pos.y - 4.0f);
        const ImVec2 bmax(pos.x + ts.x + 6.0f, pos.y + ts.y + 4.0f);
        dl->AddRectFilled(bmin, bmax, IM_COL32(18, 20, 26, 225), 4.0f);
        dl->AddRect(bmin, bmax, IM_COL32(255, 204, 26, 200), 4.0f);
        dl->AddText(pos, IM_COL32(255, 255, 255, 255), text);
    }

    // 開始時スケールに対する倍率（0 割り回避）
    float ScaleRatio(float now, float start)
    {
        return (std::abs(start) > 1e-6f) ? (now / start) : 1.0f;
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
    // 注: 引数なし GetBackgroundDrawList() は「カレントウィンドウのビューポート」の背景リストを
    // 返す(imgui 1.92+)。マルチビューポートでパネルが分離していると別OSウィンドウ側に描かれて
    // 見えなくなるため、必ずメインビューポートを明示する。
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
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

    // 計測（perf_stats の cpuScopeMs "gizmo"）。editorUi にも同じ時間が入る二重計上だが、
    // 「エディタUIが重い」の内訳としてギズモを名指しするのが目的。
    CpuScopeTimer _tGizmo(ctx.cpuScopeMs ? &ctx.cpuScopeMs[CpuGizmo] : nullptr);

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
    // メインビューポート明示必須(引数なしはカレントウィンドウ依存で、ImGuizmo の "gizmo" 窓が
    // 独立ビューポート化していると見えない別OSウィンドウにギズモが描かれる)
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList(ImGui::GetMainViewport()));
    ImGuizmo::SetRect(vpX, vpY, vpW, vpH);

    // ImGuizmo は既定で「カメラに近い側」に軸を反転表示する(AllowAxisFlip)。
    // カメラを動かすたびに軸の向きが入れ替わって見えて操作方向が分からなくなるため無効化し、
    // 軸の向きをカメラ位置に関係なく常に固定する。
    ImGuizmo::AllowAxisFlip(false);

    // 軸が奥を向くと ImGuizmo は軸を反転(flip)させ、その印として黒い破線(ハッチング)を
    // 矢印上に重ねて描く。これが X/Z 矢印に黒い線が走るように見える原因なので、太さ 0 にして
    // 描画を抑止する（反転自体は掴みやすさのため残す）。GetStyle はグローバル状態なので毎フレーム設定で良い。
    ImGuizmo::GetStyle().HatchedAxisLineThickness = 0.0f;

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == GizmoMode::Rotate) op = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == GizmoMode::Scale)  op = ImGuizmo::SCALE;

    // 回転ギズモが「細くて暗くて小さい」と、どの輪を掴んでるか分からず変な軸で回してしまう。
    // 線を太く・色を鮮やかに・サイズを大きくして、どの軸の輪か一目で分かるようにする。
    //
    // さらにホバー中は線をもう一段太く・SELECTION をより明るくして「今どれを掴めるか」を
    // 押す前に見せる。ImGuizmo には GetHoveredHandleType() が無い版があるため（vcpkg の
    // バージョンは今回上げない方針）、IsOver(OPERATION) で代用する。IsOver は直前の
    // Manipulate 時の当たり判定を見るので 1 フレーム遅れるが、強調表示なので実用上問題ない。
    const bool gizmoHovered = ImGuizmo::IsUsing() || ImGuizmo::IsOver(op);
    {
        ImGuizmo::Style& gz = ImGuizmo::GetStyle();
        const float boost = gizmoHovered ? 1.5f : 1.0f;
        gz.RotationLineThickness      = 4.0f * boost;   // 軸リング（既定2.0）
        gz.RotationOuterLineThickness = 3.0f * boost;   // 外周のスクリーン回転円
        gz.TranslationLineThickness   = 4.0f * boost;
        gz.ScaleLineThickness         = 4.0f * boost;
        gz.CenterCircleSize           = gizmoHovered ? 8.0f : 6.0f;
        gz.Colors[ImGuizmo::DIRECTION_X] = ImVec4(0.95f, 0.22f, 0.22f, 1.0f); // 鮮やかな赤=X
        gz.Colors[ImGuizmo::DIRECTION_Y] = ImVec4(0.30f, 0.90f, 0.30f, 1.0f); // 鮮やかな緑=Y
        gz.Colors[ImGuizmo::DIRECTION_Z] = ImVec4(0.25f, 0.55f, 1.00f, 1.0f); // 鮮やかな青=Z
        gz.Colors[ImGuizmo::SELECTION]   = gizmoHovered
            ? ImVec4(1.00f, 0.92f, 0.35f, 1.00f)    // ホバー中はさらに明るい黄
            : ImVec4(1.00f, 0.80f, 0.10f, 0.90f);
    }
    ImGuizmo::SetGizmoSizeClipSpace(0.15f);   // 既定0.1より大きく＝掴みやすく・見やすく

    ImGuizmo::MODE mode = ctx.gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    // 回転ギズモは常にワールド軸（まっすぐ）で表示・操作する。
    // ローカルにするとオブジェクトの現在の回転にリングが追従して傾き、
    // 「オブジェクトごとに向きが変わる」状態になるため、回転はワールド固定にする。
    if (ctx.gizmoMode == GizmoMode::Rotate)
        mode = ImGuizmo::WORLD;

    // スナップ量はエディタ設定（EditorContext）から。既定値は従来のハードコードと同じ。
    float snapValues[3] = {ctx.snapTranslate, ctx.snapTranslate, ctx.snapTranslate};
    if (ctx.gizmoMode == GizmoMode::Rotate)
        snapValues[0] = snapValues[1] = snapValues[2] = ctx.snapRotateDeg;
    else if (ctx.gizmoMode == GizmoMode::Scale)
        snapValues[0] = snapValues[1] = snapValues[2] = ctx.snapScale;
    // ImGui 経由＝ウィンドウがフォーカスされている時だけ。常時スナップも設定で選べる。
    const bool useSnap = ctx.snapAlways || ImGui::GetIO().KeyCtrl;

    // ---- Undo の before スナップショット ----
    // ImGuizmo が掴みを開始できるのは CanActivate() == ImGui::IsMouseClicked(0) のフレームだけ。
    // そこで「押した瞬間のフレーム」に限り Manipulate の前に全選択の Transform を控える。
    // （旧実装は IsUsing() を Manipulate の前に読んでおり、掴んだ初フレームぶんの変更が
    //   before 側に混ざる＝Undo が 1 フレーム足りない状態になっていた）
    if (!m_gizmoWasUsing && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_gizmoStartGroup.clear();
        for (auto e : ctx.selectedEntities)
        {
            if (reg.valid(e) && reg.all_of<Transform>(e))
                m_gizmoStartGroup.push_back({e, reg.get<Transform>(e)});
        }
    }

    // ---- マルチ選択の仮想ピボット ----
    // 選択群の中心（各エンティティのワールド位置の平均）に平行移動だけの行列を置き、
    // それを Manipulate に渡す。返ってきた行列との差分を各エンティティのワールドへ
    // 右から掛けることで、Translate だけでなく Rotate/Scale も群全体へ効く。
    // ※ ImGuizmo の deltaMatrix は操作ごとに意味が違う（移動=ワールド平行移動 /
    //    回転=ローカル基準 / スケール=ドラッグ開始からの絶対値）ので使わない。
    //    自前の「前フレームとの差分」なら 3 操作とも同じ 1 本の式で済む。
    const bool multi = ctx.selectedEntities.size() > 1;
    XMFLOAT4X4 gizmoM = worldF;
    if (multi)
    {
        if (!m_gizmoWasUsing)
        {
            // 非ドラッグ中は毎フレーム置き直す（選択の増減にも追従）。
            // ドラッグ中は前フレームの値を持ち越して差分の基準にする。
            XMVECTOR sum = XMVectorZero();
            int count = 0;
            for (auto e : ctx.selectedEntities)
            {
                if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
                const auto& t = reg.get<Transform>(e);
                const XMMATRIX w = (t.parent != entt::null && reg.valid(t.parent))
                    ? ComputeWorldMatrix(reg, e) : t.GetWorldMatrix();
                sum = XMVectorAdd(sum, w.r[3]);
                ++count;
            }
            const XMMATRIX pivot = (count > 0)
                ? XMMatrixTranslationFromVector(
                      XMVectorScale(sum, 1.0f / static_cast<float>(count)))
                : XMMatrixIdentity();
            XMStoreFloat4x4(&m_gizmoPivot, pivot);
        }
        gizmoM = m_gizmoPivot;
        // ピボットは平行移動のみ＝軸は常にワールド向き。マルチ選択で LOCAL にすると
        // 代表 1 個の姿勢に群全体が引っ張られて直感に反するため WORLD 固定にする。
        mode = ImGuizmo::WORLD;
    }
    const XMFLOAT4X4 gizmoBefore = gizmoM;

    const bool manipulated = ImGuizmo::Manipulate(
            &viewF._11, &projF._11,
            op, mode,
            &gizmoM._11, nullptr,
            useSnap ? snapValues : nullptr);

    // IsUsing() は Manipulate の「後」に読む＝掴んだフレームの状態が今フレームで確定する。
    const bool isUsing = ImGuizmo::IsUsing();

    if (manipulated)
    {
        if (multi)
        {
            // W = inv(操作前ピボット) * 操作後ピボット ＝ ワールド空間で群に掛ける変換。
            // 各選択は worldNew = worldOld * W（行ベクトル規約）。回転/スケールは
            // ピボット中心での回転・拡大になり、平行移動は従来と同じ挙動に一致する。
            XMVECTOR det = XMVectorZero();
            const XMMATRIX invBefore = XMMatrixInverse(&det, XMLoadFloat4x4(&gizmoBefore));
            if (std::abs(XMVectorGetX(det)) > 1e-12f)
            {
                const XMMATRIX W = invBefore * XMLoadFloat4x4(&gizmoM);
                for (auto e : ctx.selectedEntities)
                {
                    if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
                    auto& t = reg.get<Transform>(e);
                    const bool childHasParent =
                        (t.parent != entt::null && reg.valid(t.parent));
                    const XMMATRIX oldWorld = childHasParent
                        ? ComputeWorldMatrix(reg, e) : t.GetWorldMatrix();
                    XMMATRIX newLocal = oldWorld * W;
                    if (childHasParent)
                        newLocal = newLocal
                                 * XMMatrixInverse(nullptr, ComputeWorldMatrix(reg, t.parent));
                    ApplyMatrixToTransform(t, newLocal);
                }
            }
            m_gizmoPivot = gizmoM;   // 次フレームの差分基準
        }
        else
        {
            // 親がいる場合: 操作後のワールド行列を親の逆行列でローカルに戻す
            XMMATRIX local = XMLoadFloat4x4(&gizmoM);
            if (hasParent)
                local = local * XMMatrixInverse(nullptr, ComputeWorldMatrix(reg, transform.parent));
            ApplyMatrixToTransform(transform, local);
            // ライトの向きは Transform 回転に追従（Application 側で direction に反映）するので
            // ここでは特別扱い不要。回転ギズモを回せば光の向きも変わる。
        }
    }

    // ---- ドラッグ中の増分を数値でオーバーレイ表示（代表エンティティ基準）----
    if (isUsing && !m_gizmoStartGroup.empty() && reg.all_of<Transform>(ctx.selectedEntity))
    {
        const Transform* start = nullptr;
        for (const auto& entry : m_gizmoStartGroup)
            if (entry.first == ctx.selectedEntity) { start = &entry.second; break; }

        if (start)
        {
            const Transform& now = reg.get<Transform>(ctx.selectedEntity);
            char buf[128];
            switch (ctx.gizmoMode)
            {
            case GizmoMode::Rotate:
                std::snprintf(buf, sizeof(buf), "R  X %+.1f  Y %+.1f  Z %+.1f",
                              now.rotation.x - start->rotation.x,
                              now.rotation.y - start->rotation.y,
                              now.rotation.z - start->rotation.z);
                break;
            case GizmoMode::Scale:
                std::snprintf(buf, sizeof(buf), "S  X %.3f  Y %.3f  Z %.3f",
                              ScaleRatio(now.scale.x, start->scale.x),
                              ScaleRatio(now.scale.y, start->scale.y),
                              ScaleRatio(now.scale.z, start->scale.z));
                break;
            case GizmoMode::Translate:
            default:
                std::snprintf(buf, sizeof(buf), "T  X %+.3f  Y %+.3f  Z %+.3f",
                              now.position.x - start->position.x,
                              now.position.y - start->position.y,
                              now.position.z - start->position.z);
                break;
            }
            DrawGizmoDeltaOverlay(buf);
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
    // ビューポートツール（地形ブラシ等）が消費したクリックは UI 編集にも 3D ピックにも渡さない
    m_uiClickConsumed = m_toolConsumed;

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
    if (m_uiDragEdges == kUiDragNone && !m_uiClickConsumed
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
        ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
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
        || m_toolConsumed                     // ビューポートツール（地形ブラシ等）が消費済み
        || m_uiClickConsumed                  // UI 編集（HandleUiEditing）がこのクリックを消費済み
        || ctx.floatingToolWindowHovered      // フローティングツール窓の上のクリックは背後のシーンを選択しない
        || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return;

    if (!camera) return;

    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    if (mousePos.x < vpX || mousePos.x >= vpX + vpW
        || mousePos.y < vpY || mousePos.y >= vpY + vpH)
        return;

    // 計測（perf_stats の cpuScopeMs "picking"）。editorUi の内数（二重計上）。
    CpuScopeTimer _tPick(ctx.cpuScopeMs ? &ctx.cpuScopeMs[CpuPicking] : nullptr);

    // 三角形単位の精密ピッキング（ScenePick）。ブロードフェーズに Application の描画リストを
    // 使うので、旧実装のように 10 万体ぶんの ComputeWorldMatrix を回し直すことはもう無い。
    const std::vector<ScenePickHit> hits = RaycastScene(
        reg, ctx.drawItems, *camera, vpX, vpY, vpW, vpH, mousePos.x, mousePos.y);

    // ヒットをエンティティ単位に畳む（同一エンティティの複数サブメッシュは 1 件扱い）。
    // 順序は距離（アイコン優先）そのままなので「手前 → 奥」の並びになる。
    std::vector<entt::entity> chain;
    chain.reserve(hits.size());
    for (const ScenePickHit& h : hits)
        if (std::find(chain.begin(), chain.end(), h.entity) == chain.end())
            chain.push_back(h.entity);

    const bool ctrl = ImGui::GetIO().KeyCtrl;

    // ---- 重なりの循環選択（Unity と同じ）----
    // 同じ画面座標（数 px 以内）を短時間に連続クリックすると、手前 → 奥へ選択が進む。
    // 重なり列が前回と一致していることも条件にする（カメラを動かして中身が変わったら先頭へ戻す）。
    // Ctrl+クリック（トグル選択）中は循環させず常に最前面を対象にする
    //   ＝「奥のを意図せず足す/外す」が起きない＝マルチ選択の操作感を壊さない。
    constexpr f32    kCycleRadiusSq = 4.0f * 4.0f;   // 4px 以内は同じ場所とみなす
    constexpr double kCycleTimeout  = 1.2;           // 秒。空くと先頭へリセット
    const double nowSec = ImGui::GetTime();
    const f32 mdx = mousePos.x - m_cyclePos.x;
    const f32 mdy = mousePos.y - m_cyclePos.y;

    int index = 0;
    if (!ctrl && chain.size() > 1
        && (mdx * mdx + mdy * mdy) <= kCycleRadiusSq
        && (nowSec - m_cycleTime) <= kCycleTimeout
        && chain == m_cycleChain)
    {
        index = (m_cycleIndex + 1) % static_cast<int>(chain.size());
    }

    m_cyclePos   = {mousePos.x, mousePos.y};
    m_cycleTime  = nowSec;
    m_cycleChain = chain;
    m_cycleIndex = index;

    const entt::entity picked =
        chain.empty() ? entt::null : chain[static_cast<size_t>(index)];

    if (ctrl)
    {
        if (picked != entt::null)
            ctx.ToggleSelection(picked);
    }
    else
    {
        ctx.Select(picked);
    }
}

bool SceneViewPanel::RunViewportTools(EditorContext& ctx,
                                      Camera* camera,
                                      f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    m_toolConsumed = false;
    if (ctx.viewportToolHandlers.empty() || !camera || vpW <= 0.0f || vpH <= 0.0f)
        return false;

    const ImGuiIO& io = ImGui::GetIO();

    ViewportInput in;
    in.mouseX   = io.MousePos.x;
    in.mouseY   = io.MousePos.y;
    in.vpX      = vpX;
    in.vpY      = vpY;
    in.vpW      = vpW;
    in.vpH      = vpH;
    in.camera   = camera;
    in.inViewport = !ctx.floatingToolWindowHovered
        && in.mouseX >= vpX && in.mouseX < vpX + vpW
        && in.mouseY >= vpY && in.mouseY < vpY + vpH;
    in.pressed  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    in.dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f);
    in.released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    ScreenRay(*camera, vpX, vpY, vpW, vpH, in.mouseX, in.mouseY, in.rayOrigin, in.rayDir);

    for (auto& handler : ctx.viewportToolHandlers)
    {
        if (handler && handler(in)) { m_toolConsumed = true; break; }
    }
    return m_toolConsumed;
}

SubmeshPickResult SceneViewPanel::PickEntityAndSubmesh(entt::registry& reg,
                                                       EditorContext& ctx,
                                                       Camera* camera,
                                                       f32 vpX, f32 vpY, f32 vpW, f32 vpH)
{
    SubmeshPickResult result;
    if (!camera) return result;

    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    if (mousePos.x < vpX || mousePos.x >= vpX + vpW
        || mousePos.y < vpY || mousePos.y >= vpY + vpH)
        return result;

    CpuScopeTimer _tPick(ctx.cpuScopeMs ? &ctx.cpuScopeMs[CpuPicking] : nullptr);

    // マテリアル適用先の特定なのでメッシュ限定（アイコン/ワールドスプライトは拾わない）。
    // 床グリッドの除外は RaycastScene 側で済んでいる（描画リストが GridPlane を含まない／
    // フォールバック経路も exclude<GridPlane>）。これが無いと巨大な床平面が最近ヒットを奪い、
    // D&D したマテリアルがグリッドに吸われて「適用したのに何も変わらない」状態になる。
    ScenePickOptions opt;
    opt.includeNonMesh = false;

    const std::vector<ScenePickHit> hits = RaycastScene(
        reg, ctx.drawItems, *camera, vpX, vpY, vpW, vpH, mousePos.x, mousePos.y, opt);
    if (!hits.empty())
    {
        result.entity       = hits.front().entity;
        result.submeshIndex = hits.front().submeshIndex;
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
        m_textureCtxTarget = inViewport
            ? PickEntityAndSubmesh(reg, ctx, camera, vpX, vpY, vpW, vpH)
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

    // --- ホイール/パンの基準距離を毎フレーム更新 ---
    // m_orbitDistance は元々 Alt オービット中(下の 1 箇所)でしか更新されず、
    // オービットを一度もしなければ初期値 10.0 のままだった。その結果ホイールの
    // step が max(0.5, 10*0.15)=1.5 に固定され、カメラを遠くへ引くほど 1 ノッチの
    // 移動量が相対的に小さくなり「引いても距離が変わらない」ように見えていた。
    // 注視点(選択物、無ければ原点)までの実距離を毎フレーム入れて、距離に比例した
    // 指数的なズーム感にする(Unity/Blender と同じ挙動)。パン量も同じ距離を使う。
    if (!(io.KeyAlt && io.MouseDown[ImGuiMouseButton_Left]))   // オービット中は確定済みの距離を維持
    {
        XMFLOAT3 pivot{0, 0, 0};
        if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
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
        XMFLOAT3 camPosNow = camera->GetPosition();
        XMVECTOR d = XMLoadFloat3(&camPosNow) - XMLoadFloat3(&pivot);
        m_orbitDistance = std::clamp(XMVectorGetX(XMVector3Length(d)), 0.5f, 1000.0f);
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
