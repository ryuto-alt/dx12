// 起動画面 Graphite Gauge の描画器（core/SplashGaugeRenderer.*。W1-BD3）のテスト。窓は作らない（D3D11 の WARP + D2D のオフスクリーン）。
//   使い方: SplashGaugeRendererTests <リポジトリのルート> [--update-golden]
// 受け入れ（EXEC_W05_W1 §2.6 BD3 / PROGRAM_PLAN PP-BD3）:
//  (1) 純ロジック: 段階 L / M / S の選択、工程の対応、読み取りの文字、色トークンが tokens.json と一致
//  (2) キーフレーム 10 枚 x DPI 3 種のゴールデン（平均差 1.0/255 以下、8x8 の局所差 24/255 以下）。WARP で描く
//  (3) 参照画像 ref_boot_hero.png との SSIM >= 0.85
//  (4) WARP で 1 フレーム 12ms 以下
//  (5) 静的な層のキャッシュ（描き直しは 1 回）、決定論（同じ入力で同じ画素）
//  (6) 黄の予算（画素比 <= 1.0%、黄の面（9x9 侵食後）= 0）
//  (7) スキップ（ランダムな 5 時刻）: 以降の絵が速い経路のロック以降と一致
#include "core/SplashGaugeRenderer.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/SplashCommon.h"

#pragma comment(lib, "windowscodecs.lib")

namespace
{
int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define CHECK_MSG(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

namespace sp = dx12e::splash;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

struct Image { int w = 0, h = 0; std::vector<std::uint8_t> bgra; };

bool ReadPng(const fs::path& path, Image& out)
{
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    out.w = static_cast<int>(w); out.h = static_cast<int>(h);
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()));
}

bool WritePng(const fs::path& path, const Image& im)
{
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&fr, &props))) return false;
    {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = WICPngFilterAdaptive;   // 画素の並びに合わせて行ごとにフィルタを選ぶ（ゴールデンを小さくする）
        props->Write(1, &opt, &v);
    }
    if (FAILED(fr->Initialize(props.Get()))) return false;
    if (FAILED(fr->SetSize(static_cast<UINT>(im.w), static_cast<UINT>(im.h)))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fr->SetPixelFormat(&fmt))) return false;
    if (FAILED(fr->WritePixels(static_cast<UINT>(im.h), static_cast<UINT>(im.w * 4), static_cast<UINT>(im.bgra.size()), const_cast<BYTE*>(im.bgra.data())))) return false;
    return SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}

// 平均差（RGB の絶対差の平均 /255 単位の 0..255）と、8x8 ブロックの平均差の最大
void Compare(const Image& a, const Image& b, double& mean, double& maxBlock)
{
    mean = 0; maxBlock = 0;
    if (a.w != b.w || a.h != b.h) { mean = maxBlock = 1e9; return; }
    double sum = 0;
    for (size_t i = 0; i < a.bgra.size(); i += 4)
        sum += (std::abs(a.bgra[i] - b.bgra[i]) + std::abs(a.bgra[i + 1] - b.bgra[i + 1]) + std::abs(a.bgra[i + 2] - b.bgra[i + 2])) / 3.0;
    mean = sum / (static_cast<double>(a.bgra.size()) / 4.0);
    for (int by = 0; by + 8 <= a.h; by += 8)
        for (int bx = 0; bx + 8 <= a.w; bx += 8)
        {
            double s = 0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                {
                    const size_t i = (static_cast<size_t>(by + y) * a.w + (bx + x)) * 4;
                    s += (std::abs(a.bgra[i] - b.bgra[i]) + std::abs(a.bgra[i + 1] - b.bgra[i + 1]) + std::abs(a.bgra[i + 2] - b.bgra[i + 2])) / 3.0;
                }
            maxBlock = std::max(maxBlock, s / 64.0);
        }
}

double Luma(const Image& im, size_t i) { return 0.2126 * im.bgra[i + 2] + 0.7152 * im.bgra[i + 1] + 0.0722 * im.bgra[i]; }

// 8x8 非重なりの窓の平均 SSIM（輝度）
double Ssim(const Image& a, const Image& b)
{
    if (a.w != b.w || a.h != b.h) return 0.0;
    const double C1 = 6.5025, C2 = 58.5225;
    double total = 0; int n = 0;
    for (int by = 0; by + 8 <= a.h; by += 8)
        for (int bx = 0; bx + 8 <= a.w; bx += 8)
        {
            double ma = 0, mb = 0;
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) { const size_t i = (static_cast<size_t>(by + y) * a.w + bx + x) * 4; ma += Luma(a, i); mb += Luma(b, i); }
            ma /= 64; mb /= 64;
            double va = 0, vb = 0, cv = 0;
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
            {
                const size_t i = (static_cast<size_t>(by + y) * a.w + bx + x) * 4;
                const double da = Luma(a, i) - ma, db = Luma(b, i) - mb;
                va += da * da; vb += db * db; cv += da * db;
            }
            va /= 63; vb /= 63; cv /= 63;
            total += ((2 * ma * mb + C1) * (2 * cv + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
            ++n;
        }
    return n ? total / n : 0.0;
}

// ---- 黄の判定（yellowBudget.yellowZone: OKLCH で C >= 0.07 かつ 75 <= h <= 105）
bool IsYellow(std::uint8_t r8, std::uint8_t g8, std::uint8_t b8)
{
    auto lin = [](double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const double r = lin(r8), g = lin(g8), b = lin(b8);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    (void)L;
    const double C = std::sqrt(A * A + B * B);
    double h = std::atan2(B, A) * 180.0 / 3.14159265358979323846;
    if (h < 0) h += 360.0;
    return C >= 0.07 && h >= 75.0 && h <= 105.0;
}

void YellowStats(const Image& im, int erode, double& ratio, long& faceCount)
{
    std::vector<std::uint8_t> m(static_cast<size_t>(im.w) * im.h);
    long cnt = 0;
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
        {
            const size_t i = (static_cast<size_t>(y) * im.w + x) * 4;
            const bool yel = IsYellow(im.bgra[i + 2], im.bgra[i + 1], im.bgra[i]);
            m[static_cast<size_t>(y) * im.w + x] = yel ? 1 : 0;
            cnt += yel;
        }
    ratio = static_cast<double>(cnt) / (static_cast<double>(im.w) * im.h);
    // 積分画像で「erode x erode の窓がすべて黄」の位置を数える（9x9 の侵食後に残る画素 = 黄の面）
    std::vector<long> sat(static_cast<size_t>(im.w + 1) * (im.h + 1), 0);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            sat[static_cast<size_t>(y + 1) * (im.w + 1) + (x + 1)] = m[static_cast<size_t>(y) * im.w + x] + sat[static_cast<size_t>(y) * (im.w + 1) + (x + 1)] +
                                                                    sat[static_cast<size_t>(y + 1) * (im.w + 1) + x] - sat[static_cast<size_t>(y) * (im.w + 1) + x];
    faceCount = 0;
    for (int y = 0; y + erode <= im.h; ++y)
        for (int x = 0; x + erode <= im.w; ++x)
        {
            const long s = sat[static_cast<size_t>(y + erode) * (im.w + 1) + (x + erode)] - sat[static_cast<size_t>(y) * (im.w + 1) + (x + erode)] -
                           sat[static_cast<size_t>(y + erode) * (im.w + 1) + x] + sat[static_cast<size_t>(y) * (im.w + 1) + x];
            if (s == static_cast<long>(erode) * erode) ++faceCount;
        }
}

// ---- 描画の台本（ヒーロー画像と同じ文言。決定論）
sp::SplashInfo HeroInfo(const std::wstring& fontFile)
{
    sp::SplashInfo i;
    i.version = L"v0.9.3";
    i.drawingNo = L"UN-0.9.3";
    i.verLines = { L"build 2611.26 · x64 · Development", L"RTX 5060 · 8 GB · Driver 581.42", L"前回 3.78 s · 推定残り 0.90 s" };
    i.stepTitle = L"アセットを読み込み中";
    i.stepDetail = L"8,536 / 12,480 件 · スクリプト 214";
    for (int k = 0; k < 5; ++k)
    {
        sp::SplashStepCell c;
        wchar_t no[8]; std::swprintf(no, 8, L"%02d", k + 1);
        c.no = no; c.name = sp::GaugeStepName(false, false, k);
        if (k < 3) c.sec = k == 0 ? 0.26 : (k == 1 ? 0.44 : 0.96);
        i.steps.push_back(c);
    }
    i.stepCur = 3;
    i.progress = 0.684;
    i.fontFile = fontFile;
    return i;
}

bool Render(sp::GaugeSurface& surf, const sp::GaugeFrame& f, const sp::SplashInfo& info, Image& out)
{
    if (!surf.Draw(f, info)) return false;
    out.w = surf.Width(); out.h = surf.Height();
    out.bgra.resize(static_cast<size_t>(out.w) * out.h * 4);
    return surf.CopyPixels(out.bgra.data(), out.w * 4);
}

bool Render2(sp::GaugeSurface& surf, Image& out)
{
    out.w = surf.Width(); out.h = surf.Height();
    out.bgra.resize(static_cast<size_t>(out.w) * out.h * 4);
    return surf.CopyPixels(out.bgra.data(), out.w * 4);
}

// ---------------------------------------------------------------- (1) 純ロジック
void Test_Pure(const fs::path& root)
{
    using namespace sp;
    // 段階の選択（boot_impl_spec §1.2 の表）
    CHECK(PickGaugeLayout(3840, 2160).tier == GaugeTier::L);          // 3840x2160 @100%
    CHECK(PickGaugeLayout(2560, 1440).tier == GaugeTier::M);          // 2560x1440 @100%
    CHECK(PickGaugeLayout(3840 / 1.5, 2160 / 1.5).tier == GaugeTier::M);   // 4K @150%
    CHECK(PickGaugeLayout(1920, 1080).tier == GaugeTier::S);          // 1920x1080 @100%
    CHECK(PickGaugeLayout(1366, 768).tier == GaugeTier::S);           // 1366x768 @100%
    CHECK(PickGaugeLayout(1366, 768).fit == 1.0f);
    {
        const GaugeLayout tiny = PickGaugeLayout(1024, 600);          // S も 60% に入らない → 80% に収まるよう縮める
        CHECK(tiny.tier == GaugeTier::S && tiny.fit < 1.0f && tiny.fit >= 0.8f);
        CHECK(tiny.WindowW() <= 1024 * 0.8 + 0.5 && tiny.WindowH() <= 600 * 0.8 + 0.5);
        const GaugeLayout tinier = PickGaugeLayout(640, 360);         // 下限 0.8（それ以上は縮めない）
        CHECK(tinier.fit == 0.8f);
    }
    int w, h;
    GaugeWindowPixels(PickGaugeLayout(3840, 2160), 1.5f, w, h);
    CHECK(w == 2400 && h == 1350);
    // 段階ごとに省く要素（§1.3）
    CHECK(MakeGaugeGeo(GaugeTier::L).labels && !MakeGaugeGeo(GaugeTier::M).labels && !MakeGaugeGeo(GaugeTier::S).grid);
    CHECK(MakeGaugeGeo(GaugeTier::L).verLines == 3 && MakeGaugeGeo(GaugeTier::M).verLines == 1);
    CHECK(MakeGaugeGeo(GaugeTier::M).titleBlock && !MakeGaugeGeo(GaugeTier::M).titleBlockFull && !MakeGaugeGeo(GaugeTier::S).titleBlock);

    // 工程の対応（§3）
    CHECK(GaugeDisplayStep(Stage::UpdateCheck, false, false) == 0 && GaugeDisplayStep(Stage::Window, false, false) == 0);
    CHECK(GaugeDisplayStep(Stage::Graphics, false, false) == 1 && GaugeDisplayStep(Stage::Physics, false, false) == 1);
    CHECK(GaugeDisplayStep(Stage::Shaders, false, false) == 2);
    CHECK(GaugeDisplayStep(Stage::Assets, false, false) == 3 && GaugeDisplayStep(Stage::ShadowMap, false, false) == 3);
    CHECK(GaugeDisplayStep(Stage::EditorUi, false, false) == 4 && GaugeDisplayStep(Stage::Finalize, false, false) == 4);
    CHECK(GaugeDisplayStep(Stage::ProjectAssets, false, false) == 4);   // --project 直開きは最後の工程に含める
    CHECK(GaugeDisplayStep(Stage::ProjectScene, true, false) == 0 && GaugeDisplayStep(Stage::ProjectFinalize, true, false) == 3);
    CHECK(GaugeDisplayStep(Stage::ProjectCreate, true, true) == 0 && GaugeDisplayStep(Stage::ProjectScene, true, true) == 1 && GaugeDisplayStep(Stage::ProjectFinalize, true, true) == 4);
    CHECK(GaugeStepCount(false, false) == 5 && GaugeStepCount(true, false) == 4 && GaugeStepCount(true, true) == 5);
    CHECK(GaugeDisplayStep(Stage::ProjectScene, false, false) == 4 && GaugeDisplayStep(Stage::Count, false, false) == -1);

    CHECK(GaugePercentText(0.684) == L"68.4" && GaugePercentText(-1) == L"0.0" && GaugePercentText(2) == L"100.0");

    // 見せる量（時刻だけで決まる）
    {
        const GaugeReveal r0 = EvaluateReveal(EvaluateGauge(GaugePath::Fast, 0.0, 0.f));
        CHECK(r0.grid == 0.f && r0.dial == 0.f && r0.ui == 0.f && r0.litTicks == 0);
        const GaugeReveal r1 = EvaluateReveal(EvaluateGauge(GaugePath::Fast, 1110.0, 0.f));
        CHECK(r1.grid == 1.f && r1.dial == 1.f && r1.ui == 1.f && r1.litTicks == 100 && r1.ring);
    }

    // 黄の許可集合（B1）: 指針・ロックの三角・四隅のトンボ・ハブ・工程の上線・残像線
    CHECK(kGaugeYellowAllowed.size() == 6 && kGaugeYellowPixelRatioMax == 0.010);

    // 色トークンが tokens.json と一致する
    {
        std::ifstream f(root / "assets" / "editor" / "theme" / "gg" / "tokens.json");
        CHECK(f.good());
        if (f)
        {
            const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            CHECK(!j.is_discarded());
            auto val = [&](const char* grp, const char* name) -> std::string {
                const auto& o = j["color"][grp][name]["value"];
                return o.is_string() ? o.get<std::string>() : std::string();
            };
            auto hex = [](const GaugeRgba& c) {
                char b[16];
                if (c.a == 255) std::snprintf(b, sizeof(b), "#%02X%02X%02X", c.r, c.g, c.b);
                else std::snprintf(b, sizeof(b), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
                return std::string(b);
            };
            const GaugeColors& c = kGaugeColors;
            CHECK_MSG(val("surface", "groove") == hex(c.groove), "groove");
            CHECK_MSG(val("surface", "bg0") == hex(c.bg0), "bg0");
            CHECK_MSG(val("surface", "bg2") == hex(c.bg2), "bg2");
            CHECK_MSG(val("surface", "border") == hex(c.border), "border");
            CHECK_MSG(val("surface", "borderStrong") == hex(c.borderStrong), "borderStrong");
            CHECK_MSG(val("surface", "inLineHover") == hex(c.inLineHover), "inLineHover");
            CHECK_MSG(val("text", "t1") == hex(c.t1), "t1");
            CHECK_MSG(val("text", "t2") == hex(c.t2), "t2");
            CHECK_MSG(val("text", "t3") == hex(c.t3), "t3");
            CHECK_MSG(val("text", "t4") == hex(c.t4), "t4");
            CHECK_MSG(val("accent", "accent") == hex(c.accent), "accent");
            CHECK_MSG(val("viewport", "gridMinor") == hex(c.gridMinor), "gridMinor");
            CHECK(j["yellowBudget"]["boot"].is_array() && j["yellowBudget"]["boot"].size() == 1);
        }
    }

    // --splash-style の解釈
    CHECK(ParseGaugeStyle(L"", L""));
    CHECK(!ParseGaugeStyle(L"DX12Engine.exe --splash-style classic", L""));
    CHECK(!ParseGaugeStyle(L"DX12Engine.exe --splash-style=classic --foo", L""));
    CHECK(ParseGaugeStyle(L"DX12Engine.exe --splash-style gauge", L"classic"));      // コマンドラインが環境変数より優先
    CHECK(!ParseGaugeStyle(L"DX12Engine.exe", L"classic"));
    CHECK(ParseGaugeStyle(L"DX12Engine.exe --splash-style", L""));
}

// ---------------------------------------------------------------- (2)〜(6) 描画
constexpr double kKeyMs[10] = { 0, 148, 268, 834, 1110, 1543, 1585, 1628, 1748, 2148 };
constexpr float kDpis[3] = { 1.0f, 1.5f, 2.0f };

std::string KeyName(double ms, float dpi)
{
    char b[64];
    std::snprintf(b, sizeof(b), "kf_%04d_dpi%02d.png", static_cast<int>(ms), static_cast<int>(dpi * 10 + 0.5f));
    return b;
}

void Test_Render(const fs::path& root, bool updateGolden)
{
    using namespace sp;
    const std::wstring font = SplashFontPath((root / "assets").wstring());
    CHECK(!font.empty());
    const fs::path goldenDir = root / "tests" / "golden" / "splash";
    fs::create_directories(goldenDir);
    const GaugeLayout lay = PickGaugeLayout(1e9, 1e9);   // 段階 L

    // 参照画像（ref_boot_hero.png）との SSIM は、ヒーロー画像と同じ文言・進捗で、指針が途中にあるキーフレームで比べる
    Image hero;
    const bool haveHero = ReadPng(goldenDir / "ref_boot_hero.png", hero);
    CHECK(haveHero);

    for (float dpi : kDpis)
    {
        GaugeSurface surf;
        if (!surf.Init(dpi, lay, /*forceWarp=*/true)) { CHECK_MSG(false, "Init: %s", surf.LastError().c_str()); continue; }
        CHECK(surf.UsedWarp());
        const SplashInfo info = HeroInfo(font);
        double yellowMax = 0; long faceMax = 0;
        for (double ms : kKeyMs)
        {
            GaugeFrame f = EvaluateGauge(GaugePath::Fast, ms, 0.f);
            SplashInfo inf = info;
            inf.drawingNo = L"UN-0.9.3";
            inf.progress = f.locked ? 1.0 : 0.684;
            if (f.locked) { inf.stepTitle = L"校正完了"; inf.stepDetail = L"すべての工程が完了しました"; }
            Image im, again;
            CHECK(Render(surf, f, inf, im));
            CHECK(Render(surf, f, inf, again));
            CHECK_MSG(im.bgra == again.bgra, "決定論 t=%.0f dpi=%.1f", ms, dpi);   // 同じ入力で同じ画素（静的な層のキャッシュ込み）
            const fs::path gp = goldenDir / KeyName(ms, dpi);
            if (updateGolden) { CHECK(WritePng(gp, im)); continue; }
            Image gold;
            if (!ReadPng(gp, gold)) { CHECK_MSG(false, "ゴールデンが読めない: %s", gp.string().c_str()); continue; }
            double mean, mb;
            Compare(im, gold, mean, mb);
            CHECK_MSG(mean <= 1.0 && mb <= 24.0, "t=%.0f dpi=%.1f mean=%.3f maxBlock=%.2f", ms, dpi, mean, mb);
            double ratio; long face;
            YellowStats(im, static_cast<int>(std::lround(9 * dpi)), ratio, face);
            yellowMax = std::max(yellowMax, ratio); faceMax = std::max(faceMax, face);
            CHECK_MSG(ratio <= kGaugeYellowPixelRatioMax, "黄の画素比 %.4f t=%.0f dpi=%.1f", ratio, ms, dpi);
            CHECK_MSG(face == 0, "黄の面 %ld t=%.0f dpi=%.1f", face, ms, dpi);
        }
        CHECK_MSG(surf.StaticBakeCount() == 1, "静的な層の焼き直し %u 回（1 回のはず）", surf.StaticBakeCount());
        std::printf("  dpi %.1f: 黄の画素比 最大 %.4f%%  黄の面 最大 %ld\n", dpi, yellowMax * 100.0, faceMax);

        // SSIM（DPI 1.0 のとき。ヒーロー = 1600x900）
        if (dpi == 1.0f && haveHero)
        {
            auto ssimAt = [&](double ms) {
                const GaugeFrame f = EvaluateGauge(GaugePath::Fast, ms, 0.f);
                SplashInfo inf = HeroInfo(font);
                Image im;
                CHECK(Render(surf, f, inf, im));
                return Ssim(im, hero);
            };
            const double sMid = ssimAt(1166.0), sLock = ssimAt(1628.0);
            std::printf("  SSIM vs ref_boot_hero: 1166ms(指針 255 度) %.4f / 1628ms(ロック) %.4f\n", sMid, sLock);
            CHECK_MSG(sMid >= 0.85, "SSIM %.4f", sMid);
            CHECK_MSG(sLock >= 0.85, "SSIM(lock) %.4f", sLock);
        }
    }
}

// ---------------------------------------------------------------- (4) WARP の描画時間
void Test_Perf()
{
    using namespace sp;
    GaugeSurface surf;
    CHECK(surf.Init(1.0f, PickGaugeLayout(1e9, 1e9), true));
    SplashInfo info = HeroInfo(L"");
    Image im;
    // 区間 A: 残像が最も重い 700〜1350。区間 B: ベゼルのスイープ 1648〜1848（弧のジオメトリの作成を含む）
    struct Range { const char* name; double t0, dt; };
    const Range ranges[2] = { { "trail 700-1350", 700.0, 16.7 }, { "sweep 1648-1848", 1650.0, 5.0 } };
    for (const Range& rg : ranges)
    {
        std::vector<double> ms;
        for (int i = 0; i < 40; ++i)
        {
            GaugeFrame f = EvaluateGauge(GaugePath::Fast, rg.t0 + i * rg.dt, 0.f);
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = surf.Draw(f, info) && Render2(surf, im);   // 描画 + 完了待ち（読み戻しで GPU / WARP の実行を待つ）
            const auto t1 = std::chrono::steady_clock::now();
            CHECK(ok);
            if (i >= 5) ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        const double med = ms[ms.size() / 2], mx = ms.back();
        std::printf("  WARP draw + readback (dpi 1.0, 1600x900, %s): median %.2f ms  max %.2f ms\n", rg.name, med, mx);
        CHECK_MSG(med <= 12.0, "WARP の描画 (%s) %.2f ms", rg.name, med);
    }
}

// ---------------------------------------------------------------- (7) スキップ
void Test_Skip(const fs::path& root)
{
    using namespace sp;
    const std::wstring font = SplashFontPath((root / "assets").wstring());
    GaugeSurface surf;
    CHECK(surf.Init(1.0f, PickGaugeLayout(1e9, 1e9), true));
    const CalibrateEvents& ev = DefaultCalibrateEvents();
    // 「ランダム」な 5 時刻（固定の種）
    std::uint32_t seed = 12345u;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24); };
    {
        // 完了要求の前にスキップしても「完了」扱いにはしない（絵はロックするが、本体はまだ出さない）
        GaugeFastDriver pre;
        for (double t = 0.0; t < 400.0; t += 1000.0 / 60.0) pre.Update(t, 0.3f, false);
        pre.Skip(400.0);
        pre.Update(500.0, 0.3f, false);
        CHECK(!pre.FinishObserved());
        CHECK(pre.Frame(500.0).locked);
        CHECK(std::isnan(pre.ExitStartWallMs()));
    }
    for (int k = 0; k < 5; ++k)
    {
        const double tSkip = 100.0 + rnd() * 1400.0;   // ロックの前
        GaugeFastDriver drv;
        // 60Hz で tSkip まで進め、完了要求は C22 より前（速い経路）に来ている
        double t = 0.0;
        for (; t < tSkip; t += 1000.0 / 60.0) drv.Update(t, 0.5f, t >= 50.0);
        drv.Update(tSkip, 0.5f, true);
        drv.Skip(tSkip);
        // 以降の絵 = 速い経路のロック以降（EvaluateAfterLock）。ピクセルでも一致する
        for (double dt : { 0.0, 40.0, 130.0, 520.0 })
        {
            drv.Update(tSkip + dt, 0.5f, true);
            const GaugeFrame fs = drv.Frame(tSkip + dt);
            const GaugeFrame fl = EvaluateAfterLock(dt, ev, drv.ExitStartWallMs() - tSkip);
            CHECK_MSG(!std::isnan(drv.ExitStartWallMs()), "skip k=%d: 完了要求の後は退場の時刻が決まる", k);
            CHECK_MSG(fs == fl, "skip k=%d dt=%.0f", k, dt);
            // 速い経路そのもの（ロックの時刻 + dt。退場の時刻は skip の分だけ揃える）とも一致する
            const double exitFast = ev.lockMs + (drv.ExitStartWallMs() - tSkip);
            const GaugeFrame ff = EvaluateGauge(GaugePath::Fast, ev.lockMs + dt, 0.f, ev, exitFast);
            CHECK_MSG(fs == ff, "skip k=%d dt=%.0f: 速い経路のロック以降と違う", k, dt);
            CHECK(fs.locked);
        }
        const GaugeFrame fs = drv.Frame(tSkip + 130.0);
        const GaugeFrame fl = EvaluateAfterLock(130.0, ev, drv.ExitStartWallMs() - tSkip);
        SplashInfo info = HeroInfo(font);
        info.progress = 1.0;
        Image a, b;
        CHECK(Render(surf, fs, info, a));
        CHECK(Render(surf, fl, info, b));
        CHECK_MSG(a.bgra == b.bgra, "skip の絵が速い経路のロック以降と違う k=%d", k);
    }
}

// ---------------------------------------------------------------- 段階 M / S と DPI 変更の描画
void Test_TiersAndDpi(const fs::path& root)
{
    using namespace sp;
    const std::wstring font = SplashFontPath((root / "assets").wstring());
    for (GaugeTier t : { GaugeTier::M, GaugeTier::S })
    {
        GaugeLayout lay;
        lay.tier = t; lay.geo = MakeGaugeGeo(t);
        GaugeSurface surf;
        CHECK(surf.Init(1.0f, lay, true));
        Image im;
        SplashInfo info = HeroInfo(font);
        CHECK(Render(surf, EvaluateGauge(GaugePath::Fast, 1628.0, 0.f), info, im));
        CHECK(im.w == static_cast<int>(lay.geo.W) && im.h == static_cast<int>(lay.geo.H));
        double ratio; long face;
        YellowStats(im, 9, ratio, face);
        CHECK_MSG(ratio <= kGaugeYellowPixelRatioMax && face == 0, "tier %d ratio %.4f face %ld", static_cast<int>(t), ratio, face);
    }
    // DPI の変更（ターゲットと静的な層の作り直し）
    GaugeSurface surf;
    CHECK(surf.Init(1.0f, PickGaugeLayout(1e9, 1e9), true));
    Image a, b;
    SplashInfo info = HeroInfo(font);
    const GaugeFrame f = EvaluateGauge(GaugePath::Fast, 1628.0, 0.f);
    CHECK(Render(surf, f, info, a));
    CHECK(a.w == 1600);
    CHECK(surf.OnDpiChanged(1.5f));
    CHECK(Render(surf, f, info, b));
    CHECK(b.w == 2400 && b.h == 1350);
    CHECK(surf.StaticBakeCount() == 2);   // DPI の変更で 1 回だけ焼き直す
    // プロジェクトを開く表示も描ける（文言の差し替え）
    {
        GaugeSurface s2;
        CHECK(s2.Init(1.0f, PickGaugeLayout(1e9, 1e9), true));
        SplashInfo pi = HeroInfo(font);
        pi.projectMode = true; pi.projectName = L"Junction"; pi.sceneName = L"scenes/main.json";
        pi.steps.resize(4);
        pi.steps[0].name = GaugeStepName(true, false, 0); pi.steps[1].name = GaugeStepName(true, false, 1);
        pi.steps[2].name = GaugeStepName(true, false, 2); pi.steps[3].name = GaugeStepName(true, false, 3);
        pi.stepCur = 2;
        Image im;
        CHECK(Render(s2, f, pi, im));
    }
}
// ---------------------------------------------------------------- 校正の振り付け（W1-BD6）の描画
// 残像の本数（画素で数える）: 最速区間のフレームを、blurDeg = 0 にした同じフレームと比べ、文字盤の内側の円周（半径 160）で差のある線を数える。
int CountTrailRuns(const Image& a, const Image& b, float dpi, double fromDeg, double toDeg)
{
    const double cx = 520.0 * dpi, cy = 450.0 * dpi, r = 160.0 * dpi;
    int runs = 0; bool in = false;
    for (double d = fromDeg; d <= toDeg; d += 0.05)
    {
        const double rad = d * 3.14159265358979323846 / 180.0;
        const int x = static_cast<int>(std::lround(cx + std::sin(rad) * r)), y = static_cast<int>(std::lround(cy - std::cos(rad) * r));
        const size_t i = (static_cast<size_t>(y) * a.w + x) * 4;
        int m = 0;
        for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(a.bgra[i + c] - b.bgra[i + c]));
        const bool on = m >= 3;
        if (on && !in) ++runs;
        in = on;
    }
    return runs;
}

void Test_Choreography(const fs::path& root)
{
    using namespace sp;
    const std::wstring font = SplashFontPath((root / "assets").wstring());
    const CalibrateEvents& ev = DefaultCalibrateEvents();
    for (float dpi : { 1.0f, 2.0f })
    {
        GaugeSurface surf;
        CHECK(surf.Init(dpi, PickGaugeLayout(1e9, 1e9), true));
        SplashInfo info = HeroInfo(font);
        int minRuns = 99;
        for (double t = ev.clickMs[13]; t <= ev.clickMs[15]; t += 4.0)
        {
            GaugeFrame f = EvaluateGauge(GaugePath::Fast, t, 0.f);
            GaugeFrame f0 = f; f0.blurDeg = 0.f;
            Image a, b;
            CHECK(Render(surf, f, info, a));
            CHECK(Render(surf, f0, info, b));
            const int n = gauge::TrailLineCount(f.blurDeg);
            const double ang = f.needleDeg;
            // 指針の後ろ側（blurDeg + 余裕）だけを調べる
            const int runs = CountTrailRuns(a, b, dpi, ang - f.blurDeg - 1.0, ang - 0.6);
            minRuns = std::min(minRuns, runs);
            CHECK_MSG(runs >= n - 1 && runs <= n + 1, "t=%.0f dpi=%.1f 描かれた線 %d / 期待 %d", t, dpi, runs, n);
        }
        std::printf("  dpi %.1f: 最速区間で画素から数えた残像の線 最少 %d 本\n", dpi, minRuns);
        CHECK_MSG(minRuns >= 6, "最速区間の残像が %d 本", minRuns);
    }
}

// グローなし: スイープと整列波が変えるのは、縁（ベゼル）の細い帯と目盛りの帯だけ。それより外・内に光が漏れない。
void Test_NoGlow(const fs::path& root)
{
    using namespace sp;
    const std::wstring font = SplashFontPath((root / "assets").wstring());
    const CalibrateEvents& ev = DefaultCalibrateEvents();
    GaugeSurface surf;
    CHECK(surf.Init(1.0f, PickGaugeLayout(1e9, 1e9), true));
    SplashInfo info = HeroInfo(font);
    info.progress = 1.0;
    const GaugeLayout lay = PickGaugeLayout(1e9, 1e9);
    const double cx = lay.geo.dcx, cy = lay.geo.dcy, rB = lay.geo.rBezel, rT = lay.geo.rTickOut;
    auto diffBands = [&](const Image& a, const Image& b, double lo, double hi, long& inBand, long& outBand) {
        inBand = outBand = 0;
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * a.w + x) * 4;
                if (a.bgra[i] == b.bgra[i] && a.bgra[i + 1] == b.bgra[i + 1] && a.bgra[i + 2] == b.bgra[i + 2]) continue;
                const double r = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                (r >= lo && r <= hi ? inBand : outBand)++;
            }
    };
    // (1) ベゼルのスイープ: bellT のみ違う 2 枚の差が、半径 [rB-9, rB+2] の外に出ない
    {
        GaugeFrame f = EvaluateGauge(GaugePath::Fast, ev.bellMs[0] + 100.0, 0.f);
        f.alignT.fill(1.f);
        GaugeFrame f0 = f; f0.bellT = 0.f;
        Image a, b;
        CHECK(Render(surf, f, info, a));
        CHECK(Render(surf, f0, info, b));
        long inB, outB;
        diffBands(a, b, rB - 9.0, rB + 2.0, inB, outB);
        std::printf("  ベゼルのスイープ: 帯の中 %ld 画素 / 帯の外 %ld 画素\n", inB, outB);
        CHECK(inB > 100 && outB == 0);
    }
    // (2) 整列波: alignT のみ違う 2 枚の差が、目盛りの帯 [rT-19, rT+1] の外に出ない
    {
        GaugeFrame f = EvaluateGauge(GaugePath::Fast, ev.bellMs[2] + 65.0, 0.f);
        f.bellT = 0.f;
        GaugeFrame f0 = f; f0.alignT.fill(0.f);
        Image a, b;
        CHECK(Render(surf, f, info, a));
        CHECK(Render(surf, f0, info, b));
        long inB, outB;
        diffBands(a, b, rT - 19.0, rT + 1.0, inB, outB);
        std::printf("  整列波: 帯の中 %ld 画素 / 帯の外 %ld 画素\n", inB, outB);
        CHECK(inB > 50 && outB == 0);
    }
    // (3) 反射が通り抜けたあと（alignT 全 1 / bellT 1）は、反射の無い絵と画素まで同じ（何も残らない）
    {
        GaugeFrame f = EvaluateGauge(GaugePath::Fast, 1900.0, 0.f);
        GaugeFrame f0 = f; f0.alignT.fill(0.f); f0.bellT = 0.f;
        Image a, b;
        CHECK(Render(surf, f, info, a));
        CHECK(Render(surf, f0, info, b));
        CHECK(a.bgra == b.bgra);
    }
}

// 周期的な明滅が無い: 120fps で 0〜2.4 秒を描き、全体と文字盤の平均輝度の時系列の 0.3〜5Hz の成分（直線の傾向を除いた Hann 窓の DFT）を測る。
double BandPeak(const std::vector<double>& y, double fps)
{
    const int n = static_cast<int>(y.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; ++i) { sx += i; sy += y[i]; sxx += double(i) * i; sxy += i * y[i]; }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx), icpt = (sy - slope * sx) / n;
    double peak = 0;
    for (double f = 0.3; f <= 5.0 + 1e-9; f += 0.1)
    {
        double re = 0, im = 0, wsum = 0;
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / (n - 1));
            const double v = (y[i] - (slope * i + icpt)) * w;
            const double ph = 2.0 * 3.14159265358979323846 * f * i / fps;
            re += v * std::cos(ph); im -= v * std::sin(ph); wsum += w;
        }
        peak = std::max(peak, 2.0 * std::hypot(re, im) / wsum);
    }
    return peak;
}

void Test_NoPeriodicFlicker(const fs::path& root)
{
    using namespace sp;
    // 検出器の感度: 2Hz・振幅 0.7（平均輝度の 1% 程度）を入れたら必ず検出する
    {
        std::vector<double> s;
        for (int i = 0; i < 288; ++i) s.push_back(60.0 + 0.7 * std::sin(2.0 * 3.14159265358979323846 * 2.0 * i / 120.0));
        CHECK(BandPeak(s, 120.0) > 0.4);
        std::vector<double> t2;
        for (int i = 0; i < 288; ++i) t2.push_back(60.0 + 0.15 * std::sin(2.0 * 3.14159265358979323846 * 1.0 * i / 120.0));
        CHECK(BandPeak(t2, 120.0) > 0.10);   // 0.15 の 1Hz も検出する（しきい値 0.05 / 0.10 より大きい）
    }
    const std::wstring font = SplashFontPath((root / "assets").wstring());
    GaugeSurface surf;
    CHECK(surf.Init(1.0f, PickGaugeLayout(1e9, 1e9), true));
    SplashInfo info = HeroInfo(font);
    info.progress = 1.0;
    // 校正の振り付け（残像・整列波・ベゼルのスイープ）だけが作る輝度の変化 = 実際のフレーム - 振り付けを切った同じフレーム。
    // 画面全体の変化（方眼・文字・退場のフェード）は 1 回きりの立ち上がりなので別に測って表示だけする。
    std::vector<double> whole, dial, chorWhole, chorDial;
    auto means = [&](const Image& im, double& w, double& d) {
        double sw = 0, sd = 0; long cd = 0;
        for (int y = 0; y < im.h; ++y)
            for (int x = 0; x < im.w; ++x)
            {
                const double l = Luma(im, (static_cast<size_t>(y) * im.w + x) * 4);
                sw += l;
                if (std::abs(x - 520) < 300 && std::abs(y - 450) < 300) { sd += l; ++cd; }
            }
        w = sw / (static_cast<double>(im.w) * im.h);
        d = sd / static_cast<double>(cd);
    };
    for (int n = 0; n < 288; ++n)
    {
        const GaugeFrame f = EvaluateGauge(GaugePath::Fast, n * 1000.0 / 120.0, 0.f);
        GaugeFrame off = f; off.alignT.fill(0.f); off.bellT = 0.f; off.blurDeg = 0.f;
        Image im, im0;
        CHECK(Render(surf, f, info, im));
        CHECK(Render(surf, off, info, im0));
        double w, d, w0, d0;
        means(im, w, d); means(im0, w0, d0);
        whole.push_back(w); dial.push_back(d);
        chorWhole.push_back(w - w0); chorDial.push_back(d - d0);
    }
    const double pw = BandPeak(whole, 120.0), pd = BandPeak(dial, 120.0);
    const double cw = BandPeak(chorWhole, 120.0), cd = BandPeak(chorDial, 120.0);
    std::printf("  0.3〜5Hz の成分の最大 全体の絵: %.4f / 文字盤 %.4f（参考。1 回きりの立ち上がりを含む）\n", pw, pd);
    std::printf("  0.3〜5Hz の成分の最大 振り付けの分: 全体 %.4f / 文字盤 %.4f（輝度 0..255）\n", cw, cd);
    CHECK_MSG(cw < 0.05, "振り付けの明滅（全体）%.4f", cw);
    CHECK_MSG(cd < 0.10, "振り付けの明滅（文字盤）%.4f", cd);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: SplashGaugeRendererTests <repo-root> [--update-golden]\n"); return 2; }
    const fs::path root = argv[1];
    const bool update = argc >= 3 && std::strcmp(argv[2], "--update-golden") == 0;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Test_Pure(root);
    Test_Render(root, update);
    Test_Perf();
    Test_Skip(root);
    Test_TiersAndDpi(root);
    Test_Choreography(root);
    Test_NoGlow(root);
    Test_NoPeriodicFlicker(root);
    if (g_fail == 0) std::printf("SplashGaugeRendererTests: OK\n");
    else std::printf("SplashGaugeRendererTests: %d FAIL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
