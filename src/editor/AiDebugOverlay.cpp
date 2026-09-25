// ============================================================================
// 選択中の Brain のデバッグ表示。中身は AiDebugOverlay.h の冒頭を参照。
// ============================================================================
#include "editor/AiDebugOverlay.h"

#include "editor/EditorContext.h"
#include "editor/LightMath.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "ai/AiSystem.h"

#include <imgui.h>

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dx12e
{

using namespace DirectX;
namespace lm = lightmath;

void AiDebugOverlayFrame(entt::registry& reg, EditorContext& ctx, Camera* camera)
{
    ai::AiSystem* ai = ctx.aiSystem;
    if (!ai || !camera || ctx.viewportW <= 1.0f || ctx.viewportH <= 1.0f) return;
    const entt::entity e = ctx.selectedEntity;
    if (e == entt::null || !reg.valid(e) || !reg.all_of<Brain, Transform>(e)) return;
    const Brain& cfg = reg.get<Brain>(e);
    if (!cfg.debugDraw) return;
    const ai::BrainState* st = ai->GetBrain(e);
    if (!st) return;   // Play していない（実行時の状態が無い）

    const f32 vpX = ctx.viewportX, vpY = ctx.viewportY, vpW = ctx.viewportW, vpH = ctx.viewportH;
    XMFLOAT4X4 viewProj{};
    XMStoreFloat4x4(&viewProj, camera->GetViewProjMatrix());
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    dl->PushClipRect(ImVec2(vpX, vpY), ImVec2(vpX + vpW, vpY + vpH), true);

    auto project = [&](const XMFLOAT3& w, ImVec2& out) -> bool
    {
        XMFLOAT2 s{};
        if (!lm::WorldToScreen(viewProj, w, vpX, vpY, vpW, vpH, s)) return false;
        out = ImVec2(s.x, s.y);
        return true;
    };
    auto line3 = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 col, f32 th)
    {
        ImVec2 sa, sb;
        if (project(a, sa) && project(b, sb)) dl->AddLine(sa, sb, col, th);
    };
    auto circle3 = [&](const XMFLOAT3& c, f32 r, ImU32 col, f32 th, int seg = 32)
    {
        XMFLOAT3 prev{ c.x + r, c.y, c.z };
        for (int i = 1; i <= seg; ++i)
        {
            const f32 a = static_cast<f32>(i) / static_cast<f32>(seg) * XM_2PI;
            const XMFLOAT3 p{ c.x + std::cos(a) * r, c.y, c.z + std::sin(a) * r };
            line3(prev, p, col, th);
            prev = p;
        }
    };

    const Transform& t = reg.get<Transform>(e);
    const bool seen = st->bb.Bool("target.seen", false);
    const bool visible = st->bb.Bool("target.visible", false);

    // ---- 視界の扇形（目の高さの水平面）と気配の円 ----
    {
        const XMFLOAT3 eye{ st->eye[0], st->eye[1], st->eye[2] };
        const f32 yaw = XMConvertToRadians(t.rotation.y);
        const f32 half = XMConvertToRadians((std::min)(cfg.sightFov, 360.0f) * 0.5f);
        const ImU32 col = seen ? IM_COL32(255, 70, 60, 220) : (visible ? IM_COL32(255, 170, 40, 220)
                                                                        : IM_COL32(255, 230, 90, 170));
        const int seg = 24;
        XMFLOAT3 prev{};
        for (int i = 0; i <= seg; ++i)
        {
            const f32 a = yaw - half + (2.0f * half) * static_cast<f32>(i) / static_cast<f32>(seg);
            const XMFLOAT3 p{ eye.x + std::sin(a) * cfg.sightRange, eye.y, eye.z + std::cos(a) * cfg.sightRange };
            if (i == 0 || i == seg) line3(eye, p, col, 1.5f);
            if (i > 0) line3(prev, p, col, 1.5f);
            prev = p;
        }
        if (cfg.nearSense > 0.0f)
            circle3({ t.position.x, t.position.y + 0.05f, t.position.z }, cfg.nearSense, IM_COL32(255, 230, 90, 90), 1.0f);
    }

    // ---- 最後の視線判定（緑 = 通った / 赤 = 遮られた / 灰 = 視野の外で撃っていない）----
    for (const auto& r : st->rays)
    {
        const ImU32 col = !r.candidate ? IM_COL32(150, 150, 150, 90)
                        : (r.blocked ? IM_COL32(255, 60, 60, 220) : IM_COL32(60, 255, 120, 230));
        line3({ r.from[0], r.from[1], r.from[2] }, { r.to[0], r.to[1], r.to[2] }, col, r.candidate ? 2.0f : 1.0f);
    }

    // ---- 聞いた音（輪。新しいほど濃い）----
    char buf[256];
    for (const auto& h : st->heard)
    {
        const f64 age = ai->Time() - h.time;
        const int alpha = static_cast<int>(255.0 * std::clamp(1.0 - age / 6.0, 0.15, 1.0));
        const ImU32 col = h.occluded ? IM_COL32(120, 160, 255, alpha) : IM_COL32(90, 220, 255, alpha);
        const XMFLOAT3 c{ h.pos[0], h.pos[1] + 0.1f, h.pos[2] };
        circle3(c, 0.4f + h.loudness * 1.2f, col, 2.0f, 20);
        ImVec2 sc;
        if (project(c, sc))
        {
            std::snprintf(buf, sizeof(buf), "%s %.0f%%%s %.1fs", h.tag.empty() ? "sound" : h.tag.c_str(),
                          h.loudness * 100.0f, h.occluded ? " (壁越し)" : "", age);
            dl->AddText(ImVec2(sc.x + 6, sc.y - 7), col, buf);
        }
    }

    // ---- 最後に見た / 聞いた位置 ----
    ai::BbVec3 lk;
    if (st->bb.Vec3("target.lastKnown", lk))
    {
        const XMFLOAT3 c{ lk.x, lk.y + 0.05f, lk.z };
        const ImU32 col = IM_COL32(255, 120, 220, 230);
        line3({ c.x - 0.5f, c.y, c.z - 0.5f }, { c.x + 0.5f, c.y, c.z + 0.5f }, col, 2.0f);
        line3({ c.x - 0.5f, c.y, c.z + 0.5f }, { c.x + 0.5f, c.y, c.z - 0.5f }, col, 2.0f);
    }

    // ---- 群衆の通路（現在地 → 角 → 目標）----
    if (const nav::NavCrowdAgent* ag = ai->GetAgent(e))
    {
        XMFLOAT3 prev{ ag->npos[0], ag->npos[1] + 0.1f, ag->npos[2] };
        for (size_t i = 0; i + 2 < ag->corners.size(); i += 3)
        {
            const XMFLOAT3 c{ ag->corners[i], ag->corners[i + 1] + 0.1f, ag->corners[i + 2] };
            line3(prev, c, IM_COL32(80, 220, 255, 200), 1.5f);
            prev = c;
        }
    }

    // ---- 今の行動と理由、得点の上位 3 つ（頭の上）----
    {
        ImVec2 head;
        const XMFLOAT3 hp{ t.position.x, st->eye[1] + 0.6f, t.position.z };
        if (project(hp, head))
        {
            const ai::Decision& d = st->last;
            std::string cur = (st->current >= 0 && st->current < static_cast<i32>(st->actions.size()))
                                  ? st->actions[static_cast<size_t>(st->current)].name : std::string("(なし)");
            std::snprintf(buf, sizeof(buf), "%s  [%s]  aw %.2f", cur.c_str(), ai::DecisionReasonName(d.reason),
                          st->bb.Number("target.awareness", 0.0));
            std::vector<const ai::ActionEval*> order;
            for (const auto& a : d.actions) order.push_back(&a);
            std::stable_sort(order.begin(), order.end(),
                             [](const ai::ActionEval* x, const ai::ActionEval* y) { return x->final > y->final; });
            const f32 lineH = ImGui::GetTextLineHeight();
            const int n = static_cast<int>((std::min)(order.size(), size_t(3)));
            const ImVec2 box0(head.x - 90, head.y - lineH * static_cast<f32>(n + 1) - 6);
            dl->AddRectFilled(box0, ImVec2(head.x + 150, head.y), IM_COL32(10, 10, 14, 170), 4.0f);
            dl->AddText(ImVec2(box0.x + 4, box0.y + 2), IM_COL32(255, 255, 255, 255), buf);
            for (int i = 0; i < n; ++i)
            {
                const ai::ActionEval* a = order[static_cast<size_t>(i)];
                std::snprintf(buf, sizeof(buf), "%-10s %.2f%s", a->name.c_str(), a->final,
                              a->cooldown ? " (cooldown)" : (a->bonus > 0.0f ? " (+粘り)" : ""));
                dl->AddText(ImVec2(box0.x + 4, box0.y + 2 + lineH * static_cast<f32>(i + 1)),
                            i == 0 ? IM_COL32(255, 230, 120, 255) : IM_COL32(200, 200, 200, 255), buf);
            }
        }
    }

    dl->PopClipRect();
}

} // namespace dx12e
