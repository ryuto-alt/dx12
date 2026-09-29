// 仮想入力モード: ImGui への流し込みを【窓も GPU も無しで】実際の ImGui に対して確かめる統合テスト。
//
// ImGui のコンテキストだけを作り（バックエンド無し・OS ウィンドウ無し）、仮想入力キューから
// NewFrame の直前に io.Add*Event で流し込み、ImGui が本当に反応するかを見る。
//   (1) クリック: ボタンがちょうど 1 回押される（down/up を別フレームに分けた効果）
//   (2) 実入力の遮断: バックエンドが NewFrame で積んだ「実マウス位置 / 修飾キー」を捨てると、
//       仮想ポインタが実カーソルに上書きされない
//   (3) キー: Ctrl+S が IsKeyChordPressed で 1 回だけ立つ
//   (4) 文字入力: テキスト欄をクリックしてアクティブにすると、text が入る（日本語含む）
//   (5) ドラッグ: スライダーの値が補間された移動で変わる
//   (6) ホイール: そのフレームだけ io.MouseWheel に乗る
//   (7) 仮想カーソル: 位置が決まってから描かれ、クリックで波紋が増える。ForegroundDrawList に描く
//   (8) find: ウィンドウ一覧に位置とサイズが出る / アンカー登録は仮想モード ON の間だけ
//   (9) 仮想モード ON で ImGui にカーソル形状を触らせない（OS のカーソルに触らない）
//
// 実行: ctest --output-on-failure -R VirtualInputImGui

#include "gui/VirtualInputImGui.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#pragma warning(pop)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what);
    }
}
bool feq(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b)); }

// テスト用 UI の観測結果。
struct Ui
{
    int    clicks = 0;
    ImVec2 btnMin{}, btnMax{};
    char   buf[64] = "";
    ImVec2 inMin{}, inMax{};
    float  slider = 0.0f;
    ImVec2 slMin{}, slMax{};
    int    chordPressed = 0;
    float  wheelSeen = 0.0f;
    bool   simulateBackendJunk = false;   // NewFrame でバックエンドが実入力を積んだ状況を再現
    int    cursorPrims = -1;
    int    fgVtx = 0;
    bool   drawCursor = false;
};

// 1 フレーム: （実バックエンドの真似）→ 仮想キューを流し込む → UI を描く。
void Frame(Ui& ui)
{
    ImGuiIO& io = ImGui::GetIO();
    vinput::Queue& q = vinput::Global();

    // ---- ImGuiManager::BeginFrame の仮想モード分岐と同じ手順 ----
    const int keep = vinput_gui::InputQueueSize();
    if (ui.simulateBackendJunk)
    {
        // Win32 バックエンドの NewFrame が積むもの: 実マウスの現在位置と、修飾キーの整合。
        io.AddMousePosEvent(5.0f, 5.0f);
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_LeftShift, true);
    }
    vinput_gui::TruncateInputQueue(keep);

    q.ClearAnchors();
    const vinput::Frame f = q.Pump();
    vinput_gui::ApplyContext ctx;
    ctx.viewportPos    = ImGui::GetMainViewport()->Pos;
    ctx.displaySize    = io.DisplaySize;
    ctx.mainViewportId = ImGui::GetMainViewport()->ID;
    vinput_gui::ApplyFrame(io, f, ctx);

    ImGui::NewFrame();

    if (io.MouseWheel != 0.0f) ui.wheelSeen = io.MouseWheel;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_None)) ++ui.chordPressed;

    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_Always);
    ImGui::Begin("TestWin", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                 | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button("Hit", ImVec2(120, 30))) ++ui.clicks;
    ui.btnMin = ImGui::GetItemRectMin(); ui.btnMax = ImGui::GetItemRectMax();
    vinput_gui::AnchorLastItem("button", "Hit");

    ImGui::SetNextItemWidth(300);
    ImGui::InputText("##t", ui.buf, sizeof(ui.buf));
    ui.inMin = ImGui::GetItemRectMin(); ui.inMax = ImGui::GetItemRectMax();

    ImGui::SetNextItemWidth(300);
    ImGui::SliderFloat("##s", &ui.slider, 0.0f, 100.0f, "%.1f");
    ui.slMin = ImGui::GetItemRectMin(); ui.slMax = ImGui::GetItemRectMax();
    ImGui::End();

    if (ui.drawCursor) vinput_gui::DrawVirtualCursor();
    if (ui.drawCursor)
    {
        ui.cursorPrims = vinput_gui::LastCursorPrimCount();
        ui.fgVtx = ImGui::GetForegroundDrawList(ImGui::GetMainViewport())->VtxBuffer.Size;
    }
    ImGui::EndFrame();
}

// キューが空になるまで + 反応待ちの数フレーム。
void RunUntilIdle(Ui& ui, int extra = 3)
{
    int guard = 0;
    while (!vinput::Global().Idle() && guard++ < 5000) Frame(ui);
    for (int i = 0; i < extra; ++i) Frame(ui);
}

ImVec2 Center(ImVec2 a, ImVec2 b) { return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }
} // namespace

int main()
{
    std::printf("virtual_input_imgui_test\n");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime   = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);   // バックエンド無しでもフォントを組んでおく

    vinput::SetEnabled(true);
    vinput::Global().Reset();
    vinput_gui::SetImGuiVirtualMode(true);
    Check((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0,
          "(9) 仮想モード ON で ImGui に OS のカーソル形状を触らせない");

    Ui ui;
    for (int i = 0; i < 3; ++i) Frame(ui);   // レイアウト確定

    // ===== (1) クリック =====
    {
        Check(ui.btnMax.x > ui.btnMin.x, "ボタンの矩形が取れている");
        const ImVec2 c = Center(ui.btnMin, ui.btnMax);
        vinput::Global().Enqueue(vinput::MakeClickScript(c.x, c.y, vinput::Button::Left));
        RunUntilIdle(ui);
        Check(ui.clicks == 1, "(1) 仮想クリックでボタンがちょうど 1 回押される");

        // 何も無い所をクリックしても増えない。
        vinput::Global().Enqueue(vinput::MakeClickScript(900, 600, vinput::Button::Left));
        RunUntilIdle(ui);
        Check(ui.clicks == 1, "(1) 外をクリックしても押されない");

        // ダブルクリックは 2 回押される（ボタンは押下ごとに反応）。
        const ImVec2 c2 = Center(ui.btnMin, ui.btnMax);
        vinput::Global().Enqueue(vinput::MakeDoubleClickScript(c2.x, c2.y, vinput::Button::Left));
        RunUntilIdle(ui);
        Check(ui.clicks == 3, "(1) double_click で 2 回押される");
    }

    // ===== (2) 実入力の遮断 =====
    {
        vinput::Global().Enqueue({vinput::Frame{vinput::MakeMove(700, 400)}});
        RunUntilIdle(ui, 1);
        Check(feq(io.MousePos.x, 700) && feq(io.MousePos.y, 400), "(2) 仮想ポインタの位置が ImGui に届く");
        ui.simulateBackendJunk = true;
        for (int i = 0; i < 4; ++i) Frame(ui);
        Check(feq(io.MousePos.x, 700) && feq(io.MousePos.y, 400),
              "(2) バックエンドが積んだ実マウス位置(5,5)は捨てられ、仮想ポインタが上書きされない");
        Check(!io.KeyCtrl && !io.KeyShift, "(2) バックエンドが積んだ修飾キーは捨てられる");
        ui.simulateBackendJunk = false;
    }

    // ===== (3) キー =====
    {
        vinput::Global().Enqueue(vinput::MakeKeyScript(vinput::ParseKeyChord("Ctrl+S")));
        RunUntilIdle(ui);
        Check(ui.chordPressed == 1, "(3) Ctrl+S が IsKeyChordPressed でちょうど 1 回立つ");
        Check(!io.KeyCtrl, "(3) 離した後は修飾キーも解除されている");
    }

    // ===== (4) 文字入力 =====
    {
        const ImVec2 c = Center(ui.inMin, ui.inMax);
        vinput::Global().Enqueue(vinput::MakeClickScript(c.x, c.y, vinput::Button::Left));
        RunUntilIdle(ui);
        Check(io.WantTextInput, "(4) テキスト欄をクリックするとテキスト入力がアクティブになる");
        vinput::Global().Enqueue(vinput::MakeTextScript("abc\xE3\x81\x82"));
        RunUntilIdle(ui);
        Check(std::string(ui.buf) == "abc\xE3\x81\x82", "(4) text がテキスト欄に入る（日本語含む）");
        // Backspace で 1 文字（日本語 1 文字 = 3 バイト）消える。
        vinput::Global().Enqueue(vinput::MakeKeyScript(vinput::ParseKeyChord("Backspace")));
        RunUntilIdle(ui);
        Check(std::string(ui.buf) == "abc", "(4) Backspace で 1 文字消える");
        // 何も無い所をクリックして欄を閉じる。
        vinput::Global().Enqueue(vinput::MakeClickScript(900, 650, vinput::Button::Left));
        RunUntilIdle(ui);
        Check(!io.WantTextInput, "(4) 外をクリックするとテキスト入力が終わる");
    }

    // ===== (5) ドラッグ =====
    {
        const float y = (ui.slMin.y + ui.slMax.y) * 0.5f;
        const float x0 = ui.slMin.x + 10.0f;
        const float x1 = ui.slMin.x + (ui.slMax.x - ui.slMin.x) * 0.5f;
        const float before = ui.slider;
        vinput::Global().Enqueue(vinput::MakeDragScript(x0, y, x1, y, 12, vinput::Button::Left));
        RunUntilIdle(ui);
        Check(ui.slider > before + 20.0f && ui.slider < 80.0f, "(5) drag でスライダーが動く（補間された移動で追従）");
        Check(!io.MouseDown[0], "(5) drag の後はボタンが離れている");
    }

    // ===== (6) ホイール =====
    {
        ui.wheelSeen = 0.0f;
        vinput::Global().Enqueue(vinput::MakeWheelScript(true, 300, 200, 0.0f, -3.0f));
        RunUntilIdle(ui, 0);
        Check(feq(ui.wheelSeen, -3.0f), "(6) wheel が io.MouseWheel に乗る");
    }

    // ===== (7) 仮想カーソルの描画 =====
    {
        vinput::Global().Reset();
        vinput_gui::ResetVirtualCursorState();
        ui.drawCursor = true;
        Frame(ui);
        Check(ui.cursorPrims == 0, "(7) 位置が決まる前は何も描かない");
        vinput::Global().Enqueue({vinput::Frame{vinput::MakeMove(400, 300)}});
        Frame(ui); Frame(ui);
        Check(ui.cursorPrims >= 3, "(7) 位置が決まると矢印 + タグを描く");
        Check(ui.fgVtx > 0, "(7) ForegroundDrawList（メインビューポート）に頂点が積まれる");
        const int base = ui.cursorPrims;
        vinput::Global().Enqueue(vinput::MakeClickScript(400, 300, vinput::Button::Left));
        Frame(ui); Frame(ui); Frame(ui);
        Check(ui.cursorPrims > base - 1, "(7) クリック中は波紋/押下リングが増える");
        Frame(ui);
        ui.drawCursor = false;
    }

    // ===== (8) find: ウィンドウ一覧とアンカー =====
    {
        Frame(ui);
        const auto wins = vinput_gui::CollectWindows(false);
        const vinput_gui::WindowInfo* w = nullptr;
        for (const auto& x : wins) if (x.name == "TestWin") w = &x;
        Check(w != nullptr, "(8) ウィンドウ一覧に TestWin が出る");
        if (w)
        {
            Check(feq(w->x, 100) && feq(w->y, 100) && feq(w->w, 400) && feq(w->h, 300), "(8) 位置とサイズ");
            Check(w->visible && !w->collapsed, "(8) visible");
        }
        Check(vinput_gui::MatchLabel("TestWin", "testwin", false), "(8) 完全一致は大小無視");
        Check(vinput_gui::MatchLabel("TestWin##id", "win", true) && !vinput_gui::MatchLabel("TestWin", "win", false),
              "(8) contains / 完全一致の違い");
        Check(vinput_gui::MatchLabel("anything", "", false), "(8) label 空は全件");

        bool anchorFound = false;
        for (const auto& a : vinput::Global().Anchors())
            if (a.label == "Hit" && a.kind == "button" && a.x1 > a.x0 && a.window == "TestWin") anchorFound = true;
        Check(anchorFound, "(8) 仮想モード ON の間はアンカー（Hit ボタン）が登録される");

        vinput::SetEnabled(false);
        vinput::Global().ClearAnchors();
        Frame(ui);
        Check(vinput::Global().Anchors().empty(), "(8) 仮想モード OFF の間はアンカーを集めない");
        vinput::SetEnabled(true);

        const vinput_gui::HoverInfo hv = vinput_gui::QueryHoverInfo();
        (void)hv;   // 実マウスが無いので hovered は不定。取得が例外なく動くことだけ確かめる
    }

    // ===== キー対応表 =====
    {
        Check(vinput_gui::VkToImGuiKey('A') == ImGuiKey_A && vinput_gui::VkToImGuiKey('Z') == ImGuiKey_Z, "VK A/Z");
        Check(vinput_gui::VkToImGuiKey('0') == ImGuiKey_0 && vinput_gui::VkToImGuiKey('9') == ImGuiKey_9, "VK 0/9");
        Check(vinput_gui::VkToImGuiKey(0x71) == ImGuiKey_F2 && vinput_gui::VkToImGuiKey(0x7B) == ImGuiKey_F12, "VK F2/F12");
        Check(vinput_gui::VkToImGuiKey(0x0D) == ImGuiKey_Enter && vinput_gui::VkToImGuiKey(0x1B) == ImGuiKey_Escape,
              "VK Enter/Esc");
        Check(vinput_gui::VkToImGuiKey(0x2E) == ImGuiKey_Delete && vinput_gui::VkToImGuiKey(0x25) == ImGuiKey_LeftArrow,
              "VK Delete/Left");
        Check(vinput_gui::VkToImGuiKey(0xFF) == ImGuiKey_None, "未知の VK は None");
    }

    // ===== モードを切ると設定が戻る =====
    {
        vinput_gui::SetImGuiVirtualMode(false);
        Check((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0, "OFF で NoMouseCursorChange が戻る");
    }

    vinput::SetEnabled(false);
    ImGui::DestroyContext();

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
