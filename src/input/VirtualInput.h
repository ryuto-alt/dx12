#pragma once

// ===========================================================================
// 仮想入力モード（純粋ロジック）
// ---------------------------------------------------------------------------
// ★なぜ要るか
//   AI(MCP)がエディタの UI を操作・撮影するとき、OS の実マウスカーソル / フォーカス /
//   キーボードに触ると、人が PC を使えなくなる（実際に一度起きた）。
//   そこで「AI の入力は OS を通さない」経路を作る:
//     ・実マウス/実キーボードのイベントは ImGui へ渡さない（Window::WndProc で遮断）
//     ・AI からの入力はこのキューに積み、ImGui::NewFrame の直前に
//       io.AddMousePosEvent / AddMouseButtonEvent / AddKeyEvent ... で流し込む
//     ・OS カーソルに触る処理（SetCursorPos / ClipCursor / ShowCursor / SetCapture /
//       SetForegroundWindow / SetFocus）は仮想モード中は全部無効
//
// ★このヘッダは Windows.h / ImGui / GPU に依存しない（ヘッダオンリー）。
//   キューの順序・down/up の間隔・drag の補間・座標変換・実入力の遮断判定を
//   窓も GPU も無しでユニットテストできるようにするため（tests/virtual_input_test.cpp）。
//   ImGui への流し込みは gui/VirtualInputImGui.{h,cpp}。
//
// 座標はすべて【エディタウィンドウのクライアント座標(px)】。ImGui の座標系
//（マルチビューポート有効時はスクリーン座標）への変換は ClientToImGui() が担当する。
// ===========================================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace dx12e::vinput
{

// ---------------------------------------------------------------------------
// 1. グローバルスイッチ（Window / InputSystem / パネルが参照する）
// ---------------------------------------------------------------------------
inline std::atomic<bool>& EnabledFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
inline bool Enabled() { return EnabledFlag().load(std::memory_order_relaxed); }
inline void SetEnabled(bool on) { EnabledFlag().store(on, std::memory_order_relaxed); }

// 人の脱出口: 仮想入力モード中は実キーボードが ImGui に届かないので、モードを外せるのは AI（MCP）だけになる。
// AI が居なくなった / 暴走した場合に人が取り戻せるよう、Ctrl+Alt+Shift+F12（実キーボード）で OFF を要求できる。
// WndProc が立て、Application がフレーム境界で拾って OFF にする（スレッド間は atomic のみ）。
inline std::atomic<bool>& EscapeFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
inline void RequestEscape() { EscapeFlag().store(true, std::memory_order_relaxed); }
inline bool ConsumeEscape() { return EscapeFlag().exchange(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// 2. 実入力メッセージの分類（Window::WndProc が「ImGui へ渡さない」を決める）
// ---------------------------------------------------------------------------
// ImGui の Win32 バックエンドが実入力として食べるメッセージと、InputSystem が実入力として
// 食べるもの。WM_* の値は Windows.h に依存しないよう数値で持つ（値は Win32 の ABI で不変）。
//   ★WM_SYSKEYDOWN 系（Alt+F4 等）は「ImGui へ渡さない」だけで DefWindowProc には流す
//    ＝ウィンドウを閉じる操作までは奪わない。
inline bool IsRealInputMessage(unsigned msg)
{
    switch (msg)
    {
    case 0x0020:                          // WM_SETCURSOR
    case 0x0007: case 0x0008:             // WM_SETFOCUS / WM_KILLFOCUS
    case 0x00A0:                          // WM_NCMOUSEMOVE
    case 0x00FF:                          // WM_INPUT（Raw Input のマウス移動量）
    case 0x0100: case 0x0101:             // WM_KEYDOWN / WM_KEYUP
    case 0x0102: case 0x0103:             // WM_CHAR / WM_DEADCHAR
    case 0x0104: case 0x0105:             // WM_SYSKEYDOWN / WM_SYSKEYUP
    case 0x0106: case 0x0107:             // WM_SYSCHAR / WM_SYSDEADCHAR
    case 0x0200:                          // WM_MOUSEMOVE
    case 0x0201: case 0x0202: case 0x0203:   // WM_LBUTTONDOWN / UP / DBLCLK
    case 0x0204: case 0x0205: case 0x0206:   // WM_RBUTTON*
    case 0x0207: case 0x0208: case 0x0209:   // WM_MBUTTON*
    case 0x020A:                          // WM_MOUSEWHEEL
    case 0x020B: case 0x020C: case 0x020D:   // WM_XBUTTON*
    case 0x020E:                          // WM_MOUSEHWHEEL
    case 0x02A2: case 0x02A3:             // WM_NCMOUSELEAVE / WM_MOUSELEAVE
        return true;
    default:
        return false;
    }
}
// 仮想モード ON かつ実入力メッセージ → true（呼び出し側は ImGui / InputSystem へ渡さない）。
inline bool BlocksRealInput(unsigned msg) { return Enabled() && IsRealInputMessage(msg); }

// ---------------------------------------------------------------------------
// 3. キー名（"Ctrl+S" / "F2" / "Enter" ...）→ VK コード
// ---------------------------------------------------------------------------
constexpr int kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12, kVkLWin = 0x5B;

inline bool IsModifierVk(int vk)
{
    return vk == kVkShift || vk == kVkControl || vk == kVkMenu || vk == kVkLWin;
}

namespace detail
{
inline std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}
} // namespace detail

// 名前 1 つ → VK。未知なら 0。修飾キー名（Ctrl / Shift / Alt / Win）も返す。
inline int VkFromName(const std::string& rawName)
{
    const std::string n = detail::Lower(detail::Trim(rawName));
    if (n.empty()) return 0;

    if (n.size() == 1)
    {
        const char c = n[0];
        if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
        if (c >= '0' && c <= '9') return c;
        switch (c)
        {
        case ';': return 0xBA;  case '=': return 0xBB;  case ',': return 0xBC;
        case '-': return 0xBD;  case '.': return 0xBE;  case '/': return 0xBF;
        case '`': return 0xC0;  case '[': return 0xDB;  case '\\': return 0xDC;
        case ']': return 0xDD;  case '\'': return 0xDE; case ' ': return 0x20;
        default: return 0;
        }
    }
    // F1..F24
    if (n[0] == 'f' && n.size() <= 3)
    {
        int num = 0;
        bool ok = true;
        for (size_t i = 1; i < n.size(); ++i)
        {
            if (n[i] < '0' || n[i] > '9') { ok = false; break; }
            num = num * 10 + (n[i] - '0');
        }
        if (ok && num >= 1 && num <= 24) return 0x70 + (num - 1);
    }
    // Num0..Num9 / Numpad0..
    {
        size_t skip = 0;
        if (n.rfind("numpad", 0) == 0) skip = 6;
        else if (n.rfind("num", 0) == 0) skip = 3;
        if (skip > 0 && n.size() == skip + 1 && n[skip] >= '0' && n[skip] <= '9')
            return 0x60 + (n[skip] - '0');
    }

    struct Named { const char* name; int vk; };
    static const Named kNamed[] = {
        {"ctrl", kVkControl}, {"control", kVkControl}, {"shift", kVkShift},
        {"alt", kVkMenu}, {"menu", kVkMenu}, {"win", kVkLWin}, {"super", kVkLWin},
        {"cmd", kVkLWin}, {"meta", kVkLWin},
        {"enter", 0x0D}, {"return", 0x0D}, {"esc", 0x1B}, {"escape", 0x1B},
        {"tab", 0x09}, {"space", 0x20}, {"spacebar", 0x20},
        {"backspace", 0x08}, {"bksp", 0x08}, {"delete", 0x2E}, {"del", 0x2E},
        {"insert", 0x2D}, {"ins", 0x2D}, {"home", 0x24}, {"end", 0x23},
        {"pageup", 0x21}, {"pgup", 0x21}, {"pagedown", 0x22}, {"pgdn", 0x22},
        {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
        {"capslock", 0x14},
        {"multiply", 0x6A}, {"add", 0x6B}, {"subtract", 0x6D}, {"decimal", 0x6E}, {"divide", 0x6F},
        {"semicolon", 0xBA}, {"equals", 0xBB}, {"plus", 0xBB}, {"comma", 0xBC},
        {"minus", 0xBD}, {"period", 0xBE}, {"dot", 0xBE}, {"slash", 0xBF},
        {"grave", 0xC0}, {"graveaccent", 0xC0}, {"backtick", 0xC0},
        {"leftbracket", 0xDB}, {"backslash", 0xDC}, {"rightbracket", 0xDD},
        {"apostrophe", 0xDE}, {"quote", 0xDE},
    };
    for (const Named& e : kNamed)
        if (n == e.name) return e.vk;
    return 0;
}

// "Ctrl+Shift+S" → 修飾キー列 + 主キー。失敗時は error に理由。
struct KeyChord
{
    std::vector<int> modifiers;   // 押す順（Ctrl, Shift, Alt, Win）
    int              key = 0;     // 主キー（VK）。修飾キー単体（"Shift"）のときは modifiers が空で key=修飾キー
    std::string      error;
    bool Ok() const { return error.empty() && key != 0; }
};

inline KeyChord ParseKeyChord(const std::string& text)
{
    KeyChord out;
    const std::string s = detail::Trim(text);
    if (s.empty()) { out.error = "key is empty"; return out; }

    // '+' 区切り。ただし末尾が "++"（例 "Ctrl++"）や "+" 単体は「プラスキー」を指す。
    std::vector<std::string> tokens;
    {
        std::string body = s;
        std::string last;
        if (body == "+") { tokens.push_back("+"); body.clear(); }
        else if (body.size() >= 2 && body.compare(body.size() - 2, 2, "++") == 0)
        {
            last = "plus";
            body.erase(body.size() - 2);   // "Ctrl++" → "Ctrl"
        }
        size_t pos = 0;
        while (!body.empty() && pos <= body.size())
        {
            const size_t plus = body.find('+', pos);
            tokens.push_back(body.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
            if (plus == std::string::npos) break;
            pos = plus + 1;
        }
        if (!last.empty()) tokens.push_back(last);
        if (tokens.size() == 1 && tokens[0] == "+") tokens[0] = "plus";
    }
    if (tokens.empty()) { out.error = "key is empty"; return out; }

    for (size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        const int vk = VkFromName(tokens[i]);
        if (!IsModifierVk(vk))
        {
            out.error = "'" + detail::Trim(tokens[i]) + "' is not a modifier (Ctrl / Shift / Alt / Win)";
            return out;
        }
        if (std::find(out.modifiers.begin(), out.modifiers.end(), vk) == out.modifiers.end())
            out.modifiers.push_back(vk);
    }
    out.key = VkFromName(tokens.back());
    if (out.key == 0)
    {
        out.error = "unknown key '" + detail::Trim(tokens.back()) + "'";
        return out;
    }
    // "Shift" 単体のような修飾キーのみの指定は、その修飾キー自体を主キーとして押す。
    if (out.modifiers.size() == 1 && IsModifierVk(out.key) && out.modifiers[0] == out.key)
        out.modifiers.clear();
    return out;
}

// ---------------------------------------------------------------------------
// 4. イベントとフレーム
// ---------------------------------------------------------------------------
enum class Button : int { Left = 0, Right = 1, Middle = 2 };

// 名前 → ボタン。空は Left。未知なら false。
inline bool ParseButton(const std::string& name, Button& out)
{
    const std::string n = detail::Lower(detail::Trim(name));
    if (n.empty() || n == "left" || n == "l")       { out = Button::Left;   return true; }
    if (n == "right" || n == "r")                    { out = Button::Right;  return true; }
    if (n == "middle" || n == "m" || n == "mid")     { out = Button::Middle; return true; }
    return false;
}

struct Event
{
    enum class Type : uint8_t { MousePos, MouseButton, MouseWheel, Key, Char };
    Type     type = Type::MousePos;
    float    x = 0.0f, y = 0.0f;   // MousePos: クライアント座標 / MouseWheel: (dx, dy)（ImGui 流: 正 = 上/右）
    int      code = 0;             // MouseButton: Button / Key: VK / Char: Unicode コードポイント
    bool     down = false;         // MouseButton / Key
};

// 1 フレームぶんの入力。ImGui の NewFrame ごとに 1 つだけ Pump() が取り出す。
using Frame = std::vector<Event>;

inline Event MakeMove(float x, float y)
{ Event e; e.type = Event::Type::MousePos; e.x = x; e.y = y; return e; }
inline Event MakeButton(Button b, bool down)
{ Event e; e.type = Event::Type::MouseButton; e.code = static_cast<int>(b); e.down = down; return e; }
inline Event MakeWheel(float dx, float dy)
{ Event e; e.type = Event::Type::MouseWheel; e.x = dx; e.y = dy; return e; }
inline Event MakeKey(int vk, bool down)
{ Event e; e.type = Event::Type::Key; e.code = vk; e.down = down; return e; }
inline Event MakeChar(uint32_t cp)
{ Event e; e.type = Event::Type::Char; e.code = static_cast<int>(cp); return e; }

// ---------------------------------------------------------------------------
// 5. スクリプト生成（純関数）。どれも「down と up を同じフレームに入れない」を守る。
//    ImGui のクリック判定はフレーム単位（IsMouseClicked / IsMouseReleased）なので、
//    同一フレームに down/up を入れると押下が消える（トリクルに任せず自前で分ける）。
// ---------------------------------------------------------------------------
constexpr int kMaxDragSteps = 600;

inline std::vector<Frame> MakeClickScript(float x, float y, Button b)
{
    // [移動][押す][離す]。移動を 1 フレーム前に置くのは、ホバー状態（HoveredId）を
    // 確定させてから押すため。
    return { Frame{MakeMove(x, y)}, Frame{MakeButton(b, true)}, Frame{MakeButton(b, false)} };
}

inline std::vector<Frame> MakeDoubleClickScript(float x, float y, Button b)
{
    // ImGui のダブルクリックは「前回押下からの経過時間 < MouseDoubleClickTime(0.30s) かつ
    // 距離 < MouseDoubleClickMaxDist」。フレーム間隔が長い（重いシーン）と時間切れになるので、
    // 2 回目の押下はできるだけ詰める（離した次のフレームで押す）。
    return { Frame{MakeMove(x, y)},
             Frame{MakeButton(b, true)}, Frame{MakeButton(b, false)},
             Frame{MakeButton(b, true)}, Frame{MakeButton(b, false)} };
}

inline std::vector<Frame> MakeDragScript(float fx, float fy, float tx, float ty, int steps, Button b)
{
    steps = std::clamp(steps, 1, kMaxDragSteps);
    std::vector<Frame> out;
    out.push_back(Frame{MakeMove(fx, fy)});          // 始点へ移動（ホバー確定）
    out.push_back(Frame{MakeButton(b, true)});       // 押す
    for (int i = 1; i <= steps; ++i)                 // 1 フレーム 1 移動で補間（最後は必ず終点ちょうど）
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        out.push_back(Frame{i == steps ? MakeMove(tx, ty)
                                       : MakeMove(fx + (tx - fx) * t, fy + (ty - fy) * t)});
    }
    out.push_back(Frame{MakeButton(b, false)});      // 離す（移動とは別フレーム）
    return out;
}

inline std::vector<Frame> MakeWheelScript(bool hasPos, float x, float y, float dx, float dy)
{
    std::vector<Frame> out;
    if (hasPos) out.push_back(Frame{MakeMove(x, y)});   // 先にホバー
    out.push_back(Frame{MakeWheel(dx, dy)});
    return out;
}

constexpr int kMaxKeyHoldFrames = 600;

// holdFrames = 押している長さ（フレーム数）。1 なら [押す][離す] の 2 フレーム。フライカメラの WASD のように
// 「押し続ける」入力は holdFrames を大きくする（押下と押下の間は空フレーム）。
inline std::vector<Frame> MakeKeyScript(const KeyChord& chord, int holdFrames = 1)
{
    holdFrames = std::clamp(holdFrames, 1, kMaxKeyHoldFrames);
    // [修飾キー押す + 主キー押す][主キー離す + 修飾キー離す]。ImGui の Ctrl+S 判定は
    // 主キーの down フレームで修飾が保持されていることが要る。
    Frame down, up;
    for (int m : chord.modifiers) down.push_back(MakeKey(m, true));
    down.push_back(MakeKey(chord.key, true));
    up.push_back(MakeKey(chord.key, false));
    for (auto it = chord.modifiers.rbegin(); it != chord.modifiers.rend(); ++it)
        up.push_back(MakeKey(*it, false));
    std::vector<Frame> out;
    out.push_back(std::move(down));
    for (int i = 1; i < holdFrames; ++i) out.push_back(Frame{});   // 押したまま待つフレーム
    out.push_back(std::move(up));
    return out;
}

// UTF-8 → コードポイント列（不正バイトは U+FFFD）。
inline std::vector<uint32_t> Utf8ToCodepoints(const std::string& s)
{
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu); len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < s.size())
        { cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu); len = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < s.size())
        { cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12)
             | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu); len = 4; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

inline std::vector<Frame> MakeTextScript(const std::string& utf8)
{
    // 文字は 1 フレームにまとめて流す（ImGui は 1 フレームで複数文字を受けられる）。
    Frame f;
    for (uint32_t cp : Utf8ToCodepoints(utf8)) f.push_back(MakeChar(cp));
    if (f.empty()) return {};
    return { std::move(f) };
}

// ---------------------------------------------------------------------------
// 6. 座標変換（クライアント座標 → ImGui 座標）
// ---------------------------------------------------------------------------
struct ImGuiPoint { float x = 0.0f, y = 0.0f; bool clamped = false; };

// クライアント座標を [0, w-1] × [0, h-1] へ丸め、ビューポート原点を足す。
//   ★丸める理由: 仮想ポインタがクライアント領域の外へ出ると ImGui のマルチビューポートが
//   「窓を引き出した」と解釈して OS ウィンドウを増やしかねない（＝実カーソルのある場所に
//   新しい窓が出る）。AI の入力ではそれを起こさない。
//   viewportPos は GetMainViewport()->Pos（マルチビューポート有効時はスクリーン座標）。
inline ImGuiPoint ClientToImGui(float cx, float cy, float vpX, float vpY, float width, float height)
{
    ImGuiPoint p;
    const float maxX = std::max(0.0f, width - 1.0f);
    const float maxY = std::max(0.0f, height - 1.0f);
    const float nx = std::clamp(std::isfinite(cx) ? cx : 0.0f, 0.0f, maxX);
    const float ny = std::clamp(std::isfinite(cy) ? cy : 0.0f, 0.0f, maxY);
    p.clamped = (nx != cx) || (ny != cy);
    p.x = vpX + nx;
    p.y = vpY + ny;
    return p;
}
// 逆変換（find が ImGui 座標をクライアント座標へ直す）。
inline void ImGuiToClient(float ix, float iy, float vpX, float vpY, float& cx, float& cy)
{
    cx = ix - vpX;
    cy = iy - vpY;
}

// ---------------------------------------------------------------------------
// 7. アンカー（find が返す「名前で狙える場所」）。パネルが描画時に登録する。
// ---------------------------------------------------------------------------
struct Anchor
{
    std::string kind;     // "property" / "header" / "button" / "row" ...
    std::string label;
    std::string window;   // 所属ウィンドウ名（分かれば）
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // ImGui 座標（スクリーン座標）。クリック対象の矩形
    bool  hasAux = false;                   // property の「ラベル側」矩形など
    float ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
};

// ---------------------------------------------------------------------------
// 8. キュー本体
// ---------------------------------------------------------------------------
class Queue
{
public:
    // ★保険: MCP が壊れて何万フレームも積んでも無限に溜めない。
    static constexpr size_t kMaxPendingFrames = 4000;

    // 適用済み（Pump を通った）入力の状態。GetAsyncKeyState / 仮想カーソル描画の元。
    struct Applied
    {
        bool     hasPos = false;
        float    x = 0.0f, y = 0.0f;          // 直近のポインタ位置（クライアント座標）
        bool     button[3] = {false, false, false};
        bool     key[256] = {};
        uint64_t clickSeq = 0;                // ボタンを押すたびに +1（波紋の合図）
        float    clickX = 0.0f, clickY = 0.0f;
        int      clickButton = 0;
        uint64_t moveSeq = 0;                 // 位置イベントのたびに +1
    };

    // 積む。戻り値 = 「この列の最後のフレームが Pump された直後の Pumped() の値」。
    //   完了待ちは `Pumped() >= 戻り値` で判定できる。
    uint64_t Enqueue(std::vector<Frame> frames)
    {
        for (Frame& f : frames)
        {
            for (const Event& e : f)
                if (e.type == Event::Type::MousePos) { m_tailHasPos = true; m_tailX = e.x; m_tailY = e.y; }
            m_frames.push_back(std::move(f));
        }
        return m_pumped + m_frames.size();
    }

    // 追加できるか（保険の上限）。
    bool CanEnqueue(size_t frames) const { return m_frames.size() + frames <= kMaxPendingFrames; }

    // 1 フレームぶん取り出して適用済み状態へ反映する。積まれていなければ空フレーム。
    Frame Pump()
    {
        Frame f;
        if (!m_frames.empty())
        {
            f = std::move(m_frames.front());
            m_frames.pop_front();
        }
        ++m_pumped;
        for (const Event& e : f) Apply(e);
        return f;
    }

    size_t   Pending() const { return m_frames.size(); }
    uint64_t Pumped() const { return m_pumped; }
    bool     Idle() const { return m_frames.empty(); }

    // 「積んだ列の終わりでのポインタ位置」。ボタンだけの操作（down/up を位置なしで）に使う。
    bool  TailHasPos() const { return m_tailHasPos; }
    float TailX() const { return m_tailX; }
    float TailY() const { return m_tailY; }

    const Applied& State() const { return m_applied; }
    bool KeyDown(int vk) const { return vk >= 0 && vk < 256 && m_applied.key[vk]; }
    bool ButtonDown(Button b) const { return m_applied.button[static_cast<int>(b)]; }

    // 直前の TakePointerDelta() 以降にポインタが動いた量（エディタカメラの視点回転に使う）。
    void TakePointerDelta(float& dx, float& dy) { dx = m_deltaX; dy = m_deltaY; m_deltaX = m_deltaY = 0.0f; }

    // 押しっぱなしのボタン/キーを離すイベントを 1 フレームぶん作って先頭へ積む
    // （仮想モードを切る / ドラッグ中に MCP が切れたときの後始末）。
    Frame MakeReleaseAllFrame() const
    {
        Frame f;
        for (int b = 0; b < 3; ++b)
            if (m_applied.button[b]) f.push_back(MakeButton(static_cast<Button>(b), false));
        for (int k = 0; k < 256; ++k)
            if (m_applied.key[k]) f.push_back(MakeKey(k, false));
        return f;
    }

    // 積んだ物を捨てる。適用済みの状態（押下中のボタン/キー）は残す（後始末は MakeReleaseAllFrame）。
    void ClearPending() { m_frames.clear(); m_tailHasPos = m_applied.hasPos; m_tailX = m_applied.x; m_tailY = m_applied.y; }
    // 全部初期状態へ（テスト用 / 仮想モードを切った後）。Pumped は数え直さない。
    void Reset()
    {
        m_frames.clear();
        m_applied = Applied{};
        m_tailHasPos = false; m_tailX = m_tailY = 0.0f;
        m_deltaX = m_deltaY = 0.0f;
    }

    // アンカー（find 用）。仮想モード ON の間だけ、毎フレーム描画側が積み直す。
    void ClearAnchors() { m_anchors.clear(); }
    void AddAnchor(Anchor a) { if (m_anchors.size() < kMaxAnchors) m_anchors.push_back(std::move(a)); }
    const std::vector<Anchor>& Anchors() const { return m_anchors; }

private:
    static constexpr size_t kMaxAnchors = 6000;

    void Apply(const Event& e)
    {
        switch (e.type)
        {
        case Event::Type::MousePos:
            if (m_applied.hasPos) { m_deltaX += e.x - m_applied.x; m_deltaY += e.y - m_applied.y; }
            m_applied.hasPos = true;
            m_applied.x = e.x;
            m_applied.y = e.y;
            ++m_applied.moveSeq;
            break;
        case Event::Type::MouseButton:
        {
            const int b = std::clamp(e.code, 0, 2);
            m_applied.button[b] = e.down;
            if (e.down)
            {
                ++m_applied.clickSeq;
                m_applied.clickX = m_applied.x;
                m_applied.clickY = m_applied.y;
                m_applied.clickButton = b;
            }
            break;
        }
        case Event::Type::Key:
            if (e.code >= 0 && e.code < 256) m_applied.key[e.code] = e.down;
            break;
        case Event::Type::MouseWheel:
        case Event::Type::Char:
            break;
        }
    }

    std::deque<Frame> m_frames;
    uint64_t          m_pumped = 0;
    Applied           m_applied;
    bool              m_tailHasPos = false;
    float             m_tailX = 0.0f, m_tailY = 0.0f;
    float             m_deltaX = 0.0f, m_deltaY = 0.0f;
    std::vector<Anchor> m_anchors;
};

// アプリ全体で 1 つのキュー（メインスレッド専用。MCP のハンドラも Poll から呼ばれる）。
inline Queue& Global()
{
    static Queue q;
    return q;
}

// ---------------------------------------------------------------------------
// 9. OS に触る操作をやっていいか（呼び出し側が "if (!vinput::MayTouchOs()) return;" と書く）
// ---------------------------------------------------------------------------
inline bool MayTouchOsCursor() { return !Enabled(); }

// ---------------------------------------------------------------------------
// 10. 仮想キー状態の読み出し（GetAsyncKeyState の代わり。仮想モード中だけ意味を持つ）
// ---------------------------------------------------------------------------
inline bool VirtualKeyDown(int vk)
{
    // VK_LBUTTON(1) / VK_RBUTTON(2) / VK_MBUTTON(4) はキーではなくボタンの状態から答える
    // （GetAsyncKeyState(VK_RBUTTON) の置き換えが同じ呼び方で動くように）。
    switch (vk)
    {
    case 0x01: return Global().ButtonDown(Button::Left);
    case 0x02: return Global().ButtonDown(Button::Right);
    case 0x04: return Global().ButtonDown(Button::Middle);
    default:   return Global().KeyDown(vk);
    }
}
inline bool VirtualRightMouseDown() { return Global().ButtonDown(Button::Right); }

} // namespace dx12e::vinput
