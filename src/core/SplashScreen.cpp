#include "core/SplashScreen.h"

#include <Windows.h>
#include <algorithm>
// gdiplus ヘッダは min/max マクロ前提で書かれており、NOMINMAX 環境ではそのままだと
// コンパイルできない。std::min/max を Gdiplus 名前空間へ引き込むのが定石の回避策。
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>     // gdiplus が必要とする IStream 等
#include <gdiplus.h>
#include <atomic>
#include <mutex>

#pragma comment(lib, "gdiplus.lib")

namespace dx12e
{
namespace
{
// ---- 見た目（エディタテーマ EditorTheme.h と同系色）----
constexpr int  kWidth  = 480;
constexpr int  kHeight = 270;
constexpr int  kCorner = 14;                      // 角丸半径
constexpr COLORREF kBg      = RGB(0x0e, 0x0f, 0x12);
constexpr COLORREF kBorder  = RGB(0x25, 0x26, 0x2c);
constexpr COLORREF kTitle   = RGB(0xf0, 0xf2, 0xf5);
constexpr COLORREF kSub     = RGB(0x9a, 0xa0, 0xad);
constexpr COLORREF kTrack   = RGB(0x20, 0x21, 0x27);
constexpr COLORREF kAccent  = RGB(0x4c, 0x8d, 0xff);
constexpr UINT kTimerId     = 1;
constexpr UINT kTimerMs     = 33;                 // ~30fps アニメ

// ---- スレッド共有状態 ----
std::mutex        g_mutex;      // 文字列群を保護
std::wstring      g_title;
std::wstring      g_version;
std::wstring      g_status;
std::wstring      g_logoPath;

HANDLE            g_thread = nullptr;
std::atomic<HWND> g_hwnd{nullptr};
HANDLE            g_readyEvent = nullptr;         // ウィンドウ生成完了の合図
float             g_anim = 0.0f;                  // 進行バーの位相 0..1

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// 描画一式（ダブルバッファのメモリ DC へ）
void Paint(HWND hwnd, HDC dc, Gdiplus::Bitmap* logo)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;

    HDC     mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // 背景 + 枠
    {
        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ oldPen = SelectObject(mem, pen);
        HGDIOBJ oldBr  = SelectObject(mem, GetStockObject(NULL_BRUSH));
        RoundRect(mem, 0, 0, w, h, kCorner * 2, kCorner * 2);
        SelectObject(mem, oldPen);
        SelectObject(mem, oldBr);
        DeleteObject(pen);
    }

    std::wstring title, version, status;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        title = g_title; version = g_version; status = g_status;
    }

    // ロゴ（GDI+。PNG のアルファをそのまま合成）
    const int logoSize = 88;
    const int logoX = 36, logoY = 52;
    bool hasLogo = false;
    if (logo && logo->GetLastStatus() == Gdiplus::Ok)
    {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(logo, logoX, logoY, logoSize, logoSize);
        hasLogo = true;
    }

    SetBkMode(mem, TRANSPARENT);

    // タイトル
    {
        HFONT f = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        HGDIOBJ of = SelectObject(mem, f);
        SetTextColor(mem, kTitle);
        RECT tr{ hasLogo ? logoX + logoSize + 24 : 36, 78, w - 24, 130 };
        DrawTextW(mem, title.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem, of);
        DeleteObject(f);
    }

    // 小さめフォント（ステータス / 版数）。※変数名 "small" は Windows ヘッダのマクロと衝突する
    HFONT fontSmall = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    HGDIOBJ oldF = SelectObject(mem, fontSmall);
    SetTextColor(mem, kSub);

    // ステータス（左下・バーの上）
    {
        RECT sr{ 36, h - 64, w - 120, h - 42 };
        DrawTextW(mem, status.c_str(), -1, &sr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // 版数（右下）
    if (!version.empty())
    {
        RECT vr{ w - 120, h - 64, w - 36, h - 42 };
        DrawTextW(mem, version.c_str(), -1, &vr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(mem, oldF);
    DeleteObject(fontSmall);

    // 不確定進行バー（トラック + 左右に流れるハイライト）
    {
        const int bx0 = 36, bx1 = w - 36, by0 = h - 36, by1 = h - 30;
        HBRUSH track = CreateSolidBrush(kTrack);
        RECT trr{ bx0, by0, bx1, by1 };
        FillRect(mem, &trr, track);
        DeleteObject(track);

        const int trackW = bx1 - bx0;
        const int segW   = trackW / 4;
        // 位相 0..1 を「左端から右端へ抜けて戻ってくる」動きに写像
        int sx = bx0 + static_cast<int>((trackW + segW) * g_anim) - segW;
        int ex = sx + segW;
        if (sx < bx0) sx = bx0;
        if (ex > bx1) ex = bx1;
        if (ex > sx)
        {
            HBRUSH seg = CreateSolidBrush(kAccent);
            RECT sgr{ sx, by0, ex, by1 };
            FillRect(mem, &sgr, seg);
            DeleteObject(seg);
        }
    }

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // ロゴはウィンドウ毎にスレッドローカル所有（このウィンドウはスプラッシュスレッド専有）
    static Gdiplus::Bitmap* s_logo = nullptr;

    switch (msg)
    {
    case WM_CREATE:
    {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            path = g_logoPath;
        }
        if (!path.empty())
        {
            s_logo = new Gdiplus::Bitmap(path.c_str());
            if (s_logo->GetLastStatus() != Gdiplus::Ok) { delete s_logo; s_logo = nullptr; }
        }
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        return 0;
    }
    case WM_TIMER:
        g_anim += kTimerMs / 1400.0f;   // 1 スイープ ≒ 1.4 秒
        if (g_anim >= 1.0f) g_anim -= 1.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(hwnd, dc, s_logo);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // ちらつき防止（WM_PAINT が全面を描く）
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        delete s_logo;
        s_logo = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

DWORD WINAPI SplashThread(LPVOID)
{
    // GDI+ はこのスレッド内で開始/終了する（PNG デコード用）
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartupInput gdipIn;
    const bool gdipOk = (Gdiplus::GdiplusStartup(&gdipToken, &gdipIn, nullptr) == Gdiplus::Ok);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SplashProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_WAIT);
    wc.lpszClassName = L"DX12SplashWnd";
    RegisterClassExW(&wc);   // 二重登録は失敗するだけで無害

    // プライマリモニタ中央
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int x = (sw - kWidth) / 2, y = (sh - kHeight) / 2;

    // WS_POPUP=枠なし / WS_EX_TOOLWINDOW=タスクバー非表示。
    // TOPMOST にはしない（アップデータの MessageBox 等を隠さないため）。
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"DX12SplashWnd", L"",
                                WS_POPUP, x, y, kWidth, kHeight,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd)
    {
        // 角丸のウィンドウ形状
        HRGN rgn = CreateRoundRectRgn(0, 0, kWidth + 1, kHeight + 1, kCorner * 2, kCorner * 2);
        SetWindowRgn(hwnd, rgn, TRUE);   // 所有権は OS へ移る
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        g_hwnd.store(hwnd);
    }
    if (g_readyEvent) SetEvent(g_readyEvent);

    if (hwnd)
    {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_hwnd.store(nullptr);
    if (gdipOk) Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
} // namespace

void SplashScreen::Show(const std::string& titleUtf8,
                        const std::string& versionUtf8,
                        const std::string& logoPathUtf8)
{
    if (g_thread) return;   // 既に表示中

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_title    = Utf8ToWide(titleUtf8);
        g_version  = Utf8ToWide(versionUtf8);
        g_logoPath = Utf8ToWide(logoPathUtf8);
        if (g_status.empty()) g_status = L"...";
    }

    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, SplashThread, nullptr, 0, nullptr);
    if (!g_thread)
    {
        if (g_readyEvent) { CloseHandle(g_readyEvent); g_readyEvent = nullptr; }
        return;
    }
    // ウィンドウ生成まで少しだけ待つ（初期化ログより先に画面へ出すため。失敗しても続行）
    if (g_readyEvent) WaitForSingleObject(g_readyEvent, 1000);
}

void SplashScreen::SetStatus(const std::string& statusUtf8)
{
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_status = Utf8ToWide(statusUtf8);
    }
    // 再描画はタイマー(33ms)が拾うので通知不要。ウィンドウ未表示なら文字列だけ更新される。
}

void SplashScreen::Close()
{
    if (!g_thread) return;

    if (HWND hwnd = g_hwnd.load())
        PostMessageW(hwnd, WM_CLOSE, 0, 0);

    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    if (g_readyEvent) { CloseHandle(g_readyEvent); g_readyEvent = nullptr; }
}

} // namespace dx12e
