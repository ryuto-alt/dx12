#include "editor/WorkspaceManager.h"
#include "editor/EditorContext.h"
#include "editor/EditorPrefs.h"
#include "editor/EditorTheme.h"
#include "editor/UiWidgets.h"
#include "editor/BottomDock.h"
#include "editor/Toast.h"
#include "core/VirtualGuard.h"   // vinput::Enabled() / guard::TestRunActive()
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dx12e
{

namespace
{
// コア 4 窓の名前（EditorLayer / 各パネルの ImGui::Begin と同じ）
constexpr const char* kWinHierarchy = "ヒエラルキー";
constexpr const char* kWinInspector = "インスペクター";
constexpr const char* kWinAssets    = "アセットブラウザ";
constexpr const char* kWinConsole   = "コンソール";

constexpr const char* kPrefLayout    = "layout.current";
constexpr const char* kPrefWorkspace = "workspace.state";

ImGuiDockNode* NodeOf(ImGuiID id) { return id ? ImGui::DockBuilderGetNode(id) : nullptr; }

// a と b の 2 つの子ノードの分割比を ratioA にする（SizeRef を書く。ImGui は次の DockSpace() で SizeRef の比で配分する）。
void SetPair(ImGuiDockNode* a, ImGuiDockNode* b, ImGuiAxis axis, float ratioA)
{
    if (!a || !b) return;
    const float total = a->Size[axis] + b->Size[axis];
    if (!(total > 1.0f)) return;
    a->SizeRef[axis] = total * ratioA;
    b->SizeRef[axis] = total * (1.0f - ratioA);
}

bool PairRatio(ImGuiDockNode* a, ImGuiDockNode* b, ImGuiAxis axis, float& out)
{
    if (!a || !b) return false;
    const float total = a->Size[axis] + b->Size[axis];
    if (!(total > 1.0f)) return false;
    out = a->Size[axis] / total;
    return true;
}

// ★下部ドックの比だけは「メインビューポートの高さ」を基準にする（ImGui の癖の再現。既定の見た目を変えないため）。
//   BuildLayout は DockBuilder にメインビューポート全高を渡して分割する一方、実際の DockSpace はツールバーとステータスバーの分だけ低い。
//   中央ノード（ビューポート）は差分を吸収し、下部ドックは分割時のピクセル高（全高 × 比）のまま残る。
//   つまり「下部ドック / DockSpace の高さ」は 0.33 ではなく約 0.363 になる（1a までの既定の 3D 矩形はこの結果）。
//   生きているノードへ比を書き戻す時も同じ基準（全高 × 比 = ピクセル）で SizeRef を書けば、既定は再構築と同じ矩形へ戻る。
float SepThickness() { return ImGui::GetStyle().DockingSeparatorSize; }
float VRefHeight() { return (std::max)(ImGui::GetMainViewport()->Size.y - SepThickness(), 1.0f); }

ws::Slot DefaultSlot(const tools::Desc& d)
{
    return d.slot == tools::DockSlot::RightTab ? ws::Slot::RightTab : ws::Slot::Floating;
}

// 整列の対象にするフローティング窓か（ツール窓・パネルの独立窓。ポップアップ / 帯 / ギズモ / 自分の管理窓は除く）
bool IsArrangeCandidate(const ImGuiWindow* w)
{
    // ★このフレームで Begin される前でも「前のフレームで出ていた」窓を拾うため WasActive も見る
    //   （Application 直属のツール窓は EditorLayer::Render のあとに描かれるので、呼ばれた時点では Active がまだ立っていない）。
    if (!w || !(w->Active || w->WasActive) || w->Hidden) return false;
    if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip |
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove)) return false;
    if (w->DockNode != nullptr || w->DockIsActive) return false;
    if (w->Name[0] == '#' || std::strncmp(w->Name, "Debug", 5) == 0 || std::strcmp(w->Name, "Camera Preview") == 0) return false;
    if (std::strstr(w->Name, "###LayoutSave") || std::strstr(w->Name, "###ToolSlots")) return false;
    return true;
}
} // namespace

bool WorkspaceManager::PersistAllowed()
{
    return !vinput::Enabled() && !guard::TestRunActive().load();
}

// ---------------------------------------------------------------- 起動

void WorkspaceManager::Initialize(EditorContext& ctx)
{
    m_persist = PersistAllowed();
    prefs::SetWriteEnabled(m_persist);
    RefreshNames(ctx);

    ws::WorkspaceState st;
    if (ws::StateFromJson(prefs::GetString(kPrefWorkspace), st)) m_state = st;
    ctx.currentWorkspace = m_state.current;

    if (m_persist)
    {
        ws::Layout l;
        if (ws::FromJson(prefs::GetString(kPrefLayout), l))
        {
            ApplyLayout(ctx, l, /*arrange=*/false);
            m_lastPersisted = Snapshot(ctx);
            m_hasPersisted = true;
        }
    }
}

// ---------------------------------------------------------------- 状態

ws::Slot WorkspaceManager::SlotOf(const tools::Desc& d) const
{
    const ws::Slot def = DefaultSlot(d);
    if (!IsMovable(d)) return def;
    return ws::SlotFor(m_layout, d.id, def);
}

bool WorkspaceManager::IsMovable(const tools::Desc& d)
{
    return d.slot == tools::DockSlot::RightTab && d.imguiName && d.imguiName[0];
}

ws::Layout WorkspaceManager::Snapshot(const EditorContext& ctx) const
{
    ws::Layout l = m_layout;
    l.open.clear();
    for (const tools::Desc& d : tools::kAll)
        if (ctx.*(d.flag) && std::strcmp(d.id, "diagnostics") != 0) l.open.push_back(d.id);
    l.bottomTabs.clear();
    for (const bottomdock::Tab& t : bottomdock::Tabs())
        if (t.open) l.bottomTabs.push_back(t.id);
    return l;
}

bool WorkspaceManager::Measure(ws::Ratios& out) const
{
    ws::Ratios r;
    if (!PairRatio(NodeOf(m_nodeLeft), NodeOf(m_nodeRemaining), ImGuiAxis_X, r.left)) return false;
    if (!PairRatio(NodeOf(m_nodeRightCol), NodeOf(m_nodeCenter), ImGuiAxis_X, r.right)) return false;
    // 下部ドック: 全高基準（上の VRefHeight の注記）
    if (ImGuiDockNode* bn = NodeOf(m_nodeBottom)) r.bottom = bn->Size.y / VRefHeight();
    else return false;
    r.rightSplit = m_layout.ratios.rightSplit;
    if (m_nodeRightSplit)
        if (!PairRatio(NodeOf(m_nodeRightSplit), NodeOf(m_nodeRightTop), ImGuiAxis_Y, r.rightSplit)) return false;
    out = r;
    return true;
}

// ---------------------------------------------------------------- ドックの構築

void WorkspaceManager::BuildLayout(ImGuiID dockspaceId)
{
    m_dockspaceId = dockspaceId;
    const ws::Ratios r = ws::Clamp(m_layout.ratios);

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // 左(r.left): ヒエラルキー | 残り
    ImGuiID dockLeft = 0, dockRemaining = 0;
    ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, r.left, &dockLeft, &dockRemaining);
    // 残り → 右(r.right): 右カラム | センター
    ImGuiID dockRightCol = 0, dockCenter = 0;
    ImGui::DockBuilderSplitNode(dockRemaining, ImGuiDir_Right, r.right, &dockRightCol, &dockCenter);
    // センター → 下(r.bottom): 下部ドック | ビューポート(中央)
    ImGuiID dockBottom = 0, dockViewport = 0;
    ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, r.bottom, &dockBottom, &dockViewport);

    // 右カラムを縦に割るのは「右分割」に置く窓があるときだけ（空のノードを作らない）
    bool wantSplit = false;
    for (const tools::Desc& t : tools::kAll)
        if (IsMovable(t) && SlotOf(t) == ws::Slot::RightSplit) { wantSplit = true; break; }
    ImGuiID dockRightTop = dockRightCol, dockRightSplit = 0;
    if (wantSplit)
        ImGui::DockBuilderSplitNode(dockRightCol, ImGuiDir_Down, r.rightSplit, &dockRightSplit, &dockRightTop);

    // 左: ヒエラルキー
    ImGui::DockBuilderDockWindow(kWinHierarchy, dockLeft);

    // 中央下: 登録タブ(BottomDock.h) → 下部に置く指定のツール窓 → コンソール → アセットブラウザ（最後にドックした窓が既定の選択タブ）。
    // レイアウトが「最後に選んでいた登録タブ」を持っていれば、それを最後にドックして選択状態にする。
    const bottomdock::Tab* active = m_layout.bottomActive.empty() ? nullptr : bottomdock::Find(m_layout.bottomActive);
    if (active && !active->open) active = nullptr;
    m_bottomDocked.clear();
    for (const bottomdock::Tab& t : bottomdock::Tabs())
    {
        if (&t == active) continue;
        ImGui::DockBuilderDockWindow(bottomdock::WindowName(t).c_str(), dockBottom);
        m_bottomDocked.insert(t.id);
    }
    for (const tools::Desc& t : tools::kAll)
        if (IsMovable(t) && SlotOf(t) == ws::Slot::BottomTab)
            ImGui::DockBuilderDockWindow(t.imguiName, dockBottom);
    ImGui::DockBuilderDockWindow(kWinConsole, dockBottom);
    ImGui::DockBuilderDockWindow(kWinAssets, dockBottom);
    if (active)
    {
        ImGui::DockBuilderDockWindow(bottomdock::WindowName(*active).c_str(), dockBottom);
        m_bottomDocked.insert(active->id);
    }

    // 右: ツール窓（右タブ / 右分割）→ 最後にインスペクター（初期表示）
    for (const tools::Desc& t : tools::kAll)
    {
        if (!IsMovable(t)) continue;
        const ws::Slot s = SlotOf(t);
        if (s == ws::Slot::RightTab)        ImGui::DockBuilderDockWindow(t.imguiName, dockRightTop);
        else if (s == ws::Slot::RightSplit) ImGui::DockBuilderDockWindow(t.imguiName, dockRightSplit);
    }
    ImGui::DockBuilderDockWindow(kWinInspector, dockRightTop);

    ImGui::DockBuilderFinish(dockspaceId);

    m_nodeLeft = dockLeft; m_nodeRemaining = dockRemaining; m_nodeRightCol = dockRightCol; m_nodeCenter = dockCenter;
    m_nodeBottom = dockBottom; m_nodeViewport = dockViewport; m_nodeRightTop = dockRightTop; m_nodeRightSplit = dockRightSplit;
    m_baselineValid = false;
    m_baselineWait = 3;
    m_bottomMax = false;
}

void WorkspaceManager::ApplyRatiosLive(const ws::Ratios& in)
{
    const ws::Ratios r = ws::Clamp(in);
    SetPair(NodeOf(m_nodeLeft), NodeOf(m_nodeRemaining), ImGuiAxis_X, r.left);
    SetPair(NodeOf(m_nodeRightCol), NodeOf(m_nodeCenter), ImGuiAxis_X, r.right);
    SetBottomPixels(r.bottom * VRefHeight());
    if (m_nodeRightSplit) SetPair(NodeOf(m_nodeRightSplit), NodeOf(m_nodeRightTop), ImGuiAxis_Y, r.rightSplit);
}

// 下部ドックの高さ(px)を直接書く。中央ノード（ビューポート）は残りを受け取る。
void WorkspaceManager::SetBottomPixels(float px)
{
    ImGuiDockNode* bn = NodeOf(m_nodeBottom);
    ImGuiDockNode* vn = NodeOf(m_nodeViewport);
    ImGuiDockNode* cn = NodeOf(m_nodeCenter);
    if (!bn || !vn || !cn) return;
    px = (std::max)(px, 1.0f);
    bn->SizeRef.y = px;
    vn->SizeRef.y = (std::max)(cn->Size.y - px - SepThickness(), 1.0f);
}

void WorkspaceManager::DockWindowTo(const char* windowName, ws::Slot slot)
{
    if (!windowName || !windowName[0]) return;
    ImGuiID node = 0;
    if (slot == ws::Slot::RightTab)        node = m_nodeRightTop;
    else if (slot == ws::Slot::BottomTab)  node = m_nodeBottom;
    else if (slot == ws::Slot::RightSplit) node = m_nodeRightSplit;
    if (node == 0) { m_needRebuild = true; return; }   // 右分割ノードが無い / フローティング: 作り直しが要る
    ImGui::DockBuilderDockWindow(windowName, node);
}

// ---------------------------------------------------------------- 適用 / 切替

void WorkspaceManager::ApplyLayout(EditorContext& ctx, const ws::Layout& in, bool arrange)
{
    const ws::Layout l = ws::Sanitize(in, [](const std::string& id) { return tools::Find(id.c_str()) != nullptr; });
    std::vector<std::string> tabs;
    for (const std::string& id : l.bottomTabs)
        if (bottomdock::Find(id)) tabs.push_back(id);

    if (m_bottomMax) SetMaximized(ctx, false);

    if (arrange)
    {
        m_arrangeBefore.clear();
        m_arrangeDone.clear();
        if (ImGuiContext* g = ImGui::GetCurrentContext())
            for (ImGuiWindow* w : g->Windows)
                if (IsArrangeCandidate(w)) m_arrangeBefore.insert(w->ID);
        m_arrangeFrames = 10;
    }

    // 開閉（診断窓は触らない＝テスト実行中の診断パネルを閉じない）
    for (const tools::Desc& d : tools::kAll)
        if (std::strcmp(d.id, "diagnostics") != 0)
            ctx.*(d.flag) = ws::Contains(l.open, d.id);
    for (bottomdock::Tab& t : bottomdock::Tabs())
    {
        const bool on = ws::Contains(tabs, t.id);
        if (t.open && !on) m_bottomDocked.erase(t.id);
        t.open = on;
    }

    // 配置先と分割比
    const ws::Layout before = m_layout;
    m_layout.slots = l.slots;
    m_layout.ratios = l.ratios;
    m_layout.bottomTabs = tabs;
    m_layout.bottomActive = l.bottomActive;

    if (m_nodeLeft != 0)   // ドック構築済み: 既存ノードへ反映（右分割 / フローティングに関わる変更だけ作り直す）
    {
        for (const tools::Desc& d : tools::kAll)
        {
            if (!IsMovable(d)) continue;
            const ws::Slot was = ws::SlotFor(before, d.id, ws::Slot::RightTab);
            const ws::Slot now = SlotOf(d);
            if (was != now) DockWindowTo(d.imguiName, now);
        }
        if (!m_needRebuild)
        {
            ApplyRatiosLive(m_layout.ratios);
            m_baselineWait = 2;
        }
    }

    if (!l.bottomActive.empty() && bottomdock::Find(l.bottomActive) && bottomdock::IsOpen(l.bottomActive))
    {
        m_focusBottomId = l.bottomActive;
        m_focusBottomFrames = 6;
    }
}

void WorkspaceManager::SwitchWorkspace(EditorContext& ctx, const std::string& id)
{
    ws::Layout out;
    if (!ws::SwitchTo(m_state, id, Snapshot(ctx), out)) return;
    ctx.currentWorkspace = m_state.current;
    ApplyLayout(ctx, out, /*arrange=*/true);
    prefs::SetString(kPrefWorkspace, ws::StateToJson(m_state));
}

// ---------------------------------------------------------------- 名前つきレイアウト

void WorkspaceManager::RefreshNames(EditorContext& ctx)
{
    ctx.savedLayoutNames.clear();
    const std::string prefix = ws::NamedLayoutPrefix();
    for (const std::string& k : prefs::KeysWithPrefix(prefix)) ctx.savedLayoutNames.push_back(k.substr(prefix.size()));
    std::sort(ctx.savedLayoutNames.begin(), ctx.savedLayoutNames.end());
}

void WorkspaceManager::SaveNamed(EditorContext& ctx, const std::string& rawName)
{
    const std::string name = ws::NormalizeLayoutName(rawName);
    if (name.empty())
    {
        ctx.Notify(ui::ToastKind::Warn, "レイアウト名が空か長すぎます（60 バイト以内）");
        return;
    }
    prefs::SetString((ws::NamedLayoutPrefix() + name).c_str(), ws::ToJson(Snapshot(ctx)));
    RefreshNames(ctx);
    ctx.Notify(ui::ToastKind::Success, "レイアウト「" + name + "」を保存しました");
}

void WorkspaceManager::RestoreNamed(EditorContext& ctx, const std::string& name)
{
    ws::Layout l;
    if (!ws::FromJson(prefs::GetString((ws::NamedLayoutPrefix() + name).c_str()), l))
    {
        // 壊れた / 古いレイアウトは黙って既定へは戻さず、知らせて何も変えない（既存の配置を失わない）
        ctx.Notify(ui::ToastKind::Warn, "レイアウト「" + name + "」を読み込めませんでした");
        return;
    }
    ApplyLayout(ctx, l, /*arrange=*/false);
    ctx.Notify(ui::ToastKind::Info, "レイアウト「" + name + "」を復元しました");
}

void WorkspaceManager::DeleteNamed(EditorContext& ctx, const std::string& name)
{
    prefs::Erase((ws::NamedLayoutPrefix() + name).c_str());
    RefreshNames(ctx);
    ctx.Notify(ui::ToastKind::Info, "レイアウト「" + name + "」を削除しました");
}

void WorkspaceManager::SetToolSlot(EditorContext& ctx, const tools::Desc& d, ws::Slot slot)
{
    (void)ctx;
    if (!IsMovable(d)) return;
    const ws::Slot was = SlotOf(d);
    ws::SetSlot(m_layout, d.id, slot, ws::Slot::RightTab);
    if (was != slot && m_nodeLeft != 0) DockWindowTo(d.imguiName, slot);
}

// ---------------------------------------------------------------- 最大化

void WorkspaceManager::SetMaximized(EditorContext& ctx, bool on)
{
    if (on == m_bottomMax) return;
    if (on)
    {
        ImGuiDockNode* c = NodeOf(m_nodeCenter);
        if (!c) return;
        SetBottomPixels(ws::MaximizedBottomRatio(c->Size.y, m_minViewportPx) * c->Size.y);
    }
    else
    {
        SetBottomPixels(ws::Clamp(m_layout.ratios).bottom * VRefHeight());
    }
    m_bottomMax = on;
    ctx.bottomDockMaximized = on;
    m_baselineWait = 2;   // 最大化の出入りはユーザーのスプリッタ操作として吸い上げない
}

// ---------------------------------------------------------------- フレーム

bool WorkspaceManager::BeginFrame(EditorContext& ctx)
{
    m_persist = PersistAllowed();
    prefs::SetWriteEnabled(m_persist);
    bool rebuild = false;

    if (ctx.resetLayout)
    {
        // 全部閉じて既定へ（ツール窓 = レジストリの全窓、下部の登録タブ、配置先、分割比、ワークスペース）
        ctx.resetLayout = false;
        if (m_bottomMax) { m_bottomMax = false; ctx.bottomDockMaximized = false; }
        tools::CloseAll(ctx);
        for (bottomdock::Tab& t : bottomdock::Tabs()) t.open = false;
        m_layout = ws::Layout{};
        m_state = ws::WorkspaceState{};
        ctx.currentWorkspace = m_state.current;
        prefs::SetString(kPrefWorkspace, ws::StateToJson(m_state));
        rebuild = true;
    }
    if (!ctx.pendingWorkspace.empty())
    {
        const std::string id = std::move(ctx.pendingWorkspace);
        ctx.pendingWorkspace.clear();
        SwitchWorkspace(ctx, id);
    }
    if (!ctx.pendingLayoutRestore.empty())
    {
        const std::string n = std::move(ctx.pendingLayoutRestore);
        ctx.pendingLayoutRestore.clear();
        RestoreNamed(ctx, n);
    }
    if (!ctx.pendingLayoutDelete.empty())
    {
        const std::string n = std::move(ctx.pendingLayoutDelete);
        ctx.pendingLayoutDelete.clear();
        DeleteNamed(ctx, n);
    }
    if (!ctx.pendingLayoutSaveName.empty())
    {
        const std::string n = std::move(ctx.pendingLayoutSaveName);
        ctx.pendingLayoutSaveName.clear();
        SaveNamed(ctx, n);
    }
    for (const auto& req : ctx.pendingToolSlots)
        if (const tools::Desc* d = tools::Find(req.first.c_str()))
            if (req.second >= 0 && req.second < ws::kSlotCount) SetToolSlot(ctx, *d, static_cast<ws::Slot>(req.second));
    ctx.pendingToolSlots.clear();
    if (ctx.bottomDockMaximizeToggle)
    {
        ctx.bottomDockMaximizeToggle = false;
        SetMaximized(ctx, !m_bottomMax);
    }
    if (ctx.layoutSaveWindowRequest)
    {
        ctx.layoutSaveWindowRequest = false;
        m_saveWindowOpen = true;
        std::memset(m_saveNameBuf, 0, sizeof(m_saveNameBuf));
    }
    if (ctx.layoutSlotsWindowRequest)
    {
        ctx.layoutSlotsWindowRequest = false;
        m_slotsWindowOpen = true;
    }
    if (m_needRebuild)
    {
        m_needRebuild = false;
        if (m_bottomMax) { m_bottomMax = false; ctx.bottomDockMaximized = false; }
        rebuild = true;
    }
    return rebuild;
}

void WorkspaceManager::RenderBottomTabs(EditorContext& ctx)
{
    for (bottomdock::Tab& t : bottomdock::Tabs())
    {
        if (!t.open) continue;
        const std::string name = bottomdock::WindowName(t);
        if (m_nodeBottom != 0 && m_bottomDocked.insert(t.id).second)
            ImGui::DockBuilderDockWindow(name.c_str(), m_nodeBottom);   // 初めて開いた（または開き直した）: 下部ドックへ
        bool open = true;
        if (ImGui::Begin(name.c_str(), &open))
        {
            if (m_focusBottomFrames > 0 && m_focusBottomId == t.id) ImGui::SetWindowFocus();
            if (t.draw) t.draw(ctx);
        }
        ImGui::End();
        if (!open)
        {
            t.open = false;
            m_bottomDocked.erase(t.id);
        }
    }
    if (m_focusBottomFrames > 0 && --m_focusBottomFrames == 0) m_focusBottomId.clear();
}

void WorkspaceManager::ArrangeNewFloating(float ax, float ay, float aw, float ah)
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || m_arrangeFrames <= 0) return;
    --m_arrangeFrames;
    int index = static_cast<int>(m_arrangeDone.size());
    for (ImGuiWindow* w : g->Windows)
    {
        if (!IsArrangeCandidate(w)) continue;
        if (m_arrangeBefore.count(w->ID) || m_arrangeDone.count(w->ID)) continue;
        if (w->Size.x < 8.0f || w->Size.y < 8.0f) continue;   // まだ大きさが決まっていない（次のフレームで置く）
        const ws::Pt p = ws::CascadePos(index++, ax, ay, aw, ah, w->Size.x, w->Size.y, ui::Px(30.0f));
        ImGui::SetWindowPos(w, ImVec2(p.x, p.y), ImGuiCond_Always);
        m_arrangeDone.insert(w->ID);
        Logger::Info("ワークスペース: 窓を整列 '{}' -> ({:.0f},{:.0f}) 大きさ ({:.0f},{:.0f})", w->Name, p.x, p.y, w->Size.x, w->Size.y);
    }
}

void WorkspaceManager::EndDockFrame(EditorContext& ctx, float areaX, float areaY, float areaW, float areaH, float minViewportPx)
{
    m_minViewportPx = minViewportPx;
    if (m_nodeLeft == 0) return;

    // 読み取り用の値（UI 自動テスト / メニュー）
    ctx.dockRatioLeft = m_layout.ratios.left;
    ctx.dockRatioRight = m_layout.ratios.right;
    ctx.dockRatioBottom = m_layout.ratios.bottom;
    ctx.dockRatioRightSplit = m_layout.ratios.rightSplit;
    ctx.dockNodeBottom = m_nodeBottom;
    ctx.dockNodeRightSplit = m_nodeRightSplit;
    {
        ws::Ratios meas;
        if (Measure(meas)) { ctx.dockMeasLeft = meas.left; ctx.dockMeasRight = meas.right; ctx.dockMeasBottom = meas.bottom; }
    }

    // ---- 分割比の吸い上げ（ユーザーがスプリッタを動かした差分だけ）----
    if (m_baselineWait > 0)
    {
        if (--m_baselineWait == 0)
        {
            m_baselineValid = Measure(m_baseline);
            if (m_baselineValid)
                Logger::Info("dock baseline: measured L{:.4f} R{:.4f} B{:.4f} / layout L{:.4f} R{:.4f} B{:.4f}",
                             m_baseline.left, m_baseline.right, m_baseline.bottom,
                             m_layout.ratios.left, m_layout.ratios.right, m_layout.ratios.bottom);
        }
    }
    else if (m_baselineValid && !m_bottomMax)
    {
        ws::Ratios m;
        if (Measure(m))
        {
            constexpr float kEps = 0.004f;
            ws::Ratios r = m_layout.ratios;
            auto upd = [&](float& applied, float meas, float& base) {
                if (std::fabs(meas - base) > kEps) { applied += meas - base; base = meas; }
            };
            upd(r.left, m.left, m_baseline.left);
            upd(r.right, m.right, m_baseline.right);
            upd(r.bottom, m.bottom, m_baseline.bottom);
            if (m_nodeRightSplit) upd(r.rightSplit, m.rightSplit, m_baseline.rightSplit);
            m_layout.ratios = ws::Round(ws::Clamp(r));
        }
    }

    // ---- 選択中の下部タブ（登録タブのとき）を覚える ----
    if (ImGuiDockNode* bn = NodeOf(m_nodeBottom))
        for (ImGuiWindow* w : bn->Windows)
        {
            if (w->TabId != bn->SelectedTabId) continue;
            std::string activeId;
            for (const bottomdock::Tab& t : bottomdock::Tabs())
                if (bottomdock::WindowName(t) == w->Name) { activeId = t.id; break; }
            m_layout.bottomActive = activeId;
        }

    // ---- 下部ドックのタブ帯のダブルクリック = 最大化 / 元に戻す ----
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        if (ImGuiDockNode* bn = NodeOf(m_nodeBottom))
            if (bn->TabBar && bn->HostWindow)
            {
                ImGuiContext& g = *ImGui::GetCurrentContext();
                const ImGuiWindow* hw = g.HoveredWindow;
                const bool inBar = bn->TabBar->BarRect.Contains(ImGui::GetIO().MousePos);
                if (inBar && hw && hw->RootWindowDockTree == bn->HostWindow->RootWindowDockTree)
                    ctx.bottomDockMaximizeToggle = true;
            }

    // ---- ワークスペース切替で開いた窓の整列 ----
    ArrangeNewFloating(areaX, areaY, areaW, areaH);

    // ---- レイアウトの永続化（変化した時だけ。20 フレームに 1 回見る）----
    if (++m_persistTick >= 20)
    {
        m_persistTick = 0;
        PersistNow(ctx);
    }
}

void WorkspaceManager::PersistNow(const EditorContext& ctx)
{
    const ws::Layout s = Snapshot(ctx);
    if (m_hasPersisted && s == m_lastPersisted) return;
    m_lastPersisted = s;
    m_hasPersisted = true;
    prefs::SetString(kPrefLayout, ws::ToJson(s));   // 書き込みは prefs 側のデバウンス（--background / テスト中は書かない）
}

// ---------------------------------------------------------------- 管理窓

void WorkspaceManager::DrawWindows(EditorContext& ctx)
{
    const ImGuiViewport* mvp = ImGui::GetMainViewport();
    const ImVec2 center(mvp->Pos.x + mvp->Size.x * 0.5f, mvp->Pos.y + mvp->Size.y * 0.4f);

    // ---- レイアウトを保存 ----
    if (m_saveWindowOpen)
    {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ui::Px(420.0f), 0.0f), ImGuiCond_Appearing);
        bool doSave = false;
        if (ImGui::Begin("レイアウトを保存###LayoutSave", &m_saveWindowOpen,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextDisabled("今の分割比・開いているツール窓・配置先・下部タブに名前を付けて保存します。");
            ImGui::Spacing();
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1.0f);
            if (ui::InputText("##LayoutName", m_saveNameBuf, sizeof(m_saveNameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                doSave = true;
            if (!ctx.savedLayoutNames.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("保存済み（クリックで名前に入れます。そのまま保存すると上書き）");
                for (const std::string& n : ctx.savedLayoutNames)
                {
                    if (ui::Chip(("##chip_" + n).c_str(), n.c_str(), false))
                        strncpy_s(m_saveNameBuf, n.c_str(), _TRUNCATE);
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
            ImGui::Spacing();
            const bool nameOk = !ws::NormalizeLayoutName(m_saveNameBuf).empty();
            ImGui::BeginDisabled(!nameOk);
            if (ui::PrimaryButton("保存")) doSave = true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("キャンセル")) m_saveWindowOpen = false;
        }
        ImGui::End();
        if (doSave && !ws::NormalizeLayoutName(m_saveNameBuf).empty())
        {
            SaveNamed(ctx, m_saveNameBuf);
            m_saveWindowOpen = false;
        }
    }

    // ---- ツール窓の配置先 ----
    if (m_slotsWindowOpen)
    {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ui::Px(460.0f), ui::Px(420.0f)), ImGuiCond_Appearing);
        if (ImGui::Begin("ツール窓の配置先###ToolSlots", &m_slotsWindowOpen,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped("右カラムのタブとして開く窓を、インスペクターと並べて見たい時は「右カラムを分割」、"
                               "アセットブラウザの隣なら「下部ドック」、独立して浮かせたい時は「フローティング」を選びます。");
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
            ImGui::TextWrapped("右分割とフローティングへの変更はレイアウトを組み直します（ビューポートの大きさは変わりません）。");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::BeginTable("##slots", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("窓", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("配置先", ImGuiTableColumnFlags_WidthFixed, ui::Px(210.0f));
                for (const tools::Desc& d : tools::kAll)
                {
                    if (!IsMovable(d)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(d.icon);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(d.title);
                    ImGui::TableSetColumnIndex(1);
                    int idx = static_cast<int>(SlotOf(d));
                    const char* items[ws::kSlotCount];
                    for (int i = 0; i < ws::kSlotCount; ++i) items[i] = ws::SlotLabel(static_cast<ws::Slot>(i));
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ui::Combo((std::string("##slot_") + d.id).c_str(), &idx, items, ws::kSlotCount))
                        SetToolSlot(ctx, d, static_cast<ws::Slot>(idx));
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (ImGui::Button("すべて既定に戻す"))
            {
                for (const tools::Desc& d : tools::kAll)
                    if (IsMovable(d)) SetToolSlot(ctx, d, ws::Slot::RightTab);
            }
        }
        ImGui::End();
    }
}

} // namespace dx12e
