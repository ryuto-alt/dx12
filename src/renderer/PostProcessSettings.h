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

    // ── ゴッドレイ（スクリーンスペース光条。平行光源(太陽)が画面内/近くにある時のみ）──
    bool  godraysOn   = false;
    float grIntensity = 0.6f;   // 光条の強さ
    float grDensity   = 0.9f;   // 行進距離（大きいほど長い光条）
    float grDecay     = 0.96f;  // タップ毎の減衰（1に近いほど遠くまで伸びる）

    // ── レンズフレア（疑似・ブルームチェーン入力。ブルームと併用推奨）──
    bool  lensflareOn  = false;
    float lfIntensity  = 0.5f;   // 強度
    int   lfGhosts     = 4;      // ゴースト数 1..8
    float lfDispersal  = 0.35f;  // ゴースト間隔
    float lfHalo       = 0.45f;  // ハロー半径
    float lfChroma     = 0.01f;  // 色収差量

    // ── 被写界深度 DoF（gather ボケ。透視カメラのみ）──
    bool  dofOn        = false;
    float dofFocusDist = 8.0f;   // フォーカス距離（カメラからのビュー距離, m）
    float dofFocusRange = 5.0f;  // ★レガシーモード専用（dofAperture<=0 のとき）。シャープな範囲の広さ
    float dofBlurSize  = 12.0f;  // ボケ半径の上限（px。物理モードでは「絞りを開けても暴れない」ための上限）
    // ▼ 合焦をエンティティに任せる。空でなければ毎フレームその名前のエンティティまでの
    //   ビュー距離を dofFocusDist の代わりに使う（見つからなければ dofFocusDist）。
    //   ＝「被写体に合焦したまま寄る / 回る」が Lua を書かずに作れる。
    std::string dofFocusName;
    // ▼ 絞り基準の錯乱円（既定＝物理モード）。0 以下にすると旧 dofFocusRange 方式へ戻る。
    //   CoC(mm) = |z-zf|/z * f^2 / (N * (zf-f))  を 35mm 判センサ(高さ 24mm)で px 化する。
    //   ＝画面解像度に依存する dofBlurSize を「上限」へ追いやり、絵作りは F 値だけで決まる。
    float dofAperture    = 2.8f;  // F 値（小さいほど浅い）
    float dofFocalLength = 0.0f;  // 焦点距離(mm)。0 ならカメラの FOV から導出（画角と一致する）

    // ── カメラモーションブラー（深度再構成方式・velocity buffer 不要）──
    bool  motionBlurOn = false;
    float mbStrength   = 0.5f;   // シャッター係数（速度に乗算）
    int   mbSamples    = 10;     // タップ数 4..16

    // ── アンチエイリアス ──
    bool  fxaaOn       = false;  // 簡易 FXAA

    // ── 仕上げ ──
    // TPDF(三角分布)ノイズ ±0.5/255 のディザで 8bit 出力のバンディングを除去。
    // 既定 ON（見た目の副作用なし・空やビネットの縞が消える）
    bool  debandOn     = true;
};

// 「名前 → PostProcessSettings のフィールド」対応表（X マクロ）。
// Lua の post.get/post.set/post.setMany が文字列キーでアクセスするための唯一の名前表。
// 個別バインドを 90 本生やす代わりにここ 1 箇所を回して get/set/names を生成する
// ＝フィールドを足すときの修正はこの表だけで済む（MCP の set_post_process も
// 同じ名前を使うので、将来あちらもこの表から生成できる）。
// 引数は種別ごとのマクロ: B=bool / F=float / I=int / V=XMFLOAT3 / S=std::string
#define DX12E_POST_FIELDS(B, F, I, V, S)                                          \
    B(enabled) I(tonemapper)                                                      \
    B(exposureOn)   F(exposure)                                                   \
    B(contrastOn)   F(contrast)                                                   \
    B(brightnessOn) F(brightness)                                                 \
    B(saturationOn) F(saturation)                                                 \
    B(warmthOn)     F(warmth)                                                     \
    B(hueOn)        F(hueShift)                                                   \
    B(tintOn)       V(tint)                                                       \
    B(bloomOn)      F(bloom) F(bloomThreshold) F(bloomKnee) F(bloomRadius)        \
    B(vignetteOn)   F(vignette)                                                   \
    B(autoExposureOn) F(aeSpeed) F(aeEvComp) F(aeLogMin) F(aeLogMax)              \
    B(lutOn)        S(lutPath) F(lutAmount)                                       \
    B(chromaticOn)  F(chromatic)                                                  \
    B(pixelizeOn)   F(pixelSize)                                                  \
    B(posterizeOn)  I(posterize)                                                  \
    B(ditherOn)     I(ditherLevels)                                               \
    B(scanlineOn)   F(scanline)                                                   \
    B(sharpenOn)    F(sharpen)                                                    \
    B(grainOn)      F(grain)                                                      \
    B(invertOn)     F(invert)                                                     \
    B(sepiaOn)      F(sepia)                                                      \
    B(grayscaleOn)  F(grayscale)                                                  \
    B(lensOn)       F(lens)                                                       \
    B(waveOn)       F(waveAmp) F(waveFreq) F(waveSpeed)                           \
    B(radialOn)     F(radial)                                                     \
    B(glitchOn)     F(glitch)                                                     \
    B(outlineOn)    F(outline) V(outlineColor)                                    \
    B(godraysOn)    F(grIntensity) F(grDensity) F(grDecay)                        \
    B(lensflareOn)  F(lfIntensity) I(lfGhosts) F(lfDispersal) F(lfHalo) F(lfChroma) \
    B(dofOn)        F(dofFocusDist) F(dofFocusRange) F(dofBlurSize)               \
    S(dofFocusName) F(dofAperture)   F(dofFocalLength)                            \
    B(motionBlurOn) F(mbStrength) I(mbSamples)                                    \
    B(fxaaOn) B(debandOn)

// SSAOSettings 版（同じ流儀。実体は renderer/SSAOSettings.h）
#define DX12E_SSAO_FIELDS(B, F, I)                                                \
    B(enabled) F(radius) F(bias) F(intensity) F(power) I(sampleCount) B(blur)
}
