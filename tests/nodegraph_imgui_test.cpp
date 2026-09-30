// マテリアルグラフ G0: 汎用ノードグラフ ビュー（src/editor/nodegraph/GraphView）を、窓も GPU も無しの ImGui コンテキストで実際に描く統合テスト。
//   (1) 性能: 500 ノード / 800 ワイヤの合成グラフで GraphView::Draw の CPU 時間（CpuScopeTimer）を測る（全体表示 / 等倍 / 引き / 大きく拡大）。
//   (2) 入力: ImGui の IO へ注入したマウス・キーで、ピンからピンへのドラッグ接続 / 空き地へのドロップ = 検索パレット /
//       Alt+クリック切断 / 矩形選択 / ノード移動 + スナップ / Ctrl+ドラッグ切断 / ホイールズーム / 中ドラッグのパン / Ctrl+Z の往復。
//       OS のカーソルには触れない（ImGui へ流すだけ）。
//   (3) DPI: 100 / 150 / 200% でノードの画面上の大きさが倍率に比例し、ピンがノードの縁に乗る。
//
// 実行: ctest --output-on-failure -R NodeGraphImGui

#include "editor/EditorTheme.h"
#include "editor/nodegraph/GraphView.h"
#include "editor/nodegraph/SandboxGraphModel.h"
#include "editor/nodegraph/SandboxSamples.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e;
using namespace dx12e::ng;

namespace
{
int g_checks = 0, g_failures = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

struct Harness
{
    SandboxGraphModel model;
    GraphDocument doc{&model};
    GraphView view;
    float w = 1600, h = 900;
    float frameMs = 0.0f;

    Harness() { view.SetDocument(&doc); view.Settings().animate = false; }

    // バックエンド無しのテクスチャ更新（動的フォントのために必要）
    static void AckTextures()
    {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        {
            if (tex->Status == ImTextureStatus_WantCreate) { tex->SetTexID(static_cast<ImTextureID>(1)); tex->SetStatus(ImTextureStatus_OK); }
            else if (tex->Status == ImTextureStatus_WantUpdates) tex->SetStatus(ImTextureStatus_OK);
            else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) { tex->SetTexID(ImTextureID_Invalid); tex->SetStatus(ImTextureStatus_Destroyed); }
        }
    }

    void Frame()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.DisplaySize = ImVec2(w, h);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("ng", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
        ImGui::PopStyleVar();
        view.Draw("##c", ImVec2(0, 0));
        view.ProcessKeys();
        ImGui::End();
        ImGui::Render();
        AckTextures();
    }
    void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }

    // ---- 入力（ImGui の IO へ）----
    void Move(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); Frames(1); }
    void Down(int b = 0) { ImGui::GetIO().AddMouseButtonEvent(b, true); Frames(2); }
    void Up(int b = 0) { ImGui::GetIO().AddMouseButtonEvent(b, false); Frames(2); }
    void Drag(ImVec2 a, ImVec2 b, int steps = 8, int btn = 0)
    {
        Move(a); Down(btn);
        for (int i = 1; i <= steps; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            Move(ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t));
        }
        Up(btn);
    }
    void Click(ImVec2 p, int btn = 0) { Move(p); Down(btn); Up(btn); Frames(2); }
    void Mod(ImGuiKey mod, bool on) { ImGui::GetIO().AddKeyEvent(mod, on); Frames(1); }
    void Key(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frames(2); ImGui::GetIO().AddKeyEvent(k, false); Frames(2); }
    void Chord(ImGuiKey mod, ImGuiKey k) { Mod(mod, true); Key(k); Mod(mod, false); }
    void Wheel(float notches) { ImGui::GetIO().AddMouseWheelEvent(0, notches); Frames(2); }
};

ImVec2 Center(const Harness& H, NodeId id)
{
    ImVec2 mx;
    const ImVec2 mn = H.view.ScreenNodeRect(id, &mx);
    return ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
}
ImVec2 HeaderPoint(const Harness& H, NodeId id)
{
    ImVec2 mx;
    const ImVec2 mn = H.view.ScreenNodeRect(id, &mx);
    return ImVec2(mn.x + (mx.x - mn.x) * 0.5f, mn.y + 10.0f * H.view.CurrentViewport().Scale());
}

PinRef In(Harness& H, NodeId n, const char* name) { const NodeData* d = H.model.FindNode(n); return PinRef{n, static_cast<uint16_t>(H.model.InputIndex(d->type, name)), false}; }
PinRef Out(Harness& H, NodeId n, const char* name) { const NodeData* d = H.model.FindNode(n); return PinRef{n, static_cast<uint16_t>(H.model.OutputIndex(d->type, name)), true}; }

// ---------------------------------------------------------------------------
void TestPerformance()
{
    Harness H;
    const SampleReport rep = BuildStressGraph(H.doc, H.model, 500, 800, 42);
    std::printf("  性能: %d ノード / %d ワイヤ（1600x900）\n", rep.nodes, rep.edges);
    H.view.Settings().animate = true;
    H.Frames(3);

    struct Case { const char* name; float zoom; bool frameAll; };
    const Case cases[] = {{"全体表示(引き)", 0, true}, {"等倍 100%", 1.0f, false}, {"引き 30%", 0.3f, false}, {"拡大 200%", 2.0f, false}};
    for (const Case& c : cases)
    {
        if (c.frameAll) { H.view.FrameAll(false); }
        else
        {
            ViewState vs = H.view.GetViewState();
            const Rect b = H.doc.ContentBounds();
            vs.zoom = c.zoom;
            vs.pan = Vec2(b.Center().x - 800.0f / c.zoom, b.Center().y - 450.0f / c.zoom);
            H.view.SetViewState(vs, false);
        }
        H.Frames(6);   // フォントの焼き込み・キャッシュの暖機
        std::vector<float> ms;
        for (int i = 0; i < 90; ++i) { H.Frame(); ms.push_back(H.view.Stats().cpuMs); }
        std::sort(ms.begin(), ms.end());
        const float med = ms[ms.size() / 2], p95 = ms[ms.size() * 95 / 100], mx = ms.back();
        const GraphViewStats& st = H.view.Stats();
        std::printf("    %-14s zoom=%.2f  描画 %3d/%d ノード %3d/%d ワイヤ  頂点 %6d   CPU 中央値 %.3f ms  p95 %.3f ms  最大 %.3f ms\n",
                    c.name, static_cast<double>(st.zoom), st.drawnNodes, st.nodes, st.drawnEdges, st.edges, st.drawnPrims,
                    static_cast<double>(med), static_cast<double>(p95), static_cast<double>(mx));
        CHECK(med <= 3.0f, "%s: 描画 CPU 中央値が 3ms 以下 (%.3f)", c.name, static_cast<double>(med));
        CHECK(st.drawnNodes > 0 && st.drawnEdges > 0, "何かが描かれている");
    }

    // 選択 + 上流下流の強調 + フロー有りの最悪ケース
    H.view.FrameAll(false);
    H.Frames(3);
    Selection sel;
    for (size_t i = 0; i < H.model.Nodes().size(); i += 40) sel.nodes.insert(H.model.Nodes()[i]);
    H.view.SetSelection(sel);
    H.view.Settings().flow = GraphViewSettings::Flow::Highlighted;
    H.Frames(6);
    std::vector<float> ms;
    for (int i = 0; i < 90; ++i) { H.Frame(); ms.push_back(H.view.Stats().cpuMs); }
    std::sort(ms.begin(), ms.end());
    std::printf("    選択 %zu 個 + 上流/下流の強調 + 流れ + グロー: 中央値 %.3f ms  p95 %.3f ms\n", sel.nodes.size(), static_cast<double>(ms[ms.size() / 2]), static_cast<double>(ms[ms.size() * 95 / 100]));
    CHECK(ms[ms.size() / 2] <= 3.5f, "強調ありでも 3.5ms 以下 (%.3f)", static_cast<double>(ms[ms.size() / 2]));
}

// ---------------------------------------------------------------------------
void TestInteractions()
{
    Harness H;
    BuildSampleGraph(H.doc, H.model);
    H.view.FrameAll(false);
    H.Frames(4);
    auto& doc = H.doc;
    auto& m = H.model;
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    // 使うノード: テクスチャ列の TextureObject / TextureSample
    NodeId texObj = 0, sample = 0, addNode = 0;
    std::vector<NodeId> texObjs, samples;
    for (NodeId id : m.Nodes())
    {
        const std::string& t = m.FindNode(id)->type;
        if (t == "texture") texObjs.push_back(id);
        if (t == "sample") samples.push_back(id);
    }
    CHECK(texObjs.size() >= 3 && samples.size() >= 3, "サンプルにテクスチャ系がある");
    texObj = texObjs[2]; sample = samples[2];   // 法線側（sample.Tex は接続済み）
    (void)addNode;

    // ---- 表示が「見えている」こと ----
    ImVec2 nmx;
    const ImVec2 nmn = H.view.ScreenNodeRect(sample, &nmx);
    CHECK(nmx.x > nmn.x && nmn.x >= H.view.CanvasMin().x - 1 && nmx.x <= H.view.CanvasMax().x + 1, "全体表示でノードが画面内");

    // 準備: コメントにも既存ノードにも重ならない空き地（内容の右下）を等倍で表示する
    const Vec2 origin = doc.ContentBounds().max + Vec2(200, 200);
    H.view.SetViewState(ViewState{origin - Vec2(150, 150), 1.0f}, false);
    H.Frames(3);

    // ---- 1. ドラッグ接続: TextureObject.Tex → 未接続の入力へ（Noise.UV ではなく Sample の UV は型違いで拒否）----
    // 空いている入力: sample[0](RGB 出力側でない) の "UV"... サンプルでは接続済みなので、新規に Add を置いて繋ぐ
    const NodeId add = doc.AddNode("add", origin + Vec2(400, 100));
    H.Frames(3);
    const size_t undo0 = doc.History().UndoDepth();
    const NodeId flt = doc.AddNode("float", origin + Vec2(60, 260));
    H.Frames(3);
    {
        const ImVec2 from = H.view.ScreenPinPos(Out(H, flt, "Out"));
        const ImVec2 to = H.view.ScreenPinPos(In(H, add, "A"));
        H.Drag(from, to, 10);
        const Edge* e = m.FindEdgeTo(In(H, add, "A"));
        CHECK(e && e->from == Out(H, flt, "Out"), "ピンからピンへのドラッグで接続できる");
        CHECK(doc.History().UndoDepth() == undo0 + 2, "ノード追加 1 + 接続 1 = 2 操作 (%zu)", doc.History().UndoDepth() - undo0);
        // Ctrl+Z で戻る
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(!m.FindEdgeTo(In(H, add, "A")), "Ctrl+Z で接続が消える");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Y);
        CHECK(m.FindEdgeTo(In(H, add, "A")) != nullptr, "Ctrl+Y でやり直す");
    }

    // ---- 2. 型の合わない接続は拒否（Texture → UV）----
    {
        const size_t d = doc.History().UndoDepth();
        H.Drag(H.view.ScreenPinPos(Out(H, texObj, "Tex")), H.view.ScreenPinPos(In(H, add, "B")), 10);
        CHECK(!m.FindEdgeTo(In(H, add, "B")), "Tex → 数値ピンは拒否される");
        CHECK(doc.History().UndoDepth() == d, "拒否された接続は履歴に載らない");
    }

    // ---- 3. Alt+クリックでピンの全切断 ----
    {
        H.Mod(ImGuiMod_Alt, true);
        H.Click(H.view.ScreenPinPos(In(H, add, "A")));
        H.Mod(ImGuiMod_Alt, false);
        CHECK(!m.FindEdgeTo(In(H, add, "A")), "Alt+クリックで切断");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.FindEdgeTo(In(H, add, "A")) != nullptr, "切断も Undo できる");
    }

    // ---- 4. 入力ピンを掴んで別の入力へ付け替え / 空き地へ落として切断 ----
    {
        H.Drag(H.view.ScreenPinPos(In(H, add, "A")), H.view.ScreenPinPos(In(H, add, "B")), 10);   // 同じノードの別入力 = 付け替え
        const Edge* eb = m.FindEdgeTo(In(H, add, "B"));
        CHECK(eb && eb->from == Out(H, flt, "Out") && !m.FindEdgeTo(In(H, add, "A")), "入力ピンを掴んで別の入力へ付け替え [%s] A=%d B=%d", H.view.StatusLine().c_str(), m.FindEdgeTo(In(H, add, "A")) ? 1 : 0, eb ? 1 : 0);
        const ImVec2 far(H.view.CanvasMin().x + 400, H.view.CanvasMax().y - 260);
        H.Drag(H.view.ScreenPinPos(In(H, add, "B")), far, 10);
        CHECK(!m.FindEdgeTo(In(H, add, "B")), "入力ピンを掴んで空き地へ落とすと切断");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.FindEdgeTo(In(H, add, "B")) != nullptr, "切断の Undo");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.FindEdgeTo(In(H, add, "A")) != nullptr && !m.FindEdgeTo(In(H, add, "B")), "付け替えの Undo = 元の入力へ戻る");
    }

    // ---- 5. 出力ピンから空き地へ = 検索パレット（型で絞り込み）→ 選んで確定すると新ノード + 自動接続 ----
    {
        const size_t nodesBefore = m.Nodes().size();
        const ImVec2 drop(H.view.CanvasMin().x + 620, H.view.CanvasMax().y - 300);
        H.Drag(H.view.ScreenPinPos(Out(H, add, "Out")), drop, 10);
        CHECK(H.view.IsPaletteOpen(), "空き地へ落とすと検索パレットが開く [%s]", H.view.StatusLine().c_str());
        H.view.SetPaletteQuery("sat");
        H.Frames(2);
        CHECK(H.view.ConfirmPalette(0), "候補を確定");
        H.Frames(3);
        CHECK(m.Nodes().size() == nodesBefore + 1, "新ノードが増えた");
        const NodeId nn = m.Nodes().back();
        const Edge* e = m.FindEdgeTo(PinRef{nn, 0, false});
        CHECK(m.FindNode(nn)->type == "saturate" && e && e->from == Out(H, add, "Out"), "新ノードが自動で接続された (%s)", m.FindNode(nn)->type.c_str());
        H.Key(ImGuiKey_Escape);   // パレットが残っていれば閉じる
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.Nodes().size() == nodesBefore && !H.view.IsPaletteOpen(), "追加 + 接続も 1 回の Undo で戻る");
    }

    // ---- 6. 矩形選択 ----
    {
        const Rect r = doc.NodeRect(add);
        const Rect r2 = doc.NodeRect(flt);
        const Vec2 a(std::min(r.min.x, r2.min.x) - 30, std::min(r.min.y, r2.min.y) - 30), b(std::max(r.max.x, r2.max.x) + 30, std::max(r.max.y, r2.max.y) + 30);
        H.view.SetSelection(Selection{});
        H.Frames(2);
        H.Drag(H.view.ToScreen(a), H.view.ToScreen(b), 10);
        const Selection& sel = H.view.GetSelection();
        CHECK(sel.Has(add) && sel.Has(flt), "矩形選択で 2 個 (%zu) [%s]", sel.nodes.size(), H.view.StatusLine().c_str());
    }

    // ---- 7. ノード移動 + スナップ + Undo ----
    {
        Selection s; s.nodes.insert(flt);
        H.view.SetSelection(s);
        const Vec2 p0 = m.FindNode(flt)->pos;
        const ImVec2 hp = HeaderPoint(H, flt);
        H.Drag(hp, ImVec2(hp.x + 137.0f, hp.y + 61.0f), 10);
        const Vec2 p1 = m.FindNode(flt)->pos;
        CHECK(p1.x != p0.x || p1.y != p0.y, "ドラッグで動く");
        CHECK(std::fmod(std::fabs(p1.x), 16.0f) < 1e-3f && std::fmod(std::fabs(p1.y), 16.0f) < 1e-3f, "グリッドスナップ (%.2f, %.2f)", static_cast<double>(p1.x), static_cast<double>(p1.y));
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.FindNode(flt)->pos == p0, "移動の Undo は 1 回で元へ");
    }

    // ---- 8. Ctrl+ドラッグでワイヤ切断 ----
    {
        const Edge e = *m.FindEdgeTo(In(H, add, "A"));
        const Vec2 mid = doc.EdgeCurve(e).At(0.5f);
        const ImVec2 sm = H.view.ToScreen(mid);
        H.Mod(ImGuiMod_Ctrl, true);
        H.Drag(ImVec2(sm.x - 6, sm.y - 60), ImVec2(sm.x + 6, sm.y + 60), 8);
        H.Mod(ImGuiMod_Ctrl, false);
        CHECK(!m.FindEdgeTo(In(H, add, "A")), "Ctrl+ドラッグで横切ったワイヤを切断");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.FindEdgeTo(In(H, add, "A")) != nullptr, "切断の Undo");
    }

    // ---- 9. ホイールズーム / 中ドラッグでパン ----
    {
        const float z0 = H.view.GetViewState().zoom;
        H.Move(ImVec2(H.view.CanvasMin().x + 500, H.view.CanvasMin().y + 300));
        const Vec2 gBefore = H.view.ToGraph(ImVec2(H.view.CanvasMin().x + 500, H.view.CanvasMin().y + 300));
        H.Wheel(3.0f);
        H.Frames(20);
        const float z1 = H.view.GetViewState().zoom;
        CHECK(z1 > z0 * 1.3f, "ホイールでズームイン (%.2f -> %.2f)", static_cast<double>(z0), static_cast<double>(z1));
        const Vec2 gAfter = H.view.ToGraph(ImVec2(H.view.CanvasMin().x + 500, H.view.CanvasMin().y + 300));
        CHECK(std::fabs(gBefore.x - gAfter.x) < 1.0f && std::fabs(gBefore.y - gAfter.y) < 1.0f, "カーソル位置を中心にズーム");
        H.Wheel(-30.0f);
        H.Frames(20);
        CHECK(H.view.GetViewState().zoom >= 0.0999f, "ズーム下限 10%%");
        H.Wheel(60.0f);
        H.Frames(30);
        CHECK(H.view.GetViewState().zoom <= 4.0001f, "ズーム上限 400%% (%.3f)", static_cast<double>(H.view.GetViewState().zoom));
        H.view.ResetZoom(false);
        H.Frames(3);
        const Vec2 pan0 = H.view.GetViewState().pan;
        H.Drag(ImVec2(600, 400), ImVec2(500, 350), 6, 2);
        const Vec2 pan1 = H.view.GetViewState().pan;
        CHECK(std::fabs((pan1.x - pan0.x) - 100.0f / H.view.CurrentViewport().Scale()) < 2.0f, "中ドラッグでパン (%.1f)", static_cast<double>(pan1.x - pan0.x));
    }

    // ---- 10. 全選択 / 削除 / Undo ----
    {
        H.view.SetViewState(ViewState{Vec2(0, 0), 0.4f}, false);
        H.Frames(3);
        H.Move(ImVec2(500, 400));
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_A);
        CHECK(H.view.GetSelection().nodes.size() == m.Nodes().size(), "Ctrl+A で全選択");
        const size_t n0 = m.Nodes().size(), e0 = m.Edges().size();
        H.Key(ImGuiKey_Delete);
        CHECK(m.Nodes().empty() && m.Edges().empty(), "Del で全部消える");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.Nodes().size() == n0 && m.Edges().size() == e0, "1 回の Undo で全部戻る");
    }

    // ---- 11. コピー / 貼り付け / 複製 ----
    {
        Selection s; s.nodes.insert(add); s.nodes.insert(flt);
        H.view.SetSelection(s);
        H.Frames(2);
        const size_t n0 = m.Nodes().size();
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_D);
        CHECK(m.Nodes().size() == n0 + 2, "Ctrl+D で複製 (%zu -> %zu)", n0, m.Nodes().size());
        CHECK(H.view.GetSelection().nodes.size() == 2 && !H.view.GetSelection().Has(add), "複製したノードが選択される");
        H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        CHECK(m.Nodes().size() == n0, "複製の Undo");
    }

    // ---- 12. F = 選択にフォーカス / Home = 全体 ----
    {
        Selection s; s.nodes.insert(add);
        H.view.SetSelection(s);
        H.Move(ImVec2(300, 300));
        H.Key(ImGuiKey_F);
        H.view.SettleAnimations();
        H.Frames(2);
        ImVec2 mx2;
        const ImVec2 mn2 = H.view.ScreenNodeRect(add, &mx2);
        const ImVec2 c((mn2.x + mx2.x) * 0.5f, (mn2.y + mx2.y) * 0.5f);
        const ImVec2 cc((H.view.CanvasMin().x + H.view.CanvasMax().x) * 0.5f, (H.view.CanvasMin().y + H.view.CanvasMax().y) * 0.5f);
        CHECK(std::fabs(c.x - cc.x) < 30.0f && std::fabs(c.y - cc.y) < 30.0f, "F で選択ノードが中央へ (%.0f,%.0f)", static_cast<double>(c.x), static_cast<double>(c.y));
        H.Key(ImGuiKey_Home);
        H.view.SettleAnimations();
        H.Frames(2);
        ImVec2 mxa;
        const ImVec2 mna = H.view.ScreenNodeRect(m.Nodes().front(), &mxa);
        CHECK(mna.x >= H.view.CanvasMin().x - 2 && mxa.x <= H.view.CanvasMax().x + 2, "Home で全体が収まる");
    }
}

// ---------------------------------------------------------------------------
void TestDpi()
{
    float baseW = 0.0f;
    for (float dpi : {1.0f, 1.5f, 2.0f})
    {
        theme::SetScale(dpi);
        Harness H;
        BuildSampleGraph(H.doc, H.model);
        H.view.SetViewState(ViewState{Vec2(-50, -50), 1.0f}, false);
        H.Frames(4);
        const NodeId n = H.model.Nodes()[5];
        ImVec2 mx;
        const ImVec2 mn = H.view.ScreenNodeRect(n, &mx);
        const float w = mx.x - mn.x;
        if (dpi == 1.0f) baseW = w;
        CHECK(std::fabs(w / baseW - dpi) < 0.02f, "DPI %.0f%%: ノード幅が倍率に比例 (%.1f / %.1f)", static_cast<double>(dpi) * 100.0, static_cast<double>(w), static_cast<double>(baseW));
        // ピンはノードの縁に乗る
        const NodeData* nd = H.model.FindNode(n);
        if (!nd->desc->outputs.empty())
        {
            const ImVec2 p = H.view.ScreenPinPos(PinRef{n, 0, true});
            CHECK(std::fabs(p.x - mx.x) < 0.5f, "DPI %.0f%%: 出力ピンが右端に乗る", static_cast<double>(dpi) * 100.0);
        }
        if (!nd->desc->inputs.empty())
        {
            const ImVec2 p = H.view.ScreenPinPos(PinRef{n, 0, false});
            CHECK(std::fabs(p.x - mn.x) < 0.5f, "DPI %.0f%%: 入力ピンが左端に乗る", static_cast<double>(dpi) * 100.0);
        }
        // 当たり判定も倍率に追従（ピンを掴んで接続できる）
        H.Frames(1);
    }
    theme::SetScale(1.0f);
}
} // namespace

int main()
{
    std::printf("nodegraph_imgui_test\n");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 900);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // 動的フォント（任意サイズの AddText）に必要
    io.Fonts->AddFontDefault();
    theme::ApplyStyle(ImGui::GetStyle());

    TestDpi();
    TestInteractions();
    TestPerformance();

    ImGui::DestroyContext();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
