#include "core/SplashPreview.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/DpiScale.h"
#include "core/SplashCommon.h"
#include "core/SplashControl.h"
#include "core/SplashDirector.h"
#include "core/SplashGaugeRenderer.h"
#include "core/SplashProgress.h"
#include "core/SplashRenderer.h"
#include "core/SplashScreen.h"
#include "core/Version.h"

#pragma comment(lib, "windowscodecs.lib")

namespace dx12e
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace sp = splash;

struct Options
{
    std::filesystem::path dir;
    float dpi = 1.0f;
    bool projectMode = false;
    std::wstring projectName = L"Junction";
    std::string backdrop = "mid";
    double fps = 30.0;
    // ---- Graphite Gauge（--splash-dump / 既定のスタイル）
    bool dump = false;                       // --splash-dump <dir>
    std::vector<double> times;               // --splash-times 0,148,...（ms）。空なら 1000/fps ごと
    std::vector<double> skipAt;              // --splash-skip-at ms,...（仮想スキップ）
    double finishAtMs = 1000.0;              // --splash-finish-at ms（完了要求の時刻。台本）
    std::string tier = "L";                  // --splash-tier L|M|S
    bool warp = false;                       // --splash-warp（WARP で測る）
    bool reduce = false;                     // --splash-reduce（Reduce Motion の姿）
    bool classic = false;                    // --splash-style classic
};

std::vector<double> ParseNumList(const std::wstring& w)
{
    std::vector<double> v;
    std::wstringstream ss(w);
    std::wstring item;
    while (std::getline(ss, item, L',')) if (!item.empty()) v.push_back(_wtof(item.c_str()));
    return v;
}

// 数値・キーワードだけの引数用（ASCII 前提）。
std::string Narrow(const std::wstring& w)
{
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

bool ParseArgs(int argc, wchar_t** argv, Options& o, bool& requested)
{
    requested = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& out) { if (i + 1 < argc) { out = argv[++i]; return true; } return false; };
        std::wstring v;
        if (a == L"--splash-preview") { if (next(v)) { o.dir = v; requested = true; } }
        else if (a == L"--splash-dump") { if (next(v)) { o.dir = v; o.dump = true; requested = true; } }
        else if (a == L"--splash-times") { if (next(v)) o.times = ParseNumList(v); }
        else if (a == L"--splash-skip-at") { if (next(v)) o.skipAt = ParseNumList(v); }
        else if (a == L"--splash-finish-at") { if (next(v)) o.finishAtMs = _wtof(v.c_str()); }
        else if (a == L"--splash-tier") { if (next(v)) o.tier = Narrow(v); }
        else if (a == L"--splash-warp") o.warp = true;
        else if (a == L"--splash-reduce") o.reduce = true;
        else if (a == L"--splash-style") { if (next(v)) o.classic = (v == L"classic"); }
        else if (a == L"--dpi-scale") { if (next(v)) { float s = 1.0f; if (dpi::ParseScale(Narrow(v), s)) o.dpi = s; } }
        else if (a.rfind(L"--dpi-scale=", 0) == 0) { float s = 1.0f; if (dpi::ParseScale(Narrow(a.substr(12)), s)) o.dpi = s; }
        else if (a == L"--splash-mode") { if (next(v)) o.projectMode = (v == L"project"); }
        else if (a == L"--splash-project") { if (next(v)) o.projectName = v; }
        else if (a == L"--splash-backdrop") { if (next(v)) o.backdrop = Narrow(v); }
        else if (a == L"--splash-fps") { if (next(v)) o.fps = std::max(5.0, std::min(120.0, _wtof(v.c_str()))); }
    }
    return requested && !o.dir.empty();
}

bool WritePng(const std::filesystem::path& path, const uint8_t* bgra, int w, int h)
{
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(enc->CreateNewFrame(&fr, nullptr))) return false;
    if (FAILED(fr->Initialize(nullptr))) return false;
    if (FAILED(fr->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fr->SetPixelFormat(&fmt))) return false;
    if (FAILED(fr->WritePixels(static_cast<UINT>(h), static_cast<UINT>(w * 4), static_cast<UINT>(w * 4 * h), const_cast<BYTE*>(bgra)))) return false;
    if (FAILED(fr->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// 背景（デスクトップ想定）の色。位置 (x,y)/(w,h) で緩いグラデーション。
void Backdrop(const std::string& kind, int x, int y, int w, int h, double& r, double& g, double& b)
{
    const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
    if (kind == "light") { const double k = 0.86 - 0.06 * v; r = k; g = k + 0.01; b = k + 0.03; }
    else if (kind == "dark") { const double k = 0.10 + 0.04 * v; r = k; g = k + 0.005; b = k + 0.015; }
    else { r = 0.30 + 0.16 * u; g = 0.40 + 0.14 * (1.0 - v) * 0.6 + 0.06 * u; b = 0.58 + 0.10 * (1.0 - v); }   // 青みのある壁紙風
}

const char* PhaseName(sp::Phase p)
{
    switch (p)
    {
    case sp::Phase::Intro: return "intro";
    case sp::Phase::Loading: return "loading";
    case sp::Phase::Ready: return "ready";
    case sp::Phase::Transition: return "transition";
    default: return "done";
    }
}

struct StageEvent { double t; sp::Stage stage; };

// 開発ツリー / 配布のどちらでも <assets>/editor/splash/fonts を探す
std::wstring FindSplashFont()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path base = std::filesystem::path(exe).parent_path();
    std::error_code ec;
    for (int up = 0; up < 5; ++up)
    {
        const std::wstring f = sp::SplashFontPath((base / L"assets").wstring());
        if (!f.empty()) return f;
        base = base.parent_path();
    }
    return {};
}

// 台本の文言（決定論。実機の GPU 名などは入れない）
sp::SplashInfo MakeDumpInfo(const Options& o)
{
    sp::SplashInfo info;
    info.projectMode = o.projectMode;
    info.projectName = o.projectName;
    info.sceneName = L"scenes/main.json";
    info.version = L"v" + sp::Utf8ToWide(kEngineVersion);
    info.drawingNo = L"UN-" + sp::Utf8ToWide(kEngineVersion);
    info.verLines = { L"2026-11-27 12:00 ビルド", L"GPU 8 GB · Driver 0.00", L"前回 3.78 s" };
    info.fontFile = FindSplashFont();
    const int n = sp::GaugeStepCount(o.projectMode, false);
    for (int i = 0; i < n; ++i)
    {
        sp::SplashStepCell c;
        wchar_t no[8];
        std::swprintf(no, 8, L"%02d", i + 1);
        c.no = no;
        c.name = sp::GaugeStepName(o.projectMode, false, i);
        info.steps.push_back(std::move(c));
    }
    return info;
}

// events.json の kind と同じ小文字の名前
const char* EventKindName(sp::calibrate::EventKind k)
{
    using K = sp::calibrate::EventKind;
    switch (k)
    {
    case K::Duck: return "duck";
    case K::Click: return "click";
    case K::Latch: return "latch";
    case K::Tok: return "tok";
    case K::Sub: return "sub";
    case K::Bell: return "bell";
    case K::Ping: return "ping";
    case K::End: return "end";
    }
    return "?";
}

// --splash-dump / 既定のプレビュー（Graphite Gauge）: 窓なし・音なしで、指定時刻の絵を PNG に書く。
// 実窓と同じ GaugeSurface + GaugeFastDriver を 60Hz の台本で走らせる（時計は仮想）。
int RunGaugeDump(const Options& o)
{
    std::error_code ec;
    std::filesystem::create_directories(o.dir, ec);
    if (o.dpi != 1.0f) dpi::SetOverride(o.dpi);
    sp::GaugeLayout lay;
    lay.tier = o.tier == "M" ? sp::GaugeTier::M : (o.tier == "S" ? sp::GaugeTier::S : sp::GaugeTier::L);
    lay.geo = sp::MakeGaugeGeo(lay.tier);
    sp::GaugeSurface surf;
    if (!surf.Init(o.dpi, lay, o.warp))
    {
        std::fprintf(stderr, "splash-dump: 描画面の初期化に失敗: %s\n", surf.LastError().c_str());
        return 1;
    }
    std::printf("splash-dump: dpi=%.2f tier=%s size=%dx%d px  %s%s\n", o.dpi, o.tier.c_str(), surf.Width(), surf.Height(),
                surf.UsedWarp() ? "WARP" : "GPU", o.reduce ? " reduce" : "");

    std::vector<double> times = o.times;
    if (times.empty())
        for (double t = 0.0; t <= 2500.0 + 1e-9; t += 1000.0 / o.fps) times.push_back(t);
    std::sort(times.begin(), times.end());
    const double maxT = times.back();

    sp::SplashInfo info = MakeDumpInfo(o);
    const int n = static_cast<int>(info.steps.size());
    const sp::CalibrateEvents& ev = sp::DefaultCalibrateEvents();
    sp::GaugeFastDriver drv;
    std::vector<double> skip = o.skipAt;
    std::sort(skip.begin(), skip.end());
    size_t nextSkip = 0;

    // 更新の時刻列 = 60Hz の格子 + 書き出す時刻（単調）
    std::vector<double> ticks;
    for (double t = 0.0; t <= maxT + 1e-9; t += 1000.0 / 60.0) ticks.push_back(t);
    for (double t : times) ticks.push_back(t);
    for (double t : skip) ticks.push_back(t);
    std::sort(ticks.begin(), ticks.end());
    ticks.erase(std::unique(ticks.begin(), ticks.end()), ticks.end());

    const int W = surf.Width(), H = surf.Height();
    std::vector<uint8_t> px(static_cast<size_t>(W) * static_cast<size_t>(H) * 4);
    std::ofstream csv(o.dir / "frames.csv");
    // t / phase / percent は classic の frames.csv と同じ列（splash_contact_sheet.py 互換）。
    // 後ろの 4 列は A/V 検証用（W1-BD8。tools/check_splash_av.py が読む）:
    //   event  = 前の書き出し時刻（含まない）〜この時刻（含む）に入る音のイベント。"kind:index"（events.json と同じ名前）を '|' でつなぐ。無ければ空
    //   frame  = 出力 fps の格子での番号（round(tMs * fps / 1000)）
    //   gpuMs  = Draw の GPU 時間（D3D11 のタイムスタンプ。読めなければ -1）、cpuMs = Draw 呼び出しの CPU 時間
    csv << "index,tMs,needleDeg,locked,file,t,phase,percent,event,frame,gpuMs,cpuMs\n";
    surf.EnableGpuTiming(true);
    double prevWrittenT = -1.0;
    size_t wi = 0;
    int written = 0;
    double drawSum = 0.0, drawMax = 0.0, copySum = 0.0;
    auto wall = [] { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart); };
    int lastStep = -1;
    for (double t : ticks)
    {
        const bool finishReq = t >= o.finishAtMs;
        const double prog = std::min(0.995, sp::Clamp01(t / std::max(1.0, o.finishAtMs)) * 0.995);
        sp::GaugeFrame f;
        bool locked = false;
        if (o.reduce)
        {
            const double finMs = finishReq ? o.finishAtMs : std::nan("");
            f = sp::EvaluateGauge(sp::GaugePath::Reduce, t, 0.f, ev, sp::GaugeExitStartMs(ev.pingMs, finMs));
            locked = finishReq;
        }
        else
        {
            drv.Update(t, static_cast<float>(prog), finishReq);
            while (nextSkip < skip.size() && skip[nextSkip] <= t + 1e-9) { drv.Skip(t); ++nextSkip; }
            f = drv.Frame(t);
            const double lw = drv.LockWallMs();
            locked = !std::isnan(lw) && t >= lw;
        }
        const int step = std::min(n - 1, static_cast<int>(prog * n));
        if (step > lastStep)
        {
            if (lastStep >= 0) info.steps[static_cast<size_t>(lastStep)].sec = 0.20 + 0.11 * lastStep;
            lastStep = step;
        }
        info.stepCur = std::max(0, lastStep);
        static const wchar_t* const kTitles[5] = { L"コアを初期化中", L"グラフィックスデバイスを初期化中", L"シェーダーとパイプラインを構築中", L"アセットを読み込み中", L"エディタUIを初期化中" };
        info.stepTitle = locked ? L"校正完了" : kTitles[std::min(info.stepCur, 4)];
        info.stepDetail = locked ? L"すべての工程が完了しました" : L"8,536 / 12,480 件";
        info.progress = locked ? 1.0 : prog;
        if (wi >= times.size() || std::fabs(t - times[wi]) > 1e-9) continue;
        ++wi;
        const double d0 = wall();
        if (!surf.Draw(f, info)) { std::fprintf(stderr, "splash-dump: 描画に失敗 (t=%.1f): %s\n", t, surf.LastError().c_str()); return 1; }
        const double d1 = wall();
        if (!surf.CopyPixels(px.data(), W * 4)) { std::fprintf(stderr, "splash-dump: 読み戻しに失敗\n"); return 1; }
        const double d2 = wall();
        drawSum += d1 - d0; drawMax = std::max(drawMax, d1 - d0); copySum += d2 - d1;
        char name[64];
        std::snprintf(name, sizeof(name), "kf_%04d_ms.png", static_cast<int>(std::lround(t)));
        if (!WritePng(o.dir / name, px.data(), W, H)) { std::fprintf(stderr, "splash-dump: PNG を書けません: %s\n", name); return 1; }
        std::string evs;
        for (const auto& e : sp::calibrate::kEvents)
        {
            if (!(e.tMs > prevWrittenT && e.tMs <= t + 1e-9)) continue;
            if (!evs.empty()) evs += '|';
            evs += EventKindName(e.kind);
            evs += ':';
            evs += std::to_string(e.index);
        }
        prevWrittenT = t;
        char row[320];
        std::snprintf(row, sizeof(row), "%d,%.1f,%.2f,%d,%s,%.4f,%s,%.1f,%s,%ld,%.4f,%.4f\n", written, t, f.needleDeg, f.locked ? 1 : 0, name, t / 1000.0,
                      locked ? "locked" : (finishReq ? "finish" : "gauge"), info.progress * 100.0, evs.c_str(),
                      std::lround(t * o.fps / 1000.0), surf.LastGpuMs(), d1 - d0);
        csv << row;
        ++written;
    }
    std::printf("splash-dump: %d 枚 -> %ls  draw avg %.2f ms max %.2f ms  readback avg %.2f ms  staticBakes=%u\n", written, o.dir.c_str(),
                written ? drawSum / written : 0.0, drawMax, written ? copySum / written : 0.0, surf.StaticBakeCount());
    return 0;
}

} // namespace

// ---- 実窓のコードを窓なしで通す自己検査
bool RunSelfTest(int argc, wchar_t** argv, int& exitCode)
{
    std::filesystem::path dir;
    float dpiScale = 1.0f;
    bool requested = false;
    std::vector<double> skipAtMs;      // --splash-skip-at ms,...: Show からの経過時刻で SplashScreen::Skip() を呼ぶ（仮想入力）
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--splash-skip-at" && i + 1 < argc) skipAtMs = ParseNumList(argv[++i]);
        else if (a == L"--splash-selftest" && i + 1 < argc) { dir = argv[++i]; requested = true; }
        else if (a == L"--dpi-scale" && i + 1 < argc) { float s = 1.0f; if (dpi::ParseScale(Narrow(argv[++i]), s)) dpiScale = s; }
    }
    if (!requested) return false;
    exitCode = 1;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (dpiScale != 1.0f) dpi::SetOverride(dpiScale);
    std::ofstream log(dir / "selftest.txt");

    auto cpuSec = [] {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        auto v = [](FILETIME f) { return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
        return v(k) + v(u);
    };
    auto wallSec = [] { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart); };

    // 開発ツリー/配布のどちらでもロゴを探す（プレビューと同じ）
    std::wstring logo;
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path base = std::filesystem::path(exe).parent_path();
        for (int up = 0; up < 5 && logo.empty(); ++up)
        {
            const auto cand = base / L"assets" / L"editor" / L"icons" / L"logo.png";
            if (std::filesystem::exists(cand, ec)) logo = cand.wstring();
            base = base.parent_path();
        }
    }
    std::string logoU8;
    for (wchar_t c : logo) logoU8.push_back(c < 128 ? static_cast<char>(c) : '?');   // 検証用: 非 ASCII のパスは想定しない

    SplashScreen::SetTestNoShow(true);
    const double w0 = wallSec(), c0 = cpuSec();
    SplashScreen::Show(kEngineName, std::string("v") + kEngineVersion, logoU8);
    log << "IsShowing after Show: " << (SplashScreen::IsShowing() ? "yes" : "no") << "\n";
    if (!SplashScreen::IsShowing()) { log << "FAIL: スレッドが起動しない\n"; return true; }

    // 初期化の流れを模す（メインスレッドがブロックしても止まらないことを確かめるため、途中で 700ms 固まる）
    const sp::Stage seq[] = { sp::Stage::UpdateCheck, sp::Stage::Window, sp::Stage::Graphics, sp::Stage::Audio, sp::Stage::Physics,
                              sp::Stage::Shaders, sp::Stage::Assets, sp::Stage::Scripts, sp::Stage::ShadowMap, sp::Stage::EditorUi,
                              sp::Stage::Renderer, sp::Stage::EnvMap, sp::Stage::Finalize };
    const double tStart = wallSec();
    size_t skipIdx = 0;
    std::sort(skipAtMs.begin(), skipAtMs.end());
    auto pumpSkips = [&] {
        while (skipIdx < skipAtMs.size() && (wallSec() - w0) * 1000.0 >= skipAtMs[skipIdx]) { sp::RequestSplashSkip(); ++skipIdx; }
    };
    for (sp::Stage st : seq)
    {
        pumpSkips();
        SplashScreen::SetStage(st);
        if (st == sp::Stage::Renderer) Sleep(700);   // 重い処理（メインスレッドが固まる）
        else Sleep(180);
    }
    const auto before = SplashScreen::GetTestStats();
    const double initElapsed = wallSec() - tStart;
    log << "frames during ~" << initElapsed << " s of init (incl. a 700 ms main-thread freeze): " << before.frames << "\n";

    // 終了: リング完成 → 周回 → ポン → メイン窓（コールバック）→ 退場
    double cbAt = -1.0;
    const double finishReq = wallSec();
    SplashScreen::Finish([&] { cbAt = wallSec() - finishReq; });
    double doneAt = -1.0;
    while (wallSec() - finishReq < 6.0)
    {
        pumpSkips();
        SplashScreen::PumpMainThread();
        if (!SplashScreen::IsShowing()) { doneAt = wallSec() - finishReq; break; }
        Sleep(8);
    }
    const double w1 = wallSec(), c1 = cpuSec();
    const auto st = SplashScreen::GetTestStats();
    const sp::TimingParams tp;
    const double expectCb = tp.ReadyDur();
    log << "renderer: " << (st.d2d ? "D2D" : "GDI+ fallback") << (st.warp ? " (WARP)" : "") << "\n";
    log << "frames total: " << st.frames << "  lifetime " << st.lifetimeSec << " s  => " << (st.lifetimeSec > 0 ? st.frames / st.lifetimeSec : 0) << " fps\n";
    log << "render+copy+ULW per frame: avg " << st.avgRenderMs << " ms  max " << st.maxRenderMs << " ms\n";
    log << "splash thread CPU: total " << st.threadCpuSec << " s (D3D/D2D 立ち上げ込み)  steady " << st.steadyCpuPercent << " % of one core\n";
    log << "process CPU: " << (c1 - c0) << " s over " << (w1 - w0) << " s wall => " << 100.0 * (c1 - c0) / (w1 - w0) << " % of one core (whole test process)\n";
    log << "showMainWindow callback at +" << cbAt << " s after Finish (expected >= " << expectCb << " s)\n";
    log << "splash gone at +" << doneAt << " s after Finish (expected ~" << expectCb + tp.transitionDur << " s)\n";
    // UNO_SPLASH_FORCE_GDI=1 のときは縮退経路（GDI+）を通す検査なので、D2D でなくてよい
    const bool forcedGdi = GetEnvironmentVariableW(L"UNO_SPLASH_FORCE_GDI", nullptr, 0) > 0;
    const bool gaugeStyle = sp::UseGaugeStyle() && !forcedGdi;
    bool ok = false;
    if (gaugeStyle)
    {
        // Graphite Gauge: 本体はロックの瞬間に出る（完了要求から 0.3〜1.7 秒の最終接近、または正規のタイムライン）。
        // 退場（ピン → 窓のフェード 180ms）まで見届けて消える。時刻は SplashGaugeMotion.h の規則（絶対値は固定しない）。
        log << "style: Graphite Gauge" << (sp::ReduceMotionRequested() ? " (Reduce Motion)" : "") << "\n";
        ok = st.d2d && st.frames > 100 && cbAt >= 0.0 && cbAt < 4.0 && doneAt > cbAt && doneAt < cbAt + 2.5;
    }
    else
    {
        ok = (st.d2d || forcedGdi) && st.frames > 100 && cbAt >= expectCb - 0.05 && cbAt < expectCb + 0.5 && doneAt > cbAt && doneAt < expectCb + tp.transitionDur + 0.6;
    }
    // 700ms 固まっている間も描画が続いていたか（フレーム数が 60fps 相当なら止まっていない）
    ok = ok && static_cast<double>(before.frames) > 60.0 * 0.8 * (initElapsed - 0.3);
    log << (ok ? "RESULT: OK\n" : "RESULT: NG\n");
    exitCode = ok ? 0 : 1;
    return true;
}

bool RunSplashPreviewIfRequested(int argc, wchar_t** argv, int& exitCode)
{
    if (RunSelfTest(argc, argv, exitCode)) return true;
    Options o;
    bool requested = false;
    if (!ParseArgs(argc, argv, o, requested)) return false;

    exitCode = 1;
    std::error_code ec;
    std::filesystem::create_directories(o.dir, ec);

    // 既定のスタイルは Graphite Gauge。--splash-style classic（または環境変数 UNO_SPLASH_STYLE=classic）で従来のカード
    {
        wchar_t envv[16] = {};
        GetEnvironmentVariableW(L"UNO_SPLASH_STYLE", envv, 16);
        const bool gaugeStyle = !o.classic && sp::ParseGaugeStyle(L"", envv);
        if (o.dump || gaugeStyle)
        {
            exitCode = RunGaugeDump(o);
            return true;
        }
    }

    // ---- 描画器（実窓と同じ）。DPI は --dpi-scale（無ければ 1.0）
    if (o.dpi != 1.0f) dpi::SetOverride(o.dpi);
    sp::SplashRenderer renderer;
    std::wstring logo;
    {
        // exe の隣（配布）→ 開発ツリー（exe = build/release/DX12Engine.exe の 2 つ上）の順に探す
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path base = std::filesystem::path(exe).parent_path();
        for (int up = 0; up < 5 && logo.empty(); ++up)
        {
            const auto cand = base / L"assets" / L"editor" / L"icons" / L"logo.png";
            if (std::filesystem::exists(cand, ec)) logo = cand.wstring();
            base = base.parent_path();
        }
    }
    if (!renderer.Init(o.dpi, logo))
    {
        std::fprintf(stderr, "splash-preview: 描画器の初期化に失敗: %s\n", renderer.LastError().c_str());
        return true;
    }
    std::printf("splash-preview: dpi=%.2f  size=%dx%d px  %s  logo=%s\n", o.dpi, renderer.Width(), renderer.Height(),
                renderer.UsedWarp() ? "WARP" : "GPU", logo.empty() ? "(none)" : "ok");

    // ---- 台本（決定論）
    sp::DirectorConfig cfg;
    cfg.projectMode = o.projectMode;
    cfg.tips = sp::TipsWide();
    cfg.tipSeed = 4242;
    cfg.base.title = sp::Utf8ToWide(kEngineName);
    cfg.base.version = L"v" + sp::Utf8ToWide(kEngineVersion);
    cfg.base.buildInfo = L"2026-09-30 12:34 ビルド";        // 決定論（実窓は exe のリンク時刻）
    cfg.base.projectName = o.projectName;
    cfg.base.sceneName = L"scenes/main.json";
    if (!o.projectMode) cfg.base.recents = { L"Junction", L"Nocturne", L"StudioLumen" };
    cfg.readyLabel = L"プロジェクトを開きます";
    if (!o.projectMode) cfg.readyLabels = sp::DefaultReadyLabels();
    sp::Director dir(cfg);

    sp::ProgressTracker tracker;
    std::vector<StageEvent> events;
    double finishAt = 0.0;
    if (!o.projectMode)
    {
        tracker.Configure(sp::StartupPlan(false));
        events = { {0.00, sp::Stage::UpdateCheck}, {0.45, sp::Stage::Window}, {0.55, sp::Stage::Graphics}, {0.95, sp::Stage::Audio},
                   {1.05, sp::Stage::Physics}, {1.15, sp::Stage::Shaders}, {2.05, sp::Stage::Assets}, {2.45, sp::Stage::Scripts},
                   {2.65, sp::Stage::ShadowMap}, {2.75, sp::Stage::EditorUi}, {3.05, sp::Stage::Renderer}, {3.25, sp::Stage::Thumbnails},
                   {3.55, sp::Stage::EnvMap}, {3.80, sp::Stage::Finalize} };
        finishAt = 4.0;
    }
    else
    {
        tracker.Configure(sp::ProjectLoadPlan(false));
        events = { {0.00, sp::Stage::ProjectScene}, {0.40, sp::Stage::ProjectAssets}, {2.60, sp::Stage::ProjectMaterials},
                   {3.30, sp::Stage::ProjectFinalize} };
        finishAt = 3.6;
    }

    // ---- 走らせる（毎フレーム進めて、書き出す時刻だけ描く）
    const double dt = 1.0 / 60.0;
    const double writeEvery = 1.0 / o.fps;
    const auto& P = dir.timeline().params();
    const double transReady = std::max(finishAt, P.introDur) + P.ReadyDur();
    const double endT = transReady + P.transitionDur + 0.12;
    double mainShownAt = -1.0;
    double nextWrite = 0.0;
    bool hit10 = false, hit40 = false, hit70 = false;
    size_t evIdx = 0;
    std::wstring status = cfg.projectMode ? L"" : L"";
    int written = 0;
    std::vector<uint8_t> px(static_cast<size_t>(renderer.Width()) * static_cast<size_t>(renderer.Height()) * 4);
    std::vector<uint8_t> out(px.size());
    std::ofstream csv(o.dir / "frames.csv");
    csv << "index,t,phase,percent,file\n";

    for (double t = 0.0; t <= endT + 1e-9; t += dt)
    {
        while (evIdx < events.size() && events[evIdx].t <= t + 1e-9)
        {
            tracker.Begin(events[evIdx].stage, t);
            status = sp::Utf8ToWide(sp::StageLabelUtf8(events[evIdx].stage));
            ++evIdx;
        }
        // 段階内の実報告のある段階（サムネイル / アセット / マテリアル）
        if (!o.projectMode) { if (t >= 3.25 && t < 3.55) tracker.SetSub((t - 3.25) / 0.30); }
        else
        {
            if (t >= 0.40 && t < 2.60) tracker.SetSub((t - 0.40) / 2.20);
            if (t >= 2.60 && t < 3.30) tracker.SetSub((t - 2.60) / 0.70);
        }
        const double target = tracker.Value(t);
        const sp::SplashFrame f = dir.Step(t, target, status, tracker.StepIndex(), tracker.StepTotal(), t >= finishAt ? finishAt : -1.0, mainShownAt);
        if (f.wantMainShow) mainShownAt = t;   // プレビューではメイン窓は即座に出せたことにする

        // 書き出し判定
        bool write = false;
        std::string tag = PhaseName(f.phase);
        const bool dense = (f.phase == sp::Phase::Intro || f.phase == sp::Phase::Ready || f.phase == sp::Phase::Transition);
        if (dense && t + 1e-9 >= nextWrite) write = true;
        if (f.phase == sp::Phase::Loading)
        {
            if (!hit10 && f.progress >= 0.10) { hit10 = write = true; tag = "loading_p10"; }
            else if (!hit40 && f.progress >= 0.40) { hit40 = write = true; tag = "loading_p40"; }
            else if (!hit70 && f.progress >= 0.70) { hit70 = write = true; tag = "loading_p70"; }
        }
        if (dense) { if (write) nextWrite = t + writeEvery - 1e-9; }
        if (!write) continue;

        if (!renderer.Draw(f, dir.content()) || !renderer.CopyPixels(px.data(), renderer.Width() * 4))
        {
            std::fprintf(stderr, "splash-preview: 描画に失敗 (t=%.3f)\n", t);
            return true;
        }
        // 窓全体のフェード（実窓の SourceConstantAlpha）+ 背景へ合成
        const double ca = f.windowAlpha;
        const int w = renderer.Width(), h = renderer.Height();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
                double br, bg, bb;
                Backdrop(o.backdrop, x, y, w, h, br, bg, bb);
                const double a = px[i + 3] / 255.0 * ca;
                const double r = px[i + 2] / 255.0 * ca + br * (1.0 - a);
                const double g = px[i + 1] / 255.0 * ca + bg * (1.0 - a);
                const double b = px[i + 0] / 255.0 * ca + bb * (1.0 - a);
                out[i + 0] = static_cast<uint8_t>(std::lround(std::min(1.0, b) * 255.0));
                out[i + 1] = static_cast<uint8_t>(std::lround(std::min(1.0, g) * 255.0));
                out[i + 2] = static_cast<uint8_t>(std::lround(std::min(1.0, r) * 255.0));
                out[i + 3] = 255;
            }
        char name[96];
        std::snprintf(name, sizeof(name), "%03d_%s_%04dms.png", written, tag.c_str(), static_cast<int>(std::lround(t * 1000.0)));
        if (!WritePng(o.dir / name, out.data(), w, h))
        {
            std::fprintf(stderr, "splash-preview: PNG を書けません: %s\n", name);
            return true;
        }
        char row[160];
        std::snprintf(row, sizeof(row), "%d,%.3f,%s,%d,%s\n", written, t, tag.c_str(), f.percent, name);
        csv << row;
        ++written;
    }
    std::printf("splash-preview: %d 枚を書き出しました -> %ls\n", written, o.dir.c_str());
    exitCode = 0;
    return true;
}

} // namespace dx12e
