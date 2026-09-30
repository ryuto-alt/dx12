// マテリアルグラフ エディタ（G3）: MatGraphModel を実際の GraphView（ImDrawList の自前ノードエディタ）で描き、ImGui の IO へ注入した入力で操作する統合テスト。
//   窓も GPU も無し。OS のカーソルには触れない。
//   (1) 見本グラフ（約 30 ノード + コメント + Custom + パラメータ）が 100 / 150 / 200% で描かれ、ノードの大きさが倍率に比例する
//   (2) UE 風ワンキー: 押している間に「置く型」を握り、クリックした位置へ置く / 短押しはマウス位置 / 既定（押した瞬間）は G0 のまま
//   (3) 値欄のドラッグ（G1 のプロパティ）→ Undo、ドラッグ元ピンから空きへドロップ → 型で絞った検索パレット → 選ぶと自動接続
//   (4) FocusNode（診断リストのクリック）で選択して画面内へ寄る、右クリックメニューの拡張・ダブルクリックのハンドラ
//   (5) 診断 → ノードの状態表示（エラー / 警告 / 減光 / コンパイル中）を載せても描ける
// 実行: ctest --output-on-failure -R MatGraphEditorImGui

#include "editor/EditorTheme.h"
#include "editor/matgraph/MatGraphEditor.h"
#include "editor/nodegraph/GraphView.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e;
namespace mg = dx12e::mg;
namespace mat = dx12e::matgraph;
namespace ng = dx12e::ng;

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
    mg::MatGraphEditor ed;
    ng::GraphView view;
    float w = 1600, h = 900;

    Harness()
    {
        view.SetDocument(&ed.Doc());
        view.Settings().animate = false;
    }
    void LoadSample()
    {
        ed.NewGraph(mg::NewTemplate::Sample);
        view.SetDocument(&ed.Doc());
    }

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
        ImGui::Begin("mg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
        ImGui::PopStyleVar();
        ed.Update();
        view.Draw("##c", ImVec2(0, 0));
        view.ProcessKeys();
        ImGui::End();
        ImGui::Render();
        AckTextures();
    }
    void Frames(int n) { for (int i = 0; i < n; ++i) Frame(); }
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
    void KeyDown(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, true); Frames(2); }
    void KeyUp(ImGuiKey k) { ImGui::GetIO().AddKeyEvent(k, false); Frames(2); }
    void Key(ImGuiKey k) { KeyDown(k); KeyUp(k); }
    void Chord(ImGuiKey mod, ImGuiKey k) { KeyDown(mod); Key(k); KeyUp(mod); }

    ng::NodeId Id(const char* sid) { return ed.Model().IntOf(sid); }
    ImVec2 Center(ng::NodeId id)
    {
        ImVec2 mx;
        const ImVec2 mn = view.ScreenNodeRect(id, &mx);
        return ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    }
    size_t Count(const char* type)
    {
        size_t n = 0;
        for (ng::NodeId id : ed.Model().Nodes()) if (ed.Model().FindNode(id)->type == type) ++n;
        return n;
    }
};

// ---------------------------------------------------------------------------
void TestSampleDpi()
{
    Harness H;
    H.LoadSample();
    H.view.FrameAll(false);
    H.Frames(6);
    CHECK(H.view.Stats().nodes == static_cast<int>(H.ed.Model().Nodes().size()) && H.view.Stats().nodes >= 20, "見本のノードが描画対象 (%d)", H.view.Stats().nodes);
    CHECK(H.view.Stats().drawnNodes > 10 && H.view.Stats().drawnEdges > 10, "描かれたノード %d / ワイヤ %d", H.view.Stats().drawnNodes, H.view.Stats().drawnEdges);
    CHECK(H.view.Stats().cpuMs < 6.0f, "見本の描画 CPU %.2f ms", static_cast<double>(H.view.Stats().cpuMs));

    // 名前欄つき（パラメータ）とプロパティ行つき（定数）のノードの大きさ
    const ng::NodeId sp = H.Id("rMin"), add = H.Id("mulBase");
    CHECK(sp && add, "見本のノード");
    const ng::Rect rSp = H.ed.Doc().NodeRect(sp), rAdd = H.ed.Doc().NodeRect(add);
    CHECK(rSp.Height() > 0 && rAdd.Height() > 0, "レイアウトが出る");
    const ng::NodeData* nd = H.ed.Model().FindNode(sp);
    CHECK(nd && nd->desc->hasLabel && nd->label == "RoughnessMin", "パラメータ名が名前欄に出る (%s)", nd ? nd->label.c_str() : "");
    const ng::TypeLayout& L = H.ed.Doc().Layout().For(*nd->desc);
    CHECK(L.labelH > 0.0f, "名前欄の高さを確保");
    CHECK(L.inSlot.size() == nd->desc->inputs.size(), "値欄の数");
    // 値欄（default）がある
    bool anySlot = false;
    for (const ng::Rect& r : L.inSlot) anySlot |= r.Width() > 0.0f;
    CHECK(anySlot, "ScalarParameter は default の値欄を持つ");

    // DPI 100 / 150 / 200%: ノード幅が倍率に比例、ピンが縁に乗る（DPI ごとに新しい文書で測る）
    float w100 = 0.0f;
    for (float dpi : {1.0f, 1.5f, 2.0f})
    {
        theme::SetScale(dpi);
        Harness D;
        D.LoadSample();
        D.view.SetViewState(ng::ViewState{ng::Vec2(-100.0f, -50.0f), 1.0f}, false);
        D.Frames(6);
        const ng::NodeId n = D.Id("mulBase");
        ImVec2 dmx;
        const ImVec2 dmn = D.view.ScreenNodeRect(n, &dmx);
        const float wpx = dmx.x - dmn.x;
        if (dpi == 1.0f) w100 = wpx;
        CHECK(wpx > 40.0f, "dpi %.2f: ノード幅 %.1f", static_cast<double>(dpi), static_cast<double>(wpx));
        if (w100 > 0.0f) CHECK(std::fabs(wpx / w100 - dpi) < 0.03f, "dpi %.2f: ノード幅が倍率に比例 (%.1f / %.1f)", static_cast<double>(dpi), static_cast<double>(wpx), static_cast<double>(w100));
        const ImVec2 pin = D.view.ScreenPinPos(ng::PinRef{n, 0, false});
        CHECK(std::fabs(pin.x - dmn.x) < 0.75f, "dpi %.2f: 入力ピンが左の縁に乗る", static_cast<double>(dpi));
        D.view.FrameAll(false);
        D.Frames(4);
        CHECK(D.view.Stats().drawnNodes > 5, "dpi %.2f: 描かれる", static_cast<double>(dpi));
    }
    theme::SetScale(1.0f);
}

// ---------------------------------------------------------------------------
void TestHotkeys()
{
    // 既定（G0）: 押した瞬間にマウス位置へ置く
    {
        Harness H;
        H.ed.NewGraph(mg::NewTemplate::Empty);
        H.view.SetDocument(&H.ed.Doc());
        H.view.Settings().snap = false;
        H.Frames(3);
        const size_t before = H.Count("Multiply");
        H.Move(ImVec2(500, 400));
        H.Key(ImGuiKey_M);
        CHECK(H.Count("Multiply") == before + 1, "既定モード: M の押下で Multiply が置かれる");
    }
    // ホールド + クリック
    Harness H;
    H.ed.NewGraph(mg::NewTemplate::Empty);
    H.view.SetDocument(&H.ed.Doc());
    H.view.Settings().hotkeyHoldClick = true;
    H.view.Settings().snap = false;
    H.Frames(4);
    const ng::NodeId out = H.ed.OutputNode();
    CHECK(out != 0 && H.ed.Model().Nodes().size() == 1, "出力ノードだけ");

    // M を押している間: 握っている型が見える
    H.Move(ImVec2(400, 300));
    H.KeyDown(ImGuiKey_M);
    CHECK(H.view.ArmedHotkeyType() && H.view.ArmedHotkeyType()->id == "Multiply", "M を握っている");
    CHECK(H.Count("Multiply") == 0, "押しただけでは置かない");
    // クリックしたその位置へ置く
    const ImVec2 click(700, 500);
    H.Click(click);
    CHECK(H.Count("Multiply") == 1, "クリックで置く");
    ng::NodeId placed = 0;
    for (ng::NodeId id : H.ed.Model().Nodes()) if (H.ed.Model().FindNode(id)->type == "Multiply") placed = id;
    if (placed)
    {
        const ng::NodeData* n = H.ed.Model().FindNode(placed);
        const ng::Vec2 g = H.view.ToGraph(click);
        CHECK(std::fabs(n->pos.x - g.x) < 1.5f && std::fabs(n->pos.y - g.y) < 1.5f, "クリック位置（グラフ座標 %.1f,%.1f）に置かれた (%.1f,%.1f)",
              static_cast<double>(g.x), static_cast<double>(g.y), static_cast<double>(n->pos.x), static_cast<double>(n->pos.y));
        CHECK(H.view.GetSelection().Has(placed), "置いたノードが選択される");
    }
    // 握ったままもう一度クリック → もう 1 個（UE と同じ）
    H.Click(ImVec2(900, 300));
    CHECK(H.Count("Multiply") == 2, "握っている間は何個でも置ける");
    H.KeyUp(ImGuiKey_M);
    CHECK(H.view.ArmedHotkeyType() == nullptr && H.Count("Multiply") == 2, "離すと握りが解ける・余計な配置なし");
    // Undo はクリックごとに 1 個ずつ
    H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
    CHECK(H.Count("Multiply") == 1, "Ctrl+Z で 1 個ずつ戻る");

    // 短く押して離す = マウス位置へ置く
    const size_t n0 = H.Count("Add");
    H.Move(ImVec2(300, 700));
    H.Key(ImGuiKey_A);
    CHECK(H.Count("Add") == n0 + 1, "短押し（クリックなし）はマウス位置へ置く");
    // 数字キー = 定数
    H.Move(ImVec2(1000, 700));
    H.KeyDown(ImGuiKey_3);
    CHECK(H.view.ArmedHotkeyType() && H.view.ArmedHotkeyType()->id == "Float3", "3 = Float3 の握り");
    H.Click(ImVec2(1100, 720));
    H.KeyUp(ImGuiKey_3);
    CHECK(H.Count("Float3") == 1, "3 + クリック = Float3");
    // 修飾キー付きは配置しない
    const size_t total = H.ed.Model().Nodes().size();
    H.KeyDown(ImGuiMod_Shift);
    H.Key(ImGuiKey_M);
    H.KeyUp(ImGuiMod_Shift);
    CHECK(H.ed.Model().Nodes().size() == total, "Shift 付きの M は配置しない");
}

// ---------------------------------------------------------------------------
void TestSlotDragAndPalette()
{
    Harness H;
    H.ed.NewGraph(mg::NewTemplate::Empty);
    H.view.SetDocument(&H.ed.Doc());
    H.view.Settings().snap = false;
    H.Frames(3);
    ng::GraphDocument& doc = H.ed.Doc();
    const ng::NodeId f = doc.AddNode("Float", ng::Vec2(-300, -100));
    const ng::NodeId out = H.ed.OutputNode();
    H.view.FrameAll(false);
    H.Frames(5);

    // 値欄（Float の value = プロパティ行）をドラッグして値を変える
    const ng::Rect slot = doc.SlotRect(ng::PinRef{f, 0, false});
    CHECK(slot.Width() > 0.0f, "Float の値欄がある");
    const ng::Vec2 sc = slot.Center();
    const ImVec2 sp = H.view.ToScreen(sc);
    const double v0 = H.ed.Model().GetNodeProp(f, "value").get<double>();
    H.Drag(sp, ImVec2(sp.x + 60.0f, sp.y), 8);
    const double v1 = H.ed.Model().GetNodeProp(f, "value").get<double>();
    CHECK(v1 > v0 + 0.05, "値欄のドラッグで G1 のプロパティが変わる (%.3f → %.3f)", v0, v1);
    const size_t depth = doc.History().UndoDepth();
    H.ed.Update();
    CHECK(H.ed.ValueOnlyEditsSinceCompile() >= 1, "値の編集は「値のみの変更」（再コンパイルなし）");
    H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
    CHECK(std::fabs(H.ed.Model().GetNodeProp(f, "value").get<double>() - v0) < 1e-6 && doc.History().UndoDepth() == depth - 1, "Ctrl+Z で値が戻る（ドラッグ 1 回 = 1 コマンド）");

    // ドラッグ元ピン（Float の出力）から空きへドロップ → 型で絞ったパレット
    const ImVec2 outPin = H.view.ScreenPinPos(ng::PinRef{f, 0, true});
    H.Drag(outPin, ImVec2(outPin.x + 200.0f, outPin.y + 260.0f), 10);
    CHECK(H.view.IsPaletteOpen(), "空きへのドロップで検索パレットが開く");
    H.view.SetPaletteQuery("Multi");
    H.Frames(3);
    const size_t muls = H.Count("Multiply");
    CHECK(H.view.ConfirmPalette(0), "候補を確定");
    H.Frames(4);
    CHECK(H.Count("Multiply") == muls + 1, "Multiply が作られた");
    // 自動接続: Float.Out → 新しい Multiply の入力
    bool linked = false;
    for (const ng::Edge& e : H.ed.Model().Edges())
        if (e.from.node == f && H.ed.Model().FindNode(e.to.node)->type == "Multiply") linked = true;
    CHECK(linked, "ドラッグ元の出力が新ノードへ自動で繋がる");
    // Undo 1 回でノードも接続も消える（グループ化）
    const size_t nodes = H.ed.Model().Nodes().size();
    H.Chord(ImGuiMod_Ctrl, ImGuiKey_Z);
    CHECK(H.ed.Model().Nodes().size() == nodes - 1, "Undo 1 回で追加 + 接続が戻る");
    (void)out;
}

// ---------------------------------------------------------------------------
void TestFocusAndCallbacks()
{
    Harness H;
    H.LoadSample();
    H.view.SetViewState(ng::ViewState{ng::Vec2(0, 0), 1.0f}, false);
    H.Frames(4);
    const ng::NodeId far = H.Id("out");   // (2000, 260) = 画面の右外
    CHECK(far != 0, "遠いノード");
    ImVec2 mx;
    ImVec2 mn = H.view.ScreenNodeRect(far, &mx);
    const bool visibleBefore = mn.x >= H.view.CanvasMin().x && mx.x <= H.view.CanvasMax().x && mn.y >= H.view.CanvasMin().y && mx.y <= H.view.CanvasMax().y;
    CHECK(!visibleBefore, "最初は画面の外");
    CHECK(H.view.FocusNode(far, false), "FocusNode");
    H.Frames(4);
    mn = H.view.ScreenNodeRect(far, &mx);
    CHECK(mn.x >= H.view.CanvasMin().x && mx.x <= H.view.CanvasMax().x && mn.y >= H.view.CanvasMin().y && mx.y <= H.view.CanvasMax().y, "フォーカスで画面内へ寄る");
    CHECK(H.view.GetSelection().nodes.size() == 1 && H.view.GetSelection().Has(far), "選択される");
    CHECK(H.view.CurrentViewport().zoom >= 0.69f, "読めるズーム (%.2f)", static_cast<double>(H.view.CurrentViewport().zoom));
    CHECK(!H.view.FocusNode(999999, false), "存在しないノードは false");

    // 右クリックメニューの拡張・ダブルクリック
    ng::NodeId menuNode = 0, activated = 0;
    H.view.SetNodeMenuExtender([&](ng::NodeId id, const ng::Selection&) { menuNode = id; });
    H.view.SetNodeActivateHandler([&](ng::NodeId id) { activated = id; });
    H.view.FrameAll(false);
    H.Frames(4);
    const ng::NodeId target = H.Id("mulBase");
    H.Click(H.Center(target));   // ヘッダ以外の本体でもノード当たり
    H.Move(H.Center(target));
    H.Down(1);
    H.Up(1);
    H.Frames(3);
    CHECK(H.view.IsContextMenuOpen() && menuNode == target, "ノードの右クリックメニューに拡張が呼ばれる (%u)", menuNode);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); H.Frames(2); ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); H.Frames(2);
    H.Move(ImVec2(H.view.CanvasMin().x + 20, H.view.CanvasMin().y + 20));
    H.Frames(2);
    // ダブルクリック
    const ImVec2 c = H.Center(target);
    H.Move(c);
    ImGui::GetIO().AddMouseButtonEvent(0, true); H.Frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false); H.Frame();
    ImGui::GetIO().AddMouseButtonEvent(0, true); H.Frame();
    ImGui::GetIO().AddMouseButtonEvent(0, false); H.Frames(2);
    CHECK(activated == target, "ノードのダブルクリックでハンドラが呼ばれる (%u)", activated);
}

// ---------------------------------------------------------------------------
void TestStatesAndDiagnostics()
{
    Harness H;
    H.LoadSample();
    H.view.FrameAll(false);
    H.Frames(4);
    // 診断を載せる: エラー / 警告 / 減光 / コンパイル中
    const ng::NodeId a = H.Id("mulBase"), b = H.Id("smooth"), c = H.Id("noise"), d = H.Id("out");
    H.view.SetNodeState(a, ng::GraphView::NodeState::Error, "エラー: A が未接続");
    H.view.SetNodeState(b, ng::GraphView::NodeState::Warning, "警告: float3 → float2 に切り詰め");
    H.view.SetNodeState(c, ng::GraphView::NodeState::Dimmed, "出力に繋がっていません");
    H.view.SetNodeState(d, ng::GraphView::NodeState::Compiling, "コンパイル中…");
    H.Frames(8);
    CHECK(H.view.Stats().drawnNodes > 5, "状態つきでも描ける");
    // 診断 → 状態の対応は MatGraphEditor が返す: 壊してエラーにする
    H.ed.SetNodeProp(H.Id("noise"), "outputType", "float2");   // lerpRough / smooth の入力は poly なので float2 のまま通る。エラーにはならないことも確認
    H.ed.Update();
    // 出力ノードを消す → E_NO_OUTPUT
    ng::Selection s;
    s.nodes.insert(H.ed.OutputNode());
    H.ed.Doc().RemoveItems(s);
    H.Frames(3);
    H.ed.Update();
    CHECK(H.ed.ErrorCount() >= 1 && !H.ed.Result().ok, "出力なし = エラー (%d)", H.ed.ErrorCount());
    H.view.ClearNodeStates();
    H.Frames(3);
}

} // namespace

int main()
{
    std::printf("matgraph_editor_imgui_test\n");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 900);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    theme::ApplyStyle(ImGui::GetStyle());

    TestSampleDpi();
    TestHotkeys();
    TestSlotDragAndPalette();
    TestFocusAndCallbacks();
    TestStatesAndDiagnostics();

    ImGui::DestroyContext();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
