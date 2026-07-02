#pragma once

#include <DirectXMath.h>
#include <string>

namespace dx12e
{
// ポストプロセスのパラメータ。Scene が保持し、シーン JSON に保存される。
// 各エフェクトは「有効/無効の bool」と「パラメータ」を持つ。
// エディタでは ON/OFF（チェックボックス）とパラメータ調整を別ウィンドウに分けている。
// エフェクトはシーンビュー・ゲームビュー両方に同じ設定で適用される。
// 編集中と Play 中で見た目を一致させ、ライティング調整をそのままゲームへ反映するため。
struct PostProcessSettings
{
    bool enabled         = true;   // マスタースイッチ（false なら全エフェクト素通し）
    bool previewInEditor = true;   // 旧設定との互換用。現在は常に Scene/Game へ同じポスト設定を適用する。

    // ── 表示変換（トーンマップ）──
    // マスターOFF でも常に適用される（シーンRT はリニアHDRなので表示変換は必須）。
    // 0=ACES（コントラスト強め・定番） 1=AgX（高輝度・高彩度光源の色割れがない/Blender4採用）
    // 2=なし（ガンマのみ。デバッグ/2D向け）
    int tonemapper = 0;

    // ── カラーグレーディング ──
    bool  exposureOn   = false;  float exposure   = 1.0f;   // 露出（乗算）
    bool  contrastOn   = false;  float contrast   = 1.0f;   // コントラスト
    bool  brightnessOn = false;  float brightness = 0.0f;   // 明るさ（加算 -0.5..0.5）
    bool  saturationOn = false;  float saturation = 1.0f;   // 彩度
    bool  warmthOn     = false;  float warmth     = 0.0f;   // 色温度 -1..1
    bool  hueOn        = false;  float hueShift   = 0.0f;   // 色相回転（度 0..360）
    bool  tintOn       = false;  DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};  // 色味の乗算

    // ── ブルーム / ビネット ──
    // ブルームは CoD:AW 方式のダウンサンプル/アップサンプルチェーン（BloomPass）。
    // bloom=合成強度 / bloomThreshold=抽出しきい値(リニアHDR輝度、1.0=白の輝度)
    // bloomKnee=しきい値のソフト肩 / bloomRadius=アップサンプル合成率(大きいほど広く柔らかい)
    bool  bloomOn      = false;  float bloom = 0.4f;  float bloomThreshold = 1.0f;
    float bloomKnee    = 0.5f;   float bloomRadius = 0.65f;
    bool  vignetteOn   = false;  float vignette = 0.5f;

    // ── 自動露出（eye adaptation）──
    // compute のヒストグラムで平均輝度を測り、露出を時間適応で追従させる。
    // aeSpeed=適応速度(1/秒) / aeEvComp=EV補正(+で明るく) / aeLogMin..Max=測光レンジ(log2輝度)
    bool  autoExposureOn = false;
    float aeSpeed   = 2.0f;
    float aeEvComp  = 0.0f;
    float aeLogMin  = -8.0f;
    float aeLogMax  = 4.0f;

    // ── 3D LUT カラーグレーディング（ストリップ形式 N*N x N、例: 1024x32）──
    // トーンマップ後の LDR に適用。lutPath は assets 基準の相対パス。
    bool        lutOn = false;
    std::string lutPath;
    float       lutAmount = 1.0f;

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

    // ── 仕上げ ──
    // TPDF(三角分布)ノイズ ±0.5/255 のディザで 8bit 出力のバンディングを除去。
    // 既定 ON（見た目の副作用なし・空やビネットの縞が消える）
    bool  debandOn     = true;
};
}
