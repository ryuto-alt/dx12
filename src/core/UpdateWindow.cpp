#include "core/UpdateWindow.h"

#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

#include "core/DpiScale.h"
#include "core/ReleaseNotes.h"
#include "core/SplashCommon.h"
#include "core/UpdateLogic.h"
#include "core/Version.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxguid.lib")

namespace dx12e::updateui
{
using Microsoft::WRL::ComPtr;
namespace ul = updatelogic;

namespace
{
// ---- 配色（src/editor/ThemeVariants.h の MakeDefault と同じ値）
constexpr uint32_t kBg0 = 0x090A10, kBg1 = 0x11131B, kBg2 = 0x181B26, kBg3 = 0x232736, kBg4 = 0x2D3247;
constexpr uint32_t kBorder = 0x1F2333, kBorderSt = 0x39415E;
constexpr uint32_t kAccent = 0x2F96FF, kAccentHi = 0x62B4FF, kAccentPr = 0x1C7BE6, kOnAccent = 0x06101F;
constexpr uint32_t kText = 0xDCDFEC, kTextMid = 0xC1C6D8, kTextDim = 0xA0A6BC, kTextFaint = 0x9299B2;
constexpr uint32_t kGood = 0x4FD08A, kWarn = 0xF0B04A, kBad = 0xFF7570;

D2D1_COLOR_F Col(uint32_t rgb, double a = 1.0)
{
    return D2D1::ColorF(static_cast<float>(((rgb >> 16) & 0xFF) / 255.0), static_cast<float>(((rgb >> 8) & 0xFF) / 255.0),
                        static_cast<float>((rgb & 0xFF) / 255.0), static_cast<float>(a));
}
D2D1_COLOR_F Mix(uint32_t a, uint32_t b, double t, double alpha = 1.0)
{
    auto ch = [&](int s) { return static_cast<double>((a >> s) & 0xFF) * (1.0 - t) + static_cast<double>((b >> s) & 0xFF) * t; };
    return D2D1::ColorF(static_cast<float>(ch(16) / 255.0), static_cast<float>(ch(8) / 255.0), static_cast<float>(ch(0) / 255.0),
                        static_cast<float>(alpha));
}
float F(double v) { return static_cast<float>(v); }

bool FontExists(IDWriteFactory* dw, const std::wstring& family)
{
    ComPtr<IDWriteFontCollection> col;
    if (FAILED(dw->GetSystemFontCollection(&col, FALSE))) return false;
    UINT32 idx = 0; BOOL ex = FALSE;
    return SUCCEEDED(col->FindFamilyName(family.c_str(), &idx, &ex)) && ex;
}
std::wstring PickIn(IDWriteFactory* dw, const std::vector<std::wstring>& cands)
{
    for (const auto& c : cands) if (FontExists(dw, c)) return c;
    return cands.back();
}

double Now()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// ---- カード内の寸法（カードのローカル論理座標）
constexpr float kPad = 36.0f;
constexpr float kBtnH = 40.0f;
constexpr float kBtnY = kCardH - 36.0f - kBtnH;   // ボタン行の上端
constexpr float kRingCx = 156.0f, kRingCy = 262.0f, kRingR = 80.0f;
constexpr float kRightX = 312.0f;
} // namespace

// ---------------------------------------------------------------- 純ロジック

std::vector<Button> LayoutButtons(const Model& m)
{
    std::vector<Button> v;
    const float y = kMargin + kBtnY;
    const float right = kMargin + kCardW - kPad;
    auto add = [&](Choice id, const wchar_t* label, Button::Style st, float w, float x) {
        Button b;
        b.id = id; b.label = label; b.style = st; b.x = x; b.y = y; b.w = w; b.h = kBtnH;
        v.push_back(std::move(b));
    };
    if (m.mode == Mode::Prompt)
    {
        add(Choice::UpdateNow, L"今すぐ更新", Button::Style::Primary, 156.0f, right - 156.0f);
        add(Choice::Later, L"後で", Button::Style::Secondary, 104.0f, right - 156.0f - 12.0f - 104.0f);
        add(Choice::SkipVersion, L"この版を飛ばす", Button::Style::Link, 132.0f, kMargin + kPad);
    }
    else if (m.mode == Mode::Error)
    {
        add(Choice::Retry, L"もう一度", Button::Style::Primary, 140.0f, right - 140.0f);
        add(Choice::StartAnyway, L"このまま起動", Button::Style::Secondary, 150.0f, right - 140.0f - 12.0f - 150.0f);
    }
    return v;
}

void SetBodyFromMarkdown(Model& m, const std::string& md)
{
    m.body.clear();
    m.more = 0;
    m.bodyMissing = md.empty();
    if (md.empty()) return;
    const ul::BodyView v = ul::FormatGithubBody(md, 6);
    for (const auto& l : v.lines)
    {
        BodyLineW w;
        w.type = l.type;
        w.text = splash::Utf8ToWide(l.text);
        m.body.push_back(std::move(w));
    }
    m.more = v.more;
    if (m.body.empty()) m.bodyMissing = true;
}

// ---------------------------------------------------------------- 描画器

struct Renderer::Impl
{
    float dpi = 1.0f;
    int   W = 0, H = 0;
    std::string err;
    bool  comInit = false;

    ComPtr<ID3D11Device>          d3d;
    ComPtr<ID2D1Factory1>         factory;
    ComPtr<ID2D1Device>           d2dDev;
    ComPtr<ID2D1DeviceContext>    dc;
    ComPtr<ID2D1Bitmap1>          target, readback, shadow, logo;
    ComPtr<IDWriteFactory>        dw;
    ComPtr<ID2D1SolidColorBrush>  solid;
    ComPtr<ID2D1LinearGradientBrush> cardFill, cardEdge;
    ComPtr<ID2D1RadialGradientBrush> glowAccent, glowCard;
    ComPtr<ID2D1BitmapBrush1>     logoBrush;
    ComPtr<ID2D1StrokeStyle>      roundCap;
    std::wstring jpFamily, monoFamily;
    std::map<std::tuple<int, int, int, int>, ComPtr<IDWriteTextFormat>> fmts;
    UINT logoW = 0, logoH = 0;

    ~Impl() { fmts.clear(); }

    bool Init(float dpiScale, const std::wstring& logoPath);
    bool BuildBrushes();
    bool BuildShadow();
    bool LoadLogo(const std::wstring& path);

    void Base() { dc->SetTransform(D2D1::Matrix3x2F::Translation(F(kMargin), F(kMargin)) * D2D1::Matrix3x2F::Scale(dpi, dpi)); }
    IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT wt, bool mono, bool wrap);
    // 1 行（はみ出したら省略記号）。w は最大幅。
    void Txt(const std::wstring& s, float x, float y, float w, float size, DWRITE_FONT_WEIGHT wt, const D2D1_COLOR_F& col,
             DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, bool mono = false);
    // 折り返しの段落。使った高さを返す。
    float Para(const std::wstring& s, float x, float y, float w, float size, DWRITE_FONT_WEIGHT wt, const D2D1_COLOR_F& col, float maxH = 2000.0f);
    float TextW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT wt, bool mono = false);
    void Glow(ID2D1RadialGradientBrush* b, double cx, double cy, double r, double op);
    void Arc(double a0deg, double sweepDeg, double r, double w, const D2D1_COLOR_F& col);
    void Check(double cx, double cy, double s, const D2D1_COLOR_F& col, double w);

    void Draw(const Model& m, int hover, int pressed);
    void Header(const Model& m);
    void DrawPrompt(const Model& m);
    void DrawProgress(const Model& m);
    void DrawError(const Model& m);
    void DrawButtons(const Model& m, int hover, int pressed);
};

bool Renderer::Impl::Init(float dpiScale, const std::wstring& logoPath)
{
    dpi = dpiScale < 0.5f ? 1.0f : (dpiScale > 4.0f ? 4.0f : dpiScale);
    W = static_cast<int>(std::lround(kWindowW * dpi));
    H = static_cast<int>(std::lround(kWindowH * dpi));

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInit = SUCCEEDED(hrCom);     // RPC_E_CHANGED_MODE（別モードで初期化済み）はそのまま使える

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) { err = "D3D11CreateDevice"; return false; }

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d.As(&dxgi))) { err = "IDXGIDevice"; return false; }
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(factory.GetAddressOf()))))
    { err = "D2D1CreateFactory"; return false; }
    if (FAILED(factory->CreateDevice(dxgi.Get(), &d2dDev))) { err = "ID2D1Factory1::CreateDevice"; return false; }
    if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) { err = "CreateDeviceContext"; return false; }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
    { err = "DWriteCreateFactory"; return false; }

    const D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(W), static_cast<UINT32>(H));
    const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf), &target))) { err = "CreateBitmap(target)"; return false; }
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf), &readback)))
    { err = "CreateBitmap(readback)"; return false; }
    dc->SetTarget(target.Get());
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // 透明な面に描くので ClearType は使えない
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    jpFamily = PickIn(dw.Get(), { L"Yu Gothic UI", L"Meiryo UI", L"Segoe UI" });
    monoFamily = PickIn(dw.Get(), { L"Cascadia Mono", L"Consolas", L"Courier New" });

    if (!BuildBrushes()) { if (err.empty()) err = "BuildBrushes"; return false; }
    if (!BuildShadow()) { if (err.empty()) err = "BuildShadow"; return false; }
    LoadLogo(logoPath);   // 失敗してもテキストのみで続行
    return true;
}

bool Renderer::Impl::BuildBrushes()
{
    if (FAILED(dc->CreateSolidColorBrush(Col(0xFFFFFF), &solid))) return false;
    auto stops2 = [&](D2D1_GRADIENT_STOP s0, D2D1_GRADIENT_STOP s1, ComPtr<ID2D1GradientStopCollection>& out) {
        const D2D1_GRADIENT_STOP st[2] = { s0, s1 };
        return SUCCEEDED(dc->CreateGradientStopCollection(st, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &out));
    };
    {
        ComPtr<ID2D1GradientStopCollection> gs;
        if (!stops2({ 0.0f, Col(kBg2) }, { 1.0f, Col(kBg0) }, gs)) return false;
        if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, F(kCardH))), gs.Get(), &cardFill))) return false;
    }
    {
        ComPtr<ID2D1GradientStopCollection> gs;
        if (!stops2({ 0.0f, Col(0xFFFFFF, 0.16) }, { 1.0f, Col(0xFFFFFF, 0.04) }, gs)) return false;
        if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, F(kCardH))), gs.Get(), &cardEdge))) return false;
    }
    auto radial = [&](uint32_t rgb, ComPtr<ID2D1RadialGradientBrush>& out, double peak) {
        D2D1_GRADIENT_STOP st[8];
        for (int i = 0; i < 8; ++i)
        {
            const double x = static_cast<double>(i) / 7.0;
            st[i] = { F(x), Col(rgb, peak * std::pow(1.0 - x, 2.4)) };
        }
        ComPtr<ID2D1GradientStopCollection> gs;
        if (FAILED(dc->CreateGradientStopCollection(st, 8, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs))) return false;
        return SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), 1.0f, 1.0f), gs.Get(), &out));
    };
    if (!radial(kAccent, glowAccent, 1.0)) return false;
    if (!radial(kAccent, glowCard, 0.16)) return false;
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    return SUCCEEDED(factory->CreateStrokeStyle(sp, nullptr, 0, &roundCap));
}

// カードの柔らかいドロップシャドウ（窓全体サイズのビットマップへ 1 度だけ焼く。起動画面と同じ）。
bool Renderer::Impl::BuildShadow()
{
    const D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(W), static_cast<UINT32>(H));
    const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf);
    ComPtr<ID2D1Bitmap1> shape;
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, bp, &shape))) return false;
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, bp, &shadow))) return false;
    ComPtr<ID2D1Effect> blur;
    if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return false;

    auto pass = [&](double sigma, double dy, double alpha) -> bool {
        dc->SetTarget(shape.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        dc->SetTransform(D2D1::Matrix3x2F::Scale(dpi, dpi));
        solid->SetColor(Col(0x000000, alpha));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(F(kMargin), F(kMargin + dy), F(kMargin + kCardW), F(kMargin + kCardH + dy)), 16.0f, 16.0f), solid.Get());
        if (FAILED(dc->EndDraw())) return false;
        blur->SetInput(0, shape.Get());
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, F(sigma * dpi));
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        dc->SetTarget(shadow.Get());
        dc->BeginDraw();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->DrawImage(blur.Get());
        return SUCCEEDED(dc->EndDraw());
    };
    dc->SetTarget(shadow.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    if (FAILED(dc->EndDraw())) return false;
    if (!pass(22.0, 14.0, 0.62)) return false;
    if (!pass(5.0, 3.0, 0.50)) return false;
    dc->SetTarget(target.Get());
    return true;
}

bool Renderer::Impl::LoadLogo(const std::wstring& path)
{
    if (path.empty()) return false;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    if (FAILED(dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &logo))) return false;
    const D2D1_SIZE_U s = logo->GetPixelSize();
    logoW = s.width; logoH = s.height;
    const D2D1_BITMAP_BRUSH_PROPERTIES1 bbp = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    if (FAILED(dc->CreateBitmapBrush(logo.Get(), &bbp, nullptr, &logoBrush))) { logo.Reset(); return false; }
    return true;
}

IDWriteTextFormat* Renderer::Impl::Fmt(float size, DWRITE_FONT_WEIGHT wt, bool mono, bool wrap)
{
    const auto key = std::make_tuple(static_cast<int>(std::lround(size * 10.0f)), static_cast<int>(wt), mono ? 1 : 0, wrap ? 1 : 0);
    auto it = fmts.find(key);
    if (it != fmts.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dw->CreateTextFormat(mono ? monoFamily.c_str() : jpFamily.c_str(), nullptr, wt, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", &f)))
        return nullptr;
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap)
    {
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ell;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f.Get(), &ell))) f->SetTrimming(&tr, ell.Get());
    }
    IDWriteTextFormat* raw = f.Get();
    fmts.emplace(key, std::move(f));
    return raw;
}

void Renderer::Impl::Txt(const std::wstring& s, float x, float y, float w, float size, DWRITE_FONT_WEIGHT wt, const D2D1_COLOR_F& col,
                         DWRITE_TEXT_ALIGNMENT al, bool mono)
{
    if (s.empty() || col.a <= 0.002f) return;
    IDWriteTextFormat* f = Fmt(size, wt, mono, false);
    if (!f) return;
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, w, size * 2.0f, &l))) return;
    l->SetTextAlignment(al);
    solid->SetColor(col);
    dc->DrawTextLayout(D2D1::Point2F(x, y), l.Get(), solid.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Renderer::Impl::Para(const std::wstring& s, float x, float y, float w, float size, DWRITE_FONT_WEIGHT wt, const D2D1_COLOR_F& col, float maxH)
{
    if (s.empty()) return 0.0f;
    IDWriteTextFormat* f = Fmt(size, wt, false, true);
    if (!f) return 0.0f;
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, w, maxH, &l))) return 0.0f;
    solid->SetColor(col);
    dc->DrawTextLayout(D2D1::Point2F(x, y), l.Get(), solid.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.height;
}

float Renderer::Impl::TextW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT wt, bool mono)
{
    IDWriteTextFormat* f = Fmt(size, wt, mono, false);
    if (!f || s.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 4000.0f, size * 2.0f, &l))) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Renderer::Impl::Glow(ID2D1RadialGradientBrush* b, double cx, double cy, double r, double op)
{
    if (op <= 0.001 || r <= 0.5) return;
    const D2D1_MATRIX_3X2_F keep = [&] { D2D1_MATRIX_3X2_F m; dc->GetTransform(&m); return m; }();
    dc->SetTransform(D2D1::Matrix3x2F::Scale(F(r), F(r)) * D2D1::Matrix3x2F::Translation(F(cx), F(cy))
                     * D2D1::Matrix3x2F::Translation(F(kMargin), F(kMargin)) * D2D1::Matrix3x2F::Scale(dpi, dpi));
    b->SetOpacity(F(std::min(1.0, op)));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), 1.0f, 1.0f), b);
    dc->SetTransform(keep);
}

// 12 時から時計回りに a0deg から sweepDeg。sweep が 360 近ければ円。
void Renderer::Impl::Arc(double a0deg, double sweepDeg, double r, double w, const D2D1_COLOR_F& col)
{
    if (sweepDeg <= 0.01 || col.a <= 0.002f) return;
    solid->SetColor(col);
    const D2D1_POINT_2F c = D2D1::Point2F(kRingCx, kRingCy);
    if (sweepDeg >= 359.5)
    {
        dc->DrawEllipse(D2D1::Ellipse(c, F(r), F(r)), solid.Get(), F(w));
        return;
    }
    const double d2r = 3.14159265358979 / 180.0;
    const double s = (a0deg - 90.0) * d2r, e = (a0deg + sweepDeg - 90.0) * d2r;
    ComPtr<ID2D1PathGeometry> g;
    if (FAILED(factory->CreatePathGeometry(&g))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(F(kRingCx + std::cos(s) * r), F(kRingCy + std::sin(s) * r)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(F(kRingCx + std::cos(e) * r), F(kRingCy + std::sin(e) * r)), D2D1::SizeF(F(r), F(r)), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, sweepDeg > 180.0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    dc->DrawGeometry(g.Get(), solid.Get(), F(w), roundCap.Get());
}

void Renderer::Impl::Check(double cx, double cy, double s, const D2D1_COLOR_F& col, double w)
{
    solid->SetColor(col);
    const D2D1_POINT_2F a = D2D1::Point2F(F(cx - s * 0.5), F(cy + s * 0.02));
    const D2D1_POINT_2F b = D2D1::Point2F(F(cx - s * 0.12), F(cy + s * 0.38));
    const D2D1_POINT_2F c = D2D1::Point2F(F(cx + s * 0.52), F(cy - s * 0.34));
    dc->DrawLine(a, b, solid.Get(), F(w), roundCap.Get());
    dc->DrawLine(b, c, solid.Get(), F(w), roundCap.Get());
}

// ---------------------------------------------------------------- 画面

void Renderer::Impl::Header(const Model& m)
{
    // ロゴ（無ければ角丸のアクセント面）
    const float lx = kPad, ly = 28.0f, ls = 44.0f;
    const D2D1_ROUNDED_RECT lr = D2D1::RoundedRect(D2D1::RectF(lx, ly, lx + ls, ly + ls), 10.0f, 10.0f);
    if (logoBrush && logoW > 0)
    {
        logoBrush->SetTransform(D2D1::Matrix3x2F::Scale(ls / static_cast<float>(logoW), ls / static_cast<float>(logoH)) * D2D1::Matrix3x2F::Translation(lx, ly));
        dc->FillRoundedRectangle(lr, logoBrush.Get());
    }
    else
    {
        solid->SetColor(Col(kAccent, 0.22));
        dc->FillRoundedRectangle(lr, solid.Get());
        Txt(L"U", lx, ly + 7.0f, ls, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kAccentHi), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    const float tx = lx + ls + 14.0f;
    Txt(L"Uno Engine アップデート", tx, 27.0f, 300.0f, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kTextMid));
    // 版: 今の版 → 新しい版
    float x = tx;
    const float vy = 51.0f;
    if (!m.curVer.empty())
    {
        Txt(m.curVer, x, vy, 120.0f, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextDim), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        x += TextW(m.curVer, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, true) + 10.0f;
        // 矢印
        solid->SetColor(Col(kTextFaint));
        dc->DrawLine(D2D1::Point2F(x, vy + 10.0f), D2D1::Point2F(x + 16.0f, vy + 10.0f), solid.Get(), 1.4f, roundCap.Get());
        dc->DrawLine(D2D1::Point2F(x + 11.0f, vy + 5.5f), D2D1::Point2F(x + 16.0f, vy + 10.0f), solid.Get(), 1.4f, roundCap.Get());
        dc->DrawLine(D2D1::Point2F(x + 11.0f, vy + 14.5f), D2D1::Point2F(x + 16.0f, vy + 10.0f), solid.Get(), 1.4f, roundCap.Get());
        x += 26.0f;
    }
    if (!m.newVer.empty())
    {
        const float w = TextW(m.newVer, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true) + 16.0f;
        solid->SetColor(Col(kAccent, 0.16));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 3.0f, vy - 2.0f, x - 3.0f + w, vy + 22.0f), 11.0f, 11.0f), solid.Get());
        solid->SetColor(Col(kAccent, 0.45));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 2.5f, vy - 1.5f, x - 3.5f + w, vy + 21.5f), 10.5f, 10.5f), solid.Get(), 1.0f);
        Txt(m.newVer, x + 5.0f, vy, w, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kAccentHi), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }
    // 区切り（中央が明るく両端へ消える光の線）
    {
        const float y = 94.0f;
        const D2D1_GRADIENT_STOP st[3] = { { 0.0f, Col(kAccent, 0.0) }, { 0.5f, Col(kAccent, 0.55) }, { 1.0f, Col(kAccent, 0.0) } };
        ComPtr<ID2D1GradientStopCollection> gs;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gs)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(kPad, y), D2D1::Point2F(kCardW - kPad, y)), gs.Get(), &br)))
            dc->FillRectangle(D2D1::RectF(kPad, y, kCardW - kPad, y + 1.0f), br.Get());
    }
}

void Renderer::Impl::DrawPrompt(const Model& m)
{
    const float cw = kCardW - kPad * 2.0f;
    Txt(L"新しいバージョンがあります", kPad, 108.0f, cw, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kText));
    float py = 150.0f;
    if (m.retrying)
    {
        Txt(L"前回この更新を適用しましたが、反映されていませんでした（ファイルが使用中だった可能性があります）。",
            kPad, 144.0f, cw, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kWarn));
        py = 174.0f;
    }
    // 本文パネル
    const float ph = kBtnY - 18.0f - py;
    const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(D2D1::RectF(kPad, py, kPad + cw, py + ph), 10.0f, 10.0f);
    solid->SetColor(Col(kBg1, 0.85));
    dc->FillRoundedRectangle(panel, solid.Get());
    solid->SetColor(Col(kBorder, 1.0));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kPad + 0.5f, py + 0.5f, kPad + cw - 0.5f, py + ph - 0.5f), 9.5f, 9.5f), solid.Get(), 1.0f);

    const float ix = kPad + 18.0f, iw = cw - 36.0f;
    if (m.body.empty())
    {
        Para(m.bodyMissing ? L"更新内容を取得できませんでした。更新後に、エディタの「ヘルプ > 更新内容を表示」でも確認できます。" : L"更新内容はありません。",
             ix, py + 18.0f, iw, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextDim));
        return;
    }
    const float rowH = 22.0f;
    float y = py + 12.0f;
    for (const BodyLineW& l : m.body)
    {
        switch (l.type)
        {
        case ul::BodyLine::Type::Heading:
            Txt(l.text, ix, y + 2.0f, iw, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kAccentHi));
            break;
        case ul::BodyLine::Type::Bullet:
            solid->SetColor(Col(kAccentHi, 0.9));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 4.0f, y + 11.5f), 2.4f, 2.4f), solid.Get());
            {
                // 「題 — 説明」は題を明るく・説明を淡く（GitHub 本文の箇条書きの形）
                const size_t dash = l.text.find(L" — ");
                const float bx = ix + 16.0f, bw = iw - 16.0f;
                if (dash != std::wstring::npos)
                {
                    const std::wstring lead = l.text.substr(0, dash), rest = l.text.substr(dash);
                    const float lw = TextW(lead, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                    if (lw < bw - 70.0f)
                    {
                        Txt(lead, bx, y + 2.0f, lw + 4.0f, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kText));
                        Txt(rest, bx + lw, y + 2.0f, bw - lw, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextDim));
                        break;
                    }
                }
                Txt(l.text, bx, y + 2.0f, bw, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextMid));
            }
            break;
        default:
            Txt(l.text, ix, y + 2.0f, iw, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kText));
            break;
        }
        y += rowH;
    }
    if (m.more > 0)
    {
        wchar_t buf[48];
        swprintf(buf, 48, L"ほか %d 件", m.more);
        Txt(buf, ix + 16.0f, y + 2.0f, iw - 16.0f, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextDim));
    }
}

void Renderer::Impl::DrawProgress(const Model& m)
{
    const double t = m.time;
    const bool indeterminate = (m.mode != Mode::Downloading) || m.fraction < 0.0;
    const double frac = std::min(1.0, std::max(0.0, m.mode == Mode::Applying ? 1.0 : m.fraction));

    // ---- 左: リング
    Glow(glowAccent.Get(), kRingCx, kRingCy, kRingR * 1.5, 0.30 + 0.06 * std::sin(t * 2.4));
    Arc(0.0, 360.0, kRingR, 6.0, Col(0xFFFFFF, 0.075));
    double a0 = 0.0, sweep = 0.0;
    if (m.mode == Mode::Applying) { a0 = 0.0; sweep = 360.0; }
    else if (indeterminate) { a0 = std::fmod(t * 220.0, 360.0); sweep = 110.0; }
    else { a0 = 0.0; sweep = 360.0 * frac; }
    if (sweep > 0.3)
    {
        Arc(a0, sweep, kRingR, 14.0, Col(kAccent, 0.05));
        Arc(a0, sweep, kRingR, 9.0, Col(kAccent, 0.11));
        Arc(a0, sweep, kRingR, 6.0, Mix(kAccent, kAccentHi, 0.5));
        if (!indeterminate && frac < 0.999)
        {
            const double ang = (360.0 * frac - 90.0) * 3.14159265358979 / 180.0;
            const double hx = kRingCx + std::cos(ang) * kRingR, hy = kRingCy + std::sin(ang) * kRingR;
            Glow(glowAccent.Get(), hx, hy, 14.0, 0.8);
            solid->SetColor(Col(0xEAF3FF, 0.95));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(F(hx), F(hy)), 2.8f, 2.8f), solid.Get());
        }
    }
    // 中央
    if (m.mode == Mode::Downloading && m.fraction >= 0.0)
    {
        wchar_t pct[16];
        swprintf(pct, 16, L"%d%%", static_cast<int>(std::floor(frac * 100.0)));
        Txt(pct, kRingCx - 70.0f, kRingCy - 26.0f, 140.0f, 38.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    else if (m.mode == Mode::Applying)
        Check(kRingCx, kRingCy, 46.0, Col(kAccentHi), 5.0);
    else
        Txt(L"展開中", kRingCx - 70.0f, kRingCy - 12.0f, 140.0f, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kTextMid), DWRITE_TEXT_ALIGNMENT_CENTER);

    // ---- 右: 段階の説明
    const float rw = kCardW - kPad - kRightX;
    const wchar_t* title = L"ダウンロード中";
    const wchar_t* desc = L"新しいバージョンのファイルを取得しています。";
    if (m.mode == Mode::Extracting) { title = L"展開中"; desc = L"ファイルを展開しています。しばらくお待ちください。"; }
    if (m.mode == Mode::Applying)   { title = L"適用して再起動します"; desc = L"まもなくエディタが閉じ、更新後に自動で起動します。"; }
    Txt(title, kRightX, 130.0f, rw, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kText));
    Para(desc, kRightX, 166.0f, rw, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextMid), 44.0f);
    if (m.mode == Mode::Downloading)
        Txt(m.detail, kRightX, 214.0f, rw, 13.0f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextDim), DWRITE_TEXT_ALIGNMENT_LEADING, true);

    // 細いバー
    {
        const float by = 248.0f, bh = 8.0f;
        solid->SetColor(Col(0xFFFFFF, 0.08));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kRightX, by, kRightX + rw, by + bh), 4.0f, 4.0f), solid.Get());
        float fx0 = kRightX, fx1 = kRightX + rw;
        if (m.mode == Mode::Applying) {}
        else if (indeterminate)
        {
            const double seg = rw * 0.32;
            const double u = std::fmod(t * 0.75, 1.0);
            const double c0 = -seg + (rw + seg) * u;
            fx0 = kRightX + static_cast<float>(std::max(0.0, c0));
            fx1 = kRightX + static_cast<float>(std::min<double>(rw, c0 + seg));
        }
        else fx1 = kRightX + static_cast<float>(rw * frac);
        if (fx1 - fx0 > 0.5f)
        {
            solid->SetColor(Col(kAccent, 0.18));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fx0 - 2.0f, by - 3.0f, fx1 + 2.0f, by + bh + 3.0f), 6.0f, 6.0f), solid.Get());
            solid->SetColor(Mix(kAccent, kAccentHi, 0.4));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fx0, by, fx1, by + bh), 4.0f, 4.0f), solid.Get());
        }
    }

    // 段階の一覧
    {
        static const wchar_t* kSteps[3] = { L"ダウンロード", L"展開", L"適用して再起動" };
        const int cur = m.mode == Mode::Downloading ? 0 : (m.mode == Mode::Extracting ? 1 : 2);
        for (int i = 0; i < 3; ++i)
        {
            const float y = 288.0f + static_cast<float>(i) * 32.0f;
            const float cx = kRightX + 9.0f, cy = y + 11.0f;
            const bool done = i < cur || (i == 2 && m.mode == Mode::Applying);
            const bool now = i == cur && !done;
            if (done)
            {
                solid->SetColor(Col(kAccent, 0.9));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.0f, 9.0f), solid.Get());
                Check(cx, cy, 9.0, Col(kOnAccent), 2.0);
            }
            else if (now)
            {
                solid->SetColor(Col(kAccent, 0.9));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.5f, 8.5f), solid.Get(), 1.6f);
                solid->SetColor(Col(kAccentHi, 0.55 + 0.35 * std::sin(t * 4.0)));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.6f, 3.6f), solid.Get());
            }
            else
            {
                solid->SetColor(Col(kBorderSt, 1.0));
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.5f, 8.5f), solid.Get(), 1.4f);
            }
            Txt(kSteps[i], kRightX + 28.0f, y + 1.0f, rw - 28.0f, 14.0f, now ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                Col(now ? kText : (done ? kTextMid : kTextFaint)));
        }
    }
}

void Renderer::Impl::DrawError(const Model& m)
{
    const float cw = kCardW - kPad * 2.0f;
    // 警告アイコン + 題
    solid->SetColor(Col(kWarn, 0.16));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPad + 17.0f, 126.0f), 17.0f, 17.0f), solid.Get());
    solid->SetColor(Col(kWarn, 0.55));
    dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kPad + 17.0f, 126.0f), 16.5f, 16.5f), solid.Get(), 1.2f);
    Txt(L"!", kPad, 112.0f, 34.0f, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kWarn), DWRITE_TEXT_ALIGNMENT_CENTER);
    Txt(m.errTitle.empty() ? std::wstring(L"更新できませんでした") : m.errTitle, kPad + 50.0f, 110.0f, cw - 50.0f, 24.0f,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kText));

    // 理由のパネル
    const float py = 160.0f, ph = 104.0f;
    solid->SetColor(Col(kBg1, 0.85));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kPad, py, kPad + cw, py + ph), 10.0f, 10.0f), solid.Get());
    solid->SetColor(Col(kBorder, 1.0));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kPad + 0.5f, py + 0.5f, kPad + cw - 0.5f, py + ph - 0.5f), 9.5f, 9.5f), solid.Get(), 1.0f);
    Para(m.errDetail, kPad + 18.0f, py + 16.0f, cw - 36.0f, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextMid), ph - 28.0f);

    std::wstring hint = L"「このまま起動」を選ぶと、今のバージョン";
    if (!m.curVer.empty()) hint += L"（" + m.curVer + L"）";
    hint += L"で起動します。更新は次回の起動時にもう一度案内されます。";
    Para(hint, kPad, py + ph + 14.0f, cw, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, Col(kTextDim), 40.0f);
}

void Renderer::Impl::DrawButtons(const Model& m, int hover, int pressed)
{
    const std::vector<Button> bs = LayoutButtons(m);
    for (size_t i = 0; i < bs.size(); ++i)
    {
        const Button& b = bs[i];
        const float x = b.x - kMargin, y = b.y - kMargin;
        const bool hv = static_cast<int>(i) == hover;
        const bool pr = static_cast<int>(i) == pressed;
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + b.w, y + b.h), 8.0f, 8.0f);
        if (b.style == Button::Style::Primary)
        {
            // 既定のボタン: ネオンのにじみ
            solid->SetColor(Col(kAccent, hv ? 0.26 : 0.16));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 3.0f, y - 3.0f, x + b.w + 3.0f, y + b.h + 3.0f), 11.0f, 11.0f), solid.Get());
            solid->SetColor(Col(pr ? kAccentPr : (hv ? kAccentHi : kAccent)));
            dc->FillRoundedRectangle(rr, solid.Get());
            Txt(b.label, x, y + (b.h - 15.0f) * 0.5f - 2.0f, b.w, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Col(kOnAccent), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        else if (b.style == Button::Style::Secondary)
        {
            solid->SetColor(Col(pr ? kBg2 : (hv ? kBg4 : kBg3)));
            dc->FillRoundedRectangle(rr, solid.Get());
            solid->SetColor(Col(hv ? kAccentHi : kBorderSt, hv ? 0.8 : 1.0));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 0.5f, x + b.w - 0.5f, y + b.h - 0.5f), 7.5f, 7.5f), solid.Get(), 1.0f);
            Txt(b.label, x, y + (b.h - 15.0f) * 0.5f - 2.0f, b.w, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, Col(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        else
        {
            const D2D1_COLOR_F c = Col(hv ? kText : kTextDim);
            Txt(b.label, x, y + (b.h - 14.0f) * 0.5f - 2.0f, b.w, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, c, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (hv)
            {
                const float tw = TextW(b.label, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
                solid->SetColor(c);
                dc->DrawLine(D2D1::Point2F(x, y + b.h * 0.5f + 10.0f), D2D1::Point2F(x + tw, y + b.h * 0.5f + 10.0f), solid.Get(), 1.0f);
            }
        }
    }
}

void Renderer::Impl::Draw(const Model& m, int hover, int pressed)
{
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(dpi, dpi));
    dc->DrawBitmap(shadow.Get(), D2D1::RectF(0, 0, F(kWindowW), F(kWindowH)), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);

    Base();
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, F(kCardW), F(kCardH)), 16.0f, 16.0f), cardFill.Get());
    Glow(glowCard.Get(), 120.0, 70.0, 300.0, 1.0);
    Base();
    solid->SetColor(Col(kBorder, 0.9));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, F(kCardW - 0.5), F(kCardH - 0.5)), 15.5f, 15.5f), solid.Get(), 1.0f);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, F(kCardW - 1.5), F(kCardH - 1.5)), 14.5f, 14.5f), cardEdge.Get(), 1.0f);
    dc->PushAxisAlignedClip(D2D1::RectF(1.0f, 1.0f, F(kCardW - 1), F(kCardH - 1)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    Header(m);
    if (m.mode == Mode::Prompt) DrawPrompt(m);
    else if (m.mode == Mode::Error) DrawError(m);
    else DrawProgress(m);
    DrawButtons(m, hover, pressed);

    dc->PopAxisAlignedClip();
    if (FAILED(dc->EndDraw())) err = "EndDraw";
}

Renderer::Renderer() : p_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::Init(float dpiScale, const std::wstring& logoPath) { return p_->Init(dpiScale, logoPath); }
int Renderer::Width() const { return p_->W; }
int Renderer::Height() const { return p_->H; }
const std::string& Renderer::LastError() const { return p_->err; }
bool Renderer::Draw(const Model& m, int hover, int pressed)
{
    p_->err.clear();
    p_->Draw(m, hover, pressed);
    return p_->err.empty();
}
bool Renderer::CopyPixels(uint8_t* dst, int dstPitch)
{
    Impl& s = *p_;
    if (FAILED(s.readback->CopyFromBitmap(nullptr, s.target.Get(), nullptr))) return false;
    D2D1_MAPPED_RECT mr{};
    if (FAILED(s.readback->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;
    const int rowBytes = s.W * 4;
    for (int y = 0; y < s.H; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(dstPitch), mr.bits + static_cast<size_t>(y) * mr.pitch, static_cast<size_t>(rowBytes));
    s.readback->Unmap();
    return true;
}

// ---------------------------------------------------------------- 実窓

struct UpdateWindow::Impl
{
    HWND hwnd = nullptr;
    Renderer renderer;
    Model m;
    float scale = 1.0f;
    int winW = 0, winH = 0;
    HDC mem = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; void* bits = nullptr;
    std::vector<Button> buttons;
    int hover = -1, pressed = -1;
    Choice choice = Choice::None;
    bool dirty = true, tracking = false;
    double t0 = 0.0, lastDraw = 0.0, shown = 0.0, targetFrac = -1.0, lastT = 0.0;
    bool visible = false;

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l);
    LRESULT Handle(HWND h, UINT msg, WPARAM w, LPARAM l);
    int HitButton(int px, int py) const;
    void Redraw();
    void Poll();
};

int UpdateWindow::Impl::HitButton(int px, int py) const
{
    const float x = static_cast<float>(px) / scale, y = static_cast<float>(py) / scale;
    for (size_t i = 0; i < buttons.size(); ++i)
    {
        const Button& b = buttons[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return static_cast<int>(i);
    }
    return -1;
}

LRESULT CALLBACK UpdateWindow::Impl::Proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, w, l);
    return self->Handle(h, msg, w, l);
}

LRESULT UpdateWindow::Impl::Handle(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    auto choose = [&](Choice c) { if (choice == Choice::None) choice = c; };
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST:
    {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &pt);
        if (HitButton(pt.x, pt.y) >= 0) return HTCLIENT;
        const float x = static_cast<float>(pt.x) / scale, y = static_cast<float>(pt.y) / scale;
        if (x >= kMargin && x < kMargin + kCardW && y >= kMargin && y < kMargin + kCardH) return HTCAPTION;   // カードをつかんで動かせる
        return HTCLIENT;
    }
    case WM_SETCURSOR:
    {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        SetCursor(LoadCursorW(nullptr, HitButton(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_MOUSEMOVE:
    {
        const int hv = HitButton(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hv != hover) { hover = hv; dirty = true; }
        if (!tracking)
        {
            TRACKMOUSEEVENT te{ sizeof(te), TME_LEAVE, h, 0 };
            TrackMouseEvent(&te);
            tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        if (hover != -1 || pressed != -1) { hover = -1; pressed = -1; dirty = true; }
        return 0;
    case WM_LBUTTONDOWN:
    {
        const int hv = HitButton(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hv >= 0) { pressed = hv; dirty = true; SetCapture(h); }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        const int hv = HitButton(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        const int was = pressed;
        pressed = -1; dirty = true;
        if (GetCapture() == h) ReleaseCapture();
        if (was >= 0 && was == hv && was < static_cast<int>(buttons.size())) choose(buttons[static_cast<size_t>(was)].id);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_RETURN || w == VK_SPACE)
        {
            for (const Button& b : buttons) if (b.style == Button::Style::Primary) { choose(b.id); break; }
        }
        else if (w == VK_ESCAPE)
        {
            for (const Button& b : buttons)
                if (b.id == Choice::Later || b.id == Choice::StartAnyway) { choose(b.id); break; }
        }
        return 0;
    case WM_CLOSE:
        // ダウンロード中などボタンが無い間は閉じさせない（半端な zip で更新が壊れないように）。
        for (const Button& b : buttons)
            if (b.id == Choice::Later || b.id == Choice::StartAnyway) { choose(b.id); break; }
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, w, l);
}

void UpdateWindow::Impl::Redraw()
{
    const double now = Now();
    m.time = now - t0;
    // 進捗はなめらかに追従（実測の更新が粗くても弧が飛ばない）
    if (targetFrac < 0.0) shown = -1.0;
    else
    {
        if (shown < 0.0) shown = targetFrac;
        const double dt = std::min(0.2, std::max(0.0, now - lastT));
        shown += (targetFrac - shown) * (1.0 - std::exp(-dt * 12.0));
    }
    lastT = now;
    m.fraction = shown;
    buttons = LayoutButtons(m);
    if (!renderer.Draw(m, hover, pressed) || !renderer.CopyPixels(static_cast<uint8_t*>(bits), winW * 4)) return;
    const double fade = std::min(1.0, (now - t0) / 0.16);
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = static_cast<BYTE>(std::lround(fade * 255.0));
    bf.AlphaFormat = AC_SRC_ALPHA;
    POINT src{ 0, 0 };
    SIZE size{ winW, winH };
    HDC scr = GetDC(nullptr);
    UpdateLayeredWindow(hwnd, scr, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, scr);
    if (!visible)
    {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        visible = true;
    }
    lastDraw = now;
    dirty = false;
}

void UpdateWindow::Impl::Poll()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const double now = Now();
    const bool animating = (m.mode != Mode::Prompt && m.mode != Mode::Error) || (now - t0) < 0.2;
    if (dirty || (animating && now - lastDraw >= 1.0 / 30.0)) Redraw();
}

UpdateWindow::UpdateWindow() : p_(std::make_unique<Impl>()) {}
UpdateWindow::~UpdateWindow() { Destroy(); }

bool UpdateWindow::Create(const std::wstring& logoPath)
{
    Impl& s = *p_;
    s.scale = splash::EffectiveDpiScale();
    if (!s.renderer.Init(s.scale, logoPath)) return false;
    s.winW = s.renderer.Width();
    s.winH = s.renderer.Height();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = s.winW;
    bi.bmiHeader.biHeight = -s.winH;                 // 上が先頭
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    s.mem = CreateCompatibleDC(scr);
    s.bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
    ReleaseDC(nullptr, scr);
    if (!s.mem || !s.bmp || !s.bits) { Destroy(); return false; }
    s.old = SelectObject(s.mem, s.bmp);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"UnoUpdateWnd";
    RegisterClassExW(&wc);                           // 二重登録は失敗するだけで無害

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    // 窓は最前面（起動画面の手前に出す）。タスクバーにも出す（見失ったときに戻れるように）。
    s.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW, wc.lpszClassName, L"Uno Engine アップデート", WS_POPUP,
                             (sw - s.winW) / 2, (sh - s.winH) / 2, s.winW, s.winH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd) { Destroy(); return false; }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.t0 = s.lastDraw = s.lastT = Now();
    s.m.mode = Mode::Prompt;
    s.dirty = true;
    return true;
}

Choice UpdateWindow::Prompt(const std::string& curVer, const std::string& newVer, const std::string& bodyMarkdown, bool bodyFetched, bool retrying)
{
    Impl& s = *p_;
    s.m = Model{};
    s.m.mode = Mode::Prompt;
    s.m.curVer = L"v" + splash::Utf8ToWide(curVer);
    s.m.newVer = L"v" + splash::Utf8ToWide(newVer);
    s.m.retrying = retrying;
    SetBodyFromMarkdown(s.m, bodyFetched ? bodyMarkdown : std::string());
    s.choice = Choice::None;
    s.targetFrac = -1.0;
    s.dirty = true;
    while (s.choice == Choice::None)
    {
        s.Poll();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    const Choice c = s.choice;
    s.choice = Choice::None;
    s.hover = s.pressed = -1;
    return c;
}

void UpdateWindow::SetDownloading(uint64_t done, uint64_t total, double bytesPerSec, double etaSec)
{
    Impl& s = *p_;
    s.m.mode = Mode::Downloading;
    s.targetFrac = total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : -1.0;
    s.m.detail = splash::Utf8ToWide(ul::FormatDownloadDetail(done, total, bytesPerSec, etaSec));
    s.dirty = true;
    s.Poll();
}

void UpdateWindow::SetExtracting()
{
    Impl& s = *p_;
    s.m.mode = Mode::Extracting;
    s.targetFrac = -1.0;
    s.m.detail.clear();
    s.dirty = true;
    s.Poll();
}

void UpdateWindow::SetApplying()
{
    Impl& s = *p_;
    s.m.mode = Mode::Applying;
    s.targetFrac = 1.0;
    s.m.detail.clear();
    s.dirty = true;
    s.Poll();
}

void UpdateWindow::Pump() { p_->Poll(); }

Choice UpdateWindow::ShowError(const std::string& title, const std::string& detail)
{
    Impl& s = *p_;
    s.m.mode = Mode::Error;
    s.m.errTitle = splash::Utf8ToWide(title);
    s.m.errDetail = splash::Utf8ToWide(detail);
    s.choice = Choice::None;
    s.hover = s.pressed = -1;
    s.dirty = true;
    while (s.choice == Choice::None)
    {
        s.Poll();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    const Choice c = s.choice;
    s.choice = Choice::None;
    s.hover = s.pressed = -1;
    return c;
}

void UpdateWindow::Destroy()
{
    Impl& s = *p_;
    if (s.hwnd) { SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0); DestroyWindow(s.hwnd); s.hwnd = nullptr; }
    if (s.mem && s.old) { SelectObject(s.mem, s.old); s.old = nullptr; }
    if (s.bmp) { DeleteObject(s.bmp); s.bmp = nullptr; }
    if (s.mem) { DeleteDC(s.mem); s.mem = nullptr; }
    s.bits = nullptr;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

// ---------------------------------------------------------------- プレビュー（窓なし）

namespace
{
std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
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

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Shot { const char* name; Mode mode; double frac; bool retry; bool noBody; bool errDownload; };
} // namespace

bool RunUpdateUiPreviewIfRequested(int argc, wchar_t** argv, int& exitCode)
{
    std::wstring state;
    std::filesystem::path out, bodyFile;
    float dpiScale = 1.0f;
    std::string cur = "2.0.0", nw = "2.1.0";
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& o) { if (i + 1 < argc) { o = argv[++i]; return true; } return false; };
        std::wstring v;
        if (a == L"--preview-update-ui") { if (next(v)) state = v; }
        else if (a == L"--out") { if (next(v)) out = v; }
        else if (a == L"--update-body") { if (next(v)) bodyFile = v; }
        else if (a == L"--update-cur") { if (next(v)) cur = Narrow(v); }
        else if (a == L"--update-new") { if (next(v)) nw = Narrow(v); }
        else if (a == L"--dpi-scale") { if (next(v)) { float s = 1.0f; if (dpi::ParseScale(Narrow(v), s)) dpiScale = s; } }
        else if (a.rfind(L"--dpi-scale=", 0) == 0) { float s = 1.0f; if (dpi::ParseScale(Narrow(a.substr(12)), s)) dpiScale = s; }
    }
    if (state.empty()) return false;
    exitCode = 1;
    if (out.empty()) { std::fprintf(stderr, "preview-update-ui: --out が要ります\n"); return true; }

    // ロゴ（開発ツリー / 配布のどちらでも探す）
    std::wstring logo;
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::filesystem::path base = std::filesystem::path(exe).parent_path();
        std::error_code ec;
        for (int up = 0; up < 5 && logo.empty(); ++up)
        {
            const auto cand = base / L"assets" / L"editor" / L"icons" / L"logo.png";
            if (std::filesystem::exists(cand, ec)) logo = cand.wstring();
            base = base.parent_path();
        }
    }
    Renderer r;
    if (!r.Init(dpiScale, logo)) { std::fprintf(stderr, "preview-update-ui: 描画器の初期化に失敗: %s\n", r.LastError().c_str()); return true; }

    // 本文: 指定が無ければ「今の版の GitHub 本文」（= 次の更新で実際に案内される形）の見本
    std::string md;
    if (!bodyFile.empty()) md = ReadFile(bodyFile);
    else if (const relnotes::Release* rel = relnotes::Find(relnotes::All(), kEngineVersion)) md = relnotes::ToMarkdown(*rel, kEngineName);

    const Shot all[] = {
        { "prompt",            Mode::Prompt,      0.0,  false, false, false },
        { "prompt_retry",      Mode::Prompt,      0.0,  true,  false, false },
        { "prompt_nobody",     Mode::Prompt,      0.0,  false, true,  false },
        { "downloading",       Mode::Downloading, 0.27, false, false, false },
        { "downloading_start", Mode::Downloading, -1.0, false, false, false },
        { "extracting",        Mode::Extracting,  -1.0, false, false, false },
        { "applying",          Mode::Applying,    1.0,  false, false, false },
        { "error",             Mode::Error,       0.0,  false, false, true  },
        { "error_extract",     Mode::Error,       0.0,  false, false, false },
    };
    const bool every = (state == L"all");
    std::vector<const Shot*> todo;
    for (const Shot& s : all)
    {
        const std::wstring nm = splash::Utf8ToWide(s.name);
        if (every || nm == state) todo.push_back(&s);
    }
    if (todo.empty()) { std::fprintf(stderr, "preview-update-ui: 状態は prompt|downloading|extracting|applying|error|all\n"); return true; }
    std::error_code ec;
    if (every) std::filesystem::create_directories(out, ec);
    else if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);

    std::vector<uint8_t> px(static_cast<size_t>(r.Width()) * static_cast<size_t>(r.Height()) * 4), outPx(px.size());
    for (const Shot* s : todo)
    {
        Model m;
        m.mode = s->mode;
        m.curVer = L"v" + splash::Utf8ToWide(cur);
        m.newVer = L"v" + splash::Utf8ToWide(nw);
        m.retrying = s->retry;
        m.time = 1.0;
        m.fraction = s->frac;
        if (s->mode == Mode::Prompt) SetBodyFromMarkdown(m, s->noBody ? std::string() : md);
        if (s->mode == Mode::Downloading && s->frac >= 0.0)
        {
            const uint64_t total = static_cast<uint64_t>(45.6 * 1048576), done = static_cast<uint64_t>(total * s->frac);
            m.detail = splash::Utf8ToWide(ul::FormatDownloadDetail(done, total, 3.2 * 1048576, 10.4));
        }
        else if (s->mode == Mode::Downloading)
            m.detail = splash::Utf8ToWide(ul::FormatDownloadDetail(0, 0, 0.0, -1.0));
        if (s->mode == Mode::Error)
        {
            m.errTitle = L"更新できませんでした";
            m.errDetail = s->errDownload
                ? L"ダウンロードに失敗しました。ネットワークの接続を確認して、もう一度お試しください。\nプロキシやセキュリティソフトが GitHub への接続を止めている場合もあります。"
                : L"ダウンロードしたファイルの展開に失敗しました。ディスクの空き容量を確認して、もう一度お試しください。";
        }
        if (!r.Draw(m, -1, -1) || !r.CopyPixels(px.data(), r.Width() * 4)) { std::fprintf(stderr, "preview-update-ui: 描画に失敗 (%s)\n", s->name); return true; }
        // 背景（デスクトップ想定の青みのあるグラデ）へ合成
        const int w = r.Width(), h = r.Height();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
                const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
                const double br = 0.30 + 0.16 * u, bg = 0.40 + 0.14 * (1.0 - v) * 0.6 + 0.06 * u, bb = 0.58 + 0.10 * (1.0 - v);
                const double a = px[i + 3] / 255.0;
                const double rr = px[i + 2] / 255.0 + br * (1.0 - a), gg = px[i + 1] / 255.0 + bg * (1.0 - a), b2 = px[i + 0] / 255.0 + bb * (1.0 - a);
                outPx[i + 0] = static_cast<uint8_t>(std::lround(std::min(1.0, b2) * 255.0));
                outPx[i + 1] = static_cast<uint8_t>(std::lround(std::min(1.0, gg) * 255.0));
                outPx[i + 2] = static_cast<uint8_t>(std::lround(std::min(1.0, rr) * 255.0));
                outPx[i + 3] = 255;
            }
        const std::filesystem::path file = every ? out / (std::string(s->name) + ".png") : out;
        if (!WritePng(file, outPx.data(), w, h)) { std::fprintf(stderr, "preview-update-ui: PNG を書けません: %ls\n", file.c_str()); return true; }
        std::printf("preview-update-ui: %s -> %ls (%dx%d)\n", s->name, file.c_str(), w, h);
    }
    exitCode = 0;
    return true;
}

} // namespace dx12e::updateui
