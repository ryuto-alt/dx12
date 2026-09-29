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

// 日本語 UI に使うフォントの選択（存在チェック + フォールバック）。テスト/診断用に公開。
// 候補を先頭から探し、最初に見つかった家族名を返す。無ければ最後の候補。
std::wstring PickFontFamily(const std::vector<std::wstring>& candidates);

} // namespace dx12e::splash
