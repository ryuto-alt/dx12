#include "editor/panels/ViewportToolbar.h"

#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/EditorCommandTable.h"
#include "gui/VirtualInputImGui.h"   // dx12_imgui_find 用アンカー
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dx12e
{

namespace th = dx12e::theme;
using namespace DirectX;

namespace
{
constexpr const char* kViewModeLabels[] = {
    "ライティング", "デプス", "法線", "ラフネス", "メタリック", "AO", "ワイヤ", "ライト複雑度", "クラスタ境界",
};
constexpr int kViewModeCount = static_cast<int>(sizeof(kViewModeLabels) / sizeof(kViewModeLabels[0]));
static_assert(kViewModeCount == vp::kViewModeCount, "ビューモードのラベル表と ViewportLogic.h の個数が食い違っている");

ImU32 U32(const ImVec4& c) { return ImGui::GetColorU32(c); }
ImU32 U32A(const ImVec4& c, float a) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// ポップオーバーの共通スタイル（Begin の前に Push / End の後に Pop）
void PushPopoverStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(6.0f, 7.0f));
}
void PopPopoverStyle() { ImGui::PopStyleVar(2); }

// ポップオーバー内の見出し行（淡い小さな文字）
void PopoverLabel(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, th::TextDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

XMFLOAT3 WorldPosOf(entt::registry& reg, entt::entity e)
{
    XMFLOAT3 p{ 0, 0, 0 };
    if (reg.valid(e) && reg.all_of<Transform>(e))
        XMStoreFloat3(&p, ComputeWorldMatrix(reg, e).r[3]);
    return p;
}
} // namespace

f32 ViewportToolbar::HeightPx(const EditorContext& ctx)
{
    return ctx.vpPrefs.barVisible ? th::Px(kBarHeight) : 0.0f;
}

// ---------------------------------------------------------------------------
// カメラ姿勢・ブックマーク
// ---------------------------------------------------------------------------
vp::CameraPose ViewportToolbar::CurrentPose(const Camera* camera) const
{
    vp::CameraPose p;
    const XMFLOAT3 pos = camera->GetPosition();
    p.pos[0] = pos.x; p.pos[1] = pos.y; p.pos[2] = pos.z;
    p.yaw = camera->GetYaw();
    p.pitch = camera->GetPitch();
    return p;
}

void ViewportToolbar::ApplyPose(Camera* camera, const vp::CameraPose& p)
{
    camera->SetPosition(XMFLOAT3(p.pos[0], p.pos[1], p.pos[2]));
    camera->SetYaw(p.yaw);        // 呼ぶ順は SetYaw → SetPitch（Camera.h の注記）
    camera->SetPitch(p.pitch);
}

void ViewportToolbar::SetAssetsDir(const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    std::string s = assetsDir;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    m_bookmarkPath.clear();
    m_bookmarksLoaded = false;
    m_bookmarks = vp::BookmarkSet{};
    if (s.empty()) return;
    const fs::path root = fs::path(s).parent_path();
    if (root.empty()) return;
    m_bookmarkPath = (root / ".dx12" / "viewport_bookmarks.txt").string();
}

void ViewportToolbar::LoadBookmarksIfNeeded()
{
    if (m_bookmarksLoaded) return;
    m_bookmarksLoaded = true;
    if (m_bookmarkPath.empty()) return;
    try
    {
        std::ifstream f(m_bookmarkPath);
        if (!f) return;
        std::stringstream ss;
        ss << f.rdbuf();
        m_bookmarks.Parse(ss.str());
    }
    catch (...) {}
}

void ViewportToolbar::PersistBookmarks() const
{
    if (m_bookmarkPath.empty()) return;
    try
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(m_bookmarkPath).parent_path(), ec);
        std::ofstream f(m_bookmarkPath, std::ios::trunc);
        if (f) f << m_bookmarks.Serialize();
    }
    catch (...) {}
}

void ViewportToolbar::Hud(EditorContext& ctx, const std::string& text, f32 seconds) const
{
    ctx.vpHudText = text;
    ctx.vpHudTimer = seconds;
}

void ViewportToolbar::SaveBookmark(EditorContext& ctx, Camera* camera, int slot)
{
    if (slot < 1 || slot > vp::kBookmarkCount || !camera) return;
    LoadBookmarksIfNeeded();
    vp::Bookmark& b = m_bookmarks.slot[slot - 1];
    b.used = true;
    b.pose = CurrentPose(camera);
    b.fovDeg = ctx.vpPrefs.fovDeg;
    PersistBookmarks();
    ctx.Notify(ui::ToastKind::Success, "カメラブックマーク " + std::to_string(slot) + " に保存しました");
}

void ViewportToolbar::JumpBookmark(EditorContext& ctx, Camera* camera, int slot)
{
    if (slot < 1 || slot > vp::kBookmarkCount || !camera) return;
    LoadBookmarksIfNeeded();
    const vp::Bookmark& b = m_bookmarks.slot[slot - 1];
    if (!b.used)
    {
        ctx.Notify(ui::ToastKind::Info, "ブックマーク " + std::to_string(slot) + " は未保存です（Ctrl+" + std::to_string(slot) + " で保存）");
        return;
    }
    ctx.vpPrefs.fovDeg = b.fovDeg;
    m_tween.Start(CurrentPose(camera), b.pose, 0.42f);
    Hud(ctx, "ブックマーク " + std::to_string(slot));
}

// ---------------------------------------------------------------------------
// 毎フレームの更新
// ---------------------------------------------------------------------------
void ViewportToolbar::Update(EditorContext& ctx, Camera* camera, Scene* /*scene*/, bool isPlaying, f32 dt)
{
    if (ctx.vpHudTimer > 0.0f) ctx.vpHudTimer = std::max(0.0f, ctx.vpHudTimer - dt);
    if (isPlaying || !camera)
    {
        m_tween.Cancel();
        ctx.pendingBookmarkJump = ctx.pendingBookmarkSet = 0;
        m_haveLast = false;
        return;
    }

    if (ctx.pendingBookmarkSet)  { SaveBookmark(ctx, camera, ctx.pendingBookmarkSet);  ctx.pendingBookmarkSet = 0; }
    if (ctx.pendingBookmarkJump) { JumpBookmark(ctx, camera, ctx.pendingBookmarkJump); ctx.pendingBookmarkJump = 0; }

    // 手動のカメラ操作（右ドラッグ / 中ドラッグ / Alt オービット / ビュー上のホイール / 2D ビュー）が始まったら補間を止める
    const ImGuiIO& io = ImGui::GetIO();
    if (m_tween.running)
    {
        const bool manual = io.MouseDown[ImGuiMouseButton_Right] || io.MouseDown[ImGuiMouseButton_Middle]
                         || (io.KeyAlt && io.MouseDown[ImGuiMouseButton_Left])
                         || (io.MouseWheel != 0.0f && ctx.IsCursorInViewport(io.MousePos.x, io.MousePos.y))
                         || ctx.view2D || ctx.flyMode;
        if (manual) m_tween.Cancel();
        else ApplyPose(camera, m_tween.Step(dt));
    }

    // スナップ量・カメラ速度の変化 → 帯の右に一時表示（ゲーム絵に重ねず、帯の中で見せる）
    const f32 speed = camera->GetMoveSpeed();
    if (m_haveLast)
    {
        char buf[96];
        if (std::fabs(speed - m_lastSpeed) > 1e-4f)
        {
            std::snprintf(buf, sizeof(buf), "カメラ速度 %.1f m/s", static_cast<double>(speed));
            Hud(ctx, buf, 1.4f);
        }
        else if (ctx.snapTranslate != m_lastSnapT)
            Hud(ctx, "位置スナップ " + vp::FormatSnap(0, ctx.snapTranslate));
        else if (ctx.snapRotateDeg != m_lastSnapR)
            Hud(ctx, "回転スナップ " + vp::FormatSnap(1, ctx.snapRotateDeg));
        else if (ctx.snapScale != m_lastSnapS)
            Hud(ctx, "スケールスナップ " + vp::FormatSnap(2, ctx.snapScale));
        else if (ctx.snapAlways != m_lastSnapAlways)
            Hud(ctx, ctx.snapAlways ? "スナップ: 常に ON" : "スナップ: Ctrl 押下中だけ");
    }
    m_lastSpeed = speed;
    m_lastSnapT = ctx.snapTranslate; m_lastSnapR = ctx.snapRotateDeg; m_lastSnapS = ctx.snapScale;
    m_lastSnapAlways = ctx.snapAlways;
    m_haveLast = true;
}

// ---------------------------------------------------------------------------
// ビューキューブ
// ---------------------------------------------------------------------------
void ViewportToolbar::GoToFace(EditorContext& ctx, Camera* camera, Scene* scene, vp::CubeFace face)
{
    if (!camera || ctx.view2D) return;
    // 中心 = 選択の平均位置（無ければ今の視線の 10m 先）。距離は今のまま（近すぎ / 遠すぎは丸める）
    float pivot[3];
    float dist = 10.0f;
    const XMFLOAT3 cp = camera->GetPosition();
    const XMFLOAT3 fw = camera->GetForward();
    bool haveSel = false;
    if (scene && ctx.HasSelection())
    {
        auto& reg = scene->GetRegistry();
        XMFLOAT3 sum{ 0, 0, 0 };
        int n = 0;
        for (entt::entity e : ctx.selectedEntities)
        {
            if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
            const XMFLOAT3 w = WorldPosOf(reg, e);
            sum.x += w.x; sum.y += w.y; sum.z += w.z;
            ++n;
        }
        if (n > 0)
        {
            pivot[0] = sum.x / n; pivot[1] = sum.y / n; pivot[2] = sum.z / n;
            const float dx = cp.x - pivot[0], dy = cp.y - pivot[1], dz = cp.z - pivot[2];
            dist = std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz), 3.0f, 200.0f);
            haveSel = true;
        }
    }
    if (!haveSel)
    {
        pivot[0] = cp.x + fw.x * dist; pivot[1] = cp.y + fw.y * dist; pivot[2] = cp.z + fw.z * dist;
    }
    const vp::ViewAngles a = vp::LookAnglesFromFace(face, camera->GetYaw());
    m_tween.Start(CurrentPose(camera), vp::PoseLookingAt(pivot, dist, a.yaw, a.pitch), 0.35f);
    Hud(ctx, std::string(vp::CubeFaceLabel(face)) + " ビュー", 1.2f);
}

void ViewportToolbar::DrawViewCube(EditorContext& ctx, Camera* camera, Scene* scene, ImVec2 cubeMin, f32 size)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 center(cubeMin.x + size * 0.5f, cubeMin.y + size * 0.5f);
    // 立方体の半辺: 斜めから見たとき（対角がいちばん広い）でも枠内に収まるよう 0.30 倍
    const f32 scale = size * 0.33f;

    ImGui::SetCursorScreenPos(cubeMin);
    ImGui::InvisibleButton("##viewCube", ImVec2(size, size));
    vinput_gui::AnchorLastItem("button", "ビューキューブ");
    const bool hov = ImGui::IsItemHovered();

    vp::CubeQuad q[vp::kCubeFaceCount];
    vp::ProjectCube(camera->GetYaw(), camera->GetPitch(), scale, q);

    m_hoverFace = -1;
    if (hov)
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        m_hoverFace = vp::HitCubeFace(q, mp.x - center.x, mp.y - center.y);
    }

    // 奥 → 手前の順に描く（見えている面だけなので重ならないが、念のため）
    int order[vp::kCubeFaceCount];
    for (int i = 0; i < vp::kCubeFaceCount; ++i) order[i] = i;
    std::sort(order, order + vp::kCubeFaceCount, [&](int a, int b) { return q[a].depth > q[b].depth; });
    for (int oi = 0; oi < vp::kCubeFaceCount; ++oi)
    {
        const vp::CubeQuad& f = q[order[oi]];
        if (!f.visible) continue;
        const bool isHot = (m_hoverFace == order[oi]);
        ImVec2 pts[4];
        for (int k = 0; k < 4; ++k) pts[k] = ImVec2(center.x + f.x[k], center.y + f.y[k]);
        // 面の向きで明るさを変える（真上 / 前 / 横で階調が付く）。色の主張はしない（節度）
        const ImVec4 base = th::Bg3;
        const float shade = (f.face == vp::CubeFace::PosY) ? 1.25f : (f.face == vp::CubeFace::NegY) ? 0.8f : 1.0f;
        const ImVec4 fill = isHot ? th::WithAlpha(th::Accent, 0.55f) : ImVec4(std::min(1.0f, base.x * shade), std::min(1.0f, base.y * shade), std::min(1.0f, base.z * shade), 0.97f);
        dl->AddConvexPolyFilled(pts, 4, U32(fill));
        dl->AddPolyline(pts, 4, U32(isHot ? th::AccentHover : th::BorderStrong), ImDrawFlags_Closed, th::PxF(1.0f));
        // ラベル（面の大きさに合わせた小さな字。小さすぎる面には出さない）
        float minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
        for (int k = 1; k < 4; ++k) { minX = std::min(minX, pts[k].x); maxX = std::max(maxX, pts[k].x); minY = std::min(minY, pts[k].y); maxY = std::max(maxY, pts[k].y); }
        const float minDim = std::min(maxX - minX, maxY - minY);
        if (minDim > th::Px(11.0f))
        {
            const char* lb = vp::CubeFaceLabel(f.face);
            const float fs = std::clamp(minDim * 0.62f, th::Px(9.0f), th::Px(13.0f));
            ImFont* font = ImGui::GetFont();
            const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, lb);
            const ImVec2 c((minX + maxX) * 0.5f, (minY + maxY) * 0.5f);
            dl->AddText(font, fs, ImVec2(std::floor(c.x - ts.x * 0.5f + 0.5f), std::floor(c.y - ts.y * 0.5f + 0.5f)),
                        U32(isHot ? th::Text : th::TextMid), lb);
        }
    }

    if (hov && m_hoverFace >= 0)
    {
        ImGui::SetTooltip("%s ビュー（%s 側から見る）", vp::CubeFaceLabel(static_cast<vp::CubeFace>(m_hoverFace)),
                          m_hoverFace == 0 ? "+X" : m_hoverFace == 1 ? "-X" : m_hoverFace == 2 ? "+Y" : m_hoverFace == 3 ? "-Y" : m_hoverFace == 4 ? "+Z" : "-Z");
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && m_hoverFace >= 0)
        GoToFace(ctx, camera, scene, static_cast<vp::CubeFace>(m_hoverFace));
}

// ---------------------------------------------------------------------------
// 帯の描画
// ---------------------------------------------------------------------------
void ViewportToolbar::Render(EditorContext& ctx, Camera* camera, Scene* scene, bool& physicsDebugDraw, bool isPlaying,
                             ImVec2 nodePos, f32 nodeW, f32 barH)
{
    if (!ctx.vpPrefs.barVisible || barH <= 0.0f || nodeW < 8.0f) return;
    LoadBookmarksIfNeeded();

    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(nodePos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(nodeW, barH), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(6.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::Px(2.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, th::Bg1);
    ImGui::Begin("##ViewportBar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    // 下端の区切り（3D の絵との境目）と、既定テーマでは光の線
    dl->AddLine(ImVec2(wp.x, wp.y + barH - 1.0f), ImVec2(wp.x + nodeW, wp.y + barH - 1.0f), U32(th::Border));
    ui::deco::EdgeLine(dl, ImVec2(wp.x, wp.y + barH - th::PxF(1.0f)), ImVec2(wp.x + nodeW, wp.y + barH - th::PxF(1.0f)), th::AccentHover, 0.35f);

    const f32 btn = th::Px(th::size::kToolbarBtn);
    const f32 topPad = std::floor((barH - btn) * 0.5f);
    ImGui::SetCursorPosY(topPad);

    // ---- Play 中: 帯は同じ高さのまま、操作は出さない（Editor ⇄ Play で 3D の矩形を揺らさない）----
    if (isPlaying)
    {
        ImGui::SetCursorPosY(std::floor((barH - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::SetCursorPosX(ui::Px(10.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, th::Good);
        ImGui::TextUnformatted(ICON_PLAY);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, ui::Px(8.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, th::TextDim);
        ImGui::TextUnformatted(ctx.paused ? "Play 中（一時停止）: 右ドラッグ + WASD でシーンビューを動かせます  ·  F1 で再開" : "Play 中");
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
        return;
    }

    const bool compact = (nodeW < th::Px(980.0f));   // 狭いときはラベルを畳んでアイコンだけに
    auto sameLine = [&](float gap = 2.0f) { ImGui::SameLine(0.0f, ui::Px(gap)); ImGui::SetCursorPosY(topPad); };
    auto sep = [&]()
    {
        ImGui::SameLine(0.0f, ui::Px(7.0f));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, wp.y + barH * 0.5f - ui::Px(9.0f)), ImVec2(p.x, wp.y + barH * 0.5f + ui::Px(9.0f)), U32(th::BorderStrong));
        ImGui::SameLine(0.0f, ui::Px(9.0f));
        ImGui::SetCursorPosY(topPad);
    };
    // ポップオーバーを直前のボタンの真下に開く
    auto openUnder = [&](const char* popupId)
    {
        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        ImGui::SetNextWindowPos(ImVec2(mn.x, mx.y + ui::Px(3.0f)), ImGuiCond_Always);
        (void)popupId;
    };
    ImGui::SetCursorPosX(ui::Px(8.0f));

    // ===== 変形（移動 / 回転 / 拡縮 / 空間）=====
    if (ui::IconButton("vpMove", ICON_MOVE, "移動  (W)", ctx.gizmoMode == GizmoMode::Translate, nullptr, btn))
        ctx.gizmoMode = GizmoMode::Translate;
    vinput_gui::AnchorLastItem("button", "ビュー:移動");
    sameLine();
    if (ui::IconButton("vpRotate", ICON_ROTATE, "回転  (E)", ctx.gizmoMode == GizmoMode::Rotate, nullptr, btn))
        ctx.gizmoMode = GizmoMode::Rotate;
    vinput_gui::AnchorLastItem("button", "ビュー:回転");
    sameLine();
    if (ui::IconButton("vpScale", ICON_SCALE, "拡大縮小  (R)", ctx.gizmoMode == GizmoMode::Scale, nullptr, btn))
        ctx.gizmoMode = GizmoMode::Scale;
    vinput_gui::AnchorLastItem("button", "ビュー:拡縮");
    sameLine();
    if (ui::IconButton("vpSpace", ctx.gizmoLocalSpace ? ICON_SPACE_LOCAL : ICON_SPACE_WORLD,
                       ctx.gizmoLocalSpace ? "ローカル空間  (T で切替)" : "ワールド空間  (T で切替)", false, nullptr, btn))
        ctx.gizmoLocalSpace = !ctx.gizmoLocalSpace;
    vinput_gui::AnchorLastItem("button", "ビュー:空間");

    // ===== スナップ =====
    sep();
    {
        const int kind = ctx.gizmoMode == GizmoMode::Rotate ? 1 : ctx.gizmoMode == GizmoMode::Scale ? 2 : 0;
        const float cur = kind == 1 ? ctx.snapRotateDeg : kind == 2 ? ctx.snapScale : ctx.snapTranslate;
        const std::string val = vp::FormatSnap(kind, cur);
        if (ui::LabelDropdownButton("vpSnap", ICON_MAGNET, nullptr,
                                    ctx.snapAlways ? "スナップ（常に ON）: 量を変える" : "スナップ: Ctrl を押しながらドラッグで有効。量を変える",
                                    ctx.snapAlways, btn, val.c_str()))
            ImGui::OpenPopup("##vpSnapPop");
        vinput_gui::AnchorLastItem("button", "ビュー:スナップ");
        openUnder("##vpSnapPop");
        PushPopoverStyle();
        if (ImGui::BeginPopup("##vpSnapPop"))
        {
            PopoverLabel("位置スナップ（m）");
            for (int i = 0; i < vp::kSnapTranslateCount; ++i)
            {
                if (i) ImGui::SameLine();
                char id[24], lb[24];
                std::snprintf(id, sizeof(id), "t%d", i);
                std::snprintf(lb, sizeof(lb), "%g", static_cast<double>(vp::kSnapTranslate[i]));
                if (ui::Chip(id, lb, std::fabs(ctx.snapTranslate - vp::kSnapTranslate[i]) < 1e-5f, ui::Px(38.0f))) ctx.snapTranslate = vp::kSnapTranslate[i];
            }
            PopoverLabel("回転スナップ（度）");
            for (int i = 0; i < vp::kSnapRotateCount; ++i)
            {
                if (i) ImGui::SameLine();
                char id[24], lb[24];
                std::snprintf(id, sizeof(id), "r%d", i);
                std::snprintf(lb, sizeof(lb), "%g", static_cast<double>(vp::kSnapRotate[i]));
                if (ui::Chip(id, lb, std::fabs(ctx.snapRotateDeg - vp::kSnapRotate[i]) < 1e-5f, ui::Px(38.0f))) ctx.snapRotateDeg = vp::kSnapRotate[i];
            }
            PopoverLabel("スケールスナップ（倍率）");
            for (int i = 0; i < vp::kSnapScaleCount; ++i)
            {
                if (i) ImGui::SameLine();
                char id[24], lb[24];
                std::snprintf(id, sizeof(id), "s%d", i);
                std::snprintf(lb, sizeof(lb), "%g", static_cast<double>(vp::kSnapScale[i]));
                if (ui::Chip(id, lb, std::fabs(ctx.snapScale - vp::kSnapScale[i]) < 1e-5f, ui::Px(38.0f))) ctx.snapScale = vp::kSnapScale[i];
            }
            ImGui::Separator();
            ui::Checkbox("常にスナップ（Ctrl を押さなくても）##snapAlways", &ctx.snapAlways);
            ImGui::EndPopup();
        }
        PopPopoverStyle();
    }

    // ===== ビューモード =====
    sameLine(4.0f);
    {
        const int vm = std::clamp(ctx.vpPrefs.viewMode, 0, kViewModeCount - 1);
        if (ui::LabelDropdownButton("vpViewMode", ICON_SCAN_EYE, compact ? nullptr : kViewModeLabels[vm],
                                    "ビューモード（ライティング / デプス / 法線 / ワイヤ…。エディタ専用の表示で、ゲームの絵は変わらない）",
                                    vm != 0, btn))
            ImGui::OpenPopup("##vpViewModePop");
        vinput_gui::AnchorLastItem("button", "ビュー:モード");
        openUnder("##vpViewModePop");
        PushPopoverStyle();
        if (ImGui::BeginPopup("##vpViewModePop"))
        {
            const bool gbuf = scene && (scene->GetTaaSettings().enabled || scene->GetSsrSettings().enabled || scene->GetSsgiSettings().enabled);
            const bool ao = scene && scene->GetSSAOSettings().enabled;
            for (int i = 0; i < kViewModeCount; ++i)
            {
                bool ok = true;
                const char* need = nullptr;
                if (i >= 2 && i <= 4 && !gbuf) { ok = false; need = "TAA / SSR / SSGI のどれかを有効にすると使えます（G-Buffer が要る）"; }
                if (i == 5 && !ao) { ok = false; need = "SSAO を有効にすると使えます"; }
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(kViewModeLabels[i], vm == i, 0, ImVec2(ui::Px(180.0f), 0)))
                {
                    ctx.vpPrefs.viewMode = i;
                    ctx.clusterDebugMode = vp::ViewModeToClusterDebug(i);   // ライト複雑度 / クラスタ境界は既存のクラスタ診断へ（他のモードでは 0）
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (!ok && need && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", need);
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, th::TextFaint);
            ImGui::TextUnformatted("アンリットは未対応（描画にその切替が無い）");
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        PopPopoverStyle();
    }

    // ===== 表示フラグ =====
    sameLine(2.0f);
    {
        if (ui::LabelDropdownButton("vpShow", ICON_EYE, compact ? nullptr : "表示", "表示するもの（グリッド / ギズモ / アイコン / コリジョン…）", false, btn))
            ImGui::OpenPopup("##vpShowPop");
        vinput_gui::AnchorLastItem("button", "ビュー:表示");
        openUnder("##vpShowPop");
        PushPopoverStyle();
        if (ImGui::BeginPopup("##vpShowPop"))
        {
            if (scene)
            {
                auto& reg = scene->GetRegistry();
                bool anyGrid = false, gridOn = true;
                for (auto [e, gp] : reg.view<GridPlane>().each()) { anyGrid = true; gridOn = gp.enabled; break; }
                if (anyGrid)
                {
                    if (ui::Checkbox("グリッド##vpGrid", &gridOn))
                        for (auto [e, gp] : reg.view<GridPlane>().each()) gp.enabled = gridOn;
                }
            }
            ui::Checkbox("変形ギズモ##vpGizmo", &ctx.vpPrefs.showGizmo);
            ui::Checkbox("ライト・カメラのアイコン##vpIcons", &ctx.vpPrefs.showIcons);
            ui::Checkbox("ライトのハンドル##vpLH", &ctx.vpPrefs.showLightHandles);
            ui::Checkbox("バウンディングボックス（選択メッシュ）##vpBounds", &ctx.vpPrefs.showBounds);
            ui::Checkbox("コリジョン（物理のワイヤ）##vpColl", &physicsDebugDraw);
            if (scene)
            {
                bool nav = scene->GetNavDebugDraw();
                if (ui::Checkbox("ナビメッシュのワイヤ##vpNav", &nav)) scene->SetNavDebugDraw(nav);
            }
            ImGui::Separator();
            ui::Checkbox("選択アウトライン##vpOutline", &ctx.vpPrefs.outline);
            ui::Checkbox("ホバーの強調##vpHover", &ctx.vpPrefs.hoverHighlight);
            ui::Checkbox("ビューキューブ##vpCube", &ctx.vpPrefs.showViewCube);
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, th::TextFaint);
            ImGui::TextUnformatted("ここの設定はエディタの表示だけ。ゲームの絵には影響しません。");
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        PopPopoverStyle();
    }

    // ===== カメラ（速度・視野角）=====
    sep();
    {
        char val[32];
        std::snprintf(val, sizeof(val), "%.1f", static_cast<double>(camera ? camera->GetMoveSpeed() : 0.0f));
        if (ui::LabelDropdownButton("vpCamera", ICON_CAMERA, nullptr, "カメラの速度・視野角（右ドラッグ中にホイールでも速度を変えられる）", false, btn, val))
            ImGui::OpenPopup("##vpCameraPop");
        vinput_gui::AnchorLastItem("button", "ビュー:カメラ");
        openUnder("##vpCameraPop");
        PushPopoverStyle();
        if (camera && ImGui::BeginPopup("##vpCameraPop"))
        {
            PopoverLabel("移動速度");
            f32 speed = camera->GetMoveSpeed();
            ImGui::SetNextItemWidth(ui::Px(230.0f));
            if (ui::SliderFloat("##vpSpeed", &speed, 0.2f, 200.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic)) camera->SetMoveSpeed(speed);
            struct Preset { const char* label; f32 speed; };
            static const Preset presets[] = { {"精密 1", 1.0f}, {"標準 5", 5.0f}, {"広域 20", 20.0f}, {"最速 80", 80.0f} };
            for (int i = 0; i < 4; ++i)
            {
                if (i) ImGui::SameLine();
                if (ui::Chip(presets[i].label, presets[i].label, std::fabs(speed - presets[i].speed) < 1e-3f, ui::Px(54.0f))) camera->SetMoveSpeed(presets[i].speed);
            }
            PopoverLabel("視野角（縦・度）");
            ImGui::BeginDisabled(ctx.view2D);
            ImGui::SetNextItemWidth(ui::Px(230.0f));
            ui::SliderFloat("##vpFov", &ctx.vpPrefs.fovDeg, 20.0f, 120.0f, "%.0f°");
            for (int i = 0; i < 4; ++i)
            {
                static const float kFov[4] = { 30.0f, 45.0f, 60.0f, 90.0f };
                char id[16], lb[16];
                std::snprintf(id, sizeof(id), "fov%d", i);
                std::snprintf(lb, sizeof(lb), "%.0f°", static_cast<double>(kFov[i]));
                if (i) ImGui::SameLine();
                if (ui::Chip(id, lb, std::fabs(ctx.vpPrefs.fovDeg - kFov[i]) < 0.5f, ui::Px(54.0f))) ctx.vpPrefs.fovDeg = kFov[i];
            }
            ImGui::EndDisabled();
            PopoverLabel("マウス感度");
            f32 sens = camera->GetMouseSensitivity();
            ImGui::SetNextItemWidth(ui::Px(230.0f));
            if (ui::SliderFloat("##vpSens", &sens, 0.0005f, 0.02f, "%.4f")) camera->SetMouseSensitivity(sens);
            ImGui::EndPopup();
        }
        PopPopoverStyle();
    }

    // ===== アスペクト =====
    sameLine(2.0f);
    {
        const int ai = std::clamp(ctx.vpPrefs.aspectIndex, 0, vp::kAspectCount - 1);
        if (ui::LabelDropdownButton("vpAspect", ICON_RATIO, nullptr, "ビューポートのアスペクト比（Play 中は常に 16:9）", ai != vp::kAspectDefault, btn, vp::kAspects[ai].label))
            ImGui::OpenPopup("##vpAspectPop");
        vinput_gui::AnchorLastItem("button", "ビュー:アスペクト");
        openUnder("##vpAspectPop");
        PushPopoverStyle();
        if (ImGui::BeginPopup("##vpAspectPop"))
        {
            for (int i = 0; i < vp::kAspectCount; ++i)
            {
                if (ImGui::Selectable(vp::kAspects[i].label, ai == i, 0, ImVec2(ui::Px(120.0f), 0)))
                {
                    ctx.vpPrefs.aspectIndex = i;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        PopPopoverStyle();
    }

    // ===== ブックマーク =====
    sameLine(2.0f);
    {
        if (ui::LabelDropdownButton("vpBookmarks", ICON_BOOKMARK, nullptr, "カメラブックマーク（1〜9 で移動 / Ctrl+1〜9 で保存）", false, btn))
            ImGui::OpenPopup("##vpBookmarkPop");
        vinput_gui::AnchorLastItem("button", "ビュー:ブックマーク");
        openUnder("##vpBookmarkPop");
        PushPopoverStyle();
        if (camera && ImGui::BeginPopup("##vpBookmarkPop"))
        {
            for (int i = 0; i < vp::kBookmarkCount; ++i)
            {
                ImGui::PushID(i);
                const vp::Bookmark& b = m_bookmarks.slot[i];
                ImGui::PushStyleColor(ImGuiCol_Text, b.used ? th::AccentHover : th::TextFaint);
                ui::PushMono();
                ImGui::Text("%d", i + 1);
                ui::PopMono();
                ImGui::PopStyleColor();
                ImGui::SameLine(ui::Px(22.0f));
                if (b.used)
                {
                    char pos[80];
                    std::snprintf(pos, sizeof(pos), "%.0f, %.0f, %.0f", static_cast<double>(b.pose.pos[0]), static_cast<double>(b.pose.pos[1]), static_cast<double>(b.pose.pos[2]));
                    ImGui::PushStyleColor(ImGuiCol_Text, th::TextMid);
                    ui::PushMono();
                    ImGui::TextUnformatted(pos);
                    ui::PopMono();
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, th::TextFaint);
                    ImGui::TextUnformatted("未保存");
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine(ui::Px(150.0f));
                if (ui::Chip("save", "保存", false, ui::Px(44.0f))) SaveBookmark(ctx, camera, i + 1);
                ImGui::SameLine();
                ImGui::BeginDisabled(!b.used);
                if (ui::Chip("go", "移動", false, ui::Px(44.0f))) { JumpBookmark(ctx, camera, i + 1); ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ui::Chip("del", "消去", false, ui::Px(44.0f)))
                {
                    m_bookmarks.slot[i] = vp::Bookmark{};
                    PersistBookmarks();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        PopPopoverStyle();
    }

    // ===== 右端: 一時表示 + ビューキューブ =====
    const f32 cubeSize = barH - th::Px(6.0f);
    const bool showCube = ctx.vpPrefs.showViewCube && !ctx.view2D && camera != nullptr;
    const f32 cubeX = nodeW - cubeSize - th::Px(8.0f);
    if (ctx.vpHudTimer > 0.0f && !ctx.vpHudText.empty() && !compact)
    {
        const f32 a = std::clamp(ctx.vpHudTimer / 0.5f, 0.0f, 1.0f);   // 最後の 0.5 秒でフェード
        ui::PushMono();
        const ImVec2 ts = ImGui::CalcTextSize(ctx.vpHudText.c_str());
        const f32 hx = wp.x + (showCube ? cubeX : nodeW - th::Px(8.0f)) - ts.x - th::Px(16.0f);
        const ImVec2 p0(hx - th::Px(8.0f), wp.y + (barH - ts.y) * 0.5f - th::Px(3.0f));
        const ImVec2 p1(hx + ts.x + th::Px(8.0f), wp.y + (barH + ts.y) * 0.5f + th::Px(3.0f));
        dl->AddRectFilled(p0, p1, U32A(th::Bg3, a), th::Px(3.0f));
        dl->AddRect(p0, p1, U32A(th::AccentHover, 0.45f * a), th::Px(3.0f), 0, th::PxF(1.0f));
        dl->AddText(ImVec2(hx, wp.y + (barH - ts.y) * 0.5f), U32A(th::Text, a), ctx.vpHudText.c_str());
        ui::PopMono();
    }
    if (showCube)
        DrawViewCube(ctx, camera, scene, ImVec2(wp.x + cubeX, wp.y + th::Px(3.0f)), cubeSize);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

} // namespace dx12e
