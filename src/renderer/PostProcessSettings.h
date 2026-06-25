#pragma once

#include <DirectXMath.h>

namespace dx12e
{
// ポストプロセスのパラメータ。Scene が保持し、シーン JSON に保存される。
// 各エフェクトは「有効/無効の bool」と「パラメータ」を持つ。
// エディタでは ON/OFF（チェックボックス）とパラメータ調整を別ウィンドウに分けている。
// エフェクトは既定でシーンビュー・ゲームビュー両方に適用される（previewInEditor=true 既定）。
// シーンとゲームで見た目を一致させるため。previewInEditor を OFF にするとゲームビュー限定になる。
struct PostProcessSettings
{
    bool enabled         = true;   // マスタースイッチ（false なら全エフェクト素通し）
    bool previewInEditor = true;   // 非シリアライズ。既定ON=シーンビューにもポスト適用（ゲームと一致）。OFFでゲーム限定

    // ── カラーグレーディング ──
    bool  exposureOn   = false;  float exposure   = 1.0f;   // 露出（乗算）
    bool  contrastOn   = false;  float contrast   = 1.0f;   // コントラスト
    bool  brightnessOn = false;  float brightness = 0.0f;   // 明るさ（加算 -0.5..0.5）
    bool  saturationOn = false;  float saturation = 1.0f;   // 彩度
    bool  warmthOn     = false;  float warmth     = 0.0f;   // 色温度 -1..1
    bool  hueOn        = false;  float hueShift   = 0.0f;   // 色相回転（度 0..360）
    bool  tintOn       = false;  DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};  // 色味の乗算

    // ── ブルーム / ビネット ──
    bool  bloomOn      = false;  float bloom = 0.5f;  float bloomThreshold = 0.75f;
    bool  vignetteOn   = false;  float vignette = 0.5f;

    // ── スタイライズ（godotshaders.com 由来） ──
    bool  chromaticOn  = false;  float chromatic = 0.5f;   // 色収差
    bool  pixelizeOn   = false;  float pixelSize = 8.0f;   // ピクセル化（ブロックpx）
    bool  posterizeOn  = false;  int   posterize = 6;      // ポスタライズ階調 2..16
    bool  ditherOn     = false;  int   ditherLevels = 4;   // 順序ディザ階調 2..8
    bool  scanlineOn   = false;  float scanline = 0.5f;    // CRT 走査線＋湾曲
    bool  sharpenOn    = false;  float sharpen = 0.5f;     // シャープ
    bool  grainOn      = false;  float grain = 0.3f;       // フィルムグレイン

    // ── カラー操作 ──
    bool  invertOn     = false;  float invert = 1.0f;      // 色反転（強度）
    bool  sepiaOn      = false;  float sepia = 1.0f;       // セピア（強度）
    bool  grayscaleOn  = false;  float grayscale = 1.0f;   // グレースケール（強度）

    // ── 歪み ──
    bool  lensOn       = false;  float lens = 0.3f;        // レンズ歪み（バレル）
    bool  waveOn       = false;  float waveAmp = 0.01f;  float waveFreq = 12.0f;  float waveSpeed = 2.0f;  // 波ゆらぎ
    bool  radialOn     = false;  float radial = 0.3f;      // 放射ブラー（ズーム）
    bool  glitchOn     = false;  float glitch = 0.3f;      // デジタルグリッチ

    // ── 輪郭 ──
    bool  outlineOn    = false;  float outline = 1.0f;  DirectX::XMFLOAT3 outlineColor{0.0f, 0.0f, 0.0f};  // 輪郭線

    // ── アンチエイリアス ──
    bool  fxaaOn       = false;  // 簡易 FXAA
};
}
