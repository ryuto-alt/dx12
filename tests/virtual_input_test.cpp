// 仮想入力モード（AI が OS のカーソル/フォーカスを奪わずエディタ UI を操作する仕組み）の純粋ロジック。
//
// 守りたい不変条件（input/VirtualInput.h / core/BackgroundMode.h）:
//   (A) キューの順序: 積んだ順に 1 フレームずつ出る。Enqueue の戻り値で「流れ切った」を判定できる。
//   (B) down と up は決して同じフレームに入らない（ImGui のクリック判定はフレーム単位）。
//       クリックは [移動][押す][離す] で、移動は押す前のフレームに置く（ホバー確定のため）。
//   (C) drag は始点→終点を steps フレームで補間し、最後は終点ちょうど。離すのは移動とは別フレーム。
//   (D) 座標変換: クライアント座標 → ImGui 座標（ビューポート原点を足す）。領域外は丸める
//       （OS ウィンドウを引き出さないため）。NaN / inf でも壊れない。
//   (E) 仮想モード ON の間だけ、実マウス/実キーボード/カーソル形状のメッセージを遮断する。
//       OFF なら 1 つも遮断しない（既定 OFF で挙動が完全に同じ）。ウィンドウ管理系は遮断しない。
//   (F) キー名 "Ctrl+S" / "F2" / "Enter" 等の解釈。
//   (G) --background の値の解釈（方式・tool の既定）。
//
// GPU も Windows.h も ImGui も不要（ヘッダオンリーの純粋ロジック）。
// 実行: ctest --output-on-failure -R VirtualInput

#include "input/VirtualInput.h"
#include "core/BackgroundMode.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::vinput;

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

bool feq(float a, float b, float tol = 1e-4f)
{
    return std::fabs(a - b) <= tol * (1.0f + std::fabs(a) + std::fabs(b));
}

// フレーム列の中で、条件を満たすイベントを含む最初のフレーム番号（無ければ -1）。
template <typename Pred>
int FirstFrameWith(const std::vector<Frame>& frames, Pred pred, int from = 0)
{
    for (int i = from; i < static_cast<int>(frames.size()); ++i)
        for (const Event& e : frames[static_cast<size_t>(i)])
            if (pred(e)) return i;
    return -1;
}
bool IsBtn(const Event& e, bool down)
{ return e.type == Event::Type::MouseButton && e.down == down; }
bool IsMove(const Event& e) { return e.type == Event::Type::MousePos; }

// スクリプトを全部 Pump して、各フレームの内容を返す。
std::vector<Frame> Drain(Queue& q)
{
    std::vector<Frame> out;
    while (!q.Idle()) out.push_back(q.Pump());
    return out;
}
} // namespace

int main()
{
    std::printf("virtual_input_test\n");

    // ===== (F) キー名 =====
    {
        KeyChord c = ParseKeyChord("Ctrl+S");
        Check(c.Ok() && c.key == 'S' && c.modifiers.size() == 1 && c.modifiers[0] == kVkControl, "Ctrl+S");
        c = ParseKeyChord("ctrl + shift + z");
        Check(c.Ok() && c.key == 'Z' && c.modifiers.size() == 2 && c.modifiers[0] == kVkControl
              && c.modifiers[1] == kVkShift, "大小・空白を無視した ctrl + shift + z");
        c = ParseKeyChord("F2");
        Check(c.Ok() && c.key == 0x71 && c.modifiers.empty(), "F2 = 0x71");
        c = ParseKeyChord("f12");
        Check(c.Ok() && c.key == 0x7B, "f12 = 0x7B");
        Check(ParseKeyChord("F24").key == 0x87 && !ParseKeyChord("F25").Ok(), "F24 まで");
        c = ParseKeyChord("Enter");
        Check(c.Ok() && c.key == 0x0D, "Enter");
        Check(ParseKeyChord("Return").key == 0x0D && ParseKeyChord("Esc").key == 0x1B
              && ParseKeyChord("Escape").key == 0x1B, "別名 Return / Esc / Escape");
        Check(ParseKeyChord("Delete").key == 0x2E && ParseKeyChord("Del").key == 0x2E, "Delete / Del");
        Check(ParseKeyChord("PageUp").key == 0x21 && ParseKeyChord("PgDn").key == 0x22, "PageUp / PgDn");
        Check(ParseKeyChord("Up").key == 0x26 && ParseKeyChord("Left").key == 0x25, "矢印");
        Check(ParseKeyChord("a").key == 'A' && ParseKeyChord("7").key == '7', "単一文字");
        Check(ParseKeyChord("Num5").key == 0x65, "Num5 = 0x65");
        Check(ParseKeyChord("Space").key == 0x20 && ParseKeyChord(" ").key == 0 /*空白のみは空扱い*/, "Space");
        c = ParseKeyChord("Ctrl++");
        Check(c.Ok() && c.key == 0xBB && c.modifiers.size() == 1, "Ctrl++ はプラスキー");
        c = ParseKeyChord("Shift");
        Check(c.Ok() && c.key == kVkShift && c.modifiers.empty(), "修飾キー単体");
        c = ParseKeyChord("Ctrl+Alt+Delete");
        Check(c.Ok() && c.key == 0x2E && c.modifiers.size() == 2, "Ctrl+Alt+Delete");
        Check(!ParseKeyChord("").Ok() && !ParseKeyChord("Ctrl+").Ok(), "空 / 末尾 + はエラー");
        c = ParseKeyChord("Foo+S");
        Check(!c.Ok() && c.error.find("Foo") != std::string::npos, "修飾でない名前はエラーに名前が出る");
        c = ParseKeyChord("Ctrl+Nope");
        Check(!c.Ok() && c.error.find("Nope") != std::string::npos, "未知の主キーはエラー");
    }

    // ===== ボタン名 =====
    {
        Button b = Button::Middle;
        Check(ParseButton("", b) && b == Button::Left, "空 = left");
        Check(ParseButton("RIGHT", b) && b == Button::Right, "RIGHT");
        Check(ParseButton("middle", b) && b == Button::Middle, "middle");
        Check(!ParseButton("side", b), "未知のボタン");
    }

    // ===== (A) キューの順序と Enqueue の戻り値 =====
    {
        Queue q;
        Check(q.Idle() && q.Pumped() == 0 && q.Pending() == 0, "初期状態は空");
        Frame empty = q.Pump();
        Check(empty.empty() && q.Pumped() == 1, "空でも Pump は 1 フレーム進む");

        const uint64_t t1 = q.Enqueue({Frame{MakeMove(10, 20)}, Frame{MakeMove(30, 40)}});
        Check(t1 == q.Pumped() + 2, "Enqueue の戻り値 = 全部流れた後の Pumped");
        const uint64_t t2 = q.Enqueue({Frame{MakeKey(0x41, true)}});
        Check(t2 == t1 + 1, "続けて積むと戻り値も続く");
        Check(q.Pending() == 3, "3 フレーム待機");

        Frame f = q.Pump();
        Check(f.size() == 1 && f[0].type == Event::Type::MousePos && feq(f[0].x, 10), "先頭から出る(1)");
        Check(q.State().hasPos && feq(q.State().x, 10) && feq(q.State().y, 20), "Pump で適用済み状態が更新");
        f = q.Pump();
        Check(feq(f[0].x, 30), "先頭から出る(2)");
        Check(q.Pumped() >= t1 && q.Pumped() < t2, "1 本目の列は流れ切ったが 2 本目はまだ");
        f = q.Pump();
        Check(f[0].type == Event::Type::Key && f[0].code == 0x41, "先頭から出る(3)");
        Check(q.Pumped() >= t1 && q.Pumped() >= t2, "流れ切ったら Pumped >= 戻り値");
        Check(q.KeyDown(0x41), "キー押下が適用済み状態に反映");
    }

    // ===== (B) クリック: down/up は別フレーム、移動が先 =====
    {
        const std::vector<Frame> s = MakeClickScript(100, 50, Button::Left);
        Check(s.size() == 3, "click は 3 フレーム");
        const int fMove = FirstFrameWith(s, IsMove);
        const int fDown = FirstFrameWith(s, [](const Event& e) { return IsBtn(e, true); });
        const int fUp   = FirstFrameWith(s, [](const Event& e) { return IsBtn(e, false); });
        Check(fMove == 0 && fDown == 1 && fUp == 2, "順序: 移動 → 押す → 離す");
        Check(fDown != fUp, "down と up は同じフレームに入らない");
        Check(feq(s[0][0].x, 100) && feq(s[0][0].y, 50), "移動先の座標");
        Check(s[1][0].code == static_cast<int>(Button::Left), "左ボタン");

        // どの再生成関数でも down と up が同一フレームに同居しない（全パターン総当たり）。
        std::vector<std::vector<Frame>> all;
        all.push_back(MakeClickScript(1, 2, Button::Right));
        all.push_back(MakeDoubleClickScript(1, 2, Button::Middle));
        all.push_back(MakeDragScript(0, 0, 100, 100, 1, Button::Left));
        all.push_back(MakeDragScript(0, 0, 100, 100, 50, Button::Left));
        all.push_back(MakeKeyScript(ParseKeyChord("Ctrl+S")));
        bool noSame = true;
        for (const auto& script : all)
            for (const Frame& f : script)
            {
                bool hasDown = false, hasUp = false;
                for (const Event& e : f)
                {
                    if (e.type != Event::Type::MouseButton && e.type != Event::Type::Key) continue;
                    // 修飾キーの up は主キーの up と同じフレームでよい（それは「離す」同士）。
                    (e.down ? hasDown : hasUp) = true;
                }
                // ボタンの down/up 同居のみ検査（キーは down フレームと up フレームが別であることを別途見る）。
                bool btnDown = false, btnUp = false;
                for (const Event& e : f)
                    if (e.type == Event::Type::MouseButton) (e.down ? btnDown : btnUp) = true;
                if (btnDown && btnUp) noSame = false;
                (void)hasDown; (void)hasUp;
            }
        Check(noSame, "どのスクリプトでもボタンの down/up は同一フレームに入らない");

        // キューを通した適用済み状態: 押している間だけ true。
        Queue q;
        q.Enqueue(MakeClickScript(5, 6, Button::Left));
        q.Pump();
        Check(!q.ButtonDown(Button::Left), "移動フレームではまだ押していない");
        q.Pump();
        Check(q.ButtonDown(Button::Left) && q.State().clickSeq == 1, "押すフレームで押下状態 + clickSeq");
        Check(feq(q.State().clickX, 5) && feq(q.State().clickY, 6), "クリック位置が記録される");
        q.Pump();
        Check(!q.ButtonDown(Button::Left), "離すフレームで解除");
    }

    // ===== ダブルクリック =====
    {
        const std::vector<Frame> s = MakeDoubleClickScript(7, 8, Button::Left);
        Check(s.size() == 5, "double_click は 5 フレーム");
        Check(FirstFrameWith(s, IsMove) == 0, "最初に移動");
        int downs = 0, ups = 0;
        for (const Frame& f : s)
            for (const Event& e : f)
                if (e.type == Event::Type::MouseButton) (e.down ? downs : ups)++;
        Check(downs == 2 && ups == 2, "down 2 回 / up 2 回");
        Check(FirstFrameWith(s, [](const Event& e) { return IsBtn(e, false); })
              < FirstFrameWith(s, [](const Event& e) { return IsBtn(e, true); }, 2),
              "1 回目の up が 2 回目の down より先");
    }

    // ===== (C) ドラッグの補間 =====
    {
        const std::vector<Frame> s = MakeDragScript(10, 20, 110, 220, 10, Button::Left);
        Check(s.size() == 10 + 3, "drag のフレーム数 = steps + 3（始点移動 / 押す / 離す）");
        Check(IsMove(s[0][0]) && feq(s[0][0].x, 10) && feq(s[0][0].y, 20), "始点へ移動");
        Check(IsBtn(s[1][0], true), "始点で押す");
        Check(IsBtn(s.back()[0], false), "最後のフレームで離す（移動とは別フレーム）");
        // 補間区間
        bool monotonic = true, single = true;
        float px = 10, py = 20;
        for (size_t i = 2; i + 1 < s.size(); ++i)
        {
            if (s[i].size() != 1 || !IsMove(s[i][0])) { single = false; continue; }
            const float x = s[i][0].x, y = s[i][0].y;
            if (x < px - 1e-4f || y < py - 1e-4f) monotonic = false;
            px = x; py = y;
        }
        Check(single, "補間フレームは 1 フレーム 1 移動");
        Check(monotonic, "補間は始点→終点へ単調");
        const Event& last = s[s.size() - 2][0];
        Check(IsMove(last) && feq(last.x, 110) && feq(last.y, 220), "離す直前は終点ちょうど");
        const Event& mid = s[2 + 4][0];   // i=5 番目（steps=10 の半分）
        Check(feq(mid.x, 10 + 100 * 0.5f) && feq(mid.y, 20 + 200 * 0.5f), "中間点は線形補間");

        Check(MakeDragScript(0, 0, 10, 10, 0, Button::Left).size() == 1 + 1 + 1 + 1, "steps<1 は 1 に丸める");
        Check(MakeDragScript(0, 0, 10, 10, 100000, Button::Left).size()
              == static_cast<size_t>(kMaxDragSteps) + 3, "steps は kMaxDragSteps に丸める");

        // キューを通すと、押している間に移動し、離すと押下が解除される。
        Queue q;
        q.Enqueue(s);
        q.Pump(); q.Pump();
        Check(q.ButtonDown(Button::Left), "押下中");
        float moved = 0;
        for (int i = 0; i < 10; ++i) q.Pump();
        float dx = 0, dy = 0;
        q.TakePointerDelta(dx, dy);
        moved = dx;
        Check(feq(moved, 100 + 0 /*始点移動は最初の位置なので delta なし*/), "押下中に移動した量 = 終点 - 始点");
        Check(feq(dy, 200), "縦の移動量");
        q.Pump();
        Check(!q.ButtonDown(Button::Left), "最後に離す");
        q.TakePointerDelta(dx, dy);
        Check(dx == 0.0f && dy == 0.0f, "TakePointerDelta は取り出すと 0 に戻る");
    }

    // ===== ホイール =====
    {
        std::vector<Frame> s = MakeWheelScript(true, 30, 40, 0, -3);
        Check(s.size() == 2 && IsMove(s[0][0]) && s[1][0].type == Event::Type::MouseWheel
              && feq(s[1][0].y, -3), "位置あり: 先に移動 → ホイール");
        s = MakeWheelScript(false, 0, 0, 1, 2);
        Check(s.size() == 1 && s[0][0].type == Event::Type::MouseWheel && feq(s[0][0].x, 1)
              && feq(s[0][0].y, 2), "位置なし: ホイールのみ");
    }

    // ===== キー / テキスト =====
    {
        const KeyChord c = ParseKeyChord("Ctrl+Shift+S");
        const std::vector<Frame> s = MakeKeyScript(c);
        Check(s.size() == 2, "キーは 2 フレーム（押す / 離す）");
        Check(s[0].size() == 3 && s[0][0].code == kVkControl && s[0][0].down
              && s[0][1].code == kVkShift && s[0][2].code == 'S' && s[0][2].down,
              "修飾 → 主キーの順に押す");
        Check(s[1].size() == 3 && s[1][0].code == 'S' && !s[1][0].down
              && s[1][1].code == kVkShift && s[1][2].code == kVkControl, "主キー → 修飾の逆順に離す");

        Queue q;
        q.Enqueue(s);
        q.Pump();
        Check(q.KeyDown('S') && q.KeyDown(kVkControl) && q.KeyDown(kVkShift), "押下フレームで全部 down");
        q.Pump();
        Check(!q.KeyDown('S') && !q.KeyDown(kVkControl) && !q.KeyDown(kVkShift), "離すフレームで全部 up");

        // UTF-8 テキスト（日本語 3 バイト / 絵文字 4 バイト / ASCII）
        const std::vector<Frame> t = MakeTextScript("aあ\xF0\x9F\x98\x80z");
        Check(t.size() == 1, "テキストは 1 フレームにまとめる");
        Check(t[0].size() == 4 && t[0][0].code == 'a' && t[0][1].code == 0x3042
              && t[0][2].code == 0x1F600 && t[0][3].code == 'z', "UTF-8 → コードポイント");
        Check(MakeTextScript("").empty(), "空文字は何も積まない");
    }

    // ===== (D) 座標変換 =====
    {
        ImGuiPoint p = ClientToImGui(100, 50, 200, 300, 1920, 1080);
        Check(feq(p.x, 300) && feq(p.y, 350) && !p.clamped, "ビューポート原点を足す（マルチビューポートはスクリーン座標）");
        p = ClientToImGui(-5, 2000, 0, 0, 1920, 1080);
        Check(p.clamped && feq(p.x, 0) && feq(p.y, 1079), "領域外は [0,size-1] へ丸める");
        p = ClientToImGui(5000, 5000, -32000, -32000, 1920, 1080);
        Check(feq(p.x, -32000 + 1919) && feq(p.y, -32000 + 1079), "画面外へ置いた窓（負の原点）でも丸めてから足す");
        p = ClientToImGui(std::nanf(""), INFINITY, 0, 0, 100, 100);
        Check(std::isfinite(p.x) && std::isfinite(p.y), "NaN / inf でも有限値");
        p = ClientToImGui(10, 10, 0, 0, 0, 0);
        Check(feq(p.x, 0) && feq(p.y, 0), "大きさ 0 でも壊れない");
        float cx = 0, cy = 0;
        ImGuiToClient(300, 350, 200, 300, cx, cy);
        Check(feq(cx, 100) && feq(cy, 50), "逆変換");
    }

    // ===== (E) 実入力の遮断 =====
    {
        SetEnabled(false);
        const unsigned realMsgs[] = {
            0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207, 0x0208, 0x0209,
            0x020A, 0x020B, 0x020C, 0x020D, 0x020E, 0x0100, 0x0101, 0x0102, 0x0104, 0x0105,
            0x0106, 0x00FF, 0x0020, 0x0007, 0x0008, 0x00A0, 0x02A2, 0x02A3 };
        bool noneBlocked = true;
        for (unsigned m : realMsgs) if (BlocksRealInput(m)) noneBlocked = false;
        Check(noneBlocked, "OFF の間は 1 つも遮断しない（既定 OFF で挙動が同じ）");
        Check(!Enabled() && MayTouchOsCursor(), "OFF: OS のカーソルに触れてよい");

        SetEnabled(true);
        bool allBlocked = true;
        for (unsigned m : realMsgs) if (!BlocksRealInput(m)) allBlocked = false;
        Check(allBlocked, "ON の間は実マウス/実キーボード/カーソル形状/フォーカスを全部遮断");
        Check(Enabled() && !MayTouchOsCursor(), "ON: OS のカーソルに触れない");

        // ウィンドウ管理・描画・DPI・終了は遮断しない（窓が閉じられない/リサイズできないは困る）。
        const unsigned passMsgs[] = {
            0x0001 /*CREATE*/, 0x0002 /*DESTROY*/, 0x0005 /*SIZE*/, 0x000F /*PAINT*/, 0x0010 /*CLOSE*/,
            0x0012 /*QUIT*/, 0x0014 /*ERASEBKGND*/, 0x0021 /*MOUSEACTIVATE*/, 0x007E /*DISPLAYCHANGE*/,
            0x0083 /*NCCALCSIZE*/, 0x0084 /*NCHITTEST*/, 0x00A1 /*NCLBUTTONDOWN*/, 0x0112 /*SYSCOMMAND*/,
            0x0219 /*DEVICECHANGE*/, 0x02E0 /*DPICHANGED*/, 0x0281 /*IME_SETCONTEXT*/, 0x010F /*IME_COMPOSITION*/ };
        bool nonePassBlocked = true;
        for (unsigned m : passMsgs) if (BlocksRealInput(m)) nonePassBlocked = false;
        Check(nonePassBlocked, "窓の管理/描画/DPI/終了系は遮断しない");

        // 仮想入力キュー自体はモードと独立に動く（OFF でも積める＝呼び出し側で弾く）。
        SetEnabled(false);
    }

    // ===== 仮想キー状態（GetAsyncKeyState の代替）=====
    {
        Global().Reset();
        Global().Enqueue({Frame{MakeKey(0x57, true)}, Frame{MakeButton(Button::Right, true)}});
        Global().Pump();
        Check(VirtualKeyDown(0x57), "仮想 W が押されている");
        Check(!VirtualKeyDown(0x02) && !VirtualRightMouseDown(), "右ボタンはまだ");
        Global().Pump();
        Check(VirtualKeyDown(0x02) && VirtualRightMouseDown(), "VK_RBUTTON(2) はボタン状態から答える");
        Check(!VirtualKeyDown(0x01) && !VirtualKeyDown(0x04), "左/中ボタンは押していない");
        Check(!VirtualKeyDown(-1) && !VirtualKeyDown(999), "範囲外の VK は false");
        Global().Reset();
        Check(!VirtualKeyDown(0x57) && !VirtualRightMouseDown(), "Reset で全部離れる");
    }

    // ===== 全部離す / 上限 / アンカー =====
    {
        Queue q;
        q.Enqueue({Frame{MakeButton(Button::Left, true), MakeKey(0x11, true), MakeKey(0x53, true)}});
        q.Pump();
        const Frame rel = q.MakeReleaseAllFrame();
        int btn = 0, key = 0;
        for (const Event& e : rel)
        {
            if (e.down) btn = -100;
            if (e.type == Event::Type::MouseButton) ++btn;
            if (e.type == Event::Type::Key) ++key;
        }
        Check(btn == 1 && key == 2, "押しっぱなしのボタン 1 + キー 2 を離すフレームを作る");
        Queue idle;
        Check(idle.MakeReleaseAllFrame().empty(), "何も押していなければ空");

        Queue big;
        std::vector<Frame> many(Queue::kMaxPendingFrames);
        Check(big.CanEnqueue(many.size()), "上限ちょうどは積める");
        big.Enqueue(many);
        Check(!big.CanEnqueue(1), "上限を超える分は積めない（暴走の保険）");
        big.ClearPending();
        Check(big.Idle() && big.CanEnqueue(1), "ClearPending で空に戻る");

        Queue a;
        Anchor an; an.kind = "property"; an.label = "Position";
        a.AddAnchor(an); a.AddAnchor(an);
        Check(a.Anchors().size() == 2, "アンカーが積める");
        a.ClearAnchors();
        Check(a.Anchors().empty(), "毎フレーム捨てられる");
    }

    // ===== ClearPending は適用済み位置を保つ =====
    {
        Queue q;
        q.Enqueue({Frame{MakeMove(1, 2)}});
        q.Pump();
        q.Enqueue({Frame{MakeMove(50, 60)}});
        Check(feq(q.TailX(), 50), "積んだ列の終わりの位置");
        q.ClearPending();
        Check(feq(q.TailX(), 1) && feq(q.TailY(), 2) && q.State().hasPos, "捨てると、実際に適用済みの位置へ戻る");
    }

    // ===== キーを押し続ける（hold）=====
    {
        const KeyChord w = ParseKeyChord("W");
        Check(MakeKeyScript(w).size() == 2, "既定 hold=1 は 2 フレーム");
        const std::vector<Frame> s = MakeKeyScript(w, 5);
        Check(s.size() == 6, "hold=5 は 押す + 4 空 + 離す = 6 フレーム");
        Check(s[0][0].down && s[1].empty() && s[4].empty() && !s[5][0].down, "間は空フレーム、最後で離す");
        Check(MakeKeyScript(w, 0).size() == 2 && MakeKeyScript(w, 999999).size() == static_cast<size_t>(kMaxKeyHoldFrames) + 1,
              "hold は 1..kMaxKeyHoldFrames に丸める");
        Queue q;
        q.Enqueue(s);
        q.Pump();
        bool heldAll = q.KeyDown('W');
        for (int i = 0; i < 4; ++i) { q.Pump(); heldAll = heldAll && q.KeyDown('W'); }
        Check(heldAll, "押している間は仮想 W が押されたまま（GetAsyncKeyState の代替が true）");
        q.Pump();
        Check(!q.KeyDown('W'), "最後のフレームで離れる");
    }

    // ===== 人の脱出口（Ctrl+Alt+Shift+F12 → OFF 要求）=====
    {
        (void)ConsumeEscape();   // 前のテストの残りを捨てる
        Check(!ConsumeEscape(), "要求が無ければ false");
        RequestEscape();
        Check(ConsumeEscape(), "要求を 1 回だけ受け取れる");
        Check(!ConsumeEscape(), "受け取ったら消える");
    }

    // ===== (G) --background =====
    {
        BackgroundOptions o;
        std::string err;
        Check(ParseBackgroundOption("", o, err) && o.mode == BackgroundMode::Offscreen && o.toolWindow,
              "引数のみ = 既定 offscreen（+ タスクバー非表示）");
        Check(ParseBackgroundOption("minimized", o, err) && o.mode == BackgroundMode::Minimized && !o.toolWindow,
              "minimized は tool 既定 OFF");
        Check(ParseBackgroundOption("noactivate", o, err) && o.mode == BackgroundMode::NoActivate,
              "noactivate");
        Check(ParseBackgroundOption("hidden", o, err) && o.mode == BackgroundMode::Hidden && o.toolWindow,
              "hidden");
        Check(ParseBackgroundOption("noactivate,tool", o, err) && o.mode == BackgroundMode::NoActivate && o.toolWindow,
              "noactivate,tool");
        Check(ParseBackgroundOption("offscreen,notool", o, err) && o.mode == BackgroundMode::Offscreen && !o.toolWindow,
              "offscreen,notool");
        Check(ParseBackgroundOption(" Tool , MINIMIZED ", o, err) && o.mode == BackgroundMode::Minimized && o.toolWindow,
              "大小・空白・順序を問わない");
        Check(ParseBackgroundOption("tool", o, err) && o.mode == BackgroundMode::Offscreen && o.toolWindow,
              "修飾のみ = 方式は既定 offscreen");
        err.clear();
        Check(!ParseBackgroundOption("fullscreen", o, err) && err.find("fullscreen") != std::string::npos,
              "未知の値はエラーに値が出る");
        Check(o.Active(), "失敗しても直前に確定した値は壊さない");
        Check(BackgroundOptions{}.Active() == false && BackgroundOptions{}.mode == BackgroundMode::None,
              "既定は None（挙動が従来と同じ）");
        Check(kBackgroundClientWidth == 1920 && kBackgroundClientHeight == 1080, "論理解像度は 1920x1080");
        Check(std::string(BackgroundModeName(BackgroundMode::Offscreen)) == "offscreen"
              && std::string(BackgroundModeName(BackgroundMode::None)) == "none", "方式名");
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
