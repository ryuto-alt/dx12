#include "core/SplashGaugeRenderer.h"

#include <Windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <unordered_map>

#include "core/SplashBrandGeometry.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")

namespace dx12e::splash
{
using Microsoft::WRL::ComPtr;

namespace
{
namespace brand = uno::brand;
namespace L = gauge_layout;

D2D1_COLOR_F Col(const GaugeRgba& c, double alpha = 1.0)
{
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, static_cast<float>(c.a / 255.0 * alpha));
}
float F(double v) { return static_cast<float>(v); }

// 起動画面の文字（等幅 Light 1 本。Uno Splash Mono = PlemolJP Light のサブセット）。実寸は DIP。
// 文字は段階 M / S でも小さくしない（最小: 等幅 11 / 和文 12）。
struct TextSpec
{
    float size;        // DIP
    float lineH;       // 行の高さ DIP
    float trackEm;     // 字間（em）
};

// 点（brand::Pt）の配列から閉じた多角形の図形を足す
void AddPoly(ID2D1GeometrySink* sink, const brand::Pt* pts, size_t n)
{
    if (n < 3) return;
    sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < n; ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}
template <size_t N> void AddPolyArr(ID2D1GeometrySink* sink, const brand::Pt (&a)[N]) { AddPoly(sink, a, N); }

const wchar_t kFontFamilyCustom[] = L"Uno Splash Mono";
} // namespace

// ===========================================================================
// GaugeRenderer
// ===========================================================================
struct GaugeRenderer::Impl
{
    ID2D1DeviceContext* dc = nullptr;
    IDWriteFactory* dw = nullptr;
    float dpi = 1.0f;
    GaugeLayout layout = PickGaugeLayout(1e9, 1e9);
    std::string err;
    int pxW = 0, pxH = 0;
    unsigned bakes = 0;

    ComPtr<ID2D1SolidColorBrush> solid;
    ComPtr<ID2D1StrokeStyle> dash, roundJoin;
    ComPtr<ID2D1RadialGradientBrush> radBig, radSmall;
    ComPtr<ID2D1PathGeometry> markU, wordmark, needle;
    ComPtr<ID2D1GeometryGroup> hubGroup;
    ComPtr<ID2D1Bitmap1> layerBg, layerGrid, layerDial, layerUi;
    bool layersDirty = true;
    std::uint64_t uiKey = 0;     // 静的な UI の層の作り直しの鍵

    // 書体
    ComPtr<IDWriteFontCollection1> collection;
    std::wstring family = L"Consolas";
    std::wstring triedFontFile;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextFormat>> formats;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> layouts;

    // 表題欄の固定幅（静的な UI の層と動く値で共有する）
    std::vector<float> cellW;
    float blockX0 = 0.f, blockY0 = 0.f, blockH = 0.f;

    float Sc() const { return dpi * layout.fit; }
    const GaugeGeo& G() const { return layout.geo; }

    bool Init(ID2D1DeviceContext* d, IDWriteFactory* w, float dpiScale);
    bool BuildGeometries();
    void LoadFont(const std::wstring& file);
    void ResetFonts() { formats.clear(); layouts.clear(); }
    IDWriteTextFormat* Format(float size, float lineH);
    IDWriteTextLayout* Layout(const std::wstring& s, const TextSpec& ts, float maxW, DWRITE_TEXT_ALIGNMENT al);
    float TextW(const std::wstring& s, const TextSpec& ts);
    float Baseline(const std::wstring& s, const TextSpec& ts);
    void Text(const std::wstring& s, const TextSpec& ts, float x, float y, const GaugeRgba& c, double a = 1.0,
              float maxW = 2000.f, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING);

    void Line(float x0, float y0, float x1, float y1, const GaugeRgba& c, double a, float w, ID2D1StrokeStyle* ss = nullptr);
    void Polar(float x, float y, double ang, float r, float& ox, float& oy) const
    {
        const double a = ang * kPi / 180.0;
        ox = x + static_cast<float>(std::sin(a) * r);
        oy = y - static_cast<float>(std::cos(a) * r);
    }

    TextSpec Ts(float size, float trackEm = 0.f, float minSize = 11.f, float lineMul = 1.4f) const
    {
        const float k = G().k;
        const float s = std::max(size * k, std::min(size, minSize));
        return TextSpec{ s, s * lineMul, trackEm };
    }

    bool NeedBake(const SplashInfo& info);
    bool BakeLayer(ComPtr<ID2D1Bitmap1>& bmp, const std::function<void()>& draw);
    bool BakeAll(const SplashInfo& info);
    void DrawBg();
    void DrawGrid();
    void DrawDialStatic();
    void DrawUiStatic(const SplashInfo& info);
    void ComputeCells(const SplashInfo& info);

    void DrawLabels(double alpha);
    void DrawDynamic(const GaugeFrame& f, const GaugeReveal& rv, const SplashInfo& info);
    void DrawTicks(std::uint32_t lit, double alpha, const GaugeFrame& f);
    void DrawBezelSweep(const GaugeFrame& f);
    // スイープの弧（2 度刻みの固定の格子）のジオメトリのキャッシュ。[リング 0/1][格子の番号]
    ComPtr<ID2D1PathGeometry> sweepGeo[2][180];
    float sweepCx = -1.f, sweepCy = -1.f, sweepR = -1.f, sweepRIn = -1.f;
    void DrawUiDynamic(const GaugeFrame& f, const GaugeReveal& rv, const SplashInfo& info);
    void DrawLayer(ID2D1Bitmap1* bmp, double opacity);
    bool Render(const GaugeFrame& f, const SplashInfo& info);
};

static std::uint64_t HashStr(std::uint64_t h, const std::wstring& s)
{
    for (wchar_t c : s) { h ^= static_cast<std::uint64_t>(c); h *= 1099511628211ull; }
    h ^= 0xFFull; h *= 1099511628211ull;
    return h;
}

bool GaugeRenderer::Impl::Init(ID2D1DeviceContext* d, IDWriteFactory* w, float dpiScale)
{
    dc = d; dw = w;
    // 弧のジオメトリは作った factory に属するので、別の dc で Init し直したら捨てる
    for (auto& row : sweepGeo) for (auto& p : row) p.Reset();
    sweepCx = sweepCy = sweepR = sweepRIn = -1.f;
    dpi = dpiScale < 0.5f ? 1.0f : (dpiScale > 4.0f ? 4.0f : dpiScale);
    if (FAILED(dc->CreateSolidColorBrush(Col(kGaugeColors.t1), &solid))) { err = "CreateSolidColorBrush"; return false; }
    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    {
        const float dashes[4] = { 12.f, 3.f, 2.f, 3.f };
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f);
        if (FAILED(fac->CreateStrokeStyle(sp, dashes, 4, &dash))) { err = "CreateStrokeStyle(dash)"; return false; }
        D2D1_STROKE_STYLE_PROPERTIES rj = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND);
        if (FAILED(fac->CreateStrokeStyle(rj, nullptr, 0, &roundJoin))) { err = "CreateStrokeStyle(round)"; return false; }
    }
    if (!BuildGeometries()) return false;
    layersDirty = true;
    return true;
}

bool GaugeRenderer::Impl::BuildGeometries()
{
    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    // 主マークの U（100 グリッド → 文字盤の中心へ。K 倍）
    {
        if (FAILED(fac->CreatePathGeometry(&markU))) { err = "CreatePathGeometry(U)"; return false; }
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(markU->Open(&s))) return false;
        AddPolyArr(s.Get(), brand::kMarkU_L2);
        if (FAILED(s->Close())) return false;
    }
    // ワードマーク（キャップ 100。O は外周と内周を ALTERNATE で）
    {
        if (FAILED(fac->CreatePathGeometry(&wordmark))) { err = "CreatePathGeometry(wm)"; return false; }
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(wordmark->Open(&s))) return false;
        s->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        AddPolyArr(s.Get(), brand::kWmStd_0_U);
        AddPolyArr(s.Get(), brand::kWmStd_1_N);
        AddPolyArr(s.Get(), brand::kWmStd_2_O_Outer);
        AddPolyArr(s.Get(), brand::kWmStd_2_O_Inner);
        AddPolyArr(s.Get(), brand::kWmStd_3_E);
        AddPolyArr(s.Get(), brand::kWmStd_4_N);
        AddPolyArr(s.Get(), brand::kWmStd_5_G);
        AddPolyArr(s.Get(), brand::kWmStd_6_I);
        AddPolyArr(s.Get(), brand::kWmStd_7_N);
        AddPolyArr(s.Get(), brand::kWmStd_8_E);
        if (FAILED(s->Close())) return false;
    }
    // 指針（ハブを原点、真上 = -y。長さは rTickOut - 6、根元はピン穴の外）。段階ごとに寸法が違うので BakeAll で作り直す
    return true;
}

// ---------------------------------------------------------------- 書体

void GaugeRenderer::Impl::LoadFont(const std::wstring& file)
{
    if (file == triedFontFile) return;
    triedFontFile = file;
    collection.Reset();
    family = L"Consolas";
    ResetFonts();
    if (file.empty()) return;
    ComPtr<IDWriteFactory5> f5;
    if (FAILED(dw->QueryInterface(IID_PPV_ARGS(&f5)))) return;
    ComPtr<IDWriteFontFile> ff;
    if (FAILED(f5->CreateFontFileReference(file.c_str(), nullptr, &ff))) return;
    ComPtr<IDWriteFontSetBuilder1> b;
    if (FAILED(f5->CreateFontSetBuilder(&b))) return;
    if (FAILED(b->AddFontFile(ff.Get()))) return;
    ComPtr<IDWriteFontSet> set;
    if (FAILED(b->CreateFontSet(&set))) return;
    ComPtr<IDWriteFontCollection1> col;
    if (FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), &col))) return;
    UINT32 idx = 0; BOOL ex = FALSE;
    if (FAILED(col->FindFamilyName(kFontFamilyCustom, &idx, &ex)) || !ex) return;
    collection = col;
    family = kFontFamilyCustom;
}

IDWriteTextFormat* GaugeRenderer::Impl::Format(float size, float lineH)
{
    wchar_t key[48];
    swprintf(key, 48, L"%.3f|%.3f", size, lineH);
    auto it = formats.find(key);
    if (it != formats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    // 太さは Light だけ（収録しているのが Light 1 本。太字を頼むと擬似ボールドが掛かるので頼まない）
    if (FAILED(dw->CreateTextFormat(family.c_str(), collection.Get(), DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", &f)))
    {
        if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", &f)))
            return nullptr;
    }
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineH, lineH * 0.8f);
    auto* raw = f.Get();
    formats.emplace(key, std::move(f));
    return raw;
}

IDWriteTextLayout* GaugeRenderer::Impl::Layout(const std::wstring& s, const TextSpec& ts, float maxW, DWRITE_TEXT_ALIGNMENT al)
{
    wchar_t head[96];
    swprintf(head, 96, L"%.3f|%.3f|%.3f|%.1f|%d|", ts.size, ts.lineH, ts.trackEm, maxW, static_cast<int>(al));
    const std::wstring key = std::wstring(head) + s;
    auto it = layouts.find(key);
    if (it != layouts.end()) return it->second.Get();
    if (layouts.size() > 512) layouts.clear();
    IDWriteTextFormat* fmt = Format(ts.size, ts.lineH);
    if (!fmt) return nullptr;
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, maxW, ts.lineH * 2.0f, &l))) return nullptr;
    l->SetTextAlignment(al);
    if (ts.trackEm != 0.f)
    {
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(l.As(&l1)))
            l1->SetCharacterSpacing(0.f, ts.trackEm * ts.size, 0.f, DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(s.size()) });
    }
    auto* raw = l.Get();
    layouts.emplace(key, std::move(l));
    return raw;
}

float GaugeRenderer::Impl::TextW(const std::wstring& s, const TextSpec& ts)
{
    IDWriteTextLayout* l = Layout(s, ts, 4000.f, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!l) return 0.f;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float GaugeRenderer::Impl::Baseline(const std::wstring& s, const TextSpec& ts)
{
    IDWriteTextLayout* l = Layout(s, ts, 4000.f, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!l) return ts.size;
    UINT32 n = 0;
    l->GetLineMetrics(nullptr, 0, &n);
    if (n == 0) return ts.size;
    std::vector<DWRITE_LINE_METRICS> lm(n);
    l->GetLineMetrics(lm.data(), n, &n);
    return lm[0].baseline;
}

void GaugeRenderer::Impl::Text(const std::wstring& s, const TextSpec& ts, float x, float y, const GaugeRgba& c, double a, float maxW,
                               DWRITE_TEXT_ALIGNMENT al)
{
    if (s.empty() || a <= 0.0) return;
    IDWriteTextLayout* l = Layout(s, ts, maxW, al);
    if (!l) return;
    solid->SetColor(Col(c, a));
    dc->DrawTextLayout(D2D1::Point2F(x, y), l, solid.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void GaugeRenderer::Impl::Line(float x0, float y0, float x1, float y1, const GaugeRgba& c, double a, float w, ID2D1StrokeStyle* ss)
{
    if (a <= 0.0) return;
    solid->SetColor(Col(c, a));
    dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), solid.Get(), w, ss);
}

// ---------------------------------------------------------------- 静的な層

bool GaugeRenderer::Impl::BakeLayer(ComPtr<ID2D1Bitmap1>& bmp, const std::function<void()>& draw)
{
    if (!bmp)
    {
        const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(pxW), static_cast<UINT32>(pxH)), nullptr, 0,
                                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf), &bmp)))
        { err = "CreateBitmap(layer)"; return false; }
    }
    ComPtr<ID2D1Image> prev;
    dc->GetTarget(&prev);
    dc->SetTarget(bmp.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(Sc(), Sc()));
    draw();
    const HRESULT hr = dc->EndDraw();
    dc->SetTarget(prev.Get());
    if (FAILED(hr)) { err = "EndDraw(layer)"; return false; }
    return true;
}

void GaugeRenderer::Impl::DrawBg()
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    dc->Clear(Col(c.groove));
    auto radial = [&](D2D1_POINT_2F center, float rx, float ry, const D2D1_GRADIENT_STOP* st, UINT n, ComPtr<ID2D1RadialGradientBrush>& out) {
        ComPtr<ID2D1GradientStopCollection> gs;
        if (FAILED(dc->CreateGradientStopCollection(st, n, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs))) return false;
        return SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0, 0), rx, ry), gs.Get(), &out));
    };
    // 地: radial-gradient(1500px 900px at 800px 420px, bg0, groove 78%)
    const D2D1_GRADIENT_STOP s1[3] = { { 0.0f, Col(c.bg0) }, { 0.78f, Col(c.groove) }, { 1.0f, Col(c.groove) } };
    if (radial(D2D1::Point2F(800.f * g.k, 420.f * g.k), 1500.f * g.k, 900.f * g.k, s1, 3, radBig))
        dc->FillRectangle(D2D1::RectF(0, 0, g.W, g.H), radBig.Get());
    // 文字盤の周りの明かり: radial-gradient(640px 560px at 520px 450px, bg2, transparent 72%)
    const D2D1_GRADIENT_STOP s2[3] = { { 0.0f, Col(c.bg2) }, { 0.72f, Col(c.bg2, 0.0) }, { 1.0f, Col(c.bg2, 0.0) } };
    if (radial(D2D1::Point2F(g.dcx, g.dcy), 640.f * g.k, 560.f * g.k, s2, 3, radSmall))
        dc->FillRectangle(D2D1::RectF(0, 0, g.W, g.H), radSmall.Get());
}

void GaugeRenderer::Impl::DrawGrid()
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    const float in = L::kFrameInset;
    if (g.grid)
    {
        for (float x = in + L::kGridStep; x < g.W - in; x += L::kGridStep) Line(std::floor(x) + 0.5f, in, std::floor(x) + 0.5f, g.H - in, c.gridMinor, L::kGridOpacity, 1.f);
        for (float y = in + L::kGridStep; y < g.H - in; y += L::kGridStep) Line(in, std::floor(y) + 0.5f, g.W - in, std::floor(y) + 0.5f, c.gridMinor, L::kGridOpacity, 1.f);
    }
    // 枠（1DIP の border）
    solid->SetColor(Col(c.border));
    dc->DrawRectangle(D2D1::RectF(in + 0.5f, in + 0.5f, g.W - in - 0.5f, g.H - in - 0.5f), solid.Get(), 1.f);
}

void GaugeRenderer::Impl::DrawDialStatic()
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    const D2D1_POINT_2F ctr = D2D1::Point2F(g.dcx, g.dcy);
    solid->SetColor(Col(c.borderStrong));
    dc->DrawEllipse(D2D1::Ellipse(ctr, g.rBezel, g.rBezel), solid.Get(), 2.f);
    solid->SetColor(Col(c.border));
    dc->DrawEllipse(D2D1::Ellipse(ctr, g.rBezel - 7.f * g.k, g.rBezel - 7.f * g.k), solid.Get(), 1.f);
    solid->SetColor(Col(c.bg0));
    dc->FillEllipse(D2D1::Ellipse(ctr, g.rDisc, g.rDisc), solid.Get());
    solid->SetColor(Col(c.border));
    dc->DrawEllipse(D2D1::Ellipse(ctr, g.rDisc, g.rDisc), solid.Get(), 1.f);
    // U（文字盤の中央。マーク座標の (50,60) = ハブの中心を文字盤の中心へ）
    solid->SetColor(Col(c.t1));
    dc->SetTransform(D2D1::Matrix3x2F::Translation(-brand::kHubCenter.x, -brand::kHubCenter.y) * D2D1::Matrix3x2F::Scale(g.K, g.K) *
                     D2D1::Matrix3x2F::Translation(g.dcx, g.dcy) * D2D1::Matrix3x2F::Scale(Sc(), Sc()));
    dc->FillGeometry(markU.Get(), solid.Get());
    dc->SetTransform(D2D1::Matrix3x2F::Scale(Sc(), Sc()));
    // 中心線（一点鎖線。L だけ）
    if (g.labels)
    {
        const float R = g.rBezel;
        solid->SetColor(Col(c.t4, 0.6));
        auto dl = [&](float x0, float y0, float x1, float y1) { dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), solid.Get(), 1.f, dash.Get()); };
        dl(g.dcx - R - 18.f, g.dcy, g.dcx - R + 26.f, g.dcy);
        dl(g.dcx + R - 26.f, g.dcy, g.dcx + R + 18.f, g.dcy);
        dl(g.dcx, g.dcy + R - 26.f, g.dcx, g.dcy + R + 18.f);
    }
}

// 表題欄の列幅（静的な層と動く値で同じ幅を使う）
void GaugeRenderer::Impl::ComputeCells(const SplashInfo&)
{
    cellW.clear();
    const GaugeGeo& g = G();
    if (!g.titleBlock) return;
    const TextSpec lab = Ts(11.f, 0.f, 11.f), val = Ts(12.f, 0.f, 11.f);
    struct C { const wchar_t* label; const wchar_t* reserve; bool full; };
    static const C cells[5] = { { L"図番", L"UN-00.00.00", false }, { L"尺度", L"1 : 1", true }, { L"公差", L"±0.01", true },
                                { L"工程", L"04 / 05", false }, { L"状態", L"校正済", false } };
    for (const C& c : cells)
    {
        if (c.full && !g.titleBlockFull) { cellW.push_back(0.f); continue; }
        cellW.push_back(std::max(TextW(c.label, lab), TextW(c.reserve, val)) + 26.f);
    }
    float total = 0.f;
    for (float w : cellW) total += w;
    blockH = 8.f + 14.f + 6.f + 14.f + 9.f;
    blockX0 = g.W - 52.f - total;
    blockY0 = g.H - 46.f - blockH;
}

void GaugeRenderer::Impl::DrawUiStatic(const SplashInfo& info)
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    const float k = g.k;
    const float colX = L::kColX * k;
    const float colW = L::kColW * k;

    // 眉書き（28DIP の線 + 説明）
    {
        const float y = L::kEyebrowTop * k;
        Line(colX, y + 8.5f, colX + 28.f, y + 8.5f, c.t3, 1.0, 1.f);
        std::wstring eb = info.projectMode ? (info.isNew ? L"新規プロジェクト" : L"プロジェクト") : L"DX12 リアルタイムエンジン · エディタ";
        Text(eb, Ts(13.f, 0.12f, 12.f), colX + 42.f, y, c.t3, 1.0);
    }
    if (!info.projectMode)
    {
        // ワードマーク（キャップ 60。1 色 t1）
        const float cap = L::kWordmarkCap * k;
        solid->SetColor(Col(c.t1));
        dc->SetTransform(D2D1::Matrix3x2F::Scale(cap / 100.f, cap / 100.f) * D2D1::Matrix3x2F::Translation(colX, L::kWordmarkTop * k + 2.f) *
                         D2D1::Matrix3x2F::Scale(Sc(), Sc()));
        dc->FillGeometry(wordmark.Get(), solid.Get());
        dc->SetTransform(D2D1::Matrix3x2F::Scale(Sc(), Sc()));
        Text(L"精度で、世界をつくる。", Ts(17.f, 0.34f, 12.f), colX, L::kCatchTop * k, c.t2, 1.0);
    }
    else
    {
        // ワードマークの代わりにプロジェクト名
        Text(info.projectName, Ts(38.f, 0.f, 12.f, 1.3f), colX, L::kWordmarkTop * k + 4.f, c.t1, 1.0, colW);
        if (!info.sceneName.empty()) Text(L"シーン  " + info.sceneName, Ts(13.f, 0.04f, 12.f), colX, L::kCatchTop * k, c.t3, 1.0, colW);
    }

    // 左下の版情報
    {
        const TextSpec ts = Ts(12.f, 0.04f, 11.f, 1.75f);
        const int n = std::min<int>(g.verLines, static_cast<int>(info.verLines.size()) + 1);
        const float top = g.H - 46.f - n * ts.lineH;
        for (int i = 0; i < n; ++i)
        {
            const float y = top + i * ts.lineH;
            if (i == 0)
            {
                Text(info.version, ts, 52.f, y, c.t1, 1.0);
                const float w = TextW(info.version + L"  ", ts);
                if (!info.verLines.empty()) Text(info.verLines[0], ts, 52.f + w, y, c.t3, 1.0);
            }
            else if (static_cast<size_t>(i) < info.verLines.size()) Text(info.verLines[static_cast<size_t>(i)], ts, 52.f, y, c.t3, 1.0);
        }
    }

    // 表題欄（枠・仕切り・見出し）
    if (g.titleBlock)
    {
        float total = 0.f;
        for (float w : cellW) total += w;
        const float h = blockH, x0 = blockX0, y0 = blockY0;
        solid->SetColor(Col(c.bg0));
        dc->FillRectangle(D2D1::RectF(x0, y0, x0 + total, y0 + h), solid.Get());
        solid->SetColor(Col(c.borderStrong));
        dc->DrawRectangle(D2D1::RectF(x0 + 0.5f, y0 + 0.5f, x0 + total - 0.5f, y0 + h - 0.5f), solid.Get(), 1.f);
        static const wchar_t* const labels[5] = { L"図番", L"尺度", L"公差", L"工程", L"状態" };
        float x = x0;
        for (size_t i = 0; i < cellW.size(); ++i)
        {
            if (cellW[i] <= 0.f) continue;
            if (x > x0 + 1.f) Line(std::floor(x) + 0.5f, y0 + 1.f, std::floor(x) + 0.5f, y0 + h - 1.f, c.border, 1.0, 1.f);
            Text(labels[i], Ts(11.f, 0.f, 11.f), x + 13.f, y0 + 8.f, c.t3, 1.0);
            if (i == 1) Text(L"1 : 1", Ts(12.f, 0.f, 11.f), x + 13.f, y0 + 8.f + 14.f + 6.f, c.t1, 1.0);
            if (i == 2) Text(L"±0.01", Ts(12.f, 0.f, 11.f), x + 13.f, y0 + 8.f + 14.f + 6.f, c.t1, 1.0);
            x += cellW[i];
        }
    }
}

bool GaugeRenderer::Impl::NeedBake(const SplashInfo& info)
{
    std::uint64_t h = 1469598103934665603ull;
    h = HashStr(h, info.version);
    for (const auto& s : info.verLines) h = HashStr(h, s);
    h = HashStr(h, info.projectName);
    h = HashStr(h, info.sceneName);
    h = HashStr(h, std::wstring(1, info.projectMode ? L'P' : L'S'));
    h = HashStr(h, std::wstring(1, info.isNew ? L'N' : L'O'));
    h = HashStr(h, family);
    if (h != uiKey) { uiKey = h; return true; }
    return layersDirty;
}

bool GaugeRenderer::Impl::BakeAll(const SplashInfo& info)
{
    const GaugeGeo& g = G();
    GaugeWindowPixels(layout, dpi, pxW, pxH);
    const bool geoChanged = layersDirty;
    if (geoChanged) { layerBg.Reset(); layerGrid.Reset(); layerDial.Reset(); layerUi.Reset(); }
    // 指針の形（段階ごとに寸法が違う）。原点 = ハブ、真上 = -y
    {
        ComPtr<ID2D1Factory> fac;
        dc->GetFactory(&fac);
        needle.Reset();
        if (FAILED(fac->CreatePathGeometry(&needle))) { err = "CreatePathGeometry(needle)"; return false; }
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(needle->Open(&s))) return false;
        const float len = g.rTickOut - 6.f;
        const float root = brand::kPinR * g.K + 1.2f;
        const float w0 = L::kNeedleW0 * g.K * L::kNeedleWScaleRoot, w1 = L::kNeedleW1 * g.K * L::kNeedleWScaleTip;
        s->BeginFigure(D2D1::Point2F(-w0 / 2.f, -root), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(-w1 / 2.f, -len));
        s->AddLine(D2D1::Point2F(w1 / 2.f, -len));
        s->AddLine(D2D1::Point2F(w0 / 2.f, -root));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(s->Close())) return false;
        // ハブとピン穴（ALTERNATE の穴あき円）
        ComPtr<ID2D1EllipseGeometry> eo, ei;
        if (FAILED(fac->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(0, 0), brand::kHubR * g.K, brand::kHubR * g.K), &eo))) return false;
        if (FAILED(fac->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(0, 0), brand::kPinR * g.K, brand::kPinR * g.K), &ei))) return false;
        ID2D1Geometry* gs[2] = { eo.Get(), ei.Get() };
        ComPtr<ID2D1GeometryGroup> grp;
        if (FAILED(fac->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, gs, 2, &grp))) return false;
        hubGroup = grp;
    }
    ComputeCells(info);
    if (!BakeLayer(layerBg, [&] { DrawBg(); })) return false;
    if (!BakeLayer(layerGrid, [&] { DrawGrid(); })) return false;
    if (!BakeLayer(layerDial, [&] { DrawDialStatic(); })) return false;
    if (!BakeLayer(layerUi, [&] { DrawUiStatic(info); })) return false;
    layersDirty = false;
    ++bakes;
    return true;
}

// ---------------------------------------------------------------- 動く層

void GaugeRenderer::Impl::DrawLayer(ID2D1Bitmap1* bmp, double opacity)
{
    if (!bmp || opacity <= 0.0) return;
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->DrawBitmap(bmp, nullptr, F(opacity), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(Sc(), Sc()));
}

void GaugeRenderer::Impl::DrawTicks(std::uint32_t lit, double alpha, const GaugeFrame& f)
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    const float kk = std::max(g.k, 0.8f);
    for (std::uint32_t i = 0; i < 100 && i < lit; ++i)
    {
        const bool major = i % 10 == 0, mid = i % 5 == 0;
        if (!g.minor && !mid) continue;
        const float ln = (major ? 18.f : (mid ? 12.f : 7.f)) * kk;
        const GaugeRgba& col = major ? c.t2 : (mid ? c.t3 : c.t4);
        double op = major ? 1.0 : (mid ? 0.75 : 0.45);
        // 整列波（BD6）: 反射が通る目盛りは t1 へ寄せて濃くする（線の色の切り替えだけ。光らせない）。通り抜けたあとは元に戻る
        const double gl = gauge::AlignGlintMax(f.alignT, i * 3.6);
        GaugeRgba cc = col;
        if (gl > 0.0)
        {
            auto mix = [&](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>(std::lround(a + (b - a) * gl)); };
            cc = GaugeRgba{ mix(col.r, c.t1.r), mix(col.g, c.t1.g), mix(col.b, c.t1.b), col.a };
            op = op + (1.0 - op) * gl;
        }
        float x0, y0, x1, y1;
        Polar(g.dcx, g.dcy, i * 3.6, g.rTickOut, x0, y0);
        Polar(g.dcx, g.dcy, i * 3.6, g.rTickOut - ln, x1, y1);
        Line(x0, y0, x1, y1, cc, op * alpha, major ? 2.f : 1.f);
    }
}

// ベゼルの鏡面スイープ（BD6）: 縁（外の 2px の線と内側の 1px の線）に沿って、細い弧の重ね描きで反射を動かす。ぼかし・グロー・面は使わない。
void GaugeRenderer::Impl::DrawBezelSweep(const GaugeFrame& f)
{
    if (!(f.bellT > 0.f && f.bellT < 1.f)) return;
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    const double step = gauge::kSweepStepDeg;
    struct Ring { float r; float w; double scale; };
    const Ring rings[2] = { { g.rBezel, 2.f, 1.0 }, { g.rBezel - 7.f * g.k, 1.f, 0.6 } };
    if (sweepCx != g.dcx || sweepCy != g.dcy || sweepR != g.rBezel || sweepRIn != rings[1].r)
    {
        for (auto& row : sweepGeo) for (auto& p : row) p.Reset();
        sweepCx = g.dcx; sweepCy = g.dcy; sweepR = g.rBezel; sweepRIn = rings[1].r;
    }
    const int cells = static_cast<int>(std::lround(360.0 / step));
    for (int second = 0; second < 2; ++second)
    {
        const double centre = gauge::SweepCentreDeg(f.bellT) + (second ? 180.0 : 0.0);
        // 固定の格子（step 度刻み）に沿って描く。弧の形は格子ごとに 1 度だけ作ってキャッシュする（フレームごとに作らない）
        const int k0 = static_cast<int>(std::floor((centre - gauge::kSweepHalfWidthDeg) / step));
        const int k1 = static_cast<int>(std::ceil((centre + gauge::kSweepHalfWidthDeg) / step));
        for (int k = k0; k < k1; ++k)
        {
            const double a0 = k * step;
            const double p = gauge::SweepProfileAt(f.bellT, std::fmod(a0 + step * 0.5 + 720.0, 360.0), second != 0);
            if (p <= 0.004) continue;
            const int ci = ((k % cells) + cells) % cells;
            for (int ri = 0; ri < 2; ++ri)
            {
                const Ring& rg = rings[ri];
                ComPtr<ID2D1PathGeometry>& pg = sweepGeo[ri][ci];
                if (!pg)
                {
                    if (FAILED(fac->CreatePathGeometry(&pg))) continue;
                    ComPtr<ID2D1GeometrySink> s;
                    if (FAILED(pg->Open(&s))) { pg.Reset(); continue; }
                    float sx, sy, ex, ey;
                    // 継ぎ目は重ねない（半透明を二重に塗ると継ぎ目だけ明るい玉になる）。弧は隣と ちょうど接する
                    Polar(g.dcx, g.dcy, ci * step, rg.r, sx, sy);
                    Polar(g.dcx, g.dcy, ci * step + step, rg.r, ex, ey);
                    s->BeginFigure(D2D1::Point2F(sx, sy), D2D1_FIGURE_BEGIN_HOLLOW);
                    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(ex, ey), D2D1::SizeF(rg.r, rg.r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                    s->Close();
                }
                solid->SetColor(Col(c.t1, gauge::kSweepPeakAlpha * p * rg.scale));
                dc->DrawGeometry(pg.Get(), solid.Get(), rg.w);
            }
        }
    }
}

void GaugeRenderer::Impl::DrawLabels(double alpha)
{
    const GaugeGeo& g = G();
    if (!g.labels || alpha <= 0.0) return;
    const GaugeColors& c = kGaugeColors;
    const TextSpec ts = Ts(11.f, 0.f, 11.f, 1.2f);
    for (int i = 0; i < 10; ++i)
    {
        float x, y;
        Polar(g.dcx, g.dcy, i * 36.0, g.rLabel, x, y);
        wchar_t buf[8];
        if (i == 0)
        {
            // 真上は 0% と 100% が重なる点: 左に 100、右に 000
            swprintf(buf, 8, L"100");
            Text(buf, ts, x - 7.f - TextW(buf, ts), y - ts.lineH / 2.f, c.t2, alpha);
            Line(x, y - 6.f, x, y + 6.f, c.t4, 0.8 * alpha, 1.f);
            Text(L"000", ts, x + 7.f, y - ts.lineH / 2.f, c.t3, alpha);
        }
        else
        {
            swprintf(buf, 8, L"%03d", i * 10);
            Text(buf, ts, x - TextW(buf, ts) / 2.f, y - ts.lineH / 2.f, c.t3, alpha);
        }
    }
}

void GaugeRenderer::Impl::DrawDynamic(const GaugeFrame& f, const GaugeReveal& rv, const SplashInfo&)
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    const D2D1_POINT_2F ctr = D2D1::Point2F(g.dcx, g.dcy);
    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Scale(Sc(), Sc());

    DrawTicks(rv.litTicks, rv.dial > 0.f ? 1.0 : 0.0, f);
    DrawLabels(rv.labels);

    if (rv.ring)
    {
        solid->SetColor(Col(c.border));
        dc->DrawEllipse(D2D1::Ellipse(ctr, g.rArc, g.rArc), solid.Get(), 1.f);
        // 進捗の軌跡（無彩色。進捗の主役は指針なので弧は黄にしない）
        const double deg = std::min(static_cast<double>(f.needleDeg), 359.99);
        if (deg > 0.2)
        {
            ComPtr<ID2D1PathGeometry> pg;
            if (SUCCEEDED(fac->CreatePathGeometry(&pg)))
            {
                ComPtr<ID2D1GeometrySink> s;
                if (SUCCEEDED(pg->Open(&s)))
                {
                    float ex, ey;
                    Polar(g.dcx, g.dcy, deg, g.rArc, ex, ey);
                    s->BeginFigure(D2D1::Point2F(g.dcx, g.dcy - g.rArc), D2D1_FIGURE_BEGIN_HOLLOW);
                    s->AddArc(D2D1::ArcSegment(D2D1::Point2F(ex, ey), D2D1::SizeF(g.rArc, g.rArc), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                               deg > 180.0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                    s->Close();
                    solid->SetColor(Col(c.t3, 0.85));
                    dc->DrawGeometry(pg.Get(), solid.Get(), 1.5f);
                }
            }
        }
    }

    DrawBezelSweep(f);

    // 校正三角（ベゼルの真上）。ロックの前は輪郭だけ、ロックで面になる
    if (rv.litTicks >= 100)
    {
        ComPtr<ID2D1PathGeometry> pg;
        if (SUCCEEDED(fac->CreatePathGeometry(&pg)))
        {
            ComPtr<ID2D1GeometrySink> s;
            if (SUCCEEDED(pg->Open(&s)))
            {
                const float R = g.rBezel;
                s->BeginFigure(D2D1::Point2F(g.dcx - 7.f, g.dcy - R - 14.f), D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(D2D1::Point2F(g.dcx + 7.f, g.dcy - R - 14.f));
                s->AddLine(D2D1::Point2F(g.dcx, g.dcy - R - 3.f));
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                solid->SetColor(Col(c.accent));
                if (f.locked) dc->FillGeometry(pg.Get(), solid.Get());
                else dc->DrawGeometry(pg.Get(), solid.Get(), 1.5f, roundJoin.Get());
            }
        }
    }

    // 指針の残像（重ね描きのモーションブラー。面にしない。1px の線を blurDeg に応じて 1.2 度刻みで最大 12 本。BD6）
    if (f.needleShown && !f.locked)
    {
        const int n = gauge::TrailLineCount(f.blurDeg);
        const float len = g.rTickOut - 6.f;
        const double ang = f.needleDeg + gauge::OvershootDegOf(f.overshootPx);
        for (int i = 0; i < n; ++i)
        {
            const gauge::TrailLine tl = gauge::TrailLineAt(i, n, f.blurDeg);
            float x1, y1;
            Polar(g.dcx, g.dcy, ang - tl.offsetDeg, len, x1, y1);
            Line(g.dcx, g.dcy, x1, y1, c.accent, tl.alpha, 1.f);
        }
    }

    // 指針（外周に bg0 の縁）
    if (f.needleShown)
    {
        const double ang = f.needleDeg + gauge::OvershootDegOf(f.overshootPx);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(F(ang), D2D1::Point2F(0, 0)) * D2D1::Matrix3x2F::Translation(g.dcx, g.dcy) * base);
        solid->SetColor(Col(c.bg0));
        dc->DrawGeometry(needle.Get(), solid.Get(), L::kNeedleHaloW, roundJoin.Get());
        solid->SetColor(Col(c.accent));
        dc->FillGeometry(needle.Get(), solid.Get());
        dc->SetTransform(base);
    }
    // ハブとピン穴
    if (rv.dial > 0.f)
    {
        solid->SetColor(Col(c.bg0, rv.dial));
        dc->FillEllipse(D2D1::Ellipse(ctr, brand::kHubR * g.K + 1.5f, brand::kHubR * g.K + 1.5f), solid.Get());
        dc->SetTransform(D2D1::Matrix3x2F::Translation(g.dcx, g.dcy) * base);
        solid->SetColor(Col(c.accent, rv.dial));
        dc->FillGeometry(hubGroup.Get(), solid.Get());
        dc->SetTransform(base);
    }

    // 四隅のトンボ（黄 2DIP）と、辺の中点の細い印
    if (f.tomboT > 0.f)
    {
        const float kk = std::max(g.k, 0.8f);
        const float arm = L::kTomboArm * f.tomboT * kk;
        const float W = g.W, H = g.H, in = L::kTomboInset;
        const float cx[4] = { in, W - in, in, W - in }, cy[4] = { in, in, H - in, H - in };
        const float sx[4] = { 1, -1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
        solid->SetColor(Col(c.accent));
        for (int i = 0; i < 4; ++i)
        {
            ComPtr<ID2D1PathGeometry> pg;
            if (FAILED(fac->CreatePathGeometry(&pg))) continue;
            ComPtr<ID2D1GeometrySink> s;
            if (FAILED(pg->Open(&s))) continue;
            s->BeginFigure(D2D1::Point2F(cx[i] + sx[i] * arm, cy[i]), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddLine(D2D1::Point2F(cx[i], cy[i]));
            s->AddLine(D2D1::Point2F(cx[i], cy[i] + sy[i] * arm));
            s->EndFigure(D2D1_FIGURE_END_OPEN);
            s->Close();
            dc->DrawGeometry(pg.Get(), solid.Get(), L::kTomboWidth);
        }
        if (f.tomboT >= 1.f)
        {
            Line(W / 2.f, in, W / 2.f, in + 14.f, c.t4, 0.55, 1.f);
            Line(W / 2.f, H - in - 14.f, W / 2.f, H - in, c.t4, 0.55, 1.f);
            Line(in, H / 2.f, in + 14.f, H / 2.f, c.t4, 0.55, 1.f);
            Line(W - in - 14.f, H / 2.f, W - in, H / 2.f, c.t4, 0.55, 1.f);
        }
    }
}

void GaugeRenderer::Impl::DrawUiDynamic(const GaugeFrame& f, const GaugeReveal& rv, const SplashInfo& info)
{
    const GaugeGeo& g = G();
    const GaugeColors& c = kGaugeColors;
    const double a = rv.ui;
    if (a <= 0.0) return;
    const float k = g.k;
    const float colX = L::kColX * k, colW = L::kColW * k;
    const int n = static_cast<int>(info.steps.size());
    const int cur = std::max(0, std::min(info.stepCur, std::max(0, n - 1)));

    // 上端: 工程の位置だけ（製品名はワードマークが担う）
    {
        const TextSpec lab = Ts(12.f, 0.04f, 12.f, 1.34f), mono = Ts(12.f, 0.08f, 11.f, 1.34f);
        const std::wstring head = info.projectMode ? (info.isNew ? L"新しいプロジェクトを作成" : L"プロジェクトを開く") : L"起動シーケンス";
        Text(head, lab, 52.f, 46.f, c.t3, a);
        wchar_t seq[24];
        swprintf(seq, 24, L"%02d / %02d", cur + 1, std::max(n, 1));
        Text(seq, mono, 52.f + TextW(head, lab) + 12.f, 46.f, c.t2, a);
    }

    // 実進捗の読み取り
    {
        const TextSpec big{ 58.f * k, 58.f * k, -0.02f }, smallTs{ 22.f * k, 22.f * k, 0.f };
        const float top = L::kLoadTop * k + 2.f * k;
        const std::wstring num = GaugePercentText(info.progress);
        Text(num, big, colX, top, c.t1, a);
        const float w = TextW(num, big);
        Text(L"%", smallTs, colX + w + 6.f, top + Baseline(num, big) - Baseline(L"%", smallTs), c.t3, a);
        const float bottom = L::kLoadTop * k + 60.f * k - 3.f;
        Text(info.stepTitle, Ts(14.f, 0.f, 12.f, 1.43f), colX, bottom - 40.f, c.t1, a, colW, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(info.stepDetail, Ts(12.f, 0.f, 11.f, 1.5f), colX, bottom - 18.f, c.t3, a, colW, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // 工程の帯
    if (n > 0)
    {
        const float y = L::kStepsTop * k;
        if (g.stepBands)
        {
            const float gap = 10.f;
            const float cw = (colW - gap * (n - 1)) / static_cast<float>(n);
            for (int i = 0; i < n; ++i)
            {
                const float x = colX + i * (cw + gap);
                const SplashStepCell& s = info.steps[static_cast<size_t>(i)];
                const bool done = i < cur, now = i == cur;
                if (now) { solid->SetColor(Col(c.accent, a)); dc->FillRectangle(D2D1::RectF(x, y, x + cw, y + 2.f), solid.Get()); }
                else Line(x, y + 0.5f, x + cw, y + 0.5f, done ? c.inLineHover : c.borderStrong, a, 1.f);
                const TextSpec nt = Ts(11.5f, 0.f, 11.f, 1.4f), nm = Ts(13.f, 0.f, 12.f, 1.55f);
                const float ty = y + (now ? 7.f : 8.f) + 1.f;
                Text(s.no, nt, x, ty, now ? c.t1 : (done ? c.t3 : c.t4), a);
                if (done && s.sec >= 0.0)
                {
                    wchar_t t[16];
                    swprintf(t, 16, L"%.2f s", s.sec);
                    Text(t, nt, x, ty, c.t3, a, cw, DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
                Text(s.name, nm, x, ty + 19.f, now ? c.t1 : (done ? c.t2 : c.t4), a);
            }
        }
        else
        {
            const SplashStepCell& s = info.steps[static_cast<size_t>(cur)];
            wchar_t t[24];
            swprintf(t, 24, L"%02d / %02d", cur + 1, n);
            solid->SetColor(Col(c.accent, a));
            dc->FillRectangle(D2D1::RectF(colX, y, colX + colW, y + 2.f), solid.Get());
            const TextSpec nt = Ts(11.5f, 0.f, 11.f, 1.4f), nm = Ts(13.f, 0.f, 12.f, 1.55f);
            Text(t, nt, colX, y + 9.f, c.t1, a);
            Text(s.name, nm, colX + TextW(t, nt) + 12.f, y + 6.f, c.t1, a);
        }
    }

    // 表題欄の値（図番は登録のピンで刻印が出る）
    if (g.titleBlock && !cellW.empty())
    {
        const TextSpec vt = Ts(12.f, 0.f, 11.f);
        const float vy = blockY0 + 8.f + 14.f + 6.f;
        float x = blockX0;
        for (size_t i = 0; i < cellW.size(); ++i)
        {
            if (cellW[i] <= 0.f) continue;
            if (i == 0) Text(info.drawingNo, vt, x + 13.f, vy, c.t1, a * f.pinT);
            if (i == 3)
            {
                wchar_t t[24];
                swprintf(t, 24, L"%02d / %02d", cur + 1, std::max(n, 1));
                Text(t, vt, x + 13.f, vy, c.t1, a);
            }
            if (i == 4) Text(f.seated ? L"校正済" : L"校正中", vt, x + 13.f, vy, f.seated ? c.t1 : c.t2, a);
            x += cellW[i];
        }
    }
}

bool GaugeRenderer::Impl::Render(const GaugeFrame& f, const SplashInfo& info)
{
    if (!dc) { err = "not initialized"; return false; }
    LoadFont(info.fontFile);
    if (NeedBake(info))
    {
        if (!BakeAll(info)) return false;
    }
    const GaugeReveal rv = EvaluateReveal(f);
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawLayer(layerBg.Get(), 1.0);
    DrawLayer(layerGrid.Get(), rv.grid);
    DrawLayer(layerDial.Get(), rv.dial);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(Sc(), Sc()));
    DrawDynamic(f, rv, info);
    DrawLayer(layerUi.Get(), rv.ui);
    DrawUiDynamic(f, rv, info);
    const HRESULT hr = dc->EndDraw();
    if (FAILED(hr)) { err = "EndDraw"; return false; }
    return true;
}

GaugeRenderer::GaugeRenderer() : p_(std::make_unique<Impl>()) {}
GaugeRenderer::~GaugeRenderer() = default;
void GaugeRenderer::SetLayout(const GaugeLayout& layout) { p_->layout = layout; p_->layersDirty = true; }
bool GaugeRenderer::Init(ID2D1DeviceContext* dc, IDWriteFactory* dw, float dpi) { return p_->Init(dc, dw, dpi); }
bool GaugeRenderer::Render(const GaugeFrame& frame, const SplashInfo& info) { return p_->Render(frame, info); }
void GaugeRenderer::OnDpiChanged(float dpi) { p_->dpi = dpi < 0.5f ? 1.0f : (dpi > 4.0f ? 4.0f : dpi); p_->layersDirty = true; }
const std::string& GaugeRenderer::LastError() const { return p_->err; }
unsigned GaugeRenderer::StaticBakeCount() const { return p_->bakes; }

// ===========================================================================
// GaugeSurface
// ===========================================================================
struct GaugeSurface::Impl
{
    float dpi = 1.0f;
    GaugeLayout layout;
    int W = 0, H = 0;
    std::string err;
    bool warp = false, comInit = false;
    std::wstring adapter;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2dDev;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> target, readback;
    ComPtr<IDWriteFactory> dw;
    GaugeRenderer renderer;

    bool MakeTarget()
    {
        target.Reset(); readback.Reset();
        GaugeWindowPixels(layout, dpi, W, H);
        const D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(W), static_cast<UINT32>(H));
        const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        if (FAILED(dc->CreateBitmap(sz, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf), &target))) { err = "CreateBitmap(target)"; return false; }
        if (FAILED(dc->CreateBitmap(sz, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf), &readback)))
        { err = "CreateBitmap(readback)"; return false; }
        dc->SetTarget(target.Get());
        return true;
    }

    bool Init(float dpiScale, const GaugeLayout& lay, bool forceWarp)
    {
        dpi = dpiScale < 0.5f ? 1.0f : (dpiScale > 4.0f ? 4.0f : dpiScale);
        layout = lay;
        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit = SUCCEEDED(hrCom);
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        HRESULT hr = E_FAIL;
        if (!forceWarp)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
            warp = SUCCEEDED(hr);
        }
        if (FAILED(hr)) { err = "D3D11CreateDevice"; return false; }
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(d3d.As(&dxgi))) { err = "IDXGIDevice"; return false; }
        {
            ComPtr<IDXGIAdapter> ad;
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(dxgi->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&d))) adapter = d.Description;
        }
        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(factory.GetAddressOf()))))
        { err = "D2D1CreateFactory"; return false; }
        if (FAILED(factory->CreateDevice(dxgi.Get(), &d2dDev))) { err = "ID2D1Factory1::CreateDevice"; return false; }
        if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) { err = "CreateDeviceContext"; return false; }
        ComPtr<IDWriteFactory5> f5;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(f5.GetAddressOf()))))
            dw = f5;
        else if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
        { err = "DWriteCreateFactory"; return false; }
        if (!MakeTarget()) return false;
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // 検査（黄の画素）はグレースケール AA 前提
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        renderer.SetLayout(layout);
        if (!renderer.Init(dc.Get(), dw.Get(), dpi)) { err = renderer.LastError(); return false; }
        return true;
    }
};

GaugeSurface::GaugeSurface() : p_(std::make_unique<Impl>()) {}
GaugeSurface::~GaugeSurface()
{
    const bool ci = p_->comInit;
    p_.reset();
    if (ci) CoUninitialize();
}
bool GaugeSurface::Init(float dpiScale, const GaugeLayout& layout, bool forceWarp) { return p_->Init(dpiScale, layout, forceWarp); }
int GaugeSurface::Width() const { return p_->W; }
int GaugeSurface::Height() const { return p_->H; }
bool GaugeSurface::Draw(const GaugeFrame& frame, const SplashInfo& info)
{
    if (!p_->renderer.Render(frame, info)) { p_->err = p_->renderer.LastError(); return false; }
    return true;
}
bool GaugeSurface::CopyPixels(std::uint8_t* dst, int dstPitch)
{
    Impl& s = *p_;
    if (FAILED(s.readback->CopyFromBitmap(nullptr, s.target.Get(), nullptr))) return false;
    D2D1_MAPPED_RECT m{};
    if (FAILED(s.readback->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    const size_t rowBytes = static_cast<size_t>(s.W) * 4;
    for (int y = 0; y < s.H; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(dstPitch), m.bits + static_cast<size_t>(y) * m.pitch, rowBytes);
    s.readback->Unmap();
    return true;
}
bool GaugeSurface::OnDpiChanged(float dpiScale)
{
    Impl& s = *p_;
    s.dpi = dpiScale < 0.5f ? 1.0f : (dpiScale > 4.0f ? 4.0f : dpiScale);
    s.dc->SetTarget(nullptr);
    const bool ok = s.MakeTarget();   // 失敗したら描けない（呼び側が従来の経路へ切り替える。次の Draw も失敗を返す）
    s.renderer.OnDpiChanged(s.dpi);
    return ok;
}
const std::string& GaugeSurface::LastError() const { return p_->err; }
bool GaugeSurface::UsedWarp() const { return p_->warp; }
std::wstring GaugeSurface::AdapterName() const { return p_->adapter; }
unsigned GaugeSurface::StaticBakeCount() const { return p_->renderer.StaticBakeCount(); }

} // namespace dx12e::splash
