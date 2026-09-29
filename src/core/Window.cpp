#include "Window.h"
#include "Logger.h"
#include "Version.h"   // kEngineNameW（ウィンドウタイトルの既定表示名）
#include "input/InputSystem.h"
#include "input/VirtualInput.h"   // 仮想入力モード中は実マウス/実キーボードを ImGui にも InputSystem にも渡さない
#include "DpiScale.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <algorithm>
#include <numeric>      // std::gcd

// ImGui Win32 WndProc handler (forward declaration)
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dx12e
{

namespace
{
// user32 の DPI 関数（Windows 10 1607+）。古い OS では null → 96 DPI / 従来の関数へ落とす。
struct DpiApi
{
    using GetDpiForWindowFn      = UINT(WINAPI*)(HWND);
    using GetDpiForSystemFn      = UINT(WINAPI*)();
    using AdjustFn               = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    using GetMetricsForDpiFn     = int(WINAPI*)(int, UINT);
    using GetDpiForMonitorFn     = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    GetDpiForWindowFn  getDpiForWindow  = nullptr;
    GetDpiForSystemFn  getDpiForSystem  = nullptr;
    AdjustFn           adjust           = nullptr;
    GetMetricsForDpiFn getMetricsForDpi = nullptr;
    DpiApi()
    {
        if (HMODULE u = GetModuleHandleW(L"user32.dll"))
        {
            getDpiForWindow  = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(u, "GetDpiForWindow"));
            getDpiForSystem  = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(u, "GetDpiForSystem"));
            adjust           = reinterpret_cast<AdjustFn>(GetProcAddress(u, "AdjustWindowRectExForDpi"));
            getMetricsForDpi = reinterpret_cast<GetMetricsForDpiFn>(GetProcAddress(u, "GetSystemMetricsForDpi"));
        }
    }
};
const DpiApi& Api() { static const DpiApi a; return a; }

UINT DpiOfWindow(HWND h)
{
    const DpiApi& a = Api();
    if (h && a.getDpiForWindow) { const UINT d = a.getDpiForWindow(h); if (d) return d; }
    if (a.getDpiForSystem) { const UINT d = a.getDpiForSystem(); if (d) return d; }
    return 96;
}
// 窓がまだ無いときの DPI（プライマリモニタ）。Per-Monitor では GetDpiForSystem がプライマリの DPI を返す。
UINT DpiPrimary()
{
    const DpiApi& a = Api();
    if (a.getDpiForSystem) { const UINT d = a.getDpiForSystem(); if (d) return d; }
    return 96;
}
int MetricForDpi(int index, UINT dpi)
{
    const DpiApi& a = Api();
    if (a.getMetricsForDpi) return a.getMetricsForDpi(index, dpi);
    return GetSystemMetrics(index);
}
} // namespace

bool Window::AdjustRectForDpi(RECT* rect, DWORD style, DWORD exStyle, unsigned dpi)
{
    const DpiApi& a = Api();
    if (a.adjust) return a.adjust(rect, style, FALSE, exStyle, dpi) != FALSE;
    return AdjustWindowRectEx(rect, style, FALSE, exStyle) != FALSE;
}

float Window::GetUiScale() const
{
    return dpi::EffectiveScale(GetOsDpiScale());
}

unsigned Window::ScaleLogical(unsigned logicalPx) const
{
    return static_cast<unsigned>(dpi::LogicalToPhysical(static_cast<int>(logicalPx), GetUiScale()));
}

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
    const bool bg = m_bg.Active();
    m_dpi = DpiPrimary();   // 窓を作る前の仮値（作成後に GetDpiForWindow で確定し、違えば作り直す）
    if (bg)
    {
        // --background: 論理解像度（1920x1080）× 倍率 で作る（画面外 / 最小化でも 0x0 や作業領域への縮小をしない）。
        //   倍率は --dpi-scale（あれば）/ 無ければ OS の倍率。既定 100% では従来どおり 1920x1080 の物理 px。
        width  = ScaleLogical(kBackgroundClientWidth);
        height = ScaleLogical(kBackgroundClientHeight);
        startMaximized = false;
    }
    m_width = width;
    m_height = height;
    m_title = title ? title : kEngineNameW;   // 既定の表示名は Version.h で一元管理
    m_startMaximized = startMaximized;

    // exe に埋め込んだアプリアイコン（resources/app.ico, IDI_APPICON=101）を読む。
    // 大（タスクバー/Alt+Tab）と小（タイトルバー）を別サイズで読み、失敗時は既定にフォールバック。
    HICON appIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101 /*IDI_APPICON*/),
        IMAGE_ICON, MetricForDpi(SM_CXICON, m_dpi), MetricForDpi(SM_CYICON, m_dpi), LR_DEFAULTCOLOR));
    HICON appIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(101 /*IDI_APPICON*/),
        IMAGE_ICON, MetricForDpi(SM_CXSMICON, m_dpi), MetricForDpi(SM_CYSMICON, m_dpi), LR_DEFAULTCOLOR));

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
    AdjustRectForDpi(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, m_dpi);

    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    if (bg)
    {
        const int fullW = rect.right - rect.left;
        if (m_bg.mode == BackgroundMode::Offscreen)
        {
            // 全モニタの仮想デスクトップの外側（左）へ。-32000 のような「最小化の指定席」ではなく、
            // 最小化扱いにならない普通の窓のまま画面に映らない場所へ置く。
            winX = GetSystemMetrics(SM_XVIRTUALSCREEN) - fullW - 256;
            winY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        }
        else
        {
            RECT wa{};
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) { winX = wa.left; winY = wa.top; }
        }
    }
    else if (!startMaximized)
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
                AdjustRectForDpi(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, m_dpi);
                Logger::Info("ウィンドウを作業領域に合わせて縮小: {}x{}", m_width, m_height);
            }
            winX = wa.left + ((wa.right - wa.left) - (rect.right - rect.left)) / 2;
            winY = wa.top  + ((wa.bottom - wa.top) - (rect.bottom - rect.top)) / 2;
        }
    }

    // --background: WS_EX_NOACTIVATE（クリックされてもアクティブ化しない）。
    //   tool 指定（画面外/非表示は既定）は WS_EX_TOOLWINDOW でタスクバー/Alt+Tab から隠す。
    DWORD exStyle = 0;
    if (bg)
    {
        exStyle |= WS_EX_NOACTIVATE;
        if (m_bg.toolWindow) exStyle |= WS_EX_TOOLWINDOW;
    }
    m_hwnd = CreateWindowExW(
        exStyle,
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

    // 実際に乗ったモニターの DPI を確定する。仮値（プライマリ）と違えば、フレーム分が違うので
    // クライアントサイズを保ったまま窓を取り直す（論理サイズ = 物理 ÷ 倍率 が狂わないように）。
    {
        const UINT real = DpiOfWindow(m_hwnd);
        if (real != m_dpi)
        {
            m_dpi = real;
            if (bg)
            {
                m_width  = ScaleLogical(kBackgroundClientWidth);
                height   = ScaleLogical(kBackgroundClientHeight);
                m_height = height;
            }
            RECT r2 = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
            AdjustRectForDpi(&r2, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, exStyle, m_dpi);
            SetWindowPos(m_hwnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            m_resized = false;
        }
        Logger::Info("DPI: window={} ({}%), uiScale={:.2f}{}", m_dpi, static_cast<int>(m_dpi * 100 / 96), GetUiScale(),
                     dpi::HasOverride() ? " (--dpi-scale override)" : "");
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
        // ★--background は論理解像度(1920x1080)のまま。作業領域へは広げない。
        RECT wa{};
        if (!bg && SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
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
    if (!m_hwnd) return;

    // ===== --background: 前面に出ない表示。SetForegroundWindow / SW_SHOW / SW_SHOWMAXIMIZED は使わない =====
    if (m_bg.Active())
    {
        if (m_bg.mode == BackgroundMode::Hidden) return;   // 一度も表示しない（HWND だけ作る）
        if (IsWindowVisible(m_hwnd)) return;
        switch (m_bg.mode)
        {
        case BackgroundMode::Minimized:
            ShowWindow(m_hwnd, SW_SHOWMINNOACTIVE);        // 最小化のまま・アクティブ化しない
            break;
        case BackgroundMode::Offscreen:
        case BackgroundMode::NoActivate:
        default:
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);         // アクティブ化しない
            // 表示時に Z オーダーの最上段へ来るのを、最背面へ送って避ける（フォアグラウンドは変えない）。
            SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        }
        UpdateWindow(m_hwnd);
        return;
    }

    if (IsWindowVisible(m_hwnd)) return;
    ShowWindow(m_hwnd, m_startMaximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(m_hwnd);
    SetForegroundWindow(m_hwnd);
}

void Window::DisablePowerThrottling()
{
    // EcoQoS（実行速度の間引き）と、タイマー分解能の無視（Windows 11 は「見えていない窓」の
    // プロセスの timeBeginPeriod を無視して 15ms 刻みに戻す）の両方から外れる。
    // StateMask=0 + ControlMask にビット = 「OS の自動判断に任せず、間引きを明示的に OFF」。
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    st.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#endif
    st.StateMask   = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st)))
        Logger::Warn("power throttling opt-out failed (err={})", GetLastError());
    else
        Logger::Info("power throttling disabled (background window)");
}

void Window::EnableCustomTitleBar()
{
    if (!m_hwnd || m_customTitleBar) return;
    m_customTitleBar = true;
    // WM_NCCALCSIZE を発火させてフレームを再計算(キャプション領域をクライアントに取り込む)
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (m_bg.Active() && !IsIconic(m_hwnd))
    {
        // --background: キャプションを外すとクライアント領域が縦に伸びる。論理解像度
        // (1920x1080 × 表示倍率)を保つため、窓の大きさを取り直して合わせる（スワップチェイン生成前）。
        // ★目標は m_width/m_height ではなく論理解像度から計算し直す: 上の SWP_FRAMECHANGED で届く WM_SIZE が
        //   m_width/m_height を「キャプションを外した後の（縦に伸びた）クライアント」に書き換えるため、
        //   それと比べると「合っている」と誤判定して縮めず、クライアントが 1920x1111 のまま残っていた。
        ApplyBackgroundClientSize();
        m_width  = ScaleLogical(kBackgroundClientWidth);
        m_height = ScaleLogical(kBackgroundClientHeight);
        m_resized = false;
    }
    Logger::Info("カスタムタイトルバー有効化");
}

// --background の窓は「論理 1920x1080 × 倍率」のクライアントで保つ（--dpi-scale や OS 倍率が変わった時に呼ぶ）。
void Window::ApplyBackgroundClientSize()
{
    if (!m_hwnd || !m_bg.Active() || IsIconic(m_hwnd)) return;
    const LONG w = static_cast<LONG>(ScaleLogical(kBackgroundClientWidth));
    const LONG h = static_cast<LONG>(ScaleLogical(kBackgroundClientHeight));
    RECT cr{}, wr{};
    if (!GetClientRect(m_hwnd, &cr) || !GetWindowRect(m_hwnd, &wr)) return;
    if (cr.right == w && cr.bottom == h) return;
    SetWindowPos(m_hwnd, nullptr, 0, 0,
                 (wr.right - wr.left) + (w - cr.right), (wr.bottom - wr.top) + (h - cr.bottom),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // WM_SIZE が m_width/m_height を実際のクライアントサイズで更新して m_resized を立てる（同期）。
    // ★要求した値ではなく実測値を採る（OS が窓を切り詰めた時に、スワップチェインだけ大きくなって ImGui の表示と食い違わないように）。
    RECT after{};
    if (GetClientRect(m_hwnd, &after) && after.right > 0 && after.bottom > 0)
    {
        m_width  = static_cast<u32>(after.right);
        m_height = static_cast<u32>(after.bottom);
    }
    m_resized = true;
}

void Window::SetUiScaleOverride(float scale)
{
    dpi::SetOverride(scale);
    if (m_bg.Active()) ApplyBackgroundClientSize();
    m_dpiChanged = true;
}

void Window::SetTitle(const std::wstring& title)
{
    if (m_hwnd)
        SetWindowTextW(m_hwnd, title.c_str());
}

void Window::ToggleFullscreen()
{
    if (m_bg.Active()) return;   // --background: 窓が画面内へ戻る/最大化されるのを防ぐ
    // F11: ウィンドウ ⇄ ボーダレス（従来挙動）
    SetMode(m_mode == WindowMode::Windowed ? WindowMode::Borderless : WindowMode::Windowed);
}

void Window::SetMode(WindowMode mode, u32 width, u32 height)
{
    if (!m_hwnd) return;
    if (m_bg.Active()) return;   // --background: ボーダレス/フルスクリーン遷移は窓を前面へ出すので無効

    // Fullscreen から抜けるときはディスプレイモードを元に戻す
    if (m_displayModeChanged && mode != WindowMode::Fullscreen)
    {
        ChangeDisplaySettingsW(nullptr, 0);
        m_displayModeChanged = false;
    }

    // ウィンドウ位置の復元用に、ウィンドウモードから出るときだけ矩形を覚える
    if (m_mode == WindowMode::Windowed && mode != WindowMode::Windowed)
        GetWindowRect(m_hwnd, &m_windowedRect);

    switch (mode)
    {
    case WindowMode::Windowed:
    {
        // 一度もウィンドウ表示していない(起動直後にボーダレス等へ入った)場合の復元先を用意
        if (m_windowedRect.right - m_windowedRect.left <= 0
            || m_windowedRect.bottom - m_windowedRect.top <= 0)
        {
            RECT r = { 0, 0, 1280, 720 };
            AdjustRectForDpi(&r, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, m_dpi);
            const LONG w = r.right - r.left, h = r.bottom - r.top;
            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            m_windowedRect = { wa.left + ((wa.right - wa.left) - w) / 2,
                               wa.top + ((wa.bottom - wa.top) - h) / 2, 0, 0 };
            m_windowedRect.right  = m_windowedRect.left + w;
            m_windowedRect.bottom = m_windowedRect.top + h;
        }
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX | WS_VISIBLE);
        SetWindowPos(m_hwnd, HWND_NOTOPMOST,
            m_windowedRect.left, m_windowedRect.top,
            m_windowedRect.right - m_windowedRect.left,
            m_windowedRect.bottom - m_windowedRect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_NORMAL);
        m_mode = WindowMode::Windowed;
        m_fullscreen = false;
        if (width > 0 && height > 0)
            SetClientSize(width, height);
        Logger::Info("Windowed mode restored");
        break;
    }
    case WindowMode::Borderless:
    {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
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
        m_mode = WindowMode::Borderless;
        m_fullscreen = true;
        Logger::Info("Borderless fullscreen enabled");
        break;
    }
    case WindowMode::Fullscreen:
    {
        // 解像度指定なしなら現在のデスクトップ解像度
        if (width == 0 || height == 0)
        {
            DEVMODEW cur = {};
            cur.dmSize = sizeof(cur);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur))
            {
                width  = cur.dmPelsWidth;
                height = cur.dmPelsHeight;
            }
        }
        DEVMODEW dm = {};
        dm.dmSize       = sizeof(dm);
        dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;
        dm.dmPelsWidth  = width;
        dm.dmPelsHeight = height;
        // CDS_FULLSCREEN = 一時的なモード変更（プロセス終了で OS が自動復元）
        LONG r = ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN);
        if (r != DISP_CHANGE_SUCCESSFUL)
        {
            Logger::Warn("ディスプレイモード変更失敗 ({}x{}, code={}) — ボーダレスにフォールバック",
                         width, height, r);
            SetMode(WindowMode::Borderless);
            return;
        }
        m_displayModeChanged = true;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_hwnd, HWND_TOP, 0, 0,
            static_cast<int>(width), static_cast<int>(height),
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOW);
        m_mode = WindowMode::Fullscreen;
        m_fullscreen = true;
        Logger::Info("Fullscreen enabled ({}x{})", width, height);
        break;
    }
    }
}

void Window::SetClientSize(u32 width, u32 height)
{
    if (!m_hwnd || m_mode != WindowMode::Windowed || width == 0 || height == 0) return;
    if (m_bg.Active()) return;   // --background: 論理解像度を固定
    RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustRectForDpi(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, 0, m_dpi);
    SetWindowPos(m_hwnd, nullptr, 0, 0,
        rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Logger::Info("Client size set to {}x{}", width, height);
}

std::vector<std::pair<u32, u32>> Window::EnumResolutions()
{
    std::vector<std::pair<u32, u32>> out;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i)
    {
        if (dm.dmBitsPerPel != 32) continue;
        if (dm.dmPelsWidth < 1024 || dm.dmPelsHeight < 576) continue;   // 極小モードは除外
        std::pair<u32, u32> mode{ dm.dmPelsWidth, dm.dmPelsHeight };
        if (std::find(out.begin(), out.end(), mode) == out.end())
            out.push_back(mode);
    }
    std::sort(out.begin(), out.end());
    return out;
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

    // ★仮想入力モード（--virtual-input / --background）: 実マウス/実キーボードのメッセージは
    //   ImGui にも InputSystem にも渡さない。AI（MCP）の入力は vinput のキューだけが正で、
    //   人が触った実入力と混ざらない（＝AI の操作を人が壊さない / 人の操作を AI が奪わない）。
    //   ImGui の Win32 バックエンドは WM_LBUTTONDOWN で SetCapture、WM_SETCURSOR で SetCursor を
    //   呼ぶので、ここで止めることが「OS のカーソルに触らない」ことにもなる。
    const bool vblock = vinput::BlocksRealInput(msg);

    // マウスキャプチャ中は WM_SETCURSOR を自前で処理してカーソルを消す
    if (!vblock && window && window->m_inputSystem && window->m_inputSystem->IsMouseCaptured()
        && msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
    {
        SetCursor(nullptr);
        return TRUE;
    }

    // ImGui にイベントを渡す（キー/マウス等は結果を無視して InputSystem にも常に通知する）
    LRESULT imguiResult = (vblock || msg == WM_DPICHANGED) ? 0 : ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

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
                    const UINT wdpi = DpiOfWindow(hwnd);
                    const int frame = MetricForDpi(SM_CYSIZEFRAME, wdpi) + MetricForDpi(SM_CXPADDEDBORDER, wdpi);
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
                    const UINT wdpi = DpiOfWindow(hwnd);
                    const int frame = MetricForDpi(SM_CYSIZEFRAME, wdpi) + MetricForDpi(SM_CXPADDEDBORDER, wdpi);
                    if (pt.y >= 0 && pt.y < frame) return HTTOP;
                }
                // 仮想入力モード中は ImGui のホバー（＝仮想ポインタ）で「ドラッグ可能」を決めているので、
                // 実カーソルの位置と食い違う。人が実際に窓を掴んで動かせるよう HTCAPTION は返さない。
                if (window->m_captionDraggable && !vinput::Enabled()
                    && pt.y < static_cast<LONG>(window->m_captionHeight))
                    return HTCAPTION;
                return HTCLIENT;
            }
            break;

        // --background: クリックされても（別のプロセスの窓から）アクティブ化しない。
        case WM_MOUSEACTIVATE:
            if (window->m_bg.Active()) return MA_NOACTIVATE;
            break;

        // --background の窓は「論理 1920x1080 × 倍率」の大きさで居続ける。OS 既定の最大追従サイズ（≒ 画面の大きさ）を
        // 超えると（200% で 3840x2160 など）窓が切り詰められて論理サイズが崩れるので、上限を外す。
        case WM_GETMINMAXINFO:
            if (window->m_bg.Active())
            {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
                mm->ptMaxTrackSize.x = 16384;  mm->ptMaxTrackSize.y = 16384;
                mm->ptMaxSize.x = 16384;       mm->ptMaxSize.y = 16384;
                return 0;
            }
            break;

        // ===== 表示倍率が変わった（別倍率のモニターへ動かした / 実行中に OS の倍率を変えた）=====
        // Per-Monitor V2: OS が推奨する窓矩形（論理サイズを保つ新しい物理サイズ）が lParam で届く。
        // それを適用すれば WM_SIZE → スワップチェイン/RT のリサイズ → ImGui のスタイル/フォント再構築が続く。
        // ImGui のバックエンドには渡さない（メイン窓の SetWindowPos は自前で行う。最大化中は OS に任せる）。
        case WM_DPICHANGED:
        {
            window->m_dpi = HIWORD(wParam);
            window->m_dpiChanged = true;
            const RECT* sug = reinterpret_cast<const RECT*>(lParam);
            if (sug && !IsZoomed(hwnd) && !window->m_fullscreen && !window->m_bg.Active())
            {
                SetWindowPos(hwnd, nullptr, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            else if (window->m_bg.Active())
            {
                window->ApplyBackgroundClientSize();   // 背景窓は論理 1920x1080 × 新しい倍率を保つ
            }
            Logger::Info("WM_DPICHANGED: {} DPI ({}%)", window->m_dpi, static_cast<int>(window->m_dpi * 100 / 96));
            return 0;
        }

        case WM_SIZE:
        {
            // 最小化では (0,0) が来る。論理解像度を保つため、明示的に無視する
            // （下の 0 チェックと同じ結果だが、意図を残す＝スワップチェインを 0x0 にしない）。
            if (wParam == SIZE_MINIMIZED) return 0;
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
            if (vblock) return 0;   // 仮想入力モード: 実キーボードは InputSystem へも渡さない
            if (wParam == VK_F11)
            {
                window->ToggleFullscreen();
            }
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnKeyDown(static_cast<int>(wParam));
            }
            return 0;

        // 人の脱出口: 仮想入力モード中に Ctrl+Alt+Shift+F12（実キーボード）で OFF を要求する。
        // Alt を押しているので WM_SYSKEYDOWN で届く。GetKeyState は読み取りだけ（OS には何も書かない）。
        // それ以外の Alt 系（Alt+F4 で閉じる等）は素通し＝DefWindowProc へ流す。
        case WM_SYSKEYDOWN:
            if (vblock && wParam == VK_F12
                && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000))
            {
                vinput::RequestEscape();
                return 0;
            }
            break;

        case WM_KEYUP:
            if (vblock) return 0;
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnKeyUp(static_cast<int>(wParam));
            }
            return 0;

        case WM_INPUT:
            if (vblock) break;      // Raw Input の後始末は DefWindowProc に任せる（移動量は捨てる）
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnRawInput(lParam);
            }
            return 0;

        case WM_KILLFOCUS:
            if (vblock) break;
            // 他ウィンドウ/タブへフォーカスが移ると以降の WM_KEYUP が届かず、
            // 最後に押したキーが押しっぱなし判定で残る → 全キー状態をクリア。
            // 同時にカーソルの拘束（非表示/クリップ/中央固定）も解いて裏で作業できるようにする。
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnFocusLost();
            }
            break;

        case WM_SETFOCUS:
            if (vblock) break;
            // 戻ってきたら、論理キャプチャが立っていればカーソル拘束を掛け直す
            if (window->m_inputSystem)
            {
                window->m_inputSystem->OnFocusGained();
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
