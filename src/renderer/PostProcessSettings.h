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
    // ビネットは「強度 1 個」では絵が作れなかった（半径も柔らかさも固定なので、
    // 濃くすると画面の真ん中まで一様に暗くなるだけ）。形を決める 3 つと色を分けて出す。
    bool  vignetteOn   = false;  float vignette = 0.5f;   // 減光の濃さ 0..1
    float vignetteRadius    = 0.75f;  // 減光が始まる正規化半径（大きいほど四隅だけ）
    float vignetteSoftness  = 0.45f;  // 境界のぼけ幅（小さいほどハッキリした縁）
    float vignetteRoundness = 1.0f;   // 1=真円 / 0=画面のアスペクト比なりの楕円
    DirectX::XMFLOAT3 vignetteColor{0.0f, 0.0f, 0.0f};  // 減光先の色（黒以外にもできる）

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
    bool  chromaticOn  = false;  float chromatic = 0.5f;   // 色収差（強度）
    int   chromaMode   = 0;      // 0=放射（画面端ほど強い） 1=水平 2=垂直
    bool  pixelizeOn   = false;  float pixelSize = 8.0f;   // ピクセル化（ブロックpx）
    bool  posterizeOn  = false;  int   posterize = 6;      // ポスタライズ階調 2..16
    bool  ditherOn     = false;  int   ditherLevels = 4;   // 順序ディザ階調 2..8
    // CRT は「強度」1 本に走査線の濃さ・本数・画面湾曲が全部ぶら下がっていて調整できなかった。
    bool  scanlineOn   = false;  float scanline = 0.5f;    // 走査線の濃さ 0..1
    float scanCount    = 240.0f; // 走査線の本数（画面の縦に何本引くか）
    float scanCurve    = 0.18f;  // 画面湾曲の量（0 で平面＝湾曲なし）
    bool  sharpenOn    = false;  float sharpen = 0.5f;     // シャープ
    bool  grainOn      = false;  float grain = 0.3f;       // フィルムグレイン
    float grainSize    = 1.0f;   // 粒の大きさ（px。大きいほど粗い）
    bool  grainColored = false;  // true=RGB 独立のカラーノイズ / false=輝度ノイズ

    // ── カラー操作 ──
    bool  invertOn     = false;  float invert = 1.0f;      // 色反転（強度）
    bool  sepiaOn      = false;  float sepia = 1.0f;       // セピア（強度）
    bool  grayscaleOn  = false;  float grayscale = 1.0f;   // グレースケール（強度）

    // ── 歪み ──
    // レンズ歪み / 魚眼。
    // ★旧実装は「d * (1 + k*r^2*1.5)」を【画面比を無視した UV 空間】で掛けていた。
    //   16:9 では横方向だけ強く歪む＝円が楕円に潰れ、魚眼にしようと強くするほど破綻する。
    //   さらに範囲外は端の 1 ピクセルが引き伸ばされるだけで、魚眼の「丸い像」にならなかった。
    //   今は縦横比を補正した座標で歪ませ（lensCircular）、モードで写像式そのものを選ぶ。
    bool  lensOn       = false;  float lens = 0.3f;        // 歪み量（+=樽/魚眼, -=糸巻き）
    int   lensMode     = 0;      // 0=バレル(多項式 k1/k2) 1=魚眼(等距離射影) 2=魚眼(等立体角)
    float lensK2       = 0.0f;   // 2 次の歪み係数（バレルのみ。端だけ余計に曲げる）
    float lensZoom     = 1.0f;   // 拡大補正（魚眼で四隅が空くときに上げる）
    bool  lensCircular = true;   // 縦横比を補正して「円形に」歪ませる（false=旧来の UV 空間）
    int   lensEdge     = 1;      // はみ出した所: 0=端を引き伸ばす 1=黒 2=鏡映
    float lensChroma   = 0.0f;   // 倍率色収差（歪みに比例して RGB がずれる。本物のレンズ味）

    bool  waveOn       = false;  float waveAmp = 0.01f;  float waveFreq = 12.0f;  float waveSpeed = 2.0f;  // 波ゆらぎ

    bool  radialOn     = false;  float radial = 0.3f;      // 放射ブラー（ズーム）
    int   radialSamples = 8;     // タップ数 2..32（多いほど滑らかで重い）
    float radialCenterX = 0.5f;  // 中心（0..1 のビューポート座標）
    float radialCenterY = 0.5f;

    bool  glitchOn     = false;  float glitch = 0.3f;      // デジタルグリッチ（横ずれ量）
    float glitchBlocks = 24.0f;  // 横帯の本数
    float glitchSpeed  = 12.0f;  // 崩れが差し替わる速さ（1/秒）
    float glitchColor  = 0.5f;   // RGB 分離の量（0 で色ずれ無し）

    // ── 輪郭 ──
    bool  outlineOn    = false;  float outline = 1.0f;  DirectX::XMFLOAT3 outlineColor{0.0f, 0.0f, 0.0f};  // 輪郭線
    float outlineThickness = 1.0f;   // Sobel のタップ間隔（px）＝線の太さ
    float outlineThreshold = 0.02f;  // これ未満の勾配は線にしない（暗部のノイズ止め）
    bool  outlineOnly      = false;  // true=絵を捨てて線画だけ描く（下地は outlineBg）
    DirectX::XMFLOAT3 outlineBg{1.0f, 1.0f, 1.0f};  // outlineOnly のときの下地色

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
    B(vignetteOn)   F(vignette) F(vignetteRadius) F(vignetteSoftness)             \
    F(vignetteRoundness) V(vignetteColor)                                         \
    B(autoExposureOn) F(aeSpeed) F(aeEvComp) F(aeLogMin) F(aeLogMax)              \
    B(lutOn)        S(lutPath) F(lutAmount)                                       \
    B(chromaticOn)  F(chromatic) I(chromaMode)                                    \
    B(pixelizeOn)   F(pixelSize)                                                  \
    B(posterizeOn)  I(posterize)                                                  \
    B(ditherOn)     I(ditherLevels)                                               \
    B(scanlineOn)   F(scanline) F(scanCount) F(scanCurve)                         \
    B(sharpenOn)    F(sharpen)                                                    \
    B(grainOn)      F(grain) F(grainSize) B(grainColored)                         \
    B(invertOn)     F(invert)                                                     \
    B(sepiaOn)      F(sepia)                                                      \
    B(grayscaleOn)  F(grayscale)                                                  \
    B(lensOn)       F(lens) I(lensMode) F(lensK2) F(lensZoom)                     \
    B(lensCircular) I(lensEdge) F(lensChroma)                                     \
    B(waveOn)       F(waveAmp) F(waveFreq) F(waveSpeed)                           \
    B(radialOn)     F(radial) I(radialSamples) F(radialCenterX) F(radialCenterY)  \
    B(glitchOn)     F(glitch) F(glitchBlocks) F(glitchSpeed) F(glitchColor)       \
    B(outlineOn)    F(outline) V(outlineColor)                                    \
    F(outlineThickness) F(outlineThreshold) B(outlineOnly) V(outlineBg)           \
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
