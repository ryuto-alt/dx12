#include "core/SplashRenderer.h"

#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dxguid.lib")

namespace dx12e::splash
{
using Microsoft::WRL::ComPtr;

namespace
{
// ---- 配色（src/editor/EditorTheme.h のトークン）
constexpr uint32_t kBg0      = 0x0E0E10;
constexpr uint32_t kBg1      = 0x171719;
constexpr uint32_t kBg2      = 0x1F1F23;
constexpr uint32_t kBorder   = 0x2A2A2F;
constexpr uint32_t kBorderSt = 0x3A3A42;
constexpr uint32_t kAccent   = 0x2F8CFF;
constexpr uint32_t kAccentHi = 0x57A3FF;
constexpr uint32_t kText     = 0xD6D6DB;
constexpr uint32_t kTextMid  = 0xBEBEC7;
constexpr uint32_t kTextDim  = 0x9C9CA6;
constexpr uint32_t kTextFaint= 0x90909A;
constexpr uint32_t kAmber    = 0xE7B55A;

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

// D2D1_MATRIX_3X2_F 同士の積（行ベクトル規約: まず a、次に b を適用）。d2d1helper の演算子は Matrix3x2F 側にしか無い。
D2D1_MATRIX_3X2_F Mul(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b)
{
    D2D1_MATRIX_3X2_F r;
    r.m11 = a.m11 * b.m11 + a.m12 * b.m21;
    r.m12 = a.m11 * b.m12 + a.m12 * b.m22;
    r.m21 = a.m21 * b.m11 + a.m22 * b.m21;
    r.m22 = a.m21 * b.m12 + a.m22 * b.m22;
    r.dx  = a.dx * b.m11 + a.dy * b.m21 + b.dx;
    r.dy  = a.dx * b.m12 + a.dy * b.m22 + b.dy;
    return r;
}

// ---- ロゴ・カード内の配置（カードのローカル論理座標）
constexpr double kRingCx = 172.0, kRingCy = 190.0, kRingR = 80.0;
constexpr double kLogoSize = 100.0;
constexpr double kRightX = 352.0, kRightW = 332.0;

enum FontId { kFWord, kFVersion, kFStepNo, kFStepLabel, kFPercent, kFPercentSign, kFTip, kFChip, kFHead, kFRecent, kFProject, kFScene, kFontCount };

struct FontDef { bool latin; float size; DWRITE_FONT_WEIGHT weight; };
constexpr FontDef kFonts[kFontCount] = {
    /* kFWord        */ { true,  50.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },
    /* kFVersion     */ { false, 13.0f, DWRITE_FONT_WEIGHT_NORMAL },
    /* kFStepNo      */ { true,  11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },
    /* kFStepLabel   */ { false, 15.0f, DWRITE_FONT_WEIGHT_NORMAL },
    /* kFPercent     */ { true,  26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },
    /* kFPercentSign */ { true,  13.0f, DWRITE_FONT_WEIGHT_NORMAL },
    /* kFTip         */ { false, 13.0f, DWRITE_FONT_WEIGHT_NORMAL },
    /* kFChip        */ { true,  10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },
    /* kFHead        */ { false, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },
    /* kFRecent      */ { false, 13.0f, DWRITE_FONT_WEIGHT_NORMAL },
    /* kFProject     */ { false, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD },
    /* kFScene       */ { false, 13.0f, DWRITE_FONT_WEIGHT_NORMAL },
};

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
} // namespace

std::wstring PickFontFamily(const std::vector<std::wstring>& candidates)
{
    if (candidates.empty()) return L"Segoe UI";
    ComPtr<IDWriteFactory> dw;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()))))
        return candidates.back();
    return PickIn(dw.Get(), candidates);
}

struct SplashRenderer::Impl
{
    float dpi = 1.0f;
    int   W = 0, H = 0;                          // 物理 px
    std::string err;
    bool  warp = false;
    bool  comInit = false;

    ComPtr<ID3D11Device>          d3d;
    ComPtr<ID2D1Factory1>         factory;
    ComPtr<ID2D1Device>           d2dDev;
    ComPtr<ID2D1DeviceContext>    dc;
    ComPtr<ID2D1Bitmap1>          target, readback, shadow, logo;
    ComPtr<IDWriteFactory>        dw;
    ComPtr<ID2D1SolidColorBrush>  solid;
    ComPtr<ID2D1LinearGradientBrush> cardFill, cardEdge;
    ComPtr<ID2D1RadialGradientBrush> glowAccent, glowWhite, glowAmber, glowCard;
    ComPtr<ID2D1BitmapBrush1>     logoBrush;
    ComPtr<ID2D1StrokeStyle>      roundCap;
    ComPtr<ID2D1PathGeometry>     star;
    ComPtr<IDWriteTextFormat>     fmt[kFontCount];
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> layouts;
    std::wstring wordTitle;                      // ワードマークの字の位置（title が変わるまで使い回す）
    std::vector<float> wordX;
    float wordW = 0.0f;
    UINT  logoW = 0, logoH = 0;

    D2D1_MATRIX_3X2_F cardBase = D2D1::Matrix3x2F::Identity();

    ~Impl() { layouts.clear(); }

    bool Init(float dpiScale, const std::wstring& logoPath);
    bool BuildShadow();
    bool LoadLogo(const std::wstring& path);
    bool BuildBrushes();
    bool BuildFonts();
    bool Draw(const SplashFrame& f, const SplashContent& c);

    // ---- 描画部品（座標はカードのローカル論理 px）
    void Xf(const D2D1_MATRIX_3X2_F& local) { dc->SetTransform(Mul(local, cardBase)); }
    void XfCard() { dc->SetTransform(cardBase); }
    void Glow(ID2D1RadialGradientBrush* b, double cx, double cy, double r, double op);
    void Arc(double a0deg, double sweepDeg, double r, double w, const D2D1_COLOR_F& col);
    void Sparks(const BurstSpec& b, double age, double cx, double cy, double popMix);
    void Text(FontId f, const std::wstring& s, double x, double y, double w, const D2D1_COLOR_F& col,
              DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextWidth(FontId f, const std::wstring& s);
    IDWriteTextLayout* Layout(FontId f, const std::wstring& s, float w, DWRITE_TEXT_ALIGNMENT al);
    void EnsureWordmark(const std::wstring& title);
};

// ---------------------------------------------------------------- 初期化

bool SplashRenderer::Impl::Init(float dpiScale, const std::wstring& logoPath)
{
    dpi = dpiScale < 0.5f ? 1.0f : (dpiScale > 4.0f ? 4.0f : dpiScale);
    W = static_cast<int>(std::lround(kWindowW * dpi));
    H = static_cast<int>(std::lround(kWindowH * dpi));

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInit = SUCCEEDED(hrCom);     // RPC_E_CHANGED_MODE は「既に別モードで初期化済み」= そのまま使える（失敗扱いにしない）

    // D3D11（BGRA サポート）: ハードウェア → WARP
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        warp = SUCCEEDED(hr);
    }
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
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf), &target)))
    { err = "CreateBitmap(target)"; return false; }
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf), &readback)))
    { err = "CreateBitmap(readback)"; return false; }
    dc->SetTarget(target.Get());
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // 透明な面に描くので ClearType は使えない
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (!BuildBrushes()) { if (err.empty()) err = "BuildBrushes"; return false; }
    if (!BuildFonts()) { if (err.empty()) err = "BuildFonts"; return false; }
    if (!BuildShadow()) { if (err.empty()) err = "BuildShadow"; return false; }
    LoadLogo(logoPath);   // 失敗してもテキストのみで続行
    return true;
}

bool SplashRenderer::Impl::BuildBrushes()
{
    if (FAILED(dc->CreateSolidColorBrush(Col(0xFFFFFF), &solid))) return false;

    auto stops2 = [&](D2D1_GRADIENT_STOP s0, D2D1_GRADIENT_STOP s1, ComPtr<ID2D1GradientStopCollection>& out) {
        const D2D1_GRADIENT_STOP st[2] = { s0, s1 };
        return SUCCEEDED(dc->CreateGradientStopCollection(st, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &out));
    };
    // カード面: 上 → 下へわずかに暗く
    {
        ComPtr<ID2D1GradientStopCollection> gs;
        if (!stops2({ 0.0f, Col(0x1A1A1E) }, { 1.0f, Col(kBg0) }, gs)) return false;
        if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, F(kCardH))), gs.Get(), &cardFill))) return false;
    }
    // 枠: 上辺だけ少し明るい（縁の光）
    {
        ComPtr<ID2D1GradientStopCollection> gs;
        if (!stops2({ 0.0f, Col(0xFFFFFF, 0.16) }, { 1.0f, Col(0xFFFFFF, 0.04) }, gs)) return false;
        if (FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, F(kCardH))), gs.Get(), &cardEdge))) return false;
    }
    // 単位円の放射グラデ（半径 1 の円を Scale して使う）
    auto radial = [&](uint32_t rgb, ComPtr<ID2D1RadialGradientBrush>& out, double peak) {
        // 減衰は (1-x)^2.4。縁で傾きが急に変わらないので、光の輪郭（円盤の縁）が見えない。
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
    if (!radial(0xFFFFFF, glowWhite, 1.0)) return false;
    if (!radial(kAmber, glowAmber, 1.0)) return false;
    if (!radial(kAccent, glowCard, 0.16)) return false;

    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    if (FAILED(factory->CreateStrokeStyle(sp, nullptr, 0, &roundCap))) return false;

    // 4 点の星（単位半径）
    if (FAILED(factory->CreatePathGeometry(&star))) return false;
    {
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(star->Open(&sink))) return false;
        const double inner = 0.24;
        sink->BeginFigure(D2D1::Point2F(1.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 0; i < 4; ++i)
        {
            const double a1 = (i + 0.5) * 3.14159265358979 / 2.0;
            const double a2 = (i + 1) * 3.14159265358979 / 2.0;
            sink->AddLine(D2D1::Point2F(F(std::cos(a1) * inner), F(std::sin(a1) * inner)));
            sink->AddLine(D2D1::Point2F(F(std::cos(a2)), F(std::sin(a2))));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return false;
    }
    return true;
}

bool SplashRenderer::Impl::BuildFonts()
{
    const std::wstring latin = PickIn(dw.Get(), { L"Segoe UI Variable Display", L"Segoe UI", L"Arial" });
    const std::wstring jp    = PickIn(dw.Get(), { L"Yu Gothic UI", L"Meiryo UI", L"Segoe UI" });
    for (int i = 0; i < kFontCount; ++i)
    {
        const FontDef& d = kFonts[i];
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(dw->CreateTextFormat(d.latin ? latin.c_str() : jp.c_str(), nullptr, d.weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, d.size, L"ja-jp", &f)))
            return false;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ell;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f.Get(), &ell))) f->SetTrimming(&tr, ell.Get());
        fmt[i] = f;
    }
    return true;
}

// カードの柔らかいドロップシャドウ（窓全体サイズのビットマップへ 1 度だけ焼く）。
bool SplashRenderer::Impl::BuildShadow()
{
    const D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(W), static_cast<UINT32>(H));
    const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf);
    ComPtr<ID2D1Bitmap1> shape;
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, bp, &shape))) return false;
    if (FAILED(dc->CreateBitmap(sz, nullptr, 0, bp, &shadow))) return false;

    ComPtr<ID2D1Effect> blur;
    if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return false;

    auto pass = [&](double sigmaLogical, double dyLogical, double alpha) -> bool {
        // 形（カードの角丸矩形）を描く
        dc->SetTarget(shape.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        dc->SetTransform(D2D1::Matrix3x2F::Scale(dpi, dpi));
        solid->SetColor(Col(0x000000, alpha));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(F(kMargin), F(kMargin + dyLogical), F(kMargin + kCardW), F(kMargin + kCardH + dyLogical)), 18.0f, 18.0f), solid.Get());
        if (FAILED(dc->EndDraw())) return false;
        // ぼかして shadow へ加算合成
        blur->SetInput(0, shape.Get());
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, F(sigmaLogical * dpi));
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
    if (!pass(22.0, 14.0, 0.62)) return false;    // 広く柔らかい影
    if (!pass(5.0, 3.0, 0.50)) return false;      // 接地感の濃い影
    dc->SetTarget(target.Get());
    return true;
}

bool SplashRenderer::Impl::LoadLogo(const std::wstring& path)
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

// ---------------------------------------------------------------- 描画部品

void SplashRenderer::Impl::Glow(ID2D1RadialGradientBrush* b, double cx, double cy, double r, double op)
{
    if (op <= 0.001 || r <= 0.5) return;
    Xf(D2D1::Matrix3x2F::Scale(F(r), F(r)) * D2D1::Matrix3x2F::Translation(F(cx), F(cy)));
    b->SetOpacity(F(std::min(1.0, op)));
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), 1.0f, 1.0f), b);
}

// 12 時から時計回りに a0deg（12 時 = 0°）から sweepDeg。sweep が 360 近ければ円。
void SplashRenderer::Impl::Arc(double a0deg, double sweepDeg, double r, double w, const D2D1_COLOR_F& col)
{
    if (sweepDeg <= 0.01 || col.a <= 0.002f) return;
    solid->SetColor(col);
    const D2D1_POINT_2F c = D2D1::Point2F(F(kRingCx), F(kRingCy));
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

void SplashRenderer::Impl::Sparks(const BurstSpec& b, double age, double cx, double cy, double popMix)
{
    for (int i = 0; i < b.count; ++i)
    {
        Spark s;
        if (!SparkAt(b, i, age, s)) continue;
        const double x = cx + s.x, y = cy + s.y;
        uint32_t col = s.tint == 0 ? 0xFFFFFF : (s.tint == 1 ? 0xBBDBFF : (s.tint == 2 ? kAccentHi : kAmber));
        if (s.tint == 3 && popMix < 0.5) col = 0xFFFFFF;   // 琥珀の粒は pop のバーストだけ
        // 淡い光暈
        Glow(s.tint == 3 ? glowAmber.Get() : (s.tint == 2 || s.tint == 1 ? glowAccent.Get() : glowWhite.Get()), x, y, s.size * 3.4, s.alpha * 0.55);
        Xf(D2D1::Matrix3x2F::Scale(F(s.size), F(s.size)) * D2D1::Matrix3x2F::Rotation(F(s.rot * 180.0 / 3.14159265358979)) * D2D1::Matrix3x2F::Translation(F(x), F(y)));
        solid->SetColor(Col(col, s.alpha));
        if (s.kind == 0) dc->FillGeometry(star.Get(), solid.Get());
        else dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), 0.55f, 0.55f), solid.Get());
    }
}

IDWriteTextLayout* SplashRenderer::Impl::Layout(FontId f, const std::wstring& s, float w, DWRITE_TEXT_ALIGNMENT al)
{
    std::wstring key;
    key.reserve(s.size() + 16);
    key += static_cast<wchar_t>(L'A' + f);
    key += static_cast<wchar_t>(L'0' + static_cast<int>(al));
    key += std::to_wstring(static_cast<int>(w));
    key += L'|';
    key += s;
    auto it = layouts.find(key);
    if (it != layouts.end()) return it->second.Get();
    if (layouts.size() > 300) layouts.clear();
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt[f].Get(), w, 200.0f, &l))) return nullptr;
    l->SetTextAlignment(al);
    IDWriteTextLayout* raw = l.Get();
    layouts.emplace(std::move(key), std::move(l));
    return raw;
}

void SplashRenderer::Impl::Text(FontId f, const std::wstring& s, double x, double y, double w, const D2D1_COLOR_F& col, DWRITE_TEXT_ALIGNMENT al)
{
    if (s.empty() || col.a <= 0.002f) return;
    IDWriteTextLayout* l = Layout(f, s, F(w), al);
    if (!l) return;
    solid->SetColor(col);
    dc->DrawTextLayout(D2D1::Point2F(F(x), F(y)), l, solid.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float SplashRenderer::Impl::TextWidth(FontId f, const std::wstring& s)
{
    IDWriteTextLayout* l = Layout(f, s, 2000.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!l) return 0.0f;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void SplashRenderer::Impl::EnsureWordmark(const std::wstring& title)
{
    if (title == wordTitle && !wordX.empty()) return;
    wordTitle = title;
    wordX.assign(title.size(), 0.0f);
    IDWriteTextLayout* l = Layout(kFWord, title, 2000.0f, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!l) return;
    for (size_t i = 0; i < title.size(); ++i)
    {
        float x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS hm{};
        if (SUCCEEDED(l->HitTestTextPosition(static_cast<UINT32>(i), FALSE, &x, &y, &hm))) wordX[i] = x;
    }
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    wordW = m.widthIncludingTrailingWhitespace;
}

// ---------------------------------------------------------------- 1 フレーム

bool SplashRenderer::Impl::Draw(const SplashFrame& f, const SplashContent& c)
{
    const double t = f.t;
    // カードのローカル → 窓の論理 → （窓中心で拡大）→ 物理 px
    cardBase = D2D1::Matrix3x2F::Translation(F(kMargin), F(kMargin))
             * D2D1::Matrix3x2F::Scale(F(f.zoom), F(f.zoom), D2D1::Point2F(F(kWindowW * 0.5), F(kWindowH * 0.5)))
             * D2D1::Matrix3x2F::Scale(dpi, dpi);

    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // --- 影（窓全体のビットマップ。カードと同じ拡大に乗せる）
    dc->SetTransform(D2D1::Matrix3x2F::Scale(F(f.zoom), F(f.zoom), D2D1::Point2F(F(kWindowW * 0.5), F(kWindowH * 0.5)))
                     * D2D1::Matrix3x2F::Scale(dpi, dpi));
    dc->DrawBitmap(shadow.Get(), D2D1::RectF(0, 0, F(kWindowW), F(kWindowH)), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);

    // --- カード面
    XfCard();
    const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(D2D1::RectF(0, 0, F(kCardW), F(kCardH)), 18.0f, 18.0f);
    dc->FillRoundedRectangle(card, cardFill.Get());
    // 淡い光（ロゴの周りが少し青く明るい）+ 右下の抜け
    Glow(glowCard.Get(), kRingCx, kRingCy, 250.0, 1.0);
    // 枠
    solid->SetColor(Col(kBorder, 0.9));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, F(kCardW - 0.5), F(kCardH - 0.5)), 17.5f, 17.5f), solid.Get(), 1.0f);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, F(kCardW - 1.5), F(kCardH - 1.5)), 16.5f, 16.5f), cardEdge.Get(), 1.0f);

    // カード内はカードの矩形でクリップ（火花・光がカードの外へはみ出さない）。
    // ★角丸ジオメトリの PushLayer はやめた: 毎フレーム窓サイズの一時テクスチャを確保して重い。
    //   光は半径をカード内に収め（角に届かない）、火花は中央寄りなので、軸並行のクリップで十分。
    XfCard();
    dc->PushAxisAlignedClip(D2D1::RectF(1.0f, 1.0f, F(kCardW - 1), F(kCardH - 1)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // --- ロゴの背後の光（呼吸 + ポン）
    Glow(glowAccent.Get(), kRingCx, kRingCy, kRingR * 1.55, f.glow * 0.62);

    // --- リング: トラック
    {
        const double sweep = 360.0 * f.ringTrack;
        XfCard();
        Arc(0.0, sweep, kRingR, 3.0, Col(0xFFFFFF, 0.085));
        // 進捗（弧）と発光
        const double sw = 360.0 * f.progress * f.ringTrack;
        const double flash = f.ringFlash;
        const D2D1_COLOR_F core = Mix(kAccent, 0xFFFFFF, 0.55 * flash);
        if (sw > 0.3)
        {
            Arc(0.0, sw, kRingR, 12.0, Col(kAccent, 0.040 + 0.10 * flash));
            Arc(0.0, sw, kRingR, 7.5, Col(kAccent, 0.085 + 0.16 * flash));
            Arc(0.0, sw, kRingR, 4.6, core);
            // 先端の輝点
            const double ang = (360.0 * f.progress - 90.0) * 3.14159265358979 / 180.0;
            const double hx = kRingCx + std::cos(ang) * kRingR, hy = kRingCy + std::sin(ang) * kRingR;
            if (f.progress < 0.999)
            {
                Glow(glowAccent.Get(), hx, hy, 13.0, 0.85);
                XfCard();
                solid->SetColor(Col(0xEAF3FF, 0.95));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(F(hx), F(hy)), 2.6f, 2.6f), solid.Get());
            }
        }
        // 周回ハイライト（彗星）
        if (f.lapActive)
        {
            const double head = 360.0 * f.lapAngle;
            const int N = 22;
            const double tail = 95.0;
            for (int k = 0; k < N; ++k)
            {
                const double u0 = static_cast<double>(k) / N, u1 = static_cast<double>(k + 1) / N;
                const double a1 = head - tail * u0, a0 = head - tail * u1;
                if (a1 <= 0.0) break;
                const double fade = std::pow(1.0 - u0, 1.7) * f.lapAlpha;
                XfCard();
                Arc(std::max(0.0, a0), a1 - std::max(0.0, a0), kRingR, 4.6 + 3.0 * (1.0 - u0), Mix(kAccentHi, 0xFFFFFF, 0.7 * (1.0 - u0), 0.92 * fade));
            }
            const double ang = (head - 90.0) * 3.14159265358979 / 180.0;
            Glow(glowWhite.Get(), kRingCx + std::cos(ang) * kRingR, kRingCy + std::sin(ang) * kRingR, 22.0, 0.9 * f.lapAlpha);
        }
        // 衝撃波
        if (f.shock >= 0.0)
        {
            const double e = EaseOutExpo(f.shock);
            const double r = kRingR + 8.0 + 46.0 * e;
            XfCard();
            solid->SetColor(Col(kAccentHi, (1.0 - f.shock) * 0.6));
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(F(kRingCx), F(kRingCy)), F(r), F(r)), solid.Get(), F(2.4 * (1.0 - f.shock) + 0.6));
        }
    }

    // --- ロゴ
    {
        const double S = kLogoSize;
        const D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(F(f.logoScaleX), F(f.logoScaleY))
                                      * D2D1::Matrix3x2F::Rotation(F(f.logoRotDeg))
                                      * D2D1::Matrix3x2F::Translation(F(kRingCx), F(kRingCy + f.logoDy));
        if (f.logoScaleX > 0.002 && f.logoScaleY > 0.002)
        {
            Xf(local);
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(F(-S / 2), F(-S / 2), F(S / 2), F(S / 2)), F(S * 0.17), F(S * 0.17));
            if (logoBrush)
            {
                logoBrush->SetTransform(D2D1::Matrix3x2F::Scale(F(S / logoW), F(S / logoH)) * D2D1::Matrix3x2F::Translation(F(-S / 2), F(-S / 2)));
                dc->FillRoundedRectangle(rr, logoBrush.Get());
                solid->SetColor(Col(0xFFFFFF, 0.10));
                dc->DrawRoundedRectangle(rr, solid.Get(), 1.0f);
            }
            else
            {
                solid->SetColor(Col(kBg2));
                dc->FillRoundedRectangle(rr, solid.Get());
                solid->SetColor(Col(kAccent));
                dc->DrawRoundedRectangle(rr, solid.Get(), 2.0f);
                Text(kFWord, L"U", -S / 2, -S * 0.36, S, Col(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            // ポンの閃光（立方体のあたりが一瞬光る）。★Glow はカード座標を取る（上のロゴ行列とは別）。
            if (f.flash > 0.01)
            {
                Glow(glowWhite.Get(), kRingCx, kRingCy + f.logoDy - 2.0, S * 0.62, 0.85 * f.flash);
                Glow(glowAccent.Get(), kRingCx, kRingCy + f.logoDy - 2.0, S * 0.95, 0.9 * f.flash);
            }
        }
    }

    // --- 火花
    if (f.introBurstAge >= 0.0) Sparks(IntroBurst(), f.introBurstAge, kRingCx, kRingCy, 0.0);
    if (f.popBurstAge >= 0.0)   Sparks(PopBurst(), f.popBurstAge, kRingCx, kRingCy, 1.0);

    // --- 進捗の数字（リングの下）
    if (f.ringTrack > 0.5)
    {
        const double a = Clamp01((f.ringTrack - 0.5) / 0.5);
        const std::wstring num = std::to_wstring(f.percent);
        const float wn = TextWidth(kFPercent, num), wp = TextWidth(kFPercentSign, L"%");
        const double x0 = kRingCx - (wn + 2.0 + wp) * 0.5, y0 = kRingCy + kRingR + 22.0;
        const bool done = f.percent >= 100;
        const D2D1_COLOR_F col = done ? Mix(kText, kAccentHi, 0.6 + 0.4 * f.ringFlash, a) : Col(kText, a);
        XfCard();
        Text(kFPercent, num, x0, y0, wn + 8.0, col);
        Text(kFPercentSign, L"%", x0 + wn + 2.0, y0 + 10.0, wp + 8.0, Col(kTextDim, a));
    }

    // --- 右カラム: ワードマーク
    EnsureWordmark(c.title);
    {
        const double baseY = 70.0;
        for (size_t i = 0; i < c.title.size(); ++i)
        {
            if (c.title[i] == L' ') continue;
            LetterAnim a = LetterIntro(static_cast<int>(i), t);
            if (f.sincePop >= 0.0) a.dy += LetterWave(static_cast<int>(i), f.sincePop).dy;
            if (a.alpha <= 0.002) continue;
            const double x = kRightX + wordX[i];
            const std::wstring ch(1, c.title[i]);
            const float cw = TextWidth(kFWord, ch);
            // 文字の下端中心を軸にスクワッシュ&ストレッチ
            const double px = x + cw * 0.5, py = baseY + 58.0;
            Xf(D2D1::Matrix3x2F::Scale(F(a.sx), F(a.sy), D2D1::Point2F(F(px), F(py))) * D2D1::Matrix3x2F::Translation(0.0f, F(a.dy)));
            // 「Uno」の U だけアクセント色（ロゴの青に呼応）
            const D2D1_COLOR_F col = (i == 0) ? Mix(kText, kAccentHi, 0.55, a.alpha) : Col(0xF2F2F5, a.alpha);
            Text(kFWord, ch, x, baseY, cw + 12.0, col);
        }
        XfCard();
    }

    // --- 右カラム: バージョン行 / プロジェクト情報
    {
        const Reveal r = RevealAt(t, 0.50);
        XfCard();
        if (!c.projectMode)
        {
            std::wstring line = c.version;
            if (!c.buildInfo.empty()) line += (line.empty() ? L"" : L"   ·   ") + c.buildInfo;
            Text(kFVersion, line, kRightX + 3.0, 140.0 + r.dy, kRightW, Col(kTextDim, r.alpha));
        }
        else
        {
            const Reveal r2 = RevealAt(t, 0.58);
            // 「PROJECT」チップ + 名前 + シーン + 版/ビルド
            Text(kFHead, L"PROJECT", kRightX + 3.0, 150.0 + r.dy, 200.0, Col(kAccentHi, r.alpha * 0.9));
            Text(kFProject, c.projectName, kRightX + 2.0, 166.0 + r2.dy, kRightW, Col(kText, r2.alpha));
            if (!c.sceneName.empty())
                Text(kFScene, L"シーン  " + c.sceneName, kRightX + 3.0, 198.0 + r2.dy, kRightW, Col(kTextDim, r2.alpha));
            std::wstring line = c.version;
            if (!c.buildInfo.empty()) line += (line.empty() ? L"" : L"   ·   ") + c.buildInfo;
            Text(kFVersion, line, kRightX + 3.0, 220.0 + r2.dy, kRightW, Col(kTextFaint, r2.alpha));
        }
    }

    // --- 右カラム: 手順とステップ文言（クロスフェード）
    {
        const Reveal r = RevealAt(t, 0.60);
        const double base = c.projectMode ? 268.0 : 196.0;
        XfCard();
        if (c.stepTotal > 0)
        {
            wchar_t buf[48];
            std::swprintf(buf, 48, L"STEP %02d / %02d", std::min(c.stepIndex, c.stepTotal), c.stepTotal);
            Text(kFStepNo, buf, kRightX + 3.0, base + r.dy, 200.0, Col(kTextFaint, r.alpha));
        }
        const double e = EaseOutCubic(c.stepU);
        const double y = base + 20.0;
        if (!c.stepPrev.empty() && c.stepU < 1.0)
            Text(kFStepLabel, c.stepPrev, kRightX + 2.0, y - 9.0 * e + r.dy, kRightW, Col(kText, std::max(0.0, 1.0 - e * 1.6) * r.alpha));
        Text(kFStepLabel, c.stepCur, kRightX + 2.0, y + 9.0 * (1.0 - e) + r.dy, kRightW, Col(kText, e * r.alpha));
    }

    // --- 右カラム: 最近のプロジェクト（起動時のみ・控えめ）
    if (!c.projectMode && !c.recents.empty())
    {
        const Reveal r = RevealAt(t, 0.78);
        XfCard();
        const double y0 = 262.0;
        Text(kFHead, L"最近のプロジェクト", kRightX + 2.0, y0 + r.dy, kRightW, Col(kTextFaint, r.alpha * 0.9));
        const size_t n = std::min<size_t>(c.recents.size(), 3);
        for (size_t i = 0; i < n; ++i)
        {
            const Reveal ri = RevealAt(t, 0.84 + 0.06 * static_cast<double>(i), 0.4, 8.0);
            const double y = y0 + 20.0 + 20.0 * static_cast<double>(i) + ri.dy;
            solid->SetColor(Col(kAccent, 0.75 * ri.alpha));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(F(kRightX + 5.0), F(y + 9.0)), 2.0f, 2.0f), solid.Get());
            Text(kFRecent, c.recents[i], kRightX + 16.0, y, kRightW - 16.0, Col(kTextDim, ri.alpha));
        }
    }

    // --- 下部: 区切り線 + Tips
    {
        const Reveal r = RevealAt(t, 0.70);
        XfCard();
        solid->SetColor(Col(0xFFFFFF, 0.06 * r.alpha));
        dc->FillRectangle(D2D1::RectF(36.0f, 358.0f, F(kCardW - 36), 359.0f), solid.Get());
        // TIP チップ
        const double cy0 = 373.0 + r.dy;
        solid->SetColor(Col(kAccent, 0.16 * r.alpha));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(36.0f, F(cy0), 68.0f, F(cy0 + 20.0)), 5.0f, 5.0f), solid.Get());
        Text(kFChip, L"TIP", 36.0, cy0 + 3.0, 32.0, Col(kAccentHi, r.alpha), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!c.tip.empty())
            Text(kFTip, c.tip, 80.0, cy0 + 1.5 + c.tipDy, kCardW - 80.0 - 36.0, Col(kTextMid, c.tipAlpha * r.alpha));
    }

    XfCard();
    dc->PopAxisAlignedClip();

    const HRESULT hr = dc->EndDraw();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------- 公開 API

SplashRenderer::SplashRenderer() : p_(std::make_unique<Impl>()) {}
SplashRenderer::~SplashRenderer()
{
    // COM のオブジェクト（WIC/D2D/DWrite）を全部解放してから CoUninitialize する（逆順だと解放時に落ちうる）。
    const bool co = p_ && p_->comInit;
    p_.reset();
    if (co) CoUninitialize();
}

bool SplashRenderer::Init(float dpiScale, const std::wstring& logoPath) { return p_->Init(dpiScale, logoPath); }
int SplashRenderer::Width() const { return p_->W; }
int SplashRenderer::Height() const { return p_->H; }
bool SplashRenderer::Draw(const SplashFrame& frame, const SplashContent& content) { return p_->Draw(frame, content); }
const std::string& SplashRenderer::LastError() const { return p_->err; }
bool SplashRenderer::UsedWarp() const { return p_->warp; }

bool SplashRenderer::CopyPixels(uint8_t* dst, int dstPitch)
{
    Impl& s = *p_;
    if (FAILED(s.readback->CopyFromBitmap(nullptr, s.target.Get(), nullptr))) return false;
    D2D1_MAPPED_RECT m{};
    if (FAILED(s.readback->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    const int rowBytes = s.W * 4;
    for (int y = 0; y < s.H; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(dstPitch), m.bits + static_cast<size_t>(y) * m.pitch, static_cast<size_t>(rowBytes));
    s.readback->Unmap();
    return true;
}

} // namespace dx12e::splash
