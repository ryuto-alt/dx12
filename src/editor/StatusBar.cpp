#include "editor/StatusBar.h"
#include "editor/EditorChromeLogic.h"
#include "editor/EditorContext.h"
#include "editor/EditorTheme.h"
#include "editor/ThemeGraphite.gen.h"   // GgLayout / GgChrome の寸法と色だけ読む
#include "editor/UiMotion.h"
#include "editor/UiWidgets.h"
#include "core/GameClock.h"
#include "core/Version.h"
#include "core/mcp/FleetGuard.h"        // QueryVideoMemoryMB
#include "ecs/Components.h"
#include "gui/UiClock.h"
#include "renderer/Camera.h"
#include "scene/Scene.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui_internal.h>
#pragma warning(pop)

#include <dxgi1_4.h>
#include <wrl/client.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dx12e::status
{
namespace th = dx12e::theme;

namespace
{
ID3D12Device* g_device = nullptr;

struct Probe
{
    double nextAt = -1e18;
    double vramUsedMB = -1.0, vramBudgetMB = -1.0;
    double memMB = -1.0;
    std::string gpuName;
    bool gpuTried = false;
};
Probe g_probe;

std::string WideToUtf8(const wchar_t* w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();
    return s;
}

// 1 秒に 1 回だけ OS に問い合わせる（毎フレーム DXGI を触らない）。
void RefreshProbe()
{
    const double now = ui::clock::Now();
    if (now < g_probe.nextAt) return;
    g_probe.nextAt = now + 1.0;
    if (g_device)
    {
        double u = -1.0, b = -1.0;
        if (fleet::QueryVideoMemoryMB(g_device, u, b)) { g_probe.vramUsedMB = u; g_probe.vramBudgetMB = b; }
        if (!g_probe.gpuTried)
        {
            g_probe.gpuTried = true;
            Microsoft::WRL::ComPtr<IDXGIFactory4> f;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
            if (SUCCEEDED(::CreateDXGIFactory1(IID_PPV_ARGS(&f))) &&
                SUCCEEDED(f->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&a))))
            {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d))) g_probe.gpuName = chrome::ShortGpuName(WideToUtf8(d.Description));
            }
        }
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        g_probe.memMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

// 右の帯の 1 項目（ラベル + 値 + 任意でミニゲージ）。
struct Seg
{
    const char* label = "";
    std::string value;
    int         cells = -1;       // ミニゲージの塗り数（-1 = ゲージ無し）
    int         level = 0;        // 0 通常 / 1 警告 / 2 危険
    int         priority = 0;     // 小さいほど先に落とす
    bool        button = false;   // カメラ速度のボタン
    float       w = 0.0f;
};

constexpr int kGaugeCells = 8;
} // namespace

void SetDevice(ID3D12Device* device) { if (device) g_device = device; }

float GraphiteHeightPx()
{
    const th::Spec& sp = th::CurrentSpec();
    return ui::Px(static_cast<float>(sp.gg ? sp.gg->layout.statusBarH : 22));
}

void CameraQuickPopupBody(Camera* camera, entt::registry& reg)
{
    ImGui::TextDisabled("カメラ");
    f32 speed = camera->GetMoveSpeed();
    ImGui::SetNextItemWidth(ui::Px(200.0f));
    // 対数目盛り: 遅い側(1〜10)を細かく、速い側(〜200)をざっくり動かせる
    if (ui::SliderFloat("移動速度", &speed, 0.2f, 200.0f, "%.1f m/s",
                        ImGuiSliderFlags_Logarithmic))
        camera->SetMoveSpeed(speed);

    struct Preset { const char* label; f32 speed; };
    static const Preset presets[] = {
        {"精密 1",  1.0f}, {"標準 5",  5.0f}, {"広域 20", 20.0f}, {"最速 80", 80.0f}};
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(presets[i].label)) camera->SetMoveSpeed(presets[i].speed);
    }

    f32 sens = camera->GetMouseSensitivity();
    ImGui::SetNextItemWidth(ui::Px(200.0f));
    if (ui::SliderFloat("マウス感度", &sens, 0.0005f, 0.02f, "%.4f"))
        camera->SetMouseSensitivity(sens);

    // グリッド床の表示/非表示。UI やライティングを見るとき邪魔になるので手元に置く。
    ImGui::Separator();
    for (auto [e, gp] : reg.view<GridPlane>().each())
    {
        ui::Checkbox("グリッドを表示", &gp.enabled);
        break;   // グリッドは1枚だけ
    }
}

void RenderGraphite(const Input& in)
{
    const th::Spec& sp = th::CurrentSpec();
    if (!sp.gg || !in.ctx || !in.scene || !in.camera || !in.clock) return;
    const th::gg::GgSpec& gg = *sp.gg;
    EditorContext& ctx = *in.ctx;
    auto& reg = in.scene->GetRegistry();
    const bool det = ui::clock::Deterministic();
    if (!det) RefreshProbe();

    const float H = GraphiteHeightPx();
    const ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mainVp->Pos.x, mainVp->Pos.y + mainVp->Size.y - H), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mainVp->Size.x, H), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainVp->ID);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, th::Bg0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(12.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ui::Px(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));   // 既定の最小サイズ（32）が 22px の帯を太らせない
    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav       | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float ww = ImGui::GetWindowSize().x;
    const float lineH = ImGui::GetTextLineHeight();
    const float textY = std::floor(wp.y + (H - lineH) * 0.5f);
    const ImU32 cText = ImGui::GetColorU32(th::Text), cDim = ImGui::GetColorU32(th::TextDim), cFaint = ImGui::GetColorU32(th::TextFaint);
    const float grooveH = ui::Px(12.0f);
    auto groove = [&](float gx)
    {
        const float y0 = std::floor(wp.y + (H - grooveH) * 0.5f);
        dl->AddRectFilled(ImVec2(std::floor(gx), y0), ImVec2(std::floor(gx) + th::HairPx(), y0 + grooveH), ImGui::GetColorU32(th::Border));
    };

    // ---- 左 ----
    const chrome::RunState rs = !in.isPlaying ? chrome::RunState::Edit
                              : (ctx.paused ? chrome::RunState::Paused : chrome::RunState::Play);
    float x = wp.x + ui::Px(12.0f);
    if (rs != chrome::RunState::Edit)
    {
        // 予算 09: 状態表示の左 2px（一時停止は破線 4/2）。再生中だけ出る＝編集中の黄には数えない。
        const float y0 = wp.y + ui::Px(4.0f), y1 = wp.y + H - ui::Px(4.0f);
        const ImU32 yc = ImGui::GetColorU32(th::Accent);
        if (rs == chrome::RunState::Play)
            ui::deco::NeedleBar(dl, ImVec2(x, y0), ImVec2(x + th::AccentPx(), y1), yc);
        else
        {
            const float dash = ui::Px(4.0f), gap = ui::Px(2.0f);
            for (float y = y0; y < y1; y += dash + gap)
                ui::deco::NeedleBar(dl, ImVec2(x, y), ImVec2(x + th::AccentPx(), (std::min)(y + dash, y1)), yc);
        }
        x += th::AccentPx() + ui::Px(6.0f);
    }
    const char* stateText = chrome::StateLabel(rs);
    dl->AddText(ImVec2(x, textY), rs == chrome::RunState::Edit ? cDim : cText, stateText);
    x += ImGui::CalcTextSize(stateText).x + ui::Px(10.0f);
    groove(x);
    x += ui::Px(10.0f);

    // 状態の次: 主ジョブの細型進捗（黄の指針）／「準備完了」
    const bool job = ctx.buildRunning || ctx.sceneLoadProgress >= 0.0f;
    if (job)
    {
        const float t = ctx.buildRunning ? ctx.buildFraction : ctx.sceneLoadProgress;
        const char* what = ctx.buildRunning ? "ビルド中" : "読み込み中";
        dl->AddText(ImVec2(x, textY), cText, what);
        x += ImGui::CalcTextSize(what).x + ui::Px(8.0f);
        const float barH = ui::Px(static_cast<float>(gg.layout.progressNeedleH - 2));
        ImGui::SetCursorScreenPos(ImVec2(x, std::floor(wp.y + (H - barH) * 0.5f)));
        ui::ProgressBar("##statusJob", t, ImVec2(ui::Px(120.0f), 0.0f), ui::ProgressState::Running, ui::ProgressRole::StatusJob);
        x += ui::Px(120.0f) + ui::Px(8.0f);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%3.0f%%", std::clamp(t, 0.0f, 1.0f) * 100.0f);
        ui::PushMono();
        dl->AddText(ImVec2(x, textY), cDim, pct);
        x += ImGui::CalcTextSize(pct).x + ui::Px(10.0f);
        ui::PopMono();
    }
    else
    {
        dl->AddText(ImVec2(x, textY), cFaint, "準備完了");
        x += ImGui::CalcTextSize("準備完了").x + ui::Px(10.0f);
    }
    const float leftEnd = x;

    // ---- 右（右端から詰める。収まらない項目は優先度の低い順に落とす）----
    size_t objCount = 0;
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        (void)tag;
        if (!reg.all_of<GridPlane>(e)) ++objCount;
    }
    std::vector<Seg> segs;
    char buf[64];
    auto fmt = [&](const char* f, double v) { std::snprintf(buf, sizeof(buf), f, v); return std::string(buf); };
    const float fps = in.clock->GetFPS();
    {
        Seg s; s.label = "FPS"; s.priority = 8;
        s.value = det ? "—" : fmt("%.0f", fps);
        s.cells = det ? 0 : chrome::GaugeCells(fps, 144.0f, kGaugeCells);
        segs.push_back(s);
    }
    {
        const float ms = fps > 0.0f ? 1000.0f / fps : 0.0f;
        Seg s; s.label = "ms"; s.priority = 5;
        s.value = det ? "—" : fmt("%.1f", ms);
        s.cells = det ? 0 : chrome::GaugeCells(ms, 33.3f, kGaugeCells);
        s.level = det ? 0 : (ms > 33.3f ? 2 : (ms > 16.7f ? 1 : 0));
        segs.push_back(s);
    }
    {
        Seg s; s.label = "VRAM"; s.priority = 6;
        const float r = det ? -1.0f : chrome::VramRatio(g_probe.vramUsedMB, g_probe.vramBudgetMB);
        s.value = r < 0.0f ? "—" : fmt("%.1f GB", g_probe.vramUsedMB / 1024.0);
        s.cells = r < 0.0f ? 0 : chrome::GaugeCells(r, 1.0f, kGaugeCells);
        s.level = r < 0.0f ? 0 : chrome::VramLevel(r);
        segs.push_back(s);
    }
    {
        Seg s; s.label = "MEM"; s.priority = 3;
        s.value = (det || g_probe.memMB < 0.0) ? "—" : fmt("%.1f GB", g_probe.memMB / 1024.0);
        segs.push_back(s);
    }
    { Seg s; s.label = "obj"; s.priority = 4; s.value = fmt("%.0f", static_cast<double>(objCount)); segs.push_back(s); }
    { Seg s; s.label = "draws"; s.priority = 2; s.value = det ? "—" : fmt("%.0f", static_cast<double>(ctx.statDraws)); segs.push_back(s); }
    { Seg s; s.label = "culled"; s.priority = 1; s.value = det ? "—" : fmt("%.0f", static_cast<double>(ctx.statCulled)); segs.push_back(s); }
    {
        Seg s; s.label = "カメラ速度"; s.priority = 9; s.button = true;
        s.value = fmt("%.1f", in.camera->GetMoveSpeed());
        segs.push_back(s);
    }
    {
        Seg s; s.label = "DX12"; s.priority = 3;
        s.value = (det || g_probe.gpuName.empty()) ? std::string() : g_probe.gpuName;
        segs.push_back(s);
    }
    { Seg s; s.label = ""; s.priority = 7; s.value = chrome::DrawingNumber(kEngineVersion); segs.push_back(s); }

    const float cellW = ui::Px(5.0f), cellH = ui::Px(6.0f);
    const float gapIn = ui::Px(6.0f), gapSeg = ui::Px(20.0f);
    ui::PushMono();
    for (Seg& s : segs)
    {
        float w = 0.0f;
        if (*s.label) w += ImGui::CalcTextSize(s.label).x + (s.value.empty() ? 0.0f : gapIn);
        w += ImGui::CalcTextSize(s.value.c_str()).x;
        if (s.cells >= 0) w += gapIn + cellW * static_cast<float>(kGaugeCells);
        if (s.button) w += ImGui::GetStyle().FramePadding.x * 2.0f;
        s.w = w;
    }
    ui::PopMono();
    const float rightEdge = wp.x + ww - ui::Px(12.0f);
    const float minLeft = leftEnd + ui::Px(180.0f);   // 選択名に最低限残す幅
    auto totalW = [&](const std::vector<bool>& flags)
    {
        float t = 0.0f; int n = 0;
        for (size_t i = 0; i < segs.size(); ++i) if (flags[i]) { t += segs[i].w; ++n; }
        return t + (n > 1 ? gapSeg * static_cast<float>(n - 1) : 0.0f);
    };
    std::vector<bool> on(segs.size(), true);
    while (rightEdge - totalW(on) < minLeft)
    {
        int drop = -1;
        for (size_t i = 0; i < segs.size(); ++i)
            if (on[i] && (drop < 0 || segs[i].priority < segs[static_cast<size_t>(drop)].priority)) drop = static_cast<int>(i);
        if (drop < 0) break;
        on[static_cast<size_t>(drop)] = false;
    }
    float rx = rightEdge - totalW(on);
    const float rightStart = rx;
    bool first = true;
    for (size_t i = 0; i < segs.size(); ++i)
    {
        if (!on[i]) continue;
        const Seg& s = segs[i];
        if (!first) groove(rx - gapSeg * 0.5f);
        first = false;
        float cx = rx;
        ui::PushMono();
        if (s.button)
        {
            ImGui::SetCursorScreenPos(ImVec2(cx, std::floor(wp.y + (H - (lineH + ui::Px(2.0f))) * 0.5f)));
            char lbl[96];
            std::snprintf(lbl, sizeof(lbl), "%s %s##camQuick", s.label, s.value.c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4());
            ImGui::PushStyleColor(ImGuiCol_Text, th::TextDim);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4());
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ui::Px(8.0f, 1.0f));
            const bool clicked = ImGui::Button(lbl);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (clicked) ImGui::OpenPopup("##CamQuickSettings");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("カメラの移動速度（右ドラッグ中にホイールでも変えられます）");
        }
        else
        {
            if (*s.label)
            {
                dl->AddText(ImVec2(cx, textY), cFaint, s.label);
                cx += ImGui::CalcTextSize(s.label).x + (s.value.empty() ? 0.0f : gapIn);
            }
            const ImVec4& vc = s.level == 2 ? th::BadText : (s.level == 1 ? th::WarnText : th::Text);
            dl->AddText(ImVec2(cx, textY), s.level ? ImGui::GetColorU32(vc) : cDim, s.value.c_str());
            cx += ImGui::CalcTextSize(s.value.c_str()).x;
            if (s.cells >= 0)
            {
                cx += gapIn;
                const float by = std::floor(wp.y + (H - cellH) * 0.5f);
                const ImU32 off = ImGui::GetColorU32(gg.chrome.tickMajor);
                const ImU32 fill = ImGui::GetColorU32(s.level == 2 ? th::BadText : (s.level == 1 ? th::WarnText : th::Text));
                for (int c = 0; c < kGaugeCells; ++c)
                {
                    const float cx0 = std::floor(cx + cellW * static_cast<float>(c));
                    dl->AddRectFilled(ImVec2(cx0, by), ImVec2(cx0 + (std::max)(1.0f, cellW - th::HairPx() - 1.0f), by + cellH), c < s.cells ? fill : off);
                }
            }
        }
        ui::PopMono();
        rx += s.w + gapSeg;
    }

    // ---- 左の続き: 選択名（右の帯との間に収まる分だけ）----
    {
        const float maxX = rightStart - ui::Px(12.0f);
        if (maxX > x + ui::Px(24.0f))
        {
            dl->PushClipRect(ImVec2(x, wp.y), ImVec2(maxX, wp.y + H), true);
            if (ctx.HasSelection() && reg.valid(ctx.selectedEntity) && reg.all_of<NameTag>(ctx.selectedEntity))
            {
                std::string name = reg.get<NameTag>(ctx.selectedEntity).name;
                if (ctx.selectedEntities.size() > 1) name += "  +" + std::to_string(ctx.selectedEntities.size() - 1) + " 件";
                dl->AddText(ImVec2(x, textY), cDim, name.c_str());
            }
            else
                dl->AddText(ImVec2(x, textY), cFaint, "選択なし");
            dl->PopClipRect();
        }
    }

    if (ImGui::BeginPopup("##CamQuickSettings"))
    {
        CameraQuickPopupBody(in.camera, reg);
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor();
}

} // namespace dx12e::status
