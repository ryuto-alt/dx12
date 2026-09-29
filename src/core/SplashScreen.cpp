#include "core/SplashScreen.h"

#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
// gdiplus ヘッダは min/max マクロ前提で書かれており、NOMINMAX 環境ではそのままだと
// コンパイルできない。std::min/max を Gdiplus 名前空間へ引き込むのが定石の回避策。
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>     // gdiplus が必要とする IStream 等
#include <gdiplus.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/DpiScale.h"
#include "core/Logger.h"
#include "core/SplashAudio.h"
#include "core/SplashCommon.h"
#include "core/SplashDiag.h"
#include "core/SplashDirector.h"
#include "core/SplashProgress.h"
#include "core/SplashRenderer.h"

#pragma comment(lib, "gdiplus.lib")

namespace dx12e
{
namespace
{
namespace sp = splash;

// ---------------------------------------------------------------- スレッド間の共有状態

struct G
{
    std::mutex mu;                            // 文字列群・tracker・finishCb を保護
    std::wstring title, version, logoPath, soundDir, projectName, sceneName, status;
    bool projectMode = false;
    bool playSound = false;                   // この表示で起動音を鳴らす
    bool isNew = false;
    std::vector<std::wstring> recents;
    sp::ProgressTracker tracker;
    bool manualStep = false;
    int manualIdx = 0, manualTotal = 0;
    std::function<void()> finishCb;
    std::atomic<bool> hasCb{false};           // finishCb が保留中（Pump がロックを避けるための印）

    HANDLE thread = nullptr;
    HANDLE readyEv = nullptr;
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> done{true};             // スレッドが終わった（or 未開始）
    std::atomic<bool> suppressed{false};
    std::atomic<bool> soundOn{true};
    std::atomic<bool> expectProject{false};
    std::atomic<bool> wantMain{false};
    std::atomic<double> finishAt{-1.0};
    std::atomic<double> mainShownAt{-1.0};
    LARGE_INTEGER t0{}, freq{};
    double predictedFinishSec = 3.0;

    // 検証用（--splash-selftest）: 窓を【出さずに】スレッド・描画・ULW・終了の流れを通す。
    std::atomic<bool> testNoShow{false};
    std::atomic<uint64_t> frames{0};
    std::atomic<double> renderMsSum{0.0}, renderMsMax{0.0};
    std::atomic<bool> usedD2d{false}, usedWarp{false};
    double lifetimeSec = 0.0;
    double threadCpuSec = 0.0, cpuAt30 = 0.0, tAt30 = 0.0;   // スプラッシュのスレッド自身の CPU 時間（検証用）
};

G& S()
{
    static G* g = new G();                    // 意図的にリーク（プロセス終了時のスレッド競合を避ける）
    return *g;
}

// このスレッドの CPU 時間（カーネル + ユーザー、秒）。
double ThreadCpuSec()
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0.0;
    auto v = [](FILETIME f) { return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return v(k) + v(u);
}

double Now()
{
    G& g = S();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart - g.t0.QuadPart) / static_cast<double>(g.freq.QuadPart);
}

// ---------------------------------------------------------------- startup.json（前回までの起動所要時間の EMA）

std::filesystem::path StartupJsonPath()
{
    PWSTR known = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known)
        p = std::filesystem::path(known) / L"UnoEngine" / L"startup.json";
    if (known) CoTaskMemFree(known);
    return p;
}

// 読めなければ（無い/壊れている）0 を返す＝「履歴なし」。予測には ClampStartupMs を通す。
double ReadStartupMsRaw()
{
    const auto p = StartupJsonPath();
    if (p.empty()) return 0.0;
    std::ifstream f(p, std::ios::binary);
    if (!f) return 0.0;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.find("\"startupMs\"") == std::string::npos) return 0.0;
    return sp::ParseStartupJson(text);
}

void SaveStartupMs(double measuredMs)
{
    const auto p = StartupJsonPath();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const double next = sp::UpdateStartupEma(ReadStartupMsRaw(), measuredMs);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (f) f << sp::FormatStartupJson(next);
}

// 設定 startupSound（既定 true）。エディタ全体（プロジェクトに依らない）の設定なので、既存のグローバルな
// エディタ状態 %APPDATA%\DX12Engine\editor_state.json（ProjectManager が lastOpenedScene を持つ JSON。
// 読み書きは「既存を読んでマージ」）に camelCase のキーで持つ。プロジェクトごとの settings.json は
// プロジェクトを開く前（=スプラッシュの時点）には無いので使えない。
std::filesystem::path EditorStatePath()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) return {};
    return std::filesystem::path(appData) / L"DX12Engine" / L"editor_state.json";
}

bool ReadStartupSoundSetting()
{
    const auto p = EditorStatePath();
    if (p.empty()) return true;
    std::ifstream f(p);
    if (!f) return true;
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_object() && j.contains("startupSound") && j["startupSound"].is_boolean())
        return j["startupSound"].get<bool>();
    return true;
}

// ---------------------------------------------------------------- 縮退用の GDI+ 描画（D2D が使えないときだけ）

constexpr COLORREF kLBg     = RGB(0x0e, 0x0e, 0x10);
constexpr COLORREF kLBorder = RGB(0x2a, 0x2a, 0x2f);
constexpr COLORREF kLTitle  = RGB(0xd6, 0xd6, 0xdb);
constexpr COLORREF kLSub    = RGB(0x9c, 0x9c, 0xa6);
constexpr COLORREF kLTrack  = RGB(0x1f, 0x1f, 0x23);
constexpr COLORREF kLAccent = RGB(0x2f, 0x8c, 0xff);

void PaintLegacy(HDC mem, int w, int h, float scale, const sp::SplashFrame& f, const sp::SplashContent& c, Gdiplus::Bitmap* logo)
{
    auto Sc = [&](int v) { return static_cast<int>(v * scale + 0.5f); };
    RECT rc{ 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(kLBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, Sc(1)), kLBorder);
    HGDIOBJ op = SelectObject(mem, pen), ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
    RoundRect(mem, 0, 0, w, h, Sc(28), Sc(28));
    SelectObject(mem, op); SelectObject(mem, ob); DeleteObject(pen);

    const int logoSize = Sc(96), logoX = Sc(40), logoY = Sc(64);
    bool hasLogo = false;
    if (logo && logo->GetLastStatus() == Gdiplus::Ok)
    {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(logo, logoX, logoY, logoSize, logoSize);
        hasLogo = true;
    }
    SetBkMode(mem, TRANSPARENT);
    auto font = [&](int px, int weight) {
        return CreateFontW(-Sc(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    };
    HFONT ft = font(40, FW_SEMIBOLD);
    HGDIOBJ of = SelectObject(mem, ft);
    SetTextColor(mem, kLTitle);
    RECT tr{ hasLogo ? logoX + logoSize + Sc(28) : Sc(40), Sc(70), w - Sc(24), Sc(130) };
    DrawTextW(mem, c.title.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(mem, of); DeleteObject(ft);

    HFONT fs = font(14, FW_NORMAL);
    of = SelectObject(mem, fs);
    SetTextColor(mem, kLSub);
    std::wstring st = c.stepCur;
    if (f.percent > 0) st += L"   " + std::to_wstring(f.percent) + L"%";
    RECT sr{ Sc(40), h - Sc(70), w - Sc(140), h - Sc(44) };
    DrawTextW(mem, st.c_str(), -1, &sr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT vr{ w - Sc(140), h - Sc(70), w - Sc(40), h - Sc(44) };
    DrawTextW(mem, c.version.c_str(), -1, &vr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    SelectObject(mem, of); DeleteObject(fs);

    const int bx0 = Sc(40), bx1 = w - Sc(40), by0 = h - Sc(36), by1 = h - Sc(30);
    HBRUSH track = CreateSolidBrush(kLTrack);
    RECT trr{ bx0, by0, bx1, by1 };
    FillRect(mem, &trr, track); DeleteObject(track);
    RECT fill{ bx0, by0, bx0 + static_cast<int>((bx1 - bx0) * std::min(1.0, std::max(0.0, f.progress))), by1 };
    if (fill.right > fill.left)
    {
        HBRUSH acc = CreateSolidBrush(kLAccent);
        FillRect(mem, &fill, acc); DeleteObject(acc);
    }
}

// ---------------------------------------------------------------- 窓スレッド

LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;       // クリックしても前面を奪わない
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

struct DibSurface
{
    HDC mem = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    bool Create(int width, int height)
    {
        w = width; h = height;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;               // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC scr = GetDC(nullptr);
        mem = CreateCompatibleDC(scr);
        bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, scr);
        if (!mem || !bmp || !bits) return false;
        old = SelectObject(mem, bmp);
        return true;
    }
    ~DibSurface()
    {
        if (mem && old) SelectObject(mem, old);
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
    }
};

DWORD WINAPI SplashThread(LPVOID)
{
    G& g = S();
    const bool projectMode = g.projectMode;
    const float scale = sp::EffectiveDpiScale();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SplashProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_APPSTARTING);
    wc.lpszClassName = L"UnoSplashWnd";
    RegisterClassExW(&wc);   // 二重登録は失敗するだけで無害

    // ---- 窓を先に作って、メインスレッドを待たせない（描画器の立ち上げ D3D11/D2D は 100 ms 前後かかる）。
    //      窓の大きさは描画器の物理 px と同じ式（論理寸法 × 表示倍率）。窓は 1 枚目を載せるまで表示しない。
    int winW = static_cast<int>(std::lround(sp::kWindowW * scale));
    int winH = static_cast<int>(std::lround(sp::kWindowH * scale));
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    // WS_POPUP=枠なし / WS_EX_TOOLWINDOW=タスクバー非表示 / WS_EX_LAYERED=per-pixel alpha。
    // TOPMOST にはしない（アップデータの MessageBox 等を隠さないため。最前面へ上げるのはメイン窓を出す直前だけ）。
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"UnoSplashWnd", L"", WS_POPUP,
                                (sw - winW) / 2, (sh - winH) / 2, winW, winH, nullptr, nullptr, wc.hInstance, nullptr);
    g.hwnd.store(hwnd);
    if (g.readyEv) SetEvent(g.readyEv);                // ウィンドウ生成完了（メインスレッドを待たせない）
    if (!hwnd)
    {
        SplashDiag("スプラッシュ窓を作れませんでした");
        g.done.store(true);
        return 0;
    }

    // ---- 描画器。失敗 = GDI+ の簡易描画へ縮退（窓はカードの大きさに直す）
    auto renderer = std::make_unique<sp::SplashRenderer>();
    // UNO_SPLASH_FORCE_GDI=1: D2D の初期化に失敗したことにして、縮退経路（GDI+）を通す（検証用。--splash-selftest と併用）。
    const bool forceGdi = GetEnvironmentVariableW(L"UNO_SPLASH_FORCE_GDI", nullptr, 0) > 0;
    bool d2d = !forceGdi && renderer->Init(scale, g.logoPath);
    if (forceGdi) SplashDiag("UNO_SPLASH_FORCE_GDI: 縮退経路（GDI+）を強制します");
    else if (!d2d) SplashDiag("D2D 描画器の初期化に失敗（GDI+ の簡易描画へ縮退）: " + renderer->LastError());
    else if (renderer->UsedWarp()) SplashDiag("D3D11 はソフトウェア（WARP）で動作");
    g.usedD2d.store(d2d);
    g.usedWarp.store(d2d && renderer->UsedWarp());
    const double lifeT0 = Now();

    ULONG_PTR gdipToken = 0;
    std::unique_ptr<Gdiplus::Bitmap> legacyLogo;
    if (!d2d)
    {
        winW = static_cast<int>(std::lround(sp::kCardW * scale));
        winH = static_cast<int>(std::lround(sp::kCardH * scale));
        SetWindowPos(hwnd, nullptr, (sw - winW) / 2, (sh - winH) / 2, winW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
        HRGN rgn = CreateRoundRectRgn(0, 0, winW + 1, winH + 1, static_cast<int>(28 * scale), static_cast<int>(28 * scale));
        SetWindowRgn(hwnd, rgn, FALSE);                // 所有権は OS へ移る
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&gdipToken, &in, nullptr) == Gdiplus::Ok && !g.logoPath.empty())
        {
            legacyLogo = std::make_unique<Gdiplus::Bitmap>(g.logoPath.c_str());
            if (legacyLogo->GetLastStatus() != Gdiplus::Ok) legacyLogo.reset();
        }
    }
    DibSurface dib;
    if (!dib.Create(winW, winH))
    {
        SplashDiag("スプラッシュの描画面（DIB）を作れませんでした");
        DestroyWindow(hwnd);
        g.hwnd.store(nullptr);
        renderer.reset();
        if (gdipToken) { legacyLogo.reset(); Gdiplus::GdiplusShutdown(gdipToken); }
        g.done.store(true);
        return 0;
    }

    // ---- 演出監督
    sp::DirectorConfig cfg;
    cfg.projectMode = projectMode;
    cfg.tipSeed = static_cast<uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u));
    cfg.tips = sp::TipsWide();
    cfg.base.title = g.title;
    cfg.base.version = g.version;
    cfg.base.buildInfo = sp::BuildInfoText();
    cfg.base.recents = g.recents;
    cfg.base.projectName = g.projectName;
    cfg.base.sceneName = g.sceneName;
    cfg.readyLabel = L"プロジェクトを開きます";
    if (!projectMode) cfg.readyLabels = sp::DefaultReadyLabels();
    sp::Director director(cfg);

    // ---- 起動音（起動のみ。失敗は黙って続行）
    bool soundStarted = false, hitFired = false;
    double hitAt = 0.0;    // ready 開始からの秒
    auto startSound = [&]() {
        if (!g.playSound) return;
        const double raw = ReadStartupMsRaw();
        const double predicted = sp::ClampStartupMs(raw > 0.0 ? raw : sp::kStartupDefaultMs) / 1000.0;
        g.predictedFinishSec = predicted;
        const sp::TimingParams tp;
        soundStarted = SplashSound::Start(g.soundDir, predicted, tp.PopAfterReady());
        if (soundStarted) hitAt = SplashSound::TriggerOffsetSec();
    };

    // ---- フレームループ（60fps 固定 = 自前の高精度タイマ）
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const double frameSec = 1.0 / 60.0;
    double nextDeadline = Now();
    bool shown = false;
    int  failCount = 0;

    auto present = [&](double alpha) {
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = static_cast<BYTE>(std::lround(std::min(1.0, std::max(0.0, alpha)) * 255.0));
        bf.AlphaFormat = d2d ? AC_SRC_ALPHA : 0;
        POINT src{ 0, 0 };
        SIZE size{ winW, winH };
        HDC scr = GetDC(nullptr);
        UpdateLayeredWindow(hwnd, scr, nullptr, &size, dib.mem, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, scr);
    };

    auto tick = [&]() -> bool {
        const double t = Now();
        std::wstring status;
        double target = 0.0;
        int idx = 0, total = 0;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            status = g.status;
            target = g.tracker.Value(t);
            idx = g.tracker.StepIndex();
            total = g.tracker.StepTotal();
            if (g.manualStep) { idx = g.manualIdx; total = g.manualTotal; }
        }
        const sp::SplashFrame f = director.Step(t, target, status, idx, total, g.finishAt.load(), g.mainShownAt.load());
        if (f.wantMainShow) g.wantMain.store(true);
        if (soundStarted && !hitFired && f.sinceReady >= hitAt) { SplashSound::TriggerHit(); hitFired = true; }

        const double r0 = Now();
        bool ok = true;
        if (d2d)
        {
            ok = renderer->Draw(f, director.content()) && renderer->CopyPixels(static_cast<uint8_t*>(dib.bits), winW * 4);
        }
        else
        {
            PaintLegacy(dib.mem, winW, winH, scale, f, director.content(), legacyLogo.get());
        }
        if (!ok) { if (++failCount >= 3) return false; }
        else failCount = 0;
        present(f.windowAlpha);
        {
            const double ms = (Now() - r0) * 1000.0;
            if (g.frames.fetch_add(1) + 1 == 30) { g.cpuAt30 = ThreadCpuSec(); g.tAt30 = Now(); }   // 初期化（D3D/D2D の立ち上げ）を除いた定常の CPU を測るための基準
            double cur = g.renderMsSum.load();
            while (!g.renderMsSum.compare_exchange_weak(cur, cur + ms)) {}
            if (ms > g.renderMsMax.load()) g.renderMsMax.store(ms);
        }
        if (!shown)
        {
            if (!g.testNoShow.load()) ShowWindow(hwnd, SW_SHOWNORMAL);   // 1 枚目を載せてから出す（空の窓が一瞬見えない）
            shown = true;
            startSound();
        }
        // メイン窓を出す直前に最前面（TOPMOST）へ上げる。メイン窓は Show() で前面に出るので、上げておかないと
        // 1 フレームだけメイン窓がスプラッシュを覆ってちらつく。前面（フォーカス）は奪わない。退場（0.35 秒）で消える。
        if (f.wantMainShow && !g.testNoShow.load())
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return !f.finished;
    };

    bool running = true;
    while (running)
    {
        const double now = Now();
        if (timer)
        {
            double wait = nextDeadline - now;
            if (wait < 0.0) { wait = 0.0; if (now - nextDeadline > 0.1) nextDeadline = now; }   // 大きく遅れたら追いつくのを諦めて再同期
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        }
        const DWORD w = timer ? MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT) : WAIT_TIMEOUT;
        if (!timer) Sleep(16);
        if (w == WAIT_OBJECT_0 || !timer)
        {
            nextDeadline += frameSec;
            if (!tick()) { DestroyWindow(hwnd); }
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (timer) CloseHandle(timer);
    g.hwnd.store(nullptr);
    g.lifetimeSec = Now() - lifeT0;
    g.threadCpuSec = ThreadCpuSec();
    renderer.reset();
    if (gdipToken) { legacyLogo.reset(); Gdiplus::GdiplusShutdown(gdipToken); }
    g.done.store(true);
    return 0;
}

// 終わったスレッドの後始末（再表示に備える）
void ReapThread(G& g)
{
    if (g.thread && g.done.load())
    {
        WaitForSingleObject(g.thread, 1000);
        CloseHandle(g.thread);
        g.thread = nullptr;
        if (g.readyEv) { CloseHandle(g.readyEv); g.readyEv = nullptr; }
    }
}

void StartThread(G& g)
{
    g.done.store(false);
    g.wantMain.store(false);
    g.finishAt.store(-1.0);
    g.mainShownAt.store(-1.0);
    QueryPerformanceFrequency(&g.freq);
    QueryPerformanceCounter(&g.t0);
    g.readyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.thread = CreateThread(nullptr, 0, SplashThread, nullptr, 0, nullptr);
    if (!g.thread)
    {
        if (g.readyEv) { CloseHandle(g.readyEv); g.readyEv = nullptr; }
        g.done.store(true);
        return;
    }
    // ウィンドウ生成まで少しだけ待つ（初期化ログより先に画面へ出すため。失敗しても続行）。
    // 描画器の初期化（D3D11/D2D）は窓の生成より前に走るので、待つのは数十 ms。
    if (g.readyEv) WaitForSingleObject(g.readyEv, 1500);
}

void ShowCommon(bool projectMode, const std::string& titleUtf8, const std::string& versionUtf8, const std::string& logoPathUtf8,
                const std::string& projectNameUtf8, bool isNew)
{
    G& g = S();
    if (g.suppressed.load()) return;           // --background / --headless: スプラッシュ窓を出さない
    ReapThread(g);
    if (g.thread) return;                      // 表示中（起動の続きとして進む）

    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.title = sp::Utf8ToWide(titleUtf8);
        g.version = sp::Utf8ToWide(versionUtf8);
        g.logoPath = sp::Utf8ToWide(logoPathUtf8);
        g.projectMode = projectMode;
        g.isNew = isNew;
        g.projectName = sp::Utf8ToWide(projectNameUtf8);
        g.sceneName.clear();
        g.status = L"...";
        g.manualStep = false;
        g.finishCb = nullptr;
        g.recents = projectMode ? std::vector<std::wstring>() : sp::RecentProjectNames(3);
        g.tracker.Configure(projectMode ? sp::ProjectLoadPlan(isNew) : sp::StartupPlan(g.expectProject.load()));
        // <assets>/editor/icons/logo.png から <assets> を割り出す
        std::filesystem::path assets;
        if (!g.logoPath.empty())
            assets = std::filesystem::path(g.logoPath).parent_path().parent_path().parent_path();
        g.soundDir = assets.empty() ? std::wstring() : (assets / L"editor" / L"sounds").wstring() + L"\\";
        // 起動音: 起動のみ / 引数・環境変数・設定でオフ可 / デバイスは Start が黙って判定
        bool on = !projectMode && g.soundOn.load() && !g.testNoShow.load();
        if (on)
        {
            wchar_t env[8] = {};
            if (GetEnvironmentVariableW(L"UNO_NO_SPLASH_SOUND", env, 8) > 0 && env[0] != L'0') on = false;
        }
        if (on && !ReadStartupSoundSetting()) on = false;
        g.playSound = on;
    }
    StartThread(g);
}

} // namespace

// ---------------------------------------------------------------- 公開 API

void SplashScreen::Show(const std::string& titleUtf8, const std::string& versionUtf8, const std::string& logoPathUtf8)
{
    ShowCommon(false, titleUtf8, versionUtf8, logoPathUtf8, std::string(), false);
}

void SplashScreen::ShowProjectLoad(const std::string& titleUtf8, const std::string& versionUtf8, const std::string& logoPathUtf8,
                                   const std::string& projectNameUtf8, bool isNew)
{
    ShowCommon(true, titleUtf8, versionUtf8, logoPathUtf8, projectNameUtf8, isNew);
}

void SplashScreen::ExpectProjectLoad(bool on) { S().expectProject.store(on); }

void SplashScreen::SetSuppressed(bool on) { S().suppressed.store(on); }
void SplashScreen::SetSoundEnabled(bool on) { S().soundOn.store(on); }

void SplashScreen::SetTestNoShow(bool on) { S().testNoShow.store(on); }

SplashScreen::TestStats SplashScreen::GetTestStats()
{
    G& g = S();
    TestStats t;
    t.frames = g.frames.load();
    t.avgRenderMs = t.frames ? g.renderMsSum.load() / static_cast<double>(t.frames) : 0.0;
    t.maxRenderMs = g.renderMsMax.load();
    t.lifetimeSec = g.lifetimeSec;
    t.threadCpuSec = g.threadCpuSec;
    t.steadyCpuPercent = (g.lifetimeSec + 0.0 > g.tAt30 && g.tAt30 > 0.0) ? 100.0 * (g.threadCpuSec - g.cpuAt30) / (g.lifetimeSec - (g.tAt30)) : 0.0;
    t.d2d = g.usedD2d.load();
    t.warp = g.usedWarp.load();
    return t;
}

bool SplashScreen::GetStartupSoundSetting() { return ReadStartupSoundSetting(); }

void SplashScreen::SetStartupSoundSetting(bool on)
{
    const auto p = EditorStatePath();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream f(p);
        if (f)
        {
            nlohmann::json cur = nlohmann::json::parse(f, nullptr, false);
            if (cur.is_object()) j = std::move(cur);     // 既存のキー（lastOpenedScene 等）を保つ
        }
    }
    j["startupSound"] = on;
    std::ofstream o(p, std::ios::trunc);
    if (o) o << j.dump(2);
}
bool SplashScreen::IsShowing() { G& g = S(); return g.thread != nullptr && !g.done.load(); }

void SplashScreen::SetStatus(const std::string& statusUtf8)
{
    G& g = S();
    std::lock_guard<std::mutex> lk(g.mu);
    g.status = sp::Utf8ToWide(statusUtf8);
    // 再描画は 60fps のフレームループが拾うので通知不要。窓が未表示なら文字列だけ更新される。
}

void SplashScreen::SetStage(sp::Stage stage, const std::string& labelUtf8)
{
    G& g = S();
    if (g.done.load())
    {
        // 窓が無い（--background / --headless / ゲーム）。進捗は動かさないが、段階の開始時刻だけ診断ログへ残す
        // （プロセス開始からの実測。段階の重みの調整に使う）。1 段階につき 1 行。
        static std::atomic<int> s_last{-1};
        const int id = static_cast<int>(stage);
        if (id > s_last.load())
        {
            s_last.store(id);
            static const ULONGLONG s_base = GetTickCount64();
            char buf[112];
            std::snprintf(buf, sizeof(buf), "stage %s @ %llu ms（窓なし・起動処理の開始からの実測）", sp::StageName(stage),
                          static_cast<unsigned long long>(GetTickCount64() - s_base));
            SplashDiag(buf);
        }
        return;
    }
    const double t = Now();
    std::lock_guard<std::mutex> lk(g.mu);
    const int before = g.tracker.CurrentStageIndex();
    if (!g.tracker.Begin(stage, t)) return;   // 計画に無い段階/逆戻りは無視（文言も触らない）
    g.status = sp::Utf8ToWide(labelUtf8.empty() ? std::string(sp::StageLabelUtf8(stage)) : labelUtf8);
    g.manualStep = false;
    if (g.tracker.CurrentStageIndex() != before)
    {
        // 段階の開始時刻を診断ログへ（重みの実測調整に使う。Logger 初期化後に FlushDiagnostics が流す）
        char buf[96];
        std::snprintf(buf, sizeof(buf), "stage %s @ %.0f ms", sp::StageName(stage), t * 1000.0);
        SplashDiag(buf);
    }
}

void SplashScreen::SetStageProgress(float fraction)
{
    G& g = S();
    if (g.done.load()) return;
    std::lock_guard<std::mutex> lk(g.mu);
    g.tracker.SetSub(static_cast<double>(fraction));
}

void SplashScreen::SetProgress(float fraction)
{
    G& g = S();
    if (g.done.load()) return;
    std::lock_guard<std::mutex> lk(g.mu);
    g.tracker.SetAbsolute(static_cast<double>(fraction));
}

void SplashScreen::SetStep(int index, int total, const std::string& labelUtf8)
{
    G& g = S();
    std::lock_guard<std::mutex> lk(g.mu);
    g.manualStep = true;
    g.manualIdx = index;
    g.manualTotal = total;
    g.status = sp::Utf8ToWide(labelUtf8);
}

void SplashScreen::SetProjectInfo(const std::string& projectNameUtf8, const std::string& sceneNameUtf8)
{
    G& g = S();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!projectNameUtf8.empty()) g.projectName = sp::Utf8ToWide(projectNameUtf8);
    g.sceneName = sp::Utf8ToWide(sceneNameUtf8);
}

void SplashScreen::Finish(std::function<void()> showMainWindow)
{
    G& g = S();
    if (!IsShowing())
    {
        if (showMainWindow) showMainWindow();   // スプラッシュ無し: そのまま出す
        return;
    }
    const double t = Now();
    bool startup = false;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.finishCb = std::move(showMainWindow);
        g.hasCb.store(g.finishCb != nullptr);
        startup = !g.projectMode;
    }
    if (g.finishAt.load() < 0.0) g.finishAt.store(t);   // 二重呼び出しでは最初の時刻を保つ
    if (startup) SaveStartupMs(t * 1000.0);             // 次回の起動音の同期に使う（EMA）
    char buf[64];
    std::snprintf(buf, sizeof(buf), "finish @ %.0f ms", t * 1000.0);
    SplashDiag(buf);
}

void SplashScreen::PumpMainThread()
{
    G& g = S();
    if (SplashDiagPending()) FlushDiagnostics();
    if (!g.hasCb.load()) return;                 // 保留なし（ゲームや、スプラッシュ抑止時）: ロックも取らない
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.finishCb) return;
        // 演出が要求したとき、またはスプラッシュのスレッドが既に居ないとき（縮退・異常終了）に実行する
        if (!(g.wantMain.exchange(false) || g.done.load())) return;
        cb = std::move(g.finishCb);
        g.finishCb = nullptr;
        g.hasCb.store(false);
    }
    if (cb) cb();
    g.mainShownAt.store(Now());
}

void SplashScreen::Close()
{
    G& g = S();
    SplashSound::Abort();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.finishCb = nullptr;
        g.hasCb.store(false);
    }
    if (!g.thread) return;
    if (HWND hwnd = g.hwnd.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(g.thread, 3000);
    if (g.done.load()) ReapThread(g);
}

void SplashScreen::FlushDiagnostics()
{
    for (const auto& line : SplashDiagTake()) Logger::Info("スプラッシュ: {}", line);
}

} // namespace dx12e
