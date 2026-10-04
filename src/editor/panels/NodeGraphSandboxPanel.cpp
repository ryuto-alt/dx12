#include "editor/panels/NodeGraphSandboxPanel.h"

#include "editor/panels/MaterialGraphPanel.h"   // 下の Render がマテリアルグラフ窓の描画も呼ぶ（ApplicationRender.cpp を触らずに窓を足すための経路）

#include "core/AtomicFileJson.h"
#include "editor/EditorContext.h"
#include "editor/UiWidgets.h"
#include "editor/nodegraph/GraphView.h"
#include "editor/nodegraph/SandboxGraphModel.h"
#include "editor/nodegraph/SandboxSamples.h"
#include "gui/VirtualInputImGui.h"
#include "input/VirtualInput.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dx12e
{
namespace NodeGraphSandboxPanel
{

namespace
{
constexpr const char* kWindowName = "ノードグラフ サンドボックス###NodeGraphSandbox";

struct State
{
    ng::SandboxGraphModel model;
    ng::GraphDocument doc{&model};
    ng::GraphView view;
    bool init = false;
    bool errorDemo = false;
    std::string message;
    double messageUntil = 0.0;
    std::vector<std::string> pending;   // コマンドパレット / メニューから来たコマンド
    int sampleNodes = 0;
};

State& S()
{
    static State s;
    return s;
}

std::string SavePath()
{
    return (std::filesystem::temp_directory_path() / "uno_nodegraph_sandbox.json").string();
}

void Say(State& s, const std::string& msg)
{
    s.message = msg;
    s.messageUntil = ImGui::GetTime() + 4.0;
}

void LoadSample(State& s)
{
    const ng::SampleReport r = ng::BuildSampleGraph(s.doc, s.model);
    s.view.SetDocument(&s.doc);
    s.view.FrameAll(false);
    s.errorDemo = false;
    s.view.ClearNodeStates();
    Say(s, "サンプルを作りました（" + std::to_string(r.nodes) + " ノード / " + std::to_string(r.edges) + " ワイヤ）");
}

void LoadStress(State& s)
{
    const ng::SampleReport r = ng::BuildStressGraph(s.doc, s.model, 500, 800, 42);
    s.view.SetDocument(&s.doc);
    s.view.FrameAll(false);
    s.errorDemo = false;
    s.view.ClearNodeStates();
    Say(s, "負荷測定用グラフ（" + std::to_string(r.nodes) + " ノード / " + std::to_string(r.edges) + " ワイヤ）");
}

// 状態表示のデモ: 出力ノード = コンパイル中、いくつかにエラー / 警告 / 減光
void ApplyErrorDemo(State& s, bool on)
{
    s.view.ClearNodeStates();
    if (!on) return;
    const auto& ids = s.model.Nodes();
    if (ids.size() < 6) return;
    int k = 0;
    for (ng::NodeId id : ids)
    {
        const ng::NodeData* n = s.model.FindNode(id);
        if (n->type == "output") s.view.SetNodeState(id, ng::GraphView::NodeState::Compiling, "コンパイル中…");
        else if (n->type == "power") s.view.SetNodeState(id, ng::GraphView::NodeState::Error, "エラー: Base に未接続の必須入力があります");
        else if (n->type == "clamp") s.view.SetNodeState(id, ng::GraphView::NodeState::Warning, "警告: float3 → float2 に切り詰め");
        else if (n->type == "abs" || n->type == "time" || (n->type == "sine" && (k++ % 2 == 0))) s.view.SetNodeState(id, ng::GraphView::NodeState::Dimmed, "出力に繋がっていません");
    }
}
} // namespace

bool ExecuteCommand(EditorContext& ctx, const char* commandId)
{
    if (!ctx.showNodeGraphSandbox || !commandId) return false;
    S().pending.emplace_back(commandId);
    return true;
}

static void RenderSandbox(EditorContext& ctx)
{
    if (!ctx.showNodeGraphSandbox)
    {
        ctx.nodeGraphKeyFocus = false;
        return;
    }
    State& s = S();
    if (!s.init)
    {
        s.view.SetDocument(&s.doc);
        s.view.SetAnchorSink([](const char* kind, const std::string& label, ImVec2 mn, ImVec2 mx)
        {
            if (!vinput::Enabled()) return;
            vinput::Anchor a;
            a.kind = kind;
            a.label = label;
            a.window = kWindowName;
            a.x0 = mn.x; a.y0 = mn.y; a.x1 = mx.x; a.y1 = mx.y;
            vinput::Global().AddAnchor(std::move(a));
        }, 80);
        LoadSample(s);
        s.init = true;
    }

    const ImGuiViewport* mvp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(mvp->Pos.x + ui::Px(90.0f), mvp->Pos.y + ui::Px(80.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ui::Px(1180.0f, 760.0f), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ui::Px(0.0f, 0.0f));
    const bool open = ImGui::Begin(kWindowName, &ctx.showNodeGraphSandbox,
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open)
    {
        ImGui::End();
        ctx.nodeGraphKeyFocus = false;
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        ctx.floatingToolWindowHoveredThisFrame = true;

    ng::GraphView& v = s.view;
    ng::GraphDocument& doc = s.doc;
    ng::GraphViewSettings& set = v.Settings();

    // コマンドパレット / メニュー経由のコマンド
    for (const std::string& id : s.pending) v.Execute(id);
    s.pending.clear();

    // ---- ツールバー ----
    ImGui::SetCursorPos(ImVec2(ui::Px(8.0f), ImGui::GetCursorPosY() + ui::Px(6.0f)));   // タイトルバーの下から（SetCursorPos は窓の左上基準）
    ImGui::BeginGroup();
    {
        float rowW = ImGui::GetContentRegionAvail().x - ui::Px(8.0f);
        float x = 0.0f;
        auto place = [&](float w)
        {
            if (x > 0.0f && x + w > rowW) { x = 0.0f; }
            else if (x > 0.0f) { ImGui::SameLine(); }
            x += w + ImGui::GetStyle().ItemSpacing.x;
        };
        auto button = [&](const char* label, const char* anchor, const char* tip = nullptr) -> bool
        {
            place(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f);
            const bool r = ImGui::Button(label);
            vinput_gui::AnchorLastItem("button", anchor);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            return r;
        };
        auto check = [&](const char* label, bool* value, const char* anchor) -> bool
        {
            place(ImGui::CalcTextSize(label).x + ui::Px(34.0f));
            const bool r = ui::Checkbox(label, value);
            vinput_gui::AnchorLastItem("checkbox", anchor);
            return r;
        };

        if (button("サンプル(約40)", "ng:btn:sample", "手組みのサンプルグラフ（コメント・リルート付き）を作り直す")) LoadSample(s);
        if (button("500ノード/800ワイヤ", "ng:btn:stress", "性能測定用の合成グラフ")) LoadStress(s);
        if (button("空にする", "ng:btn:clear")) { doc.Clear(); v.ClearNodeStates(); v.SetDocument(&doc); Say(s, "グラフを空にしました"); }
        if (button("元に戻す", "ng:btn:undo", "Ctrl+Z")) v.Execute("edit.undo");
        if (button("やり直す", "ng:btn:redo", "Ctrl+Y")) v.Execute("edit.redo");
        if (button("全体表示", "ng:btn:frameAll", "Home")) v.Execute("graph.frameAll");
        if (button("選択へ", "ng:btn:frameSel", "F")) v.Execute("edit.focus");
        if (button("100%", "ng:btn:zoom100", "Ctrl+0")) v.Execute("graph.resetZoom");
        if (button("コメントで囲む", "ng:btn:comment", "C")) v.Execute("graph.comment");
        check("スナップ", &set.snap, "ng:chk:snap");
        check("グリッド", &set.showGrid, "ng:chk:grid");
        check("ミニマップ", &set.showMinimap, "ng:chk:minimap");
        check("グロー", &set.glow, "ng:chk:glow");
        check("アニメ", &set.animate, "ng:chk:animate");
        {
            place(ui::Px(150.0f));
            static const char* kFlow[] = {"流れ: なし", "流れ: 強調のみ", "流れ: すべて"};
            int f = static_cast<int>(set.flow);
            ImGui::SetNextItemWidth(ui::Px(150.0f));
            if (ui::Combo("##flow", &f, kFlow, 3)) set.flow = static_cast<ng::GraphViewSettings::Flow>(f);
        }
        if (check("診断表示のデモ", &s.errorDemo, "ng:chk:errors")) ApplyErrorDemo(s, s.errorDemo);
        if (button("JSON 保存", "ng:btn:save", "ノード位置・コメント・ズーム・パンを JSON に保存（一時フォルダ）"))
        {
            const ng::ViewState vs = v.GetViewState();
            const auto wr = dx12e::atomicfile::WriteFile(std::filesystem::path(SavePath()), ng::SaveGraphJson(doc, &vs),
                                                         dx12e::atomicfile::JsonVerifier());
            Say(s, wr ? "保存しました: " + SavePath() : "保存に失敗しました: " + wr.error);
        }
        if (button("JSON 読込", "ng:btn:load"))
        {
            std::ifstream f(SavePath(), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            ng::ViewState vs; std::string warn;
            if (f && ng::LoadGraphJson(doc, ss.str(), &vs, &warn))
            {
                v.SetDocument(&doc);
                v.SetViewState(vs, false);
                s.errorDemo = false; v.ClearNodeStates();
                Say(s, "読み込みました" + (warn.empty() ? std::string() : " / " + warn));
            }
            else Say(s, "読み込めませんでした（先に「JSON 保存」してください）");
        }
    }
    ImGui::EndGroup();

    // ---- キャンバス ----
    // ImGui の ContentRegionAvail はウィンドウの余白・スクロール量に影響されるので、ウィンドウの下端から絶対位置で決める
    //（ツールバーが折り返して行数が変わっても、キャンバスとステータスバーが窓からはみ出さない）。
    const float statusH = ui::Px(28.0f);
    ImGui::SetScrollY(0.0f);
    ImGui::SetCursorPosX(0.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::Px(4.0f));
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float winBottom = winPos.y + ImGui::GetWindowHeight();
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const float canvasH = std::max(64.0f, winBottom - canvasPos.y - statusH - 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(winPos.x, canvasPos.y));
    v.Draw("##ngcanvas", ImVec2(ImGui::GetWindowWidth(), canvasH));
    v.ProcessKeys();
    ctx.nodeGraphKeyFocus = v.WantsKeyboard();

    // ---- ステータスバー ----
    {
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + ui::Px(10.0f), winBottom - statusH + (statusH - ImGui::GetTextLineHeight()) * 0.5f));
        const ng::GraphViewStats& st = v.Stats();
        const ng::Selection& sel = v.GetSelection();
        ui::PushMono();
        char buf[320];
        std::snprintf(buf, sizeof(buf), "ノード %d  ワイヤ %d  |  描画 %d/%d  %d/%d  |  CPU %.2f ms  |  Zoom %.0f%%  |  選択 %d  Undo %zu / Redo %zu",
                      st.nodes, st.edges, st.drawnNodes, st.nodes, st.drawnEdges, st.edges, static_cast<double>(st.cpuMs), static_cast<double>(st.zoom) * 100.0,
                      static_cast<int>(sel.nodes.size() + sel.comments.size()), doc.History().UndoDepth(), doc.History().RedoDepth());
        ImGui::TextUnformatted(buf);
        ui::PopMono();
        if (ImGui::GetTime() < s.messageUntil)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
            ImGui::Text("   %s", s.message.c_str());
            ImGui::PopStyleColor();
        }
        // 仮想入力・テストが読む 1 行の状態
        if (vinput::Enabled())
        {
            vinput::Anchor a;
            a.kind = "status";
            a.label = "ng:status " + v.StatusLine();
            a.window = kWindowName;
            a.x0 = v.CanvasMin().x; a.y0 = v.CanvasMin().y; a.x1 = v.CanvasMax().x; a.y1 = v.CanvasMax().y;
            vinput::Global().AddAnchor(std::move(a));
        }
    }
    ImGui::End();
}

// Application が毎フレーム呼ぶ唯一の入口。サンドボックス窓に続けて、マテリアルグラフ窓（MaterialGraphPanel）も描く。
// ★ここでマテリアルグラフ窓を呼ぶのは、窓の描画呼び出しを 1 行足すべき ApplicationRender.cpp を触らない約束のため。
//   Application 側に MaterialGraphPanel::Render(*m_editorCtx) を足せたら、この呼び出しはそちらへ移してよい（順序: サンドボックスの後）。
void Render(EditorContext& ctx)
{
    RenderSandbox(ctx);              // 閉じていれば nodeGraphKeyFocus = false にして即 return
    MaterialGraphPanel::Render(ctx); // フォーカス中なら nodeGraphKeyFocus を true にする（サンドボックスの後に呼ぶこと）
}

} // namespace NodeGraphSandboxPanel
} // namespace dx12e
