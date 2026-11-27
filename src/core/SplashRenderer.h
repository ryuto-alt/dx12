#pragma once

// ===========================================================================
// 起動画面の描画器（Direct2D + DirectWrite。D3D11 デバイス上のデバイスコンテキストで描く）
// ---------------------------------------------------------------------------
// ・入力は SplashFrame（core/SplashMotion.h の純ロジックが出す 1 フレームぶんの値）と SplashContent（文言）だけ。
//   窓も時計も持たない。実窓（SplashScreen.cpp の UpdateLayeredWindow）とプレビュー（SplashPreview.cpp の PNG 書き出し）が
//   同じ描画コードを通る。
// ・出力は BGRA・premultiplied alpha のピクセル。角丸カード + 柔らかいドロップシャドウ込みで、
//   窓全体（カード + 余白 = 影の逃げ）を 1 枚に描く。窓全体のフェードは呼び出し側（ULW の SourceConstantAlpha）。
// ・寸法はすべて論理 px（100% 表示の px）で書き、Init の dpiScale で物理 px に直す。
// ・D3D11 は BGRA サポートのハードウェア → 失敗したら WARP。D3D12 デバイスは要らない（エンジン本体と独立）。
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/SplashMotion.h"

struct ID2D1DeviceContext;   // Windows のインターフェース（グローバル名前空間）
struct IDWriteFactory;

namespace dx12e::splash
{

// 論理寸法
constexpr int kCardW = 720;
constexpr int kCardH = 420;
constexpr int kMargin = 48;                       // カードの外側（影と、退場の拡大の逃げ）
constexpr int kWindowW = kCardW + kMargin * 2;
constexpr int kWindowH = kCardH + kMargin * 2;

// 文言（毎フレーム、必要なところだけ差し替える）
struct SplashContent
{
    std::wstring title = L"Uno Engine";
    std::wstring version;                         // 例: L"v1.19.0"
    std::wstring buildInfo;                       // 例: L"2026-09-30 12:34 ビルド"
    std::wstring stepCur, stepPrev;               // 状態の文言（クロスフェード）
    double stepU = 1.0;                           // 0..1（1 = 入替完了）
    int stepIndex = 0, stepTotal = 0;             // 「手順 3 / 14」
    std::wstring tip;                             // 現在の Tips
    double tipAlpha = 0.0, tipDy = 0.0;
    std::vector<std::wstring> recents;            // 最近のプロジェクト名（最大 3。起動時のみ）
    bool projectMode = false;                     // プロジェクトを開くスプラッシュ
    std::wstring projectName, sceneName;
};

class SplashRenderer
{
public:
    SplashRenderer();
    ~SplashRenderer();
    SplashRenderer(const SplashRenderer&) = delete;
    SplashRenderer& operator=(const SplashRenderer&) = delete;

    // dpiScale: 表示倍率（1.0 = 100%）。logoPath: PNG（無くても動く）。失敗なら false（理由は LastError）。
    bool Init(float dpiScale, const std::wstring& logoPath);

    int Width() const;                            // 窓全体の物理 px
    int Height() const;

    // 1 フレーム描く。false = デバイス消失など（呼び出し側は縮退する）。
    bool Draw(const SplashFrame& frame, const SplashContent& content);

    // 直近の Draw の結果を BGRA(premultiplied) で dst へ（行間 dstPitch バイト）。false = 失敗。
    bool CopyPixels(uint8_t* dst, int dstPitch);

    const std::string& LastError() const;
    bool UsedWarp() const;                        // ソフトウェア（WARP）で動いている

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------- Graphite Gauge 用の描画器インターフェース（W1-BD3）
// 実装は SplashGaugeRenderer.{h,cpp}。classic（上の SplashRenderer）とは独立で、classic のコードは変えない。
struct GaugeFrame;      // SplashGaugeMotion.h

struct SplashStepCell
{
    std::wstring no, name;       // 「01」「コア」
    double sec = -1.0;           // 終わった工程の実測秒（< 0 = 未表示）
};

// 起動画面（Graphite Gauge）の文言と実進捗。時間に依らない値だけ（時間の絵は GaugeFrame）。
struct SplashInfo
{
    bool projectMode = false, isNew = false;
    std::wstring projectName, sceneName;
    std::wstring version;                         // 「v2.6.1」
    std::vector<std::wstring> verLines;           // 左下の版情報（1 行目 = 版とビルド、2・3 行目 = GPU・前回時間）
    std::wstring drawingNo;                       // 図番「UN-2.6.1」
    std::wstring stepTitle, stepDetail;           // 現在の工程の 1 行（s1）と補足（s2）
    std::vector<SplashStepCell> steps;            // 表示工程（5 本。プロジェクトは 4〜5 本）
    int stepCur = 0;                              // 現在の工程の添字（steps 内）
    double progress = 0.0;                        // 実進捗 0..1
    std::wstring fontFile;                        // 起動画面用の書体（Uno Splash Mono）のパス。空 / 読めなければシステム書体
};

struct ISplashRenderer
{
    virtual ~ISplashRenderer() = default;
    virtual bool Init(ID2D1DeviceContext* dc, IDWriteFactory* dw, float dpi) = 0;
    // 現在のターゲット（窓全体）へ 1 フレーム描く。静的な層はキャッシュする。false = デバイス消失など。
    virtual bool Render(const GaugeFrame& frame, const SplashInfo& info) = 0;
    virtual void OnDpiChanged(float dpi) = 0;
};

// 日本語 UI に使うフォントの選択（存在チェック + フォールバック）。テスト/診断用に公開。
// 候補を先頭から探し、最初に見つかった家族名を返す。無ければ最後の候補。
std::wstring PickFontFamily(const std::vector<std::wstring>& candidates);

} // namespace dx12e::splash
