#include "Window.h"
#include "Logger.h"
#include "input/InputSystem.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <algorithm>
#include <numeric>      // std::gcd

// ImGui Win32 WndProc handler (forward declaration)
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dx12e
{

Window::~Window()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Window::Initialize(HINSTANCE hInstance, int /*nCmdShow*/,
                         u32 width, u32 height, const wchar_t* title,
                         bool deferShow, bool startMaximized)
{
    m_width = width;
    m_height = height;
    m_title = title;
    m_startMaximized = startMaximized;

    // exe に埋め込んだアプリアイコン（resources/app.ico, IDI_APPICON=101）を読む。
    // 大（タスクバー/Alt+Tab）と小（タイトルバー）を別サイズで読み、失敗時は既定にフォールバック。
    HICON appIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101 /*IDI_APPICON*/),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON appIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101 /*IDI_APPICON*/),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = sizeof(Window*);
    wc.hInstance     = hInstance;
    wc.hIcon         = appIcon ? appIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = L"DX12EngineWindowClass";
    wc.hIconSm       = appIconSm ? appIconSm : LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        Logger::Critical("ウィンドウクラスの登録に失敗しました");
        throw std::runtime_error("ウィンドウクラスの登録に失敗しました");
    }

    // クライアント領域が指定サイズになるよう調整
    RECT rect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE);

    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    if (!startMaximized)
    {
        // 指定解像度のまま表示するモード: 作業領域に収まらない場合はアスペクト比を保って
        // クライアント領域を縮める（比率が崩れると UI の ScaleToFit がレターボックスを作る）。
        // 収まるサイズが確定したら作業領域の中央に置く。
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        {
            const LONG frameW  = (rect.right - rect.left) - static_cast<LONG>(m_width);
            const LONG frameH  = (rect.bottom - rect.top) - static_cast<LONG>(m_height);
            const LONG maxCliW = (wa.right - wa.left) - frameW;
            const LONG maxCliH = (wa.bottom - wa.top) - frameH;
            if (maxCliW > 0 && maxCliH > 0
                && (static_cast<LONG>(m_width) > maxCliW || static_cast<LONG>(m_height) > maxCliH))
            {
                // 既約アスペクト比の整数倍へ切り下げて比を厳密に維持する。
                // 浮動小数の切り捨てで比が僅かに崩れると UI の ScaleToFit が
                // 1px 級のレターボックス（線状の余白）を作るため。
                const u32 g  = std::gcd(m_width, m_height);
                const u32 rw = m_width / g, rh = m_height / g;
                const u32 n  = std::min(static_cast<u32>(maxCliW) / rw,
                                        static_cast<u32>(maxCliH) / rh);
                if (n >= 1 && rw <= 64)
                {
                    m_width  = rw * n;
                    m_height = rh * n;
                }
                else
                {
                    // 既約比が粗すぎる/入らない場合: 幅基準で縮め、高さは比から丸めて導出
                    const float s = std::min(maxCliW / static_cast<float>(m_width),
                                             maxCliH / static_cast<float>(m_height));
                    const u32 w  = std::max(1u, static_cast<u32>(m_width * s));
                    const u32 h  = std::max(1u, static_cast<u32>(
                        (static_cast<u64>(w) * m_height + m_width / 2) / m_width));
                    m_width  = w;
                    m_height = h;
                }
                rect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
                AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE);
                Logger::Info("ウィンドウを作業領域に合わせて縮小: {}x{}", m_width, m_height);
            }
            winX = wa.left + ((wa.right - wa.left) - (rect.right - rect.left)) / 2;
            winY = wa.top  + ((wa.bottom - wa.top) - (rect.bottom - rect.top)) / 2;
        }
    }

    m_hwnd = CreateWindowExW(
        0,
        L"DX12EngineWindowClass",
        m_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        winX, winY,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        this  // WndProc で取り出す用
    );

    if (!m_hwnd)
    {
        Logger::Critical("ウィンドウの作成に失敗しました");
        throw std::runtime_error("ウィンドウの作成に失敗しました");
    }

    if (!startMaximized)
    {
        // 実クライアント領域が指定サイズと一致するか実測して補正する。
        // OS のフレーム計算が AdjustWindowRect の想定とずれるとクライアントが数 px 狂い、
        // アスペクト比の崩れ = ScaleToFit の余白として見えるため。
        RECT cr{};
        if (GetClientRect(m_hwnd, &cr)
            && (cr.right != static_cast<LONG>(m_width) || cr.bottom != static_cast<LONG>(m_height)))
        {
            RECT wr{};
            GetWindowRect(m_hwnd, &wr);
            SetWindowPos(m_hwnd, nullptr, 0, 0,
                         (wr.right - wr.left) + (static_cast<LONG>(m_width)  - cr.right),
                         (wr.bottom - wr.top) + (static_cast<LONG>(m_height) - cr.bottom),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Logger::Info("クライアント領域を補正: {}x{} -> {}x{}",
                         cr.right, cr.bottom, m_width, m_height);
            m_resized = false;   // スワップチェイン生成前の確定なのでリサイズ扱いにしない
        }
    }

    // deferShow=true なら表示しない（重い初期化中に白い未応答ウィンドウを見せないため。
    // 初回フレームの先行描画が済んでから Show() で表示する）
    if (deferShow)
    {
        // 隠れたまま作業領域サイズへ広げておく。これで初期化〜先行描画が最初から
        // ほぼ最終解像度で行われ、表示時（最大化）のリサイズ差分が最小になる。
        // WM_SIZE は同期的に届き m_width/m_height を更新するので、この後に作られる
        // スワップチェイン/RT は最初からこのサイズになる。
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
            SetWindowPos(m_hwnd, nullptr, wa.left, wa.top,
                         wa.right - wa.left, wa.bottom - wa.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        m_resized = false;   // 初期サイズ確定はリサイズ扱いにしない（初回フレームの全RT再生成を防ぐ）
    }
    else
    {
        Show();
    }

    Logger::Info("Window created: {}x{} (deferShow={})", m_width, m_height, deferShow);
}

void Window::Show()
{
    if (!m_hwnd || IsWindowVisible(m_hwnd)) return;
    ShowWindow(m_hwnd, m_startMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(m_hwnd);
    SetForegroundWindow(m_hwnd);
}

void Window::EnableCustomTitleBar()
{
    if (!m_hwnd || m_customTitleBar) return;
    m_customTitleBar = true;
    // WM_NCCALCSIZE を発火させてフレームを再計算(キャプション領域をクライアントに取り込む)
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Logger::Info("カスタムタイトルバー有効化");
}

void Window::SetTitle(const std::wstring& title)
{
    if (m_hwnd)
        SetWindowTextW(m_hwnd, title.c_str());
}

void Window::ToggleFullscreen()
{
    if (!m_fullscreen)
    {
        // ウィンドウ → ボーダレスフルスクリーン
        GetWindowRect(m_hwnd, &m_windowedRect);

        // スタイルをボーダレスに変更
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);

        // モニター情報取得
        HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitor, &mi);

        SetWindowPos(m_hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);

        ShowWindow(m_hwnd, SW_MAXIMIZE);
        m_fullscreen = true;

        Logger::Info("Fullscreen enabled");
    }
    else
    {
        // ボーダレスフルスクリーン → ウィンドウ
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX | WS_VISIBLE);

        SetWindowPos(m_hwnd, HWND_NOTOPMOST,
            m_windowedRect.left, m_windowedRect.top,
            m_windowedRect.right - m_windowedRect.left,
            m_windowedRect.bottom - m_windowedRect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);

        ShowWindow(m_hwnd, SW_NORMAL);
        m_fullscreen = false;

        Logger::Info("Windowed mode restored");
    }
}

bool Window::ProcessMessages()
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            m_shouldClose = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !m_shouldClose;
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Window* window = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    else
    {
        window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // マウスキャプチャ中は WM_SETCURSOR を自前で処理してカーソルを消す
    if (window && window->m_inputSystem && window->m_inputSystem->IsMouseCaptured()
        && msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
    {
        SetCursor(nullptr);
        return TRUE;
    }

    // ImGui にイベントを渡す（キー/マウス等は結果を無視して InputSystem にも常に通知する）
    LRESULT imguiResult = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

    if (window)
    {
        switch (msg)
        {
        // IME確定文字の二重入力バグ修正: ImGui_ImplWin32 は WM_IME_COMPOSITION を内部で
        // 一度 DefWindowProcW に渡しており、GCS_RESULTSTR（変換確定）時はその呼び出し内で
        // Windows 標準IME処理が確定文字列の WM_CHAR を生成する。ここで関数末尾の
        // DefWindowProcW にも同じメッセージを流すと確定文字列がもう一度 WM_CHAR 化され、
        // 日本語(IME)入力のたびに文字が二重に書き込まれる。ImGui側の結果を返して打ち切る。
        case WM_IME_COMPOSITION:
            return imguiResult;

        // ===== カスタムタイトルバー =====
        // WM_NCCALCSIZE: 標準キャプション分をクライアント領域へ取り込む(左右下のリサイズ枠は残す)。
        // フルスクリーン(WS_POPUP)中はOS側にキャプションが無いので素通し。
        case WM_NCCALCSIZE:
            if (window->m_customTitleBar && wParam == TRUE && !window->m_fullscreen)
            {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                const LONG originalTop = params->rgrc[0].top;
                DefWindowProcW(hwnd, msg, wParam, lParam);   // 左右下の枠を標準計算
                params->rgrc[0].top = originalTop;           // 上端はキャプション無しで窓の縁まで
                if (IsZoomed(hwnd))
                {
                    // 最大化中は枠が画面外にはみ出す仕様のため、その分だけ下げないと上端が切れる
                    const int frame = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    params->rgrc[0].top += frame;
                }
                return 0;
            }
            break;

        // WM_NCHITTEST: 上端のリサイズ帯(キャプション除去で標準判定から漏れる分)と、
        // ImGui側が「アイテムに乗ってない」と報告したタイトルバー帯のドラッグ(HTCAPTION)を自前判定。
        // HTCAPTION を返すだけで移動ドラッグ・スナップ・ダブルクリック最大化を全部OSがやってくれる。
        case WM_NCHITTEST:
            if (window->m_customTitleBar && !window->m_fullscreen)
            {
                const LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
                if (hit != HTCLIENT) return hit;   // 左右下のリサイズ枠などはそのまま
                POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                if (!IsZoomed(hwnd))
                {
                    const int frame = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                    if (pt.y >= 0 && pt.y < frame) return HTTOP;
                }
                if (window->m_captionDraggable && pt.y < static_cast<LONG>(window->m_captionHeight))
                    return HTCAPTION;
                return HTCLIENT;
            }
            break;

        case WM_SIZE:
        {
            u32 newWidth = LOWORD(lParam);
            u32 newHeight = HIWORD(lParam);
            if (newWidth > 0 && newHeight > 0)
            {
                window->m_width = newWidth;
                window->m_height = newHeight;
                window->m_resized = true;
                Logger::Debug("Window resized: {}x{}", newWidth, newHeight);
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_F11)
            {
                window->ToggleFullscreen();
            }
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnKeyDown(static_cast<int>(wParam));
            }
            return 0;

        case WM_KEYUP:
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnKeyUp(static_cast<int>(wParam));
            }
            return 0;

        case WM_INPUT:
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnRawInput(lParam);
            }
            return 0;

        case WM_KILLFOCUS:
            // 他ウィンドウ/タブへフォーカスが移ると以降の WM_KEYUP が届かず、
            // 最後に押したキーが押しっぱなし判定で残る → 全キー状態をクリア
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnFocusLost();
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_CLOSE:
            if (window->m_closeHandler && !window->m_closeHandler())
                return 0;   // 呼び出し側が処理済み（例: ランチャーに戻った）＝ウィンドウは閉じない
            window->m_shouldClose = true;
            DestroyWindow(hwnd);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace dx12e
