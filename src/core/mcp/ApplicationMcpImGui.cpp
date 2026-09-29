// ===========================================================================
// MCP: 仮想入力モード（AI が人の PC 操作を奪わずにエディタ UI を操作・撮影する口）
// ---------------------------------------------------------------------------
// ★なぜ要るか
//   これまで AI がエディタ UI を触るには実マウス/実キーボードを動かすしかなく（SendInput /
//   SetCursorPos / SetForegroundWindow）、人が PC を使えなくなった。
//   仮想入力モードでは:
//     ・実マウス/実キーボードは ImGui に届かない（Window::WndProc で遮断）
//     ・AI の入力は下のツールでキューに積まれ、ImGui::NewFrame の直前にメインスレッドで
//       io.AddMousePosEvent / AddMouseButtonEvent / AddKeyEvent ... として流し込まれる
//     ・OS のカーソル / フォーカスには一切触れない
//     ・スクショは ImGui を描いた後のバックバッファを読み戻す（PrintWindow を使わない＝背面でも撮れる）
//   純粋ロジックは input/VirtualInput.h、ImGui への流し込みは gui/VirtualInputImGui.{h,cpp}。
//
// ツール:
//   imgui_virtual_input {enable}                        モード切替 / 現在の状態
//   imgui_pointer {action,x,y,button,toX,toY,steps,dx,dy}  ポインタ操作（遅延応答: 全部適用して ImGui が反応してから返る）
//   imgui_key {key,text}                                キー押下 / 文字入力（同上）
//   imgui_find {label,contains}                         ウィンドウ / タブ / 名前つき要素の矩形（座標を推測させない）
//   imgui_screenshot {path}                             ImGui 込みの最終画面
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "gui/VirtualInputImGui.h"

namespace dx12e
{
using namespace appdetail;

namespace
{
using json = nlohmann::json;

json RectJson(float x, float y, float w, float h)
{
    return {{"x", x}, {"y", y}, {"w", w}, {"h", h}};
}
json CenterJson(float x, float y, float w, float h)
{
    return {{"x", x + w * 0.5f}, {"y", y + h * 0.5f}};
}
} // namespace

// ---------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------
json Application::McpVirtualInputState() const
{
    json j;
    j["enabled"] = vinput::Enabled();
    j["background"] = {{"mode", BackgroundModeName(m_bgOptions.mode)},
                       {"toolWindow", m_bgOptions.toolWindow}};
    // 表示倍率。座標(pointer/find/screenshot)は物理クライアント px。論理 px = 物理 ÷ dpiScale
    j["dpiScale"] = m_imguiManager ? m_imguiManager->GetUiScale() : 1.0f;

    const vinput::Queue& q = vinput::Global();
    const vinput::Queue::Applied& st = q.State();
    j["pointer"] = {{"known", st.hasPos}, {"x", st.x}, {"y", st.y},
                    {"left", st.button[0]}, {"right", st.button[1]}, {"middle", st.button[2]},
                    {"note", "エディタウィンドウのクライアント座標（物理 px。表示倍率 dpiScale を掛けた後の座標）。dx12_imgui_screenshot の画像ピクセルと同じ"}};
    j["queue"] = {{"pending", q.Pending()}, {"pumped", q.Pumped()}};

    if (ImGui::GetCurrentContext())
    {
        const ImGuiIO& io = ImGui::GetIO();
        j["client"] = {{"width", io.DisplaySize.x}, {"height", io.DisplaySize.y}};
    }
    if (m_window && m_window->GetHwnd())
    {
        const HWND hwnd = m_window->GetHwnd();
        // ★以下は全部「読むだけ」。OS のカーソル / フォーカスは変えない。
        //   実機検証で「AI の操作中も人のカーソルと前面ウィンドウが動かない」を外から確かめるための材料。
        POINT cp{};
        const bool haveCursor = ::GetCursorPos(&cp) != 0;
        j["window"] = {{"logicalWidth", m_window->GetWidth()}, {"logicalHeight", m_window->GetHeight()},
                       {"visible", ::IsWindowVisible(hwnd) != 0},
                       {"minimized", ::IsIconic(hwnd) != 0},
                       {"isForegroundWindow", ::GetForegroundWindow() == hwnd}};
        if (haveCursor)
            j["osCursor"] = {{"x", cp.x}, {"y", cp.y}, {"note", "読み取りのみ。エンジンは書き込まない"}};
    }
    return j;
}

// ---------------------------------------------------------------------------
// モード切替
// ---------------------------------------------------------------------------
void Application::ApplyVirtualInputMode(bool on)
{
    if (m_isGameMode) return;   // 配布ゲームには無い機能
    if (on == vinput::Enabled()) return;

    if (on)
    {
        vinput::Global().Reset();
        vinput::SetEnabled(true);
    }
    else
    {
        // 先にフラグを落とす（以降の OS 操作が元に戻る）。押下中のボタン/キーは
        // ImGuiManager::SetVirtualInput(false) が「離した」ことにして ImGui へ流す。
        vinput::SetEnabled(false);
    }
    if (m_imguiManager) m_imguiManager->SetVirtualInput(on);
    if (m_inputSystem)  m_inputSystem->OnVirtualModeChanged(on);

    if (!on)
    {
        for (const McpVInputWait& w : m_mcpVInputWaits)
            FailMcp(m_mcpBridge.get(), w.reply, McpErr::ModeConflict,
                    "virtual input mode was turned off before the input finished",
                    "入力の途中でモードを切った。もう一度 dx12_imgui_virtual_input {enable:true} してからやり直す");
        m_mcpVInputWaits.clear();
    }
    Logger::Info("仮想入力モード: {}", on ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// 遅延応答（キューが流れ切って ImGui が反応した後に返す）
// ---------------------------------------------------------------------------
void Application::ServiceMcpVirtualInput()
{
    if (m_mcpVInputWaits.empty()) return;
    const uint64_t pumped = vinput::Global().Pumped();
    const vinput_gui::HoverInfo hover = vinput_gui::QueryHoverInfo();

    for (auto it = m_mcpVInputWaits.begin(); it != m_mcpVInputWaits.end();)
    {
        if (pumped < it->targetPump + static_cast<uint64_t>(it->settle)) { ++it; continue; }
        json r = it->info;
        const vinput::Queue::Applied& st = vinput::Global().State();
        r["pointer"] = {{"x", st.x}, {"y", st.y},
                        {"left", st.button[0]}, {"right", st.button[1]}, {"middle", st.button[2]}};
        r["hover"] = {{"window", hover.hoveredWindow},
                      {"focusedWindow", hover.focusedWindow},
                      {"wantCaptureMouse", hover.wantCaptureMouse},
                      {"wantCaptureKeyboard", hover.wantCaptureKeyboard},
                      {"wantTextInput", hover.wantTextInput}};
        CompleteMcp(m_mcpBridge.get(), it->reply, std::move(r));
        it = m_mcpVInputWaits.erase(it);
    }
}

// ---------------------------------------------------------------------------
// ハンドラ
// ---------------------------------------------------------------------------
// 仮想入力モード ON が前提のツールの入口チェック。
void Application::McpRequireVirtualInput() const
{
    if (m_isGameMode || !m_imguiManager || !ImGui::GetCurrentContext())
        throw McpError(McpErr::ModeConflict, "editor UI is not available",
                       "エディタとして起動していること（配布ゲームには無い）。dx12_ping が通ってから呼ぶ");
    if (!vinput::Enabled())
        throw McpError(McpErr::ModeConflict, "virtual input mode is OFF",
                       "先に dx12_imgui_virtual_input {enable:true}（または起動引数 --virtual-input / "
                       "--background）。OFF のまま入力を積むと人の実入力と混ざるので受け付けない");
}

void Application::RegisterMcpImGuiMethods()
{

    // ---- モード切替 / 状態 ----
    McpDefine("imgui_virtual_input", "enable:bool", DX12E_MCP_HANDLER
        {
            if (params.contains("enable"))
            {
                if (!params["enable"].is_boolean())
                    throw McpError(McpErr::InvalidParam, "enable must be a boolean",
                                   "true で仮想入力モード ON、false で OFF。省略すると現在の状態だけ返す");
                if (m_isGameMode)
                    throw McpError(McpErr::ModeConflict, "editor only",
                                   "配布ゲームには仮想入力モードは無い");
                const bool want = params["enable"].get<bool>();
                const bool was  = vinput::Enabled();
                ApplyVirtualInputMode(want);
                // 実行中に MCP が ON にした物だけ「MCP が切れたら自動で OFF」の対象にする。
                if (!was && want) { m_virtualInputRuntime = true; m_virtualInputOrphanSec = 0.0f; }
                if (!want) m_virtualInputRuntime = false;
            }
            resp["ok"] = true;
            resp["result"] = McpVirtualInputState();
            resp["result"]["note"] =
                "ON の間は実マウス/実キーボードが ImGui に届かず、OS のカーソル/フォーカスにも触れない。"
                "操作は dx12_imgui_pointer / dx12_imgui_key、狙う場所は dx12_imgui_find、"
                "画面は dx12_imgui_screenshot（ImGui 込み）。AI がエディタ UI を触るときは必ずこのモード "
                "+ 起動引数 --background を使うこと";
        });

    // ---- ポインタ ----
    McpDefine("imgui_pointer",
              "action:string,x:number,y:number,button:string,toX:number,toY:number,steps:int,dx:number,dy:number",
              DX12E_MCP_HANDLER
        {
            McpRequireVirtualInput();
            static const std::vector<std::string> kActions =
                {"move", "down", "up", "click", "double_click", "drag", "wheel"};
            const int a = McpEnumParam(params, "action", kActions, -1,
                "action は move / down / up / click / double_click / drag / wheel のどれか");
            if (a < 0)
                throw McpError(McpErr::InvalidParam, "missing 'action'",
                               "action は move / down / up / click / double_click / drag / wheel", kActions);
            const std::string action = kActions[static_cast<size_t>(a)];

            vinput::Button btn = vinput::Button::Left;
            if (!vinput::ParseButton(params.value("button", std::string("left")), btn))
                throw McpError(McpErr::InvalidParam, "unknown button",
                               "button は left / right / middle（既定 left）",
                               {"left", "right", "middle"});

            const bool hasXY = params.contains("x") && params.contains("y");
            const f32 x = hasXY ? McpFloatParam(params, "x", 0.0f, -100000.0f, 100000.0f) : 0.0f;
            const f32 y = hasXY ? McpFloatParam(params, "y", 0.0f, -100000.0f, 100000.0f) : 0.0f;
            auto needXY = [&]()
            {
                if (!hasXY)
                    throw McpError(McpErr::InvalidParam, "x and y are required for " + action,
                                   "エディタウィンドウのクライアント座標(px)。dx12_imgui_find で狙う要素の "
                                   "rect / center を、dx12_imgui_screenshot の画像で目視した座標をそのまま渡せる");
            };

            const vinput::Queue& cq = vinput::Global();
            std::vector<vinput::Frame> frames;
            json info = {{"action", action}, {"button", params.value("button", std::string("left"))}};

            if (action == "move")
            {
                needXY();
                frames = { vinput::Frame{vinput::MakeMove(x, y)} };
            }
            else if (action == "down" || action == "up")
            {
                const bool down = (action == "down");
                if (hasXY) frames.push_back(vinput::Frame{vinput::MakeMove(x, y)});
                else if (!cq.State().hasPos && !cq.TailHasPos())
                    throw McpError(McpErr::InvalidParam, "pointer position is unknown",
                                   "まだ位置が決まっていない。x,y を付けるか、先に move / click する");
                frames.push_back(vinput::Frame{vinput::MakeButton(btn, down)});
            }
            else if (action == "click")
            {
                needXY();
                frames = vinput::MakeClickScript(x, y, btn);
            }
            else if (action == "double_click")
            {
                needXY();
                frames = vinput::MakeDoubleClickScript(x, y, btn);
                info["note"] = "ImGui のダブルクリックは 0.30 秒以内の 2 回押下。フレームが 10fps を切る重い状態では成立しないことがある";
            }
            else if (action == "drag")
            {
                needXY();
                if (!params.contains("toX") || !params.contains("toY"))
                    throw McpError(McpErr::InvalidParam, "toX and toY are required for drag",
                                   "drag は (x,y) から (toX,toY) へ steps フレームかけて補間する");
                const f32 tx = McpFloatParam(params, "toX", 0.0f, -100000.0f, 100000.0f);
                const f32 ty = McpFloatParam(params, "toY", 0.0f, -100000.0f, 100000.0f);
                const int steps = McpIntParam(params, "steps", 12, 1, vinput::kMaxDragSteps);
                frames = vinput::MakeDragScript(x, y, tx, ty, steps, btn);
                info["to"] = {{"x", tx}, {"y", ty}};
                info["steps"] = steps;
            }
            else // wheel
            {
                const f32 dx = McpFloatParam(params, "dx", 0.0f, -1000.0f, 1000.0f);
                const f32 dy = McpFloatParam(params, "dy", 0.0f, -1000.0f, 1000.0f);
                if (dx == 0.0f && dy == 0.0f)
                    throw McpError(McpErr::InvalidParam, "dx or dy is required for wheel",
                                   "ノッチ単位。dy 正 = 上へスクロール（ImGui 流）。x,y を付けるとその位置へ先に動く");
                frames = vinput::MakeWheelScript(hasXY, x, y, dx, dy);
                info["wheel"] = {{"dx", dx}, {"dy", dy}};
            }

            if (!vinput::Global().CanEnqueue(frames.size()))
                throw McpError(McpErr::ModeConflict, "virtual input queue is full",
                               "前の入力がまだ流れている。応答を待ってから次を送る（キューは最大 " +
                               std::to_string(vinput::Queue::kMaxPendingFrames) + " フレーム）");

            if (hasXY)
            {
                info["at"] = {{"x", x}, {"y", y}};
                const ImVec2 ds = ImGui::GetIO().DisplaySize;
                const vinput::ImGuiPoint p = vinput::ClientToImGui(x, y, 0.0f, 0.0f, ds.x, ds.y);
                info["clamped"] = p.clamped;
                if (p.clamped)
                    info["clampedTo"] = {{"x", p.x}, {"y", p.y},
                                         {"note", "クライアント領域の外は丸める（OS ウィンドウを引き出さないため）"}};
                info["client"] = {{"width", ds.x}, {"height", ds.y}};
            }
            info["frames"] = frames.size();

            const uint64_t target = vinput::Global().Enqueue(std::move(frames));
            McpVInputWait w;
            w.reply     = deferred;
            w.targetPump = target;
            w.settle    = 2;
            w.info      = std::move(info);
            m_mcpVInputWaits.push_back(std::move(w));
            isDeferred = true;
        });

    // ---- キー ----
    McpDefine("imgui_key", "key:string,text:string,hold:int", DX12E_MCP_HANDLER
        {
            McpRequireVirtualInput();
            const std::string keyText = params.value("key", std::string());
            const std::string text    = params.value("text", std::string());
            if (keyText.empty() && text.empty())
                throw McpError(McpErr::InvalidParam, "key or text is required",
                               "key に \"F2\" / \"Ctrl+S\" / \"Enter\" / \"Delete\" 等、text に入力したい文字列。"
                               "文字入力は先にテキスト欄をクリックしてアクティブにしておく");

            std::vector<vinput::Frame> frames;
            json info = json::object();
            if (!keyText.empty())
            {
                const vinput::KeyChord chord = vinput::ParseKeyChord(keyText);
                if (!chord.Ok())
                    throw McpError(McpErr::InvalidParam, "invalid key: " + chord.error,
                                   "例: \"F2\" \"Ctrl+S\" \"Ctrl+Shift+Z\" \"Enter\" \"Esc\" \"Delete\" \"Tab\" "
                                   "\"Up\" \"PageDown\" \"A\" \"Ctrl+A\"。修飾は Ctrl / Shift / Alt / Win");
                // hold = 押している長さ（フレーム数、既定 1）。フライカメラの WASD のように押し続けたいときに使う。
                const int hold = McpIntParam(params, "hold", 1, 1, vinput::kMaxKeyHoldFrames);
                frames = vinput::MakeKeyScript(chord, hold);
                info["key"] = keyText;
                info["vk"] = chord.key;
                info["hold"] = hold;
            }
            if (!text.empty())
            {
                std::vector<vinput::Frame> tf = vinput::MakeTextScript(text);
                for (auto& f : tf) frames.push_back(std::move(f));
                info["text"] = text;
            }
            if (!vinput::Global().CanEnqueue(frames.size()))
                throw McpError(McpErr::ModeConflict, "virtual input queue is full",
                               "前の入力がまだ流れている。応答を待ってから次を送る");
            info["frames"] = frames.size();

            const uint64_t target = vinput::Global().Enqueue(std::move(frames));
            McpVInputWait w;
            w.reply      = deferred;
            w.targetPump = target;
            w.settle     = 2;
            w.info       = std::move(info);
            m_mcpVInputWaits.push_back(std::move(w));
            isDeferred = true;
        });

    // ---- 探す ----
    McpDefine("imgui_find", "label:string,contains:bool", DX12E_MCP_HANDLER
        {
            if (m_isGameMode || !ImGui::GetCurrentContext())
                throw McpError(McpErr::ModeConflict, "editor UI is not available",
                               "エディタとして起動していること");
            const std::string label = params.value("label", std::string());
            const bool contains = params.value("contains", true);

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const ImGuiIO& io = ImGui::GetIO();
            const float vx = vp->Pos.x, vy = vp->Pos.y;
            auto toRect = [&](float x, float y, float w, float h)
            {
                float cx = 0.0f, cy = 0.0f;
                vinput::ImGuiToClient(x, y, vx, vy, cx, cy);
                return json{{"rect", RectJson(cx, cy, w, h)},
                            {"center", CenterJson(cx, cy, w, h)},
                            {"screenRect", RectJson(x, y, w, h)}};
            };

            constexpr size_t kMaxWindows = 120, kMaxItems = 300;
            json windows = json::array();
            size_t windowTotal = 0;
            for (const vinput_gui::WindowInfo& w : vinput_gui::CollectWindows(false))
            {
                if (label.empty() ? !w.visible
                                  : !(vinput_gui::MatchLabel(w.name, label, contains)
                                      || vinput_gui::MatchLabel(w.title, label, contains)))
                    continue;
                ++windowTotal;
                if (windows.size() >= kMaxWindows) continue;
                json j = toRect(w.x, w.y, w.w, w.h);
                j["name"] = w.name;
                j["title"] = w.title;
                j["visible"] = w.visible;
                j["focused"] = w.focused;
                j["hovered"] = w.hovered;
                j["collapsed"] = w.collapsed;
                j["docked"] = w.docked;
                j["popup"] = w.isPopup;
                if (w.docked) j["dockTabSelected"] = w.dockTabVisible;
                if (w.hasTabRect) j["tab"] = toRect(w.tx, w.ty, w.tw, w.th);   // ドック内のタブ見出し（クリックで前面へ）
                windows.push_back(std::move(j));
            }

            json items = json::array();
            size_t itemTotal = 0;
            for (const vinput::Anchor& a : vinput::Global().Anchors())
            {
                if (!label.empty() && !vinput_gui::MatchLabel(a.label, label, contains)) continue;
                ++itemTotal;
                if (items.size() >= kMaxItems) continue;
                json j = toRect(a.x0, a.y0, a.x1 - a.x0, a.y1 - a.y0);
                j["kind"] = a.kind;
                j["label"] = a.label;
                if (!a.window.empty()) j["window"] = a.window;
                if (a.hasAux) j["labelRect"] = toRect(a.ax0, a.ay0, a.ax1 - a.ax0, a.ay1 - a.ay0)["rect"];
                items.push_back(std::move(j));
            }

            const vinput_gui::HoverInfo hv = vinput_gui::QueryHoverInfo();
            json r;
            r["query"] = {{"label", label}, {"contains", contains}};
            r["client"] = {{"width", io.DisplaySize.x}, {"height", io.DisplaySize.y}};
            r["windows"] = std::move(windows);
            r["items"] = std::move(items);
            r["counts"] = {{"windows", windowTotal}, {"items", itemTotal},
                           {"windowsReturned", r["windows"].size()}, {"itemsReturned", r["items"].size()},
                           {"anchorsThisFrame", vinput::Global().Anchors().size()}};
            r["hover"] = {{"window", hv.hoveredWindow}, {"focusedWindow", hv.focusedWindow},
                          {"wantCaptureMouse", hv.wantCaptureMouse},
                          {"wantCaptureKeyboard", hv.wantCaptureKeyboard},
                          {"wantTextInput", hv.wantTextInput}};
            r["coordinates"] =
                "rect / center / labelRect / tab はエディタウィンドウのクライアント座標(px)で、"
                "dx12_imgui_pointer の x,y にそのまま渡せる。screenRect は ImGui 内部の（スクリーン）座標。"
                "items は仮想入力モード ON の間に描かれた『名前つき要素』（プロパティ行のラベル/値欄など。"
                "全ウィジェットではない）。見つからなければ windows の rect を基準に dx12_imgui_screenshot で目視する";
            if (!vinput::Enabled())
                r["warning"] = "仮想入力モードが OFF なので items（名前つき要素）は集めていない。"
                               "dx12_imgui_virtual_input {enable:true} の後、1 フレーム描いてから呼ぶ";
            resp["ok"] = true;
            resp["result"] = std::move(r);
        });

    // ---- スクショ（ImGui 込み）----
    McpDefine("imgui_screenshot", "path:string", DX12E_MCP_HANDLER
        {
            if (m_isGameMode || !m_imguiManager)
                throw McpError(McpErr::ModeConflict, "editor UI is not available", "エディタとして起動していること");
            if (m_mcpFinalShot.reply.client != 0 || m_mcpFinalShot.pending || m_deterministicCapture)
                throw McpError(McpErr::ModeConflict,
                    "a screenshot is already pending; retry shortly",
                    "前の imgui_screenshot / screenshot_final の応答を待ってから撮る（通常 1〜2 フレームで返る）");
            m_mcpFinalShot = {};
            m_mcpFinalShot.path      = params.value("path", std::string());
            m_mcpFinalShot.reply     = deferred;
            m_mcpFinalShot.withImGui = true;
            m_mcpFinalShot.pending   = true;   // 次に描くフレームの ImGui 描画後・Present 直前でコピーされる
            isDeferred = true;
        });
}

} // namespace dx12e
