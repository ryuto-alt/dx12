#include "editor/LightHandles.h"

#include "editor/EditorContext.h"
#include "editor/LightMath.h"
#include "editor/ScenePick.h"     // ScreenRay
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace dx12e
{

using namespace DirectX;
namespace lm = lightmath;

namespace
{

// ---- 見た目 / 操作感の定数 ----
constexpr f32 kHandleRadius    = 5.0f;    // ハンドルの描画半径(px)
constexpr f32 kHandleHitRadius = 11.0f;   // 当たり判定半径(px。掴みやすいよう描画より広め)
constexpr f32 kDirArmLength    = 2.0f;    // 平行光の回転ハンドルまでの距離(m)。
                                          // EditorIconRenderer の矢印長と同じ＝3D 矢印の先に重なる
constexpr f32 kSunDegPerPixel  = 0.30f;   // 太陽ドラッグの感度(度/px)
constexpr f32 kMinRange        = 0.1f;
constexpr f32 kMaxRange        = 500.0f;
// ワイヤ表示に使う range の上限。range のクランプ上限と同じにしておくこと。
// ここを小さくすると「ハンドルは止まっているのに range だけ伸びる」状態になる。
constexpr f32 kMaxWireRange    = kMaxRange;
constexpr int kConeSegments    = 24;

// テーマの種別カラー(theme::TypeLight = 0xf5b740)を軸にした配色
constexpr ImU32 kColSpot      = IM_COL32(128, 216, 255, 200);
constexpr ImU32 kColPoint     = IM_COL32(245, 183,  64, 200);
constexpr ImU32 kColDir       = IM_COL32(255, 226,  92, 210);
constexpr ImU32 kColHandle    = IM_COL32(250, 250, 252, 235);
constexpr ImU32 kColHandleHot = IM_COL32(255, 204,  26, 255);   // ImGuizmo の SELECTION 相当
constexpr ImU32 kColOutline   = IM_COL32(  0,   0,   0, 170);

// 「全ライトを薄く」表示のときの色(選択中の実線と区別できるよう alpha を落とす)
constexpr ImU32 kColSpotFaint  = IM_COL32(128, 216, 255, 60);
constexpr ImU32 kColPointFaint = IM_COL32(245, 183,  64, 60);
constexpr ImU32 kColDirFaint   = IM_COL32(255, 226,  92, 70);

enum class DragKind
{
    None,
    SpotOuter,   // 外コーン角
    SpotInner,   // 内コーン角
    SpotRange,   // スポットの届く距離
    PointRange,  // 点光源の届く距離
    DirRotate,   // 平行光の向き
};

struct LightHandleState
{
    // viewportToolHandlers へ登録済みか(登録先の EditorContext を覚えておく)
    EditorContext* registeredCtx = nullptr;
    // 今フレームのマウス操作をライト編集が食ったか。登録したハンドラがこれを返す。
    bool consumed = false;

    // ---- ハンドルのドラッグ ----
    DragKind         drag       = DragKind::None;
    entt::entity     dragEntity = entt::null;
    SpotLight        slBefore{};
    PointLight       plBefore{};
    DirectionalLight dlBefore{};
    XMFLOAT3         dragAxis{1.0f, 0.0f, 0.0f};   // PointRange: 掴んだ瞬間のカメラ右方向で固定

    // ---- 太陽ドラッグ(L キー) ----
    bool             sunDragging = false;
    entt::entity     sunEntity   = entt::null;
    DirectionalLight sunBefore{};
    ImVec2           sunLastMouse{0.0f, 0.0f};
    lm::SunAngles    sunAngles{};
    f32              sunHudTimer = 0.0f;   // 離した後もしばらく方位/高度を出す(秒)
};

// エンティティのワールド位置(親階層込み)
XMFLOAT3 WorldPositionOf(const entt::registry& reg, entt::entity e, const Transform& tf)
{
    if (tf.parent == entt::null || !reg.valid(tf.parent))
        return tf.position;
    XMFLOAT3 out{};
    XMStoreFloat3(&out, ComputeWorldMatrix(reg, e).r[3]);
    return out;
}

XMFLOAT3 NormalizedOr(const XMFLOAT3& v, const XMFLOAT3& fallback)
{
    const f32 len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!(len2 > 1e-8f)) return fallback;
    const f32 inv = 1.0f / std::sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

XMFLOAT3 Offset(const XMFLOAT3& p, const XMFLOAT3& dir, f32 len)
{
    return { p.x + dir.x * len, p.y + dir.y * len, p.z + dir.z * len };
}

f32 Distance3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// カメラの右方向(左手系: right = up × forward)
XMFLOAT3 CameraRight(const Camera& cam)
{
    const XMFLOAT3 fwd = cam.GetForward();
    const XMVECTOR f = XMVector3Normalize(XMLoadFloat3(&fwd));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (std::fabs(XMVectorGetY(f)) > 0.99f) up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    XMFLOAT3 out{};
    XMStoreFloat3(&out, XMVector3Normalize(XMVector3Cross(up, f)));
    return out;
}

// ドラッグ対象がまだ生きていて、必要なコンポーネントを持っているか
bool DragTargetAlive(const entt::registry& reg, entt::entity e, DragKind kind)
{
    if (e == entt::null || !reg.valid(e) || !reg.all_of<Transform>(e)) return false;
    switch (kind)
    {
    case DragKind::SpotOuter:
    case DragKind::SpotInner:
    case DragKind::SpotRange:  return reg.all_of<SpotLight>(e);
    case DragKind::PointRange: return reg.all_of<PointLight>(e);
    case DragKind::DirRotate:  return reg.all_of<DirectionalLight>(e);
    case DragKind::None:
    default:                   return false;
    }
}

} // namespace

void LightHandlesFrame(entt::registry& reg, EditorContext& ctx, Camera* camera)
{
    // プロセス寿命で 1 個。viewportToolHandlers へ差し込むラムダがこれを指す。
    static LightHandleState st;

    // 初回(および EditorContext が作り直された時)だけ自分をビューポートツールとして登録する。
    // ハンドラ本体は「今フレーム食ったか」を返すだけ。当たり判定とドラッグ処理はこの関数が
    // 今フレームのマウス位置で先に済ませてあるので、ギズモと同じく 1 フレーム遅れが出ない。
    if (st.registeredCtx != &ctx)
    {
        st.registeredCtx = &ctx;
        LightHandleState* self = &st;
        ctx.viewportToolHandlers.push_back(
            [self](const ViewportInput&) { return self->consumed; });
    }

    st.consumed = false;

    if (!camera || ctx.viewportW <= 1.0f || ctx.viewportH <= 1.0f)
        return;

    const f32 vpX = ctx.viewportX, vpY = ctx.viewportY;
    const f32 vpW = ctx.viewportW, vpH = ctx.viewportH;

    XMFLOAT4X4 viewProj{};
    XMStoreFloat4x4(&viewProj, camera->GetViewProjMatrix());

    // ギズモ/UI プレビューと同じ背景ドローリスト(3D の上・各パネルの下)。
    // 引数なし版はカレントウィンドウのビューポート依存で、マルチビューポート時に
    // 別 OS ウィンドウへ描かれて見えなくなるため必ずメインビューポートを明示する。
    ImDrawList* drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    drawList->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    // ---- 描画ヘルパ ----
    auto project = [&](const XMFLOAT3& w, XMFLOAT2& out) -> bool
    {
        return lm::WorldToScreen(viewProj, w, vpX, vpY, vpW, vpH, out);
    };
    auto line3 = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 col, f32 thickness)
    {
        XMFLOAT2 sa{}, sb{};
        if (project(a, sa) && project(b, sb))
            drawList->AddLine(ImVec2(sa.x, sa.y), ImVec2(sb.x, sb.y), col, thickness);
    };
    // コーンのワイヤ(底円 + 母線 4 本)
    auto drawCone = [&](const XMFLOAT3& apex, const XMFLOAT3& dir,
                        f32 dist, f32 halfDeg, ImU32 col, f32 thickness)
    {
        const XMFLOAT3 first = lm::ConeRimPoint(apex, dir, dist, halfDeg, 0.0f);
        XMFLOAT3 prev = first;
        for (int i = 1; i <= kConeSegments; ++i)
        {
            const f32 phase = XM_2PI * static_cast<f32>(i) / static_cast<f32>(kConeSegments);
            const XMFLOAT3 p = (i == kConeSegments)
                ? first : lm::ConeRimPoint(apex, dir, dist, halfDeg, phase);
            line3(prev, p, col, thickness);
            if ((i % (kConeSegments / 4)) == 0) line3(apex, p, col, thickness);
            prev = p;
        }
    };
    // 点光源の影響半径。カメラ正対の円としてスクリーンに描く(3D 球ワイヤは
    // EditorIconRenderer が別途出すので、ここは掴む目印としての 1 本だけ)。
    auto drawRangeRing = [&](const XMFLOAT3& center, f32 radius, ImU32 col, f32 thickness)
    {
        const XMFLOAT3 right = CameraRight(*camera);
        XMFLOAT2 sc{}, se{};
        if (!project(center, sc) || !project(Offset(center, right, radius), se)) return;
        const f32 rPx = lm::ScreenDistance(sc, se);
        if (rPx < 2.0f || rPx > 8000.0f) return;
        drawList->AddCircle(ImVec2(sc.x, sc.y), rPx, col, 40, thickness);
    };
    auto drawHandle = [&](const XMFLOAT2& s, bool hot)
    {
        drawList->AddCircleFilled(ImVec2(s.x, s.y), kHandleRadius,
                                  hot ? kColHandleHot : kColHandle, 14);
        drawList->AddCircle(ImVec2(s.x, s.y), kHandleRadius + 1.5f, kColOutline, 14, 1.5f);
    };
    // マウス脇の小さな数値ラベル(ギズモの増分表示と同じ流儀)
    auto drawTip = [&](const char* text)
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const ImVec2 pos(mp.x + 18.0f, mp.y + 18.0f);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 bmin(pos.x - 6.0f, pos.y - 4.0f);
        const ImVec2 bmax(pos.x + ts.x + 6.0f, pos.y + ts.y + 4.0f);
        drawList->AddRectFilled(bmin, bmax, IM_COL32(18, 20, 26, 225), 4.0f);
        drawList->AddRect(bmin, bmax, kColHandleHot, 4.0f);
        drawList->AddText(pos, IM_COL32(255, 255, 255, 255), text);
    };

    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inViewport = !ctx.floatingToolWindowHovered
        && mouse.x >= vpX && mouse.x < vpX + vpW
        && mouse.y >= vpY && mouse.y < vpY + vpH;
    // ギズモが掴まれている / ホバーされている時はそちらを優先(取り合いにしない)
    const bool gizmoBusy = ImGuizmo::IsUsing() || ImGuizmo::IsOver();

    // =======================================================================
    // (A) 全ライトの影響範囲を薄いワイヤで一望する(ctx.lightWireAll)
    // =======================================================================
    if (ctx.lightWireAll)
    {
        for (auto [e, pl, tf] : reg.view<const PointLight, const Transform>().each())
        {
            if (ctx.IsSelected(e)) continue;   // 選択中は下で実線＋ハンドルを描く
            drawRangeRing(WorldPositionOf(reg, e, tf),
                          (std::min)(pl.range, kMaxWireRange), kColPointFaint, 1.0f);
        }
        for (auto [e, sl, tf] : reg.view<const SpotLight, const Transform>().each())
        {
            if (ctx.IsSelected(e)) continue;
            const XMFLOAT3 pos = WorldPositionOf(reg, e, tf);
            const XMFLOAT3 dir = NormalizedOr(sl.direction, XMFLOAT3{0.0f, -1.0f, 0.0f});
            drawCone(pos, dir, (std::min)(sl.range, kMaxWireRange),
                     (std::max)(sl.outerConeDeg, sl.innerConeDeg), kColSpotFaint, 1.0f);
        }
        for (auto [e, dirLight, tf] : reg.view<const DirectionalLight, const Transform>().each())
        {
            if (ctx.IsSelected(e)) continue;
            const XMFLOAT3 pos = WorldPositionOf(reg, e, tf);
            const XMFLOAT3 dir = NormalizedOr(dirLight.direction, XMFLOAT3{0.0f, -1.0f, 0.0f});
            line3(pos, Offset(pos, dir, kDirArmLength), kColDirFaint, 1.0f);
        }
    }

    // =======================================================================
    // (B) 太陽ドラッグ: L 押しっぱなし + マウス移動 で最初の DirectionalLight を回す
    //     (UE の Ctrl+L 相当。Ctrl+L は「新規スクリプト」に取られているので L 単独)
    // =======================================================================
    {
        const bool keyHeld = ImGui::IsKeyDown(ImGuiKey_L) && !io.WantTextInput;

        if (!st.sunDragging && keyHeld && inViewport && !gizmoBusy)
        {
            // シーンの太陽 = 最初の DirectionalLight(Application のライト選択と同じ規則)
            entt::entity sun = entt::null;
            for (auto e : reg.view<DirectionalLight>())
            {
                sun = e;
                break;
            }
            if (sun != entt::null)
            {
                st.sunDragging  = true;
                st.sunEntity    = sun;
                st.sunBefore    = reg.get<DirectionalLight>(sun);
                st.sunLastMouse = mouse;
                st.sunAngles    = lm::DirectionToSunAngles(st.sunBefore.direction);
            }
        }

        if (st.sunDragging)
        {
            const bool alive = reg.valid(st.sunEntity)
                            && reg.all_of<DirectionalLight>(st.sunEntity);
            if (keyHeld && alive)
            {
                auto& sun = reg.get<DirectionalLight>(st.sunEntity);
                st.sunAngles = lm::ApplySunDrag(st.sunAngles,
                                                mouse.x - st.sunLastMouse.x,
                                                mouse.y - st.sunLastMouse.y,
                                                kSunDegPerPixel);
                st.sunLastMouse = mouse;
                sun.direction   = lm::SunAnglesToDirection(st.sunAngles);
                // Transform 回転のデルタ追従(Application::Update)が「向きを戻す」方向に
                // 効かないよう、基準回転を次フレームに取り直させる。
                sun._prevRotInit = false;

                st.sunHudTimer = 1.2f;
                st.consumed    = true;   // このフレームのクリックはピッキングへ渡さない
            }
            else
            {
                // キーを離した(またはライトが消えた) → ここで初めて Undo を 1 エントリ積む
                if (alive)
                {
                    const DirectionalLight& after = reg.get<DirectionalLight>(st.sunEntity);
                    if (after.direction.x != st.sunBefore.direction.x
                        || after.direction.y != st.sunBefore.direction.y
                        || after.direction.z != st.sunBefore.direction.z)
                    {
                        ctx.undoSystem.PushCommand(
                            std::make_unique<ComponentEditCommand<DirectionalLight>>(
                                &reg, st.sunEntity, st.sunBefore, after, "太陽の向き"));
                    }
                }
                st.sunDragging = false;
                st.sunEntity   = entt::null;
            }
        }

        // 方位/高度のオーバーレイ(ビューポート上端中央)。離した後も少しだけ残す。
        if (st.sunHudTimer > 0.0f)
        {
            st.sunHudTimer -= io.DeltaTime;

            char buf[128];
            std::snprintf(buf, sizeof(buf), "太陽  方位 %.0f°   高度 %.0f°",
                          static_cast<double>(st.sunAngles.azimuthDeg),
                          static_cast<double>(st.sunAngles.elevationDeg));
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            const ImVec2 pos(vpX + (vpW - ts.x) * 0.5f, vpY + 14.0f);
            const ImVec2 bmin(pos.x - 12.0f, pos.y - 6.0f);
            const ImVec2 bmax(pos.x + ts.x + 12.0f, pos.y + ts.y + 6.0f);
            drawList->AddRectFilled(bmin, bmax, IM_COL32(18, 20, 26, 215), 6.0f);
            drawList->AddRect(bmin, bmax, kColDir, 6.0f);
            drawList->AddText(pos, IM_COL32(255, 244, 214, 255), buf);
        }
    }

    // =======================================================================
    // (C) 選択中ライトのハンドル(コーン角 / range / 向き)
    // =======================================================================
    const entt::entity sel = ctx.selectedEntity;

    // ドラッグ中に選択が変わった / エンティティやコンポーネントが消えたら安全に打ち切る
    if (st.drag != DragKind::None
        && (st.dragEntity != sel || !DragTargetAlive(reg, st.dragEntity, st.drag)))
    {
        st.drag       = DragKind::None;
        st.dragEntity = entt::null;
    }

    const bool selIsLight = sel != entt::null && reg.valid(sel)
        && reg.all_of<Transform>(sel)
        && reg.any_of<PointLight, SpotLight, DirectionalLight>(sel);

    if (selIsLight && !st.sunDragging)
    {
        const Transform& tf = reg.get<Transform>(sel);
        const XMFLOAT3 pos = WorldPositionOf(reg, sel, tf);

        // ---- ハンドルの現在位置(ワールド → スクリーン)を組む ----
        struct HandleInfo
        {
            DragKind kind = DragKind::None;
            XMFLOAT2 screen{0.0f, 0.0f};
            bool     valid = false;
        };
        HandleInfo handles[3];
        int handleCount = 0;
        auto addHandle = [&](DragKind kind, const XMFLOAT3& world)
        {
            if (handleCount >= 3) return;
            HandleInfo h;
            h.kind  = kind;
            h.valid = project(world, h.screen);
            handles[handleCount] = h;
            ++handleCount;
        };

        if (reg.all_of<SpotLight>(sel))
        {
            const SpotLight& sl = reg.get<SpotLight>(sel);
            const XMFLOAT3 dir  = NormalizedOr(sl.direction, XMFLOAT3{0.0f, -1.0f, 0.0f});
            const f32 wireLen   = (std::min)(sl.range, kMaxWireRange);
            const f32 outerDeg  = (std::max)(sl.outerConeDeg, sl.innerConeDeg);
            const f32 innerDeg  = (std::min)(sl.innerConeDeg, outerDeg);

            drawCone(pos, dir, wireLen, outerDeg, kColSpot, 1.7f);
            drawCone(pos, dir, wireLen, innerDeg, kColSpot, 1.0f);

            // 外/内のハンドルは円周の反対側に置く(角度が近い時に重ならないように)
            addHandle(DragKind::SpotOuter, lm::ConeRimPoint(pos, dir, wireLen, outerDeg, 0.0f));
            addHandle(DragKind::SpotInner, lm::ConeRimPoint(pos, dir, wireLen, innerDeg, XM_PI));
            addHandle(DragKind::SpotRange, Offset(pos, dir, wireLen));
        }
        else if (reg.all_of<PointLight>(sel))
        {
            const PointLight& pl = reg.get<PointLight>(sel);
            const f32 wireLen = (std::min)(pl.range, kMaxWireRange);
            drawRangeRing(pos, wireLen, kColPoint, 1.7f);

            // 掴んでいる間は軸を固定(カメラを動かしてもハンドルが飛ばない)
            const XMFLOAT3 axis = (st.drag == DragKind::PointRange)
                ? st.dragAxis : CameraRight(*camera);
            addHandle(DragKind::PointRange, Offset(pos, axis, wireLen));
        }
        else   // DirectionalLight
        {
            const DirectionalLight& dirLight = reg.get<DirectionalLight>(sel);
            const XMFLOAT3 dir = NormalizedOr(dirLight.direction, XMFLOAT3{0.0f, -1.0f, 0.0f});
            const XMFLOAT3 tip = Offset(pos, dir, kDirArmLength);
            line3(pos, tip, kColDir, 2.0f);
            addHandle(DragKind::DirRotate, tip);
        }

        // ---- ホバー判定(マウスに一番近い有効ハンドル) ----
        DragKind hovered = DragKind::None;
        if (inViewport && !gizmoBusy)
        {
            f32 best = kHandleHitRadius;
            for (int i = 0; i < handleCount; ++i)
            {
                if (!handles[i].valid) continue;
                const f32 d = lm::ScreenDistance(handles[i].screen, XMFLOAT2{mouse.x, mouse.y});
                if (d <= best) { best = d; hovered = handles[i].kind; }
            }
        }

        for (int i = 0; i < handleCount; ++i)
        {
            if (!handles[i].valid) continue;
            const bool hot = (st.drag == handles[i].kind)
                          || (st.drag == DragKind::None && hovered == handles[i].kind);
            drawHandle(handles[i].screen, hot);
        }

        // ---- ドラッグ開始 ----
        if (st.drag == DragKind::None && hovered != DragKind::None
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            st.drag       = hovered;
            st.dragEntity = sel;
            if (reg.all_of<SpotLight>(sel))        st.slBefore = reg.get<SpotLight>(sel);
            if (reg.all_of<PointLight>(sel))       st.plBefore = reg.get<PointLight>(sel);
            if (reg.all_of<DirectionalLight>(sel)) st.dlBefore = reg.get<DirectionalLight>(sel);
            if (hovered == DragKind::PointRange)   st.dragAxis = CameraRight(*camera);
        }

        // ハンドルの上ではクリックをピッキングへ渡さない(掴もうとして選択が飛ぶのを防ぐ)
        if (hovered != DragKind::None) st.consumed = true;
    }

    // ---- ドラッグ継続 / 確定 ----
    if (st.drag != DragKind::None)
    {
        st.consumed = true;

        XMFLOAT3 rayOrigin{}, rayDir{};
        ScreenRay(*camera, vpX, vpY, vpW, vpH, mouse.x, mouse.y, rayOrigin, rayDir);

        const Transform& tf = reg.get<Transform>(st.dragEntity);
        const XMFLOAT3 pos  = WorldPositionOf(reg, st.dragEntity, tf);
        char tipText[96];
        tipText[0] = '\0';

        switch (st.drag)
        {
        case DragKind::SpotOuter:
        case DragKind::SpotInner:
        {
            auto& sl = reg.get<SpotLight>(st.dragEntity);
            const XMFLOAT3 dir = NormalizedOr(sl.direction, XMFLOAT3{0.0f, -1.0f, 0.0f});
            const f32 dist = (std::min)(sl.range, kMaxWireRange);
            const XMFLOAT3 baseCenter = Offset(pos, dir, dist);
            // 底面(コーン軸に垂直な平面)へレイを落とし、軸からの距離 → 半頂角へ戻す
            f32 t = 0.0f;
            if (lm::RayPlane(rayOrigin, rayDir, baseCenter, dir, t))
            {
                const XMFLOAT3 hit = Offset(rayOrigin, rayDir, t);
                const f32 deg = lm::ConeHalfAngleFromRadius(Distance3(hit, baseCenter), dist);
                if (st.drag == DragKind::SpotOuter)
                {
                    sl.outerConeDeg = lm::Clampf(deg, 1.0f, 89.0f);
                    if (sl.innerConeDeg > sl.outerConeDeg) sl.innerConeDeg = sl.outerConeDeg;
                }
                else
                {
                    sl.innerConeDeg = lm::Clampf(deg, 1.0f, sl.outerConeDeg);
                }
            }
            const bool outer = (st.drag == DragKind::SpotOuter);
            std::snprintf(tipText, sizeof(tipText), "%s %.0f°",
                          outer ? "外コーン" : "内コーン",
                          static_cast<double>(outer ? sl.outerConeDeg : sl.innerConeDeg));
            break;
        }
        case DragKind::SpotRange:
        {
            auto& sl = reg.get<SpotLight>(st.dragEntity);
            const XMFLOAT3 dir = NormalizedOr(sl.direction, XMFLOAT3{0.0f, -1.0f, 0.0f});
            f32 t = 0.0f;
            if (lm::ClosestParamOnLineToRay(pos, dir, rayOrigin, rayDir, t))
                sl.range = lm::Clampf(t, kMinRange, kMaxRange);
            std::snprintf(tipText, sizeof(tipText), "距離 %.1f m", static_cast<double>(sl.range));
            break;
        }
        case DragKind::PointRange:
        {
            auto& pl = reg.get<PointLight>(st.dragEntity);
            f32 t = 0.0f;
            if (lm::ClosestParamOnLineToRay(pos, st.dragAxis, rayOrigin, rayDir, t))
                pl.range = lm::Clampf(std::fabs(t), kMinRange, kMaxRange);
            std::snprintf(tipText, sizeof(tipText), "距離 %.1f m", static_cast<double>(pl.range));
            break;
        }
        case DragKind::DirRotate:
        {
            auto& dirLight = reg.get<DirectionalLight>(st.dragEntity);
            dirLight.direction = lm::DirectionFromRayOnSphere(pos, kDirArmLength,
                                                              rayOrigin, rayDir);
            dirLight._prevRotInit = false;   // Transform 回転デルタ追従の基準を取り直させる
            const lm::SunAngles a = lm::DirectionToSunAngles(dirLight.direction);
            std::snprintf(tipText, sizeof(tipText), "方位 %.0f° / 高度 %.0f°",
                          static_cast<double>(a.azimuthDeg),
                          static_cast<double>(a.elevationDeg));
            break;
        }
        case DragKind::None:
        default:
            break;
        }

        if (tipText[0] != '\0') drawTip(tipText);

        // ---- 確定: マウスを離した時に Undo を 1 エントリだけ積む ----
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            switch (st.drag)
            {
            case DragKind::SpotOuter:
            case DragKind::SpotInner:
            case DragKind::SpotRange:
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<SpotLight>>(
                    &reg, st.dragEntity, st.slBefore, reg.get<SpotLight>(st.dragEntity),
                    "SpotLight"));
                break;
            case DragKind::PointRange:
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<PointLight>>(
                    &reg, st.dragEntity, st.plBefore, reg.get<PointLight>(st.dragEntity),
                    "PointLight"));
                break;
            case DragKind::DirRotate:
                ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<DirectionalLight>>(
                    &reg, st.dragEntity, st.dlBefore, reg.get<DirectionalLight>(st.dragEntity),
                    "DirectionalLight"));
                break;
            case DragKind::None:
            default:
                break;
            }
            st.drag       = DragKind::None;
            st.dragEntity = entt::null;
        }
    }

    drawList->PopClipRect();
}

} // namespace dx12e
