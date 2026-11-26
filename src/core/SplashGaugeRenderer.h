#pragma once

// ===========================================================================
// 起動画面 Graphite Gauge の描画器（W1-BD3。Direct2D + DirectWrite。D3D11 デバイス上のデバイスコンテキストで描く）
// ---------------------------------------------------------------------------
// ・入力は GaugeFrame（SplashGaugeMotion.h の純ロジックが出す 1 フレームぶんの値）と SplashInfo（文言・実進捗）だけ。
//   窓も時計も持たない。実窓（SplashScreen.cpp の UpdateLayeredWindow）・プレビュー（SplashPreview.cpp）・単体テストが同じコードを通る。
// ・描画は 2 つに分かれる:
//     GaugeRenderer … ISplashRenderer。渡されたデバイスコンテキストの現在のターゲット（窓全体）へ描く。静的な層（背景・方眼と枠・
//                     文字盤・UI の文字）は ID2D1Bitmap1 にキャッシュし、DPI・段階・静的な文言が変わったときだけ作り直す。
//     GaugeSurface  … D3D11（ハードウェア → WARP）+ D2D のデバイスとターゲット・読み戻しを持つ、窓なしの描画面（classic の
//                     SplashRenderer に相当）。実窓とプレビューが使う。
// ・色と寸法は SplashGaugeTokens.h（GG tokens.json の写し）。形は SplashBrandGeometry.h（GG の brand_geometry.h の写し）。
// ・classic（SplashRenderer.cpp）のコードは変えない（--splash-style classic で従来の見た目をビット一致で残す）。
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "core/SplashGaugeMotion.h"
#include "core/SplashGaugeTokens.h"
#include "core/SplashProgress.h"
#include "core/SplashRenderer.h"

namespace dx12e::splash
{

// ---------------------------------------------------------------- 純関数（GPU 不要。単体テスト対象）

// 表示する工程（boot_impl_spec §3）。起動は 5 本、プロジェクトを開くは 4〜5 本。
inline int GaugeStepCount(bool projectMode, bool isNew) { return projectMode ? (isNew ? 5 : 4) : 5; }

// splash::Stage → 表示工程の添字（0 始まり）。計画に無い段階は -1。
inline int GaugeDisplayStep(Stage s, bool projectMode, bool isNew)
{
    if (projectMode)
    {
        const int o = isNew ? 1 : 0;
        switch (s)
        {
        case Stage::ProjectCreate: return 0;
        case Stage::ProjectScene: return o;
        case Stage::ProjectAssets: return o + 1;
        case Stage::ProjectMaterials: return o + 2;
        case Stage::ProjectFinalize: return o + 3;
        default: return -1;
        }
    }
    switch (s)
    {
    case Stage::UpdateCheck: case Stage::Window: return 0;
    case Stage::Graphics: case Stage::Audio: case Stage::Physics: return 1;
    case Stage::Shaders: return 2;
    case Stage::Assets: case Stage::Scripts: case Stage::ShadowMap: return 3;
    case Stage::EditorUi: case Stage::Renderer: case Stage::Thumbnails: case Stage::EnvMap: case Stage::Finalize: return 4;
    // --project 直開きでは起動の続きに並ぶ。6 本目の帯は作らず、最後の工程に含める
    case Stage::ProjectCreate: case Stage::ProjectScene: case Stage::ProjectAssets: case Stage::ProjectMaterials: case Stage::ProjectFinalize: return 4;
    default: return -1;
    }
}

// 工程の名前（日本語。charset_splash に収まる字だけ）
inline const wchar_t* GaugeStepName(bool projectMode, bool isNew, int i)
{
    static const wchar_t* const kStartup[5] = { L"コア", L"デバイス", L"シェーダー", L"アセット", L"エディタ UI" };
    static const wchar_t* const kProject[5] = { L"作成", L"シーン解析", L"アセット", L"マテリアル", L"仕上げ" };
    if (i < 0 || i >= 5) return L"";
    if (!projectMode) return kStartup[i];
    return isNew ? kProject[i] : kProject[i + 1 < 5 ? i + 1 : 4];
}

// 「確認済み 68.4」: 進捗 0..1 → 表示の数字（小数 1 桁）。0 未満と 1 超は丸める
inline std::wstring GaugePercentText(double progress01)
{
    const double v = progress01 < 0.0 ? 0.0 : (progress01 > 1.0 ? 1.0 : progress01);
    wchar_t buf[16];
    swprintf(buf, 16, L"%.1f", v * 100.0);
    return buf;
}

// 段階 L / M / S の窓の大きさ（物理 px）。dpiScale と fit（S の縮小）を掛ける。
inline void GaugeWindowPixels(const GaugeLayout& lay, float dpiScale, int& w, int& h)
{
    w = static_cast<int>(std::lround(lay.WindowW() * dpiScale));
    h = static_cast<int>(std::lround(lay.WindowH() * dpiScale));
}

// 論理 DIP のレイアウトで GaugeFrame が出す「見せる量」（描画器の中の判断を、テストできるように純関数にしたもの）
struct GaugeReveal
{
    float grid = 0.f;       // 方眼の濃さ 0..1（8〜408ms）
    float dial = 0.f;       // 文字盤（ベゼル・U）の濃さ
    float labels = 0.f;     // 目盛りの数字
    float ui = 0.f;         // 文字・表題欄・工程
    std::uint32_t litTicks = 0;   // 点灯している目盛り 0..100
    bool ring = false;      // 進捗の軌跡の円（指針が現れてから）
};
inline GaugeReveal EvaluateReveal(const GaugeFrame& f)
{
    GaugeReveal r;
    const double t = f.timelineMs;
    r.grid = static_cast<float>(Clamp01((t - 8.0) / 400.0));
    const double tickFrac = Clamp01(static_cast<double>(f.tickLit) / static_cast<double>(gauge::kDialTickCount));
    r.dial = static_cast<float>(Clamp01(tickFrac * 2.0));
    r.labels = static_cast<float>(tickFrac * tickFrac);
    r.ui = static_cast<float>(EaseOutCubic((t - 148.0) / 300.0));
    r.litTicks = static_cast<std::uint32_t>(std::lround(tickFrac * 100.0));
    r.ring = f.needleShown;
    return r;
}

// ---------------------------------------------------------------- 描画器

class GaugeRenderer final : public ISplashRenderer
{
public:
    GaugeRenderer();
    ~GaugeRenderer() override;
    GaugeRenderer(const GaugeRenderer&) = delete;
    GaugeRenderer& operator=(const GaugeRenderer&) = delete;

    // Init の前に呼ぶ（段階 L / M / S と fit）。作り直しが要るときは次の Render で静的な層を焼き直す。
    void SetLayout(const GaugeLayout& layout);

    bool Init(ID2D1DeviceContext* dc, IDWriteFactory* dw, float dpi) override;
    bool Render(const GaugeFrame& frame, const SplashInfo& info) override;
    void OnDpiChanged(float dpi) override;

    const std::string& LastError() const;
    // 直近に静的な層を焼き直した回数（キャッシュが効いているかのテスト用）
    unsigned StaticBakeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------- 窓なしの描画面（D3D11 + D2D + ターゲット + 読み戻し）

class GaugeSurface
{
public:
    GaugeSurface();
    ~GaugeSurface();
    GaugeSurface(const GaugeSurface&) = delete;
    GaugeSurface& operator=(const GaugeSurface&) = delete;

    // dpiScale: 表示倍率。layout: PickGaugeLayout の結果。forceWarp: 検証用（WARP で測る）。失敗なら false（LastError）。
    bool Init(float dpiScale, const GaugeLayout& layout, bool forceWarp = false);
    int Width() const;     // 物理 px
    int Height() const;
    bool Draw(const GaugeFrame& frame, const SplashInfo& info);
    // 直近の Draw の結果を BGRA（premultiplied。アルファは全面 255）で dst へ（行間 dstPitch バイト）
    bool CopyPixels(std::uint8_t* dst, int dstPitch);
    // 別の DPI のモニターへ移ったとき（ターゲットを作り直す。失敗は false）。BD3 の窓側は未配線（起動中に窓を動かさない想定。BA-12 で扱う）
    bool OnDpiChanged(float dpiScale);
    const std::string& LastError() const;
    bool UsedWarp() const;
    std::wstring AdapterName() const;    // 例: L"NVIDIA GeForce RTX 5060"（版情報の 2 行目用）
    unsigned StaticBakeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dx12e::splash
