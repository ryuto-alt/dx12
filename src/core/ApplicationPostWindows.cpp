// ===========================================================================
// Application: 設定窓（ポストプロセス / Skybox / SSAO / SSR・SSGI / ボリュメトリックフォグ）
// ---------------------------------------------------------------------------
// ApplicationRender.cpp の Render() に直書きだったものを、フェーズ 1b で移設して pg:: の
// 2 カラム（ラベル左 / 値右・ゼブラ・tip）へ揃えた（インスペクタと同じ見た目）。
// ★窓の名前（ImGui::Begin の文字列）・値の範囲・書式・既定値・リセットの挙動は移設前と同じ
//   （UI 自動テスト / ドック配置 / MCP がこの名前に依存する）。
// ★ポストプロセスの値は Undo 対象外（dirty は settingsHash の検知）。ここでも Undo は積まない。
// ★pg:: の行 ID はラベル文字列（PushID(label)）なので、同じ表の中でラベルを重複させないこと
//   （旧コードの "##suffix" は不要になった）。
// ===========================================================================
#include "core/ApplicationInternal.h"
#include "editor/PropertyGrid.h"     // pg:: 2 カラムのプロパティ行
#include "editor/UiWidgets.h"
#include "editor/EditorTheme.h"
#include "editor/PostPresets.h"
#include "editor/PostPresetSwatch.h"
#include "editor/AssetDrop.h"
#include "core/PathResolver.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <string>

namespace dx12e
{
using namespace appdetail;

namespace
{

// "\0" 区切りのコンボ（pg::Combo は配列版だけなので、同じ流儀の行を足す）。
bool PgCombo(const char* label, int* idx, const char* zeroSeparated, const char* tip = nullptr)
{
    pg::Label(label, tip);
    ImGui::PushID(label);
    const bool ch = ui::Combo("##v", idx, zeroSeparated);
    ImGui::PopID();
    return ch;
}

} // namespace

void Application::RenderPostProcessWindows()
{
    if (!m_scene) return;

    // ---- ポストプロセス: ON/OFF ＋ パラメータ（1 枚で完結する窓）----
    // ★以前は「チェックを入れる窓」と「値をいじる窓」が別々で、パラメータ窓を
    //   開いていない限り何も調整できなかった（＝「パラメータが全然いじれない」の正体）。
    //   今は Post Process 窓の中に、有効なエフェクトのパラメータがその場で出る。
    //   従来のパラメータ専用窓も残してある（別ドッキングで広く使いたい人向け）。
    auto& pp  = m_scene->GetPostSettings();
    auto& taa = m_scene->GetTaaSettings();   // TAA は PostProcessSettings とは別（下の注記参照）
    static const PostProcessSettings kDef{};  // 「このエフェクトだけ既定へ戻す」用

    // 全エフェクトのメタ情報（トグル・パラメータ描画・個別リセットを一元定義）。
    // params は pg::Begin ～ pg::End の内側で呼ばれる（行は pg:: で描く）。
    struct PostFx {
        const char* cat;                 // カテゴリ見出し
        const char* label;               // 表示名
        const char* help;                // 説明（null可）
        bool*       on;                  // 有効フラグ
        std::function<void()> params;    // パラメータ描画
        std::function<void()> reset;     // このエフェクトのパラメータだけ既定へ
    };
    const std::vector<PostFx> fx = {
        {"カラー", "露出 Exposure", "明るさを乗算で調整", &pp.exposureOn,
            [&]{ pg::SliderFloat("値", &pp.exposure, 0.0f, 8.0f, "%.3f"); },
            [&]{ pp.exposure = kDef.exposure; }},
        {"カラー", "自動露出 Auto Exposure", "平均輝度に合わせて露出を自動追従（目の順応）", &pp.autoExposureOn,
            [&]{ pg::SliderFloat("適応速度", &pp.aeSpeed, 0.1f, 10.0f, "%.2f");
                 pg::SliderFloat("EV補正", &pp.aeEvComp, -8.0f, 8.0f, "%.2f");
                 pg::SliderFloat("測光下限(log2)", &pp.aeLogMin, -16.0f, 0.0f, "%.1f");
                 pg::SliderFloat("測光上限(log2)", &pp.aeLogMax, 0.0f, 16.0f, "%.1f"); },
            [&]{ pp.aeSpeed = kDef.aeSpeed; pp.aeEvComp = kDef.aeEvComp;
                 pp.aeLogMin = kDef.aeLogMin; pp.aeLogMax = kDef.aeLogMax; }},
        {"カラー", "コントラスト Contrast", nullptr, &pp.contrastOn,
            [&]{ pg::SliderFloat("値", &pp.contrast, 0.0f, 3.0f, "%.3f"); },
            [&]{ pp.contrast = kDef.contrast; }},
        {"カラー", "明るさ Brightness", "加算で明暗を調整", &pp.brightnessOn,
            [&]{ pg::SliderFloat("値", &pp.brightness, -1.0f, 1.0f, "%.3f"); },
            [&]{ pp.brightness = kDef.brightness; }},
        {"カラー", "彩度 Saturation", nullptr, &pp.saturationOn,
            [&]{ pg::SliderFloat("値", &pp.saturation, 0.0f, 3.0f, "%.3f"); },
            [&]{ pp.saturation = kDef.saturation; }},
        {"カラー", "色温度 Warmth", "+で暖色、-で寒色", &pp.warmthOn,
            [&]{ pg::SliderFloat("値", &pp.warmth, -1.0f, 1.0f, "%.3f"); },
            [&]{ pp.warmth = kDef.warmth; }},
        {"カラー", "色相回転 Hue", "色相を回す（度）", &pp.hueOn,
            [&]{ pg::SliderFloat("角度", &pp.hueShift, 0.0f, 360.0f, "%.1f°"); },
            [&]{ pp.hueShift = kDef.hueShift; }},
        {"カラー", "色味 Tint", "RGB を乗算", &pp.tintOn,
            [&]{ pg::Color3("色", &pp.tint.x); },
            [&]{ pp.tint = kDef.tint; }},

        {"ブルーム/ビネット", "ブルーム Bloom", "明部が咲く（物理ベース・ダウンサンプルチェーン）", &pp.bloomOn,
            [&]{ pg::SliderFloat("強度", &pp.bloom, 0.0f, 3.0f, "%.3f");
                 pg::SliderFloat("しきい値", &pp.bloomThreshold, 0.0f, 8.0f, "%.3f");
                 pg::SliderFloat("ニー(肩)", &pp.bloomKnee, 0.0f, 1.0f, "%.3f");
                 pg::SliderFloat("広がり", &pp.bloomRadius, 0.05f, 0.95f, "%.3f"); },
            [&]{ pp.bloom = kDef.bloom; pp.bloomThreshold = kDef.bloomThreshold;
                 pp.bloomKnee = kDef.bloomKnee; pp.bloomRadius = kDef.bloomRadius; }},
        {"ブルーム/ビネット", "ビネット Vignette", "周辺減光。半径・柔らかさ・真円度・色まで作れる", &pp.vignetteOn,
            [&]{ pg::SliderFloat("濃さ", &pp.vignette, 0.0f, 1.0f, "%.3f");
                 pg::SliderFloat("開始半径", &pp.vignetteRadius, 0.0f, 1.5f, "%.3f", nullptr,
                                 "中心=0 / 四隅=1。上げるほど四隅だけが落ちる");
                 pg::SliderFloat("ぼけ幅", &pp.vignetteSoftness, 0.001f, 1.0f, "%.3f");
                 pg::SliderFloat("真円度", &pp.vignetteRoundness, 0.0f, 1.0f, "%.3f", nullptr,
                                 "1=真円 / 0=画面のアスペクト比なりの楕円");
                 pg::Color3("減光の色", &pp.vignetteColor.x); },
            [&]{ pp.vignette = kDef.vignette; pp.vignetteRadius = kDef.vignetteRadius;
                 pp.vignetteSoftness = kDef.vignetteSoftness;
                 pp.vignetteRoundness = kDef.vignetteRoundness;
                 pp.vignetteColor = kDef.vignetteColor; }},

        {"ライト/カメラ", "ゴッドレイ God Rays", "太陽(平行光源)からの光条。太陽が画面内/近くにある時に見える(透視カメラのみ)", &pp.godraysOn,
            [&]{ pg::SliderFloat("強度", &pp.grIntensity, 0.0f, 3.0f, "%.3f");
                 pg::SliderFloat("長さ", &pp.grDensity, 0.1f, 1.0f, "%.3f");
                 pg::SliderFloat("減衰", &pp.grDecay, 0.8f, 0.999f, "%.4f"); },
            [&]{ pp.grIntensity = kDef.grIntensity; pp.grDensity = kDef.grDensity;
                 pp.grDecay = kDef.grDecay; }},
        {"ライト/カメラ", "レンズフレア Lens Flare", "ゴースト+ハロー。強い光源があると出る(ブルームと入力共有)", &pp.lensflareOn,
            [&]{ pg::SliderFloat("強度", &pp.lfIntensity, 0.0f, 3.0f, "%.3f");
                 pg::SliderInt("ゴースト数", &pp.lfGhosts, 1, 8);
                 pg::SliderFloat("間隔", &pp.lfDispersal, 0.05f, 1.0f, "%.3f");
                 pg::SliderFloat("ハロー", &pp.lfHalo, 0.0f, 1.0f, "%.3f");
                 pg::SliderFloat("色収差", &pp.lfChroma, 0.0f, 0.1f, "%.4f"); },
            [&]{ pp.lfIntensity = kDef.lfIntensity; pp.lfGhosts = kDef.lfGhosts;
                 pp.lfDispersal = kDef.lfDispersal; pp.lfHalo = kDef.lfHalo;
                 pp.lfChroma = kDef.lfChroma; }},
        {"ライト/カメラ", "被写界深度 DoF", "フォーカス距離の前後がボケる(透視カメラのみ)", &pp.dofOn,
            [&]{ pg::SliderFloat("フォーカス距離", &pp.dofFocusDist, 0.1f, 500.0f, "%.2f");
                 // 合焦をエンティティに任せる（空なら上のフォーカス距離）
                 {
                     char buf[128]{};
                     std::snprintf(buf, sizeof(buf), "%s", pp.dofFocusName.c_str());
                     if (pg::InputText("合焦エンティティ", buf, sizeof(buf), 0, nullptr,
                                       "空ならフォーカス距離を使う"))
                         pp.dofFocusName = buf;
                 }
                 pg::SliderFloat("F値(0でレガシー)", &pp.dofAperture, 0.0f, 32.0f, "%.2f");
                 pg::SliderFloat("焦点距離mm(0=画角)", &pp.dofFocalLength, 0.0f, 400.0f, "%.0f");
                 if (pp.dofAperture <= 0.0f)
                     pg::SliderFloat("シャープ範囲", &pp.dofFocusRange, 0.1f, 100.0f, "%.2f");
                 pg::SliderFloat("最大ボケpx", &pp.dofBlurSize, 1.0f, 96.0f, "%.1f"); },
            [&]{ pp.dofFocusDist = kDef.dofFocusDist; pp.dofFocusName.clear();
                 pp.dofAperture = kDef.dofAperture; pp.dofFocalLength = kDef.dofFocalLength;
                 pp.dofFocusRange = kDef.dofFocusRange; pp.dofBlurSize = kDef.dofBlurSize; }},
        {"ライト/カメラ", "モーションブラー Motion Blur", "カメラの動きで残像(深度再構成方式・透視カメラのみ)", &pp.motionBlurOn,
            [&]{ pg::SliderFloat("強度", &pp.mbStrength, 0.0f, 3.0f, "%.3f");
                 pg::SliderInt("サンプル数", &pp.mbSamples, 4, 16); },
            [&]{ pp.mbStrength = kDef.mbStrength; pp.mbSamples = kDef.mbSamples; }},

        {"スタイライズ", "色収差 Chromatic", "RGB をずらす。放射(端ほど強い)/水平/垂直を選べる", &pp.chromaticOn,
            [&]{ pg::SliderFloat("強度", &pp.chromatic, 0.0f, 2.0f, "%.3f");
                 PgCombo("ずらし方", &pp.chromaMode, "放射（画面端ほど強い）\0水平\0垂直\0"); },
            [&]{ pp.chromatic = kDef.chromatic; pp.chromaMode = kDef.chromaMode; }},
        {"スタイライズ", "ピクセル化 Pixelize", "ブロック状にモザイク", &pp.pixelizeOn,
            [&]{ pg::SliderFloat("ブロックpx", &pp.pixelSize, 1.0f, 128.0f, "%.1f"); },
            [&]{ pp.pixelSize = kDef.pixelSize; }},
        {"スタイライズ", "ポスタライズ Posterize", "色数を段階化", &pp.posterizeOn,
            [&]{ pg::SliderInt("階調", &pp.posterize, 2, 32); },
            [&]{ pp.posterize = kDef.posterize; }},
        {"スタイライズ", "ディザ Dither", "順序ディザで階調化", &pp.ditherOn,
            [&]{ pg::SliderInt("階調", &pp.ditherLevels, 2, 16); },
            [&]{ pp.ditherLevels = kDef.ditherLevels; }},
        {"スタイライズ", "CRT走査線 Scanline", "走査線の濃さ・本数・画面湾曲をそれぞれ調整できる", &pp.scanlineOn,
            [&]{ pg::SliderFloat("濃さ", &pp.scanline, 0.0f, 1.0f, "%.3f");
                 pg::SliderFloat("本数", &pp.scanCount, 20.0f, 1080.0f, "%.0f");
                 pg::SliderFloat("画面湾曲", &pp.scanCurve, 0.0f, 1.0f, "%.3f", nullptr, "0 で平面（湾曲なし）"); },
            [&]{ pp.scanline = kDef.scanline; pp.scanCount = kDef.scanCount;
                 pp.scanCurve = kDef.scanCurve; }},
        {"スタイライズ", "シャープ Sharpen", "輪郭を強調", &pp.sharpenOn,
            [&]{ pg::SliderFloat("強度", &pp.sharpen, 0.0f, 3.0f, "%.3f"); },
            [&]{ pp.sharpen = kDef.sharpen; }},
        {"スタイライズ", "フィルムグレイン Grain", "ザラつきノイズ。粒の大きさとカラー/輝度を選べる", &pp.grainOn,
            [&]{ pg::SliderFloat("強度", &pp.grain, 0.0f, 2.0f, "%.3f");
                 pg::SliderFloat("粒の大きさpx", &pp.grainSize, 0.25f, 16.0f, "%.2f");
                 pg::Checkbox("カラーノイズ", &pp.grainColored); },
            [&]{ pp.grain = kDef.grain; pp.grainSize = kDef.grainSize;
                 pp.grainColored = kDef.grainColored; }},

        {"カラー操作", "色反転 Invert", nullptr, &pp.invertOn,
            [&]{ pg::SliderFloat("強度", &pp.invert, 0.0f, 1.0f, "%.3f"); },
            [&]{ pp.invert = kDef.invert; }},
        {"カラー操作", "セピア Sepia", nullptr, &pp.sepiaOn,
            [&]{ pg::SliderFloat("強度", &pp.sepia, 0.0f, 1.0f, "%.3f"); },
            [&]{ pp.sepia = kDef.sepia; }},
        {"カラー操作", "グレースケール Grayscale", nullptr, &pp.grayscaleOn,
            [&]{ pg::SliderFloat("強度", &pp.grayscale, 0.0f, 1.0f, "%.3f"); },
            [&]{ pp.grayscale = kDef.grayscale; }},
        {"カラー操作", "LUT グレーディング", "ストリップ画像(N*N x N, 例:1024x32)で色変換。Photoshop等で作った LUT を適用", &pp.lutOn,
            [&]{ static char lutBuf[260] = "";
                 pg::Label("LUT 画像", "assets からの相対パス（例: luts/warm.png）。アセットブラウザから画像をドロップしても指定できます");
                 ImGui::PushID("lutpath");
                 ui::InputTextWithHint("##lutpath", "luts/warm.png", lutBuf, sizeof(lutBuf));
                 if (ImGui::IsItemDeactivatedAfterEdit()) pp.lutPath = lutBuf;
                 if (!ImGui::IsItemActive() && pp.lutPath != lutBuf)
                 {
                     size_t n = pp.lutPath.size();
                     if (n >= sizeof(lutBuf)) n = sizeof(lutBuf) - 1;
                     std::memcpy(lutBuf, pp.lutPath.c_str(), n);
                     lutBuf[n] = '\0';
                 }
                 // アセットブラウザからの D&D（画像を落とすだけで LUT が刺さる）
                 {
                     std::string dropped;
                     if (assetdrop::Accept(dropped, PathResolver::AssetsDir(),
                                           {".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp"}))
                         pp.lutPath = dropped;   // 入力欄は次フレームの同期処理が追従する
                 }
                 ImGui::PopID();
                 pg::SliderFloat("適用量", &pp.lutAmount, 0.0f, 1.0f, "%.3f"); },
            [&]{ pp.lutPath.clear(); pp.lutAmount = kDef.lutAmount; }},

        {"歪み", "レンズ歪み / 魚眼 Lens", "バレル・糸巻き・魚眼。縦横比を補正するので円が楕円にならない", &pp.lensOn,
            [&]{ PgCombo("種類", &pp.lensMode,
                         "バレル / 糸巻き（多項式）\0魚眼（等距離射影）\0魚眼（等立体角射影）\0");
                 pg::SliderFloat("歪み量", &pp.lens, -1.0f, 1.0f, "%.3f", nullptr, "+ = 樽 / 魚眼、- = 糸巻き");
                 if (pp.lensMode == 0)
                     pg::SliderFloat("2次係数", &pp.lensK2, -1.0f, 1.0f, "%.3f");
                 pg::SliderFloat("ズーム補正", &pp.lensZoom, 0.2f, 3.0f, "%.3f", nullptr, "四隅が空くときに上げる");
                 pg::SliderFloat("倍率色収差", &pp.lensChroma, 0.0f, 2.0f, "%.3f");
                 pg::Checkbox("円形に歪ませる", &pp.lensCircular, "縦横比を補正して、円が楕円にならないようにする");
                 PgCombo("はみ出した所", &pp.lensEdge,
                         "端の色を引き伸ばす\0黒で塗る\0鏡のように折り返す\0"); },
            [&]{ pp.lens = kDef.lens; pp.lensMode = kDef.lensMode; pp.lensK2 = kDef.lensK2;
                 pp.lensZoom = kDef.lensZoom; pp.lensCircular = kDef.lensCircular;
                 pp.lensEdge = kDef.lensEdge; pp.lensChroma = kDef.lensChroma; }},
        {"歪み", "波ゆらぎ Wave", "水中/陽炎のゆれ", &pp.waveOn,
            [&]{ pg::SliderFloat("振幅", &pp.waveAmp, 0.0f, 0.1f, "%.4f");
                 pg::SliderFloat("周波数", &pp.waveFreq, 1.0f, 80.0f, "%.2f");
                 pg::SliderFloat("速度", &pp.waveSpeed, 0.0f, 16.0f, "%.2f"); },
            [&]{ pp.waveAmp = kDef.waveAmp; pp.waveFreq = kDef.waveFreq;
                 pp.waveSpeed = kDef.waveSpeed; }},
        {"歪み", "放射ブラー Radial", "指定した中心へズームブラー", &pp.radialOn,
            [&]{ pg::SliderFloat("強度", &pp.radial, 0.0f, 2.0f, "%.3f");
                 pg::SliderInt("サンプル数", &pp.radialSamples, 2, 32);
                 pg::SliderFloat("中心X", &pp.radialCenterX, 0.0f, 1.0f, "%.3f");
                 pg::SliderFloat("中心Y", &pp.radialCenterY, 0.0f, 1.0f, "%.3f"); },
            [&]{ pp.radial = kDef.radial; pp.radialSamples = kDef.radialSamples;
                 pp.radialCenterX = kDef.radialCenterX; pp.radialCenterY = kDef.radialCenterY; }},
        {"歪み", "グリッチ Glitch", "デジタル乱れ。帯の本数・速さ・RGB分離を調整できる", &pp.glitchOn,
            [&]{ pg::SliderFloat("横ずれ量", &pp.glitch, 0.0f, 2.0f, "%.3f");
                 pg::SliderFloat("帯の本数", &pp.glitchBlocks, 2.0f, 200.0f, "%.0f");
                 pg::SliderFloat("速さ", &pp.glitchSpeed, 0.0f, 60.0f, "%.2f");
                 pg::SliderFloat("RGB分離", &pp.glitchColor, 0.0f, 2.0f, "%.3f"); },
            [&]{ pp.glitch = kDef.glitch; pp.glitchBlocks = kDef.glitchBlocks;
                 pp.glitchSpeed = kDef.glitchSpeed; pp.glitchColor = kDef.glitchColor; }},

        {"輪郭", "輪郭線 Outline", "Sobelエッジ検出。線画モードで下地を塗り潰せる", &pp.outlineOn,
            [&]{ pg::SliderFloat("強度", &pp.outline, 0.0f, 8.0f, "%.3f");
                 pg::SliderFloat("太さpx", &pp.outlineThickness, 0.1f, 8.0f, "%.2f");
                 pg::SliderFloat("しきい値", &pp.outlineThreshold, 0.0f, 1.0f, "%.4f", nullptr,
                                 "これ未満の勾配は線にしない（暗部のノイズ止め）");
                 pg::Color3("線の色", &pp.outlineColor.x);
                 pg::Checkbox("線画モード", &pp.outlineOnly, "絵を捨てて、線だけを描く");
                 if (pp.outlineOnly)
                     pg::Color3("下地の色", &pp.outlineBg.x); },
            [&]{ pp.outline = kDef.outline; pp.outlineThickness = kDef.outlineThickness;
                 pp.outlineThreshold = kDef.outlineThreshold; pp.outlineColor = kDef.outlineColor;
                 pp.outlineOnly = kDef.outlineOnly; pp.outlineBg = kDef.outlineBg; }},

        {"アンチエイリアス", "FXAA", "簡易アンチエイリアス（TAA が有効なら無視されます）", &pp.fxaaOn, {}, {}},
        // TAA は PostProcessSettings ではなく TaaSettings（シーン単位の独立設定）に住む。
        // uber パスの「マスク付きエフェクト」ではなく、チェーンの構造そのものを変える
        // （深度+速度プリパスの強制・投影行列のジッタ・FXAA 排他）ため。
        {"アンチエイリアス", "TAA (テンポラル)",
         "速度バッファ + 前フレームの履歴でサブピクセル AA。動くものもぼけません。"
         "有効にすると FXAA は自動で無視されます（透視ビューのみ。2D 正射では無効）",
         &taa.enabled,
            [&]{ int sc = (taa.sampleCount <= 4) ? 0 : (taa.sampleCount >= 16 ? 2 : 1);
                 if (PgCombo("ジッタ数", &sc, "4 (シャープ)\0" "8 (標準)\0" "16 (滑らか)\0"))
                     taa.sampleCount = (sc == 0) ? 4 : (sc == 2 ? 16 : 8);
                 pg::SliderFloat("ジッタ量", &taa.jitterScale, 0.0f, 1.0f, "%.3f", nullptr,
                                 "1.0 = ±0.5px。ブラーが強すぎるなら下げる");
                 pg::SliderFloat("履歴 最小", &taa.feedbackMin, 0.5f, 0.98f, "%.3f", nullptr,
                                 "現フレームと食い違うピクセルで使う履歴の比率");
                 pg::SliderFloat("履歴 最大", &taa.feedbackMax, 0.5f, 0.995f, "%.3f", nullptr,
                                 "安定しているピクセルで使う履歴の比率。高いほど滑らかだがゴーストしやすい");
                 pg::SliderFloat("クリップ幅", &taa.varianceGamma, 0.25f, 3.0f, "%.3f", nullptr,
                                 "近傍色の許容幅 (μ±γσ)。下げるとゴーストが減りチラつきが増える");
                 pg::Checkbox("速度バッファを可視化", &taa.debugVelocity, "静止時に全面が均一なグレーになるのが正常"); },
            [&]{ TaaSettings d{}; bool wasOn = taa.enabled; taa = d; taa.enabled = wasOn; }},

        {"仕上げ", "デバンディング Deband", "TPDFディザで空/ビネットの縞(バンディング)を除去", &pp.debandOn, {}, {}},
    };

    // 有効中エフェクト数（両窓で使うので、窓の表示有無に関わらず先に数える）
    int enabledCount = 0;
    for (const auto& fEff : fx)
        if (*fEff.on) ++enabledCount;

    // エフェクトのパラメータ群を pg:: の表として描く（表 ID はエフェクトごとに固有＝呼び出し側の PushID が効く）。
    auto drawParams = [](const PostFx& f)
    {
        if (pg::Begin("##fxparams"))
        {
            f.params();
            pg::End();
        }
    };

    // エフェクト 1 件を「チェック + (?) + ↺ + その場のパラメータ」で描く。
    // filter が空でなければ表示名に含まれるものだけ出す（エフェクトが 30 個近くあるので）。
    static char postFilter[64] = "";
    auto matchesFilter = [&](const PostFx& f) -> bool
    {
        if (postFilter[0] == '\0') return true;
        std::string hay = std::string(f.cat) + " " + f.label + " " + (f.help ? f.help : "");
        std::string needle = postFilter;
        auto lower = [](std::string& s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        lower(hay); lower(needle);
        return hay.find(needle) != std::string::npos;
    };

    // ===== ウィンドウ1: エフェクト一覧（チェック＋その場でパラメータ）=====
    if (m_editorCtx->showPostProcess)
    {
    ImGui::Begin("Post Process");
    // ★Play 中の変更は Stop で捨てられる（Stop は Play 開始時のシーン JSON から
    //   丸ごと復元する）。以前はこの窓も MCP のセッターも Play 中に素通しで、
    //   警告もタイトルの * も出ないまま、詰めた露出やブルームが黙って巻き戻っていた。
    if (m_engineMode == EngineMode::Playing)
        ImGui::TextColored(theme::Warn,
            "Play 中の変更は Stop で破棄されます（残すなら Stop してから調整）");
    // マスター ON/OFF とトーンマップ。トーンマップ（表示変換）はマスターOFF でも常に適用されるのでディセーブル外。
    if (pg::Begin("##ppmaster"))
    {
        pg::Checkbox("有効（マスター）", &pp.enabled);
        PgCombo("トーンマップ", &pp.tonemapper, "ACES\0AgX\0なし(ガンマのみ)\0UE Filmic\0線形(クリップのみ)\0Khronos PBR Neutral\0",
                "ACES: コントラスト強めの定番\nAgX: 高輝度・高彩度光源(ネオン/発光体)の色割れがない\nなし: ガンマのみ(デバッグ/2D向け)\n"
                "UE Filmic: UE 5 の既定(ACES 系)。比較・校正用（sRGB 出力）\n線形: トーンマップ無し。1 でクリップするだけ（比較用）\n"
                "Khronos PBR Neutral: 色相を保つ glTF 標準ビューア用");
        if (pp.tonemapper == 3)
        {
            pg::SliderFloat("Slope", &pp.filmSlope, 0.3f, 1.5f, "%.3f");
            pg::SliderFloat("Toe", &pp.filmToe, 0.0f, 1.0f, "%.3f");
            pg::SliderFloat("Shoulder", &pp.filmShoulder, 0.0f, 1.0f, "%.3f");
            pg::SliderFloat("Black Clip", &pp.filmBlackClip, 0.0f, 1.0f, "%.3f");
            pg::SliderFloat("White Clip", &pp.filmWhiteClip, 0.0f, 1.0f, "%.3f");
        }
        // Q2: 露出モードとライティング単位。既定（従来 / 従来）では絵は変わらない。マスターが OFF でも 手動/自動 EV100 は効く。
        PgCombo("露出モード", &pp.exposureMode, "従来(乗算 / 自動露出)\0手動 EV100\0自動(EV100 上下限)\0",
                "従来: 下の「露出」の乗算と「自動露出」\n手動 EV100: 係数 = 1/(1.2·2^(EV100−補正))。EV100=15（晴天）+ 補正 0 が露出 0 の基準\n"
                "自動: ヒストグラムで平均輝度を 18% グレーへ。上下限は EV100 で指定");
        if (pp.exposureMode == 1)
        {
            pg::SliderFloat("EV100", &pp.ev100, -6.0f, 24.0f, "%.2f", nullptr, "1 上げるごとに 1 段暗くなる。晴天(太陽 10 万 lux)= 15、屋内の照明 = 5〜8 が目安");
            pg::SliderFloat("露出補正 EV", &pp.evComp, -6.0f, 6.0f, "%.2f");
        }
        else if (pp.exposureMode == 2)
        {
            pg::SliderFloat("露出補正 EV", &pp.evComp, -6.0f, 6.0f, "%.2f");
            pg::SliderFloat("EV100 下限", &pp.aeMinEv100, -12.0f, 24.0f, "%.1f");
            pg::SliderFloat("EV100 上限", &pp.aeMaxEv100, -12.0f, 24.0f, "%.1f");
            pg::SliderFloat("適応速度", &pp.aeSpeed, 0.1f, 10.0f, "%.2f");
            pg::SliderFloat("明るくなる方向の速度", &pp.aeSpeedUp, 0.0f, 10.0f, "%.2f", nullptr, "0 = 適応速度と同じ");
            pg::SliderFloat("暗くなる方向の速度", &pp.aeSpeedDown, 0.0f, 10.0f, "%.2f", nullptr, "0 = 適応速度と同じ");
            pg::SliderFloat("測光: 下側の除外", &pp.aeLowPercent, 0.0f, 0.99f, "%.3f", nullptr, "0..1 = 平均輝度 / 0.8..0.983 = UE のヒストグラム測光の既定");
            pg::SliderFloat("測光: 上側の上限", &pp.aeHighPercent, 0.01f, 1.0f, "%.3f");
        }
        PgCombo("ライティング単位", &pp.lightingUnits, "従来\0物理(lux / cd / nit)\0",
                "従来: 点/スポットは saturate(1−d/range)^2・強度は任意単位\n"
                "物理: 太陽 = lux / 点・スポット = cd の逆二乗（光源の半径 sourceRadius と影響半径 range の窓つき）/ 空・IBL・自己発光 = nit。"
                "シーン RT の 1.0 = 1 nit なので、露出モードを 手動 EV100 か 自動 にして表示へ変換する。切替は一瞬止まる");
        pg::End();
    }

    // ---- 見た目プリセット（複数選んで重ねられる。サムネイル付き）----
    // ★以前は「文字のボタンを 1 個押すと丸ごと置き換わる」だけだったので、
    //   (a) 押してみるまでどんな絵になるか分からない
    //   (b) シネマ + グリッチ のような組み合わせが作れない（後勝ちで消える）
    //   の 2 つが不便だった。ここではトグル選択にして、選ばれたものを
    //   毎回「既定へ戻す → 表の並び順に適用」で作り直す（＝外した分が残らない）。
    static PostProcessSettings presetBackup{};
    static bool                hasPresetBackup = false;
    static bool                presetSel[kPostPresetCount] = {};
    static u64                 presetSceneGen = ~0ull;
    // シーンを開き直したら選択も控えも捨てる
    //（別のシーンで取った控えを「戻す」で流し込むと、無関係な設定が復活する）
    if (presetSceneGen != static_cast<u64>(m_sceneGeneration))
    {
        presetSceneGen  = static_cast<u64>(m_sceneGeneration);
        hasPresetBackup = false;
        for (bool& s : presetSel) s = false;
    }

    if (ui::CollapsingHeader("見た目プリセット", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // ★窓を右へドッキングすると幅が狭いので、説明は必ず折り返す
        //   （TextDisabled のままだと右端で切れて読めなくなっていた）。
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("クリックで ON/OFF。複数選べます");
        ImGui::TextDisabled("並び順に重なり、同じ項目は後ろのプリセットが勝ちます。"
                            "露出 / DoF / ブルーム品質などシーン側の設定は残ります。");
        ImGui::PopTextWrapPos();

        // 選ばれているものから pp を作り直す。1 つも無ければ選ぶ前の状態へ戻す。
        auto recompose = [&]()
        {
            int n = 0;
            for (bool s : presetSel) if (s) ++n;
            if (n == 0)
            {
                if (hasPresetBackup) { pp = presetBackup; hasPresetBackup = false; }
            }
            else
            {
                pp = ApplyPostPresets(presetSel, kPostPresetCount, presetBackup);
            }
        };

        ImDrawList*  dl      = ImGui::GetWindowDrawList();
        const float  sp      = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 tile = ui::Px(136.0f, 92.0f);
        const float  swatchH = ui::Px(62.0f);
        const float  availW  = ImGui::GetContentRegionAvail().x;
        float        lineW   = 0.0f;
        bool         first   = true;

        for (int i = 0; i < kPostPresetCount; ++i)
        {
            const PostPreset& pr = kPostPresets[i];
            // 「素の絵」は下の「すべて外す」が担当するので、タイルには出さない
            // （ID は MCP / 保存の互換のため表には残してある）。
            if (std::strcmp(pr.id, "none") == 0) continue;

            if (!first && lineW + sp + tile.x <= availW)
            { ImGui::SameLine(); lineW += sp + tile.x; }
            else
                lineW = tile.x;
            first = false;

            ImGui::PushID(i);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##presettile", tile);
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked();
            const bool sel     = presetSel[i];

            // サムネイル: そのプリセット【単体】を素の絵に当てた結果を描く。
            // 設定値から描いているので、プリセットの数値を直せば絵も一緒に変わる。
            const PostProcessSettings preview = ApplyPostPreset(pr, PostProcessSettings{});
            const ImVec2 s0(p0.x + ui::Px(3.0f), p0.y + ui::Px(3.0f));
            const ImVec2 s1(p0.x + tile.x - ui::Px(3.0f), p0.y + ui::Px(3.0f) + swatchH);
            postswatch::DrawSwatch(dl, s0, s1, preview);

            // 選択中は重なる順番（1,2,3…）を右上に出す＝「後ろが勝つ」が見て分かる
            if (sel)
            {
                int order = 1;
                for (int k = 0; k < i; ++k) if (presetSel[k]) ++order;
                char num[8];
                std::snprintf(num, sizeof(num), "%d", order);
                const ImVec2 ts = ImGui::CalcTextSize(num);
                const ImVec2 bc(s1.x - ts.x * 0.5f - ui::Px(9.0f), s0.y + ts.y * 0.5f + ui::Px(5.0f));
                dl->AddCircleFilled(bc, ts.y * 0.72f + ui::Px(3.0f), ImGui::GetColorU32(theme::Accent), 16);
                dl->AddText(ImVec2(bc.x - ts.x * 0.5f, bc.y - ts.y * 0.5f),
                            ImGui::GetColorU32(theme::OnAccent), num);
            }

            // ラベル（選択中はアクセント色）
            const ImVec2 ls = ImGui::CalcTextSize(pr.label);
            dl->AddText(ImVec2(p0.x + (tile.x - ls.x) * 0.5f, s1.y + ui::Px(6.0f)),
                        ImGui::GetColorU32(sel ? theme::AccentHover : theme::TextMid),
                        pr.label);

            // 枠（選択 > ホバー > 通常）。選択はアイデンティティの選択カード縁（アクセント）。
            if (sel)
                ui::deco::CardSelected(dl, p0, ImVec2(p0.x + tile.x, p0.y + tile.y), ui::Px(4.0f));
            else
                dl->AddRect(p0, ImVec2(p0.x + tile.x, p0.y + tile.y),
                            ImGui::GetColorU32(hovered ? theme::BorderStrong : theme::Border),
                            ui::Px(4.0f), 0, ui::Px(1.0f));

            if (hovered)
                ImGui::SetTooltip("%s\n\nクリックで %s", pr.tip, sel ? "外す" : "重ねる");
            if (clicked)
            {
                // 最初の 1 個を選ぶ瞬間の状態を控える（「戻す」で完全に元へ帰れる）
                if (!hasPresetBackup) { presetBackup = pp; hasPresetBackup = true; }
                presetSel[i] = !sel;
                recompose();
            }
            ImGui::PopID();
        }

        // ---- 選択中の要約 + 操作 ----
        std::string summary;
        for (int i = 0; i < kPostPresetCount; ++i)
            if (presetSel[i])
            { if (!summary.empty()) summary += " + "; summary += kPostPresets[i].label; }
        if (summary.empty())
            ImGui::TextDisabled("選択中: なし");
        else
            ImGui::TextColored(theme::AccentHover, "選択中: %s", summary.c_str());

        if (ImGui::SmallButton("すべて外す（素の絵）"))
        {
            if (!hasPresetBackup) { presetBackup = pp; hasPresetBackup = true; }
            for (bool& s : presetSel) s = false;
            pp = PostPresetBaseline(presetBackup);   // 味付けだけ落として素へ
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("味付けを全部落とします（露出・DoF・ブルーム品質は残ります）");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasPresetBackup);
        if (ImGui::SmallButton("↩ 選ぶ前に戻す"))
        {
            pp = presetBackup;
            hasPresetBackup = false;
            for (bool& s : presetSel) s = false;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("プリセットを 1 つ目に選ぶ直前の状態へ完全に戻します");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(-ui::Px(90.0f));
    ui::InputTextWithHint("##postfilter", "絞り込み（例: 魚眼 / bloom / グリッチ）",
                             postFilter, sizeof(postFilter));
    ImGui::SameLine();
    if (ImGui::SmallButton("クリア##pf")) postFilter[0] = '\0';
    ImGui::TextDisabled("SceneビューとGameビューへ同じ見た目を適用します");
    ImGui::Separator();

    ImGui::BeginDisabled(!pp.enabled);
    // 下のボタン行（すべてOFF / 初期値に戻す）のぶんだけ高さを残す。
    // 0 を渡すと一覧が残り全部を食ってフッターが画面外へ落ちる。
    ImGui::BeginChild("##postlist",
        ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f)),
        false);
    const char* curCat = nullptr;
    for (const auto& f : fx)
    {
        if (!matchesFilter(f)) continue;
        if (curCat == nullptr || std::strcmp(curCat, f.cat) != 0)
        {
            curCat = f.cat;
            ImGui::SeparatorText(curCat);
        }
        ImGui::PushID(f.label);
        ui::Checkbox(f.label, f.on);
        if (f.help)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme::TextFaint);
            ImGui::TextUnformatted(ICON_HELP);
            ImGui::PopStyleColor();
            if (ImGui::BeginItemTooltip())
            { ImGui::TextUnformatted(f.help); ImGui::EndTooltip(); }
        }
        // ★有効なら「その場で」パラメータを出す。別窓を開かないと何も触れなかったのが
        //   「パラメータが全然いじれない」と言われていた原因。
        if (*f.on && f.params)
        {
            if (f.reset)
            {
                ImGui::SameLine(ImGui::GetContentRegionMax().x - ui::Px(24.0f));
                if (ImGui::SmallButton("↺"))
                    f.reset();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("このエフェクトのパラメータを既定へ戻す");
            }
            drawParams(f);
            ImGui::Spacing();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button("すべてOFF"))
        for (const auto& f : fx) *f.on = false;
    ImGui::SameLine();
    if (ImGui::Button("初期値に戻す"))
        pp = PostProcessSettings{};
    ImGui::SameLine();
    ImGui::TextDisabled("有効中: %d", enabledCount);
    ImGui::End();
    } // if showPostProcess

    // ===== ウィンドウ2: 有効なエフェクトのパラメータだけを詰めた窓 =====
    // 一覧窓を閉じて、詰め作業だけを広い画面でやりたい人向け（従来どおり）。
    if (m_editorCtx->showPostParams)
    {
    ImGui::Begin("Post Process パラメータ");
    if (!pp.enabled)
        ImGui::TextDisabled("マスターが OFF です（Post Process 窓で有効化）");
    else if (enabledCount == 0)
        ImGui::TextDisabled("エフェクトを有効にすると、ここに調整項目が出ます");
    else
    {
        for (const auto& f : fx)
        {
            if (!*f.on || !f.params) continue;
            ImGui::SeparatorText(f.label);
            ImGui::PushID(f.label);
            if (f.reset)
            {
                ImGui::SameLine(ImGui::GetContentRegionMax().x - ui::Px(24.0f));
                if (ImGui::SmallButton("↺")) f.reset();
            }
            drawParams(f);
            ImGui::PopID();
        }
    }
    ImGui::End();
    } // if showPostParams
}

void Application::RenderSceneSettingsWindows()
{
    if (!m_scene) return;

    // ---- Skybox / IBL 設定ウィンドウ（シーン単位の環境マップ・トグル表示）----
    if (m_editorCtx->showSkybox)
    {
        auto& sk = m_scene->GetSkyboxSettings();
        ImGui::Begin("Skybox / IBL");
        ImGui::TextWrapped("環境キューブ(.dds, TEXTURECUBE) から irradiance / prefiltered / BRDF LUT を生成し、"
                           "ambient を IBL 化する。空欄なら従来 ambient。");
        ImGui::Separator();

        if (pg::Begin("##skybox"))
        {
            // env map パス入力（assets 相対）
            pg::InputTextStr("Env Map", sk.envMapPath, nullptr, "環境キューブ (.dds, assets 相対パス)。空欄なら従来の ambient");
            pg::SliderFloat("IBL Intensity", &sk.iblIntensity, 0.0f, 3.0f, "%.2f");
            pg::SliderFloat("Skybox Intensity", &sk.skyboxIntensity, 0.0f, 3.0f, "%.2f");
            pg::Checkbox("Draw Skybox", &sk.drawSkybox, "背景に空を描く");
            pg::End();
        }

        // ランタイム値へ即時反映（強度/描画フラグは再ベイク不要）
        m_iblIntensity    = sk.iblIntensity;
        m_skyboxIntensity = sk.skyboxIntensity;
        m_drawSkybox      = sk.drawSkybox;

        ImGui::Separator();
        if (ImGui::Button("環境マップ適用 / 再ベイク"))
            m_skyboxDirty = true;   // 次フレーム冒頭で再ベイク（WaitIdle 込み）
        ImGui::SameLine();
        ImGui::TextDisabled(m_iblReady && m_iblBaker && m_iblBaker->HasEnvironment()
                            ? "IBL: 有効" : "IBL: フォールバック(ambient)");
        ImGui::End();
    }

    // ---- SSAO 設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
    if (m_editorCtx->showSSAO)
    {
        auto& ss = m_scene->GetSSAOSettings();
        ImGui::Begin("SSAO");
        ImGui::TextWrapped("深度プリパス + 深度から法線再構築の半球カーネル AO。"
                           "ambient/IBL へ ao を乗算する。透視ビューのみ（2D 正射では無効）。");
        ImGui::Separator();
        if (pg::Begin("##ssao"))
        {
            pg::Checkbox("SSAO 有効", &ss.enabled);
            ImGui::BeginDisabled(!ss.enabled);
            pg::SliderFloat("半径 Radius",  &ss.radius,    0.05f, 2.0f, "%.2f");
            pg::SliderFloat("バイアス Bias", &ss.bias,     0.0f,  0.1f, "%.3f");
            pg::SliderFloat("強度 Intensity", &ss.intensity, 0.0f, 2.0f, "%.2f");
            pg::SliderFloat("べき Power",    &ss.power,     0.5f,  4.0f, "%.2f");
            {
                int s16 = (ss.sampleCount >= 16) ? 1 : 0;
                if (PgCombo("サンプル数", &s16, "8\0" "16\0"))
                    ss.sampleCount = s16 ? 16 : 8;
            }
            pg::Checkbox("ブラー Blur", &ss.blur);
            ImGui::EndDisabled();
            pg::End();
        }
        ImGui::End();
    }

    // ---- SSR / SSGI 設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
    if (m_editorCtx->showScreenSpaceGi)
    {
        auto& sr = m_scene->GetSsrSettings();
        auto& sg = m_scene->GetSsgiSettings();
        ImGui::Begin("SSR / SSGI");
        ImGui::TextWrapped("深度プリパスの G-Buffer（法線/ラフネス/メタリック）と"
                           "前フレームのシーンカラーをレイマーチする。透視ビューのみ。"
                           "どちらか有効にすると深度+速度プリパスが常時走る。"
                           "反射/間接光は 1 フレーム遅れる。");
        ImGui::Separator();

        if (pg::Begin("##ssrssgi"))
        {
            pg::Group("SSR（スクリーン空間反射）");
            pg::Checkbox("SSR 有効", &sr.enabled);
            ImGui::BeginDisabled(!sr.enabled);
            pg::SliderFloat("強度",        &sr.intensity,       0.0f, 1.0f,   "%.2f");
            pg::SliderFloat("最大距離(m)",  &sr.maxDistance,     1.0f, 200.0f, "%.1f");
            pg::SliderFloat("厚み(m)",      &sr.thickness,       0.05f, 2.0f,  "%.2f");
            pg::SliderInt  ("ステップ数",    &sr.maxSteps,        16, 128);
            pg::SliderFloat("歩幅(px)",     &sr.stride,          1.0f, 8.0f,   "%.1f");
            pg::SliderFloat("ラフネス上限",  &sr.roughnessCutoff, 0.05f, 1.0f,  "%.2f", nullptr,
                            "ラフネス上限を超える面はレイを打たず IBL に任せる");
            pg::SliderFloat("画面端フェード", &sr.edgeFade,        0.0f, 0.5f,   "%.2f");
            pg::SliderFloat("バイアス(m)",   &sr.bias,            0.0f, 0.5f,   "%.3f");
            ImGui::EndDisabled();

            pg::Group("SSGI（スクリーン空間GI）");
            pg::Checkbox("SSGI 有効", &sg.enabled);
            ImGui::BeginDisabled(!sg.enabled);
            pg::SliderFloat("SSGI 強度",     &sg.intensity,  0.0f, 2.0f,  "%.2f");
            pg::SliderFloat("到達距離(m)",    &sg.radius,     0.5f, 30.0f, "%.1f");
            pg::SliderFloat("SSGI 厚み(m)",   &sg.thickness,  0.05f, 2.0f, "%.2f");
            pg::SliderInt  ("レイ数/px",      &sg.rayCount,   1, 4);
            pg::SliderInt  ("SSGI ステップ数", &sg.stepCount,  4, 24);
            pg::SliderFloat("輝度クランプ",    &sg.clampValue, 0.1f, 20.0f, "%.2f");
            pg::SliderFloat("時間蓄積 Feedback", &sg.feedback,   0.0f, 0.98f, "%.2f");
            pg::Checkbox("画面外は IBL で埋める", &sg.iblFallback,
                         "IBL 埋めを切るとカメラを回すたびに明るさが変動する");
            ImGui::EndDisabled();
            pg::End();
        }
        ImGui::End();
    }

    // ---- ボリュメトリックフォグ設定ウィンドウ（シーン単位・グローバルレンダ設定・トグル表示）----
    if (m_editorCtx->showVolumetricFog)
    {
        auto& f = m_scene->GetVolumetricFogSettings();
        ImGui::Begin("Volumetric Fog");
        ImGui::TextWrapped("視錐台に沿った 3D テクスチャ（160x90x64）へ散乱を焼いてから "
                           "画面へ合成する。空気そのものが光る＝光の筋（ゴッドレイ）が "
                           "立体的に見える。透視ビューのみ。有効にした時点で 28MB 確保する。");
        ImGui::Separator();

        if (pg::Begin("##fog"))
        {
            pg::Checkbox("有効", &f.enabled);
            ImGui::BeginDisabled(!f.enabled);

            pg::Group("媒質");
            pg::SliderFloat("濃度",         &f.density,       0.0f, 0.3f,  "%.4f");
            pg::Color3     ("散乱アルベド",  &f.albedo.x);
            pg::SliderFloat("異方性 g",      &f.anisotropy,   -0.9f, 0.9f,  "%.2f", nullptr,
                            "g>0 = 前方散乱（太陽の方を向くと明るい）。0.6-0.8 で強いシャフト");
            pg::SliderFloat("高さ減衰(1/m)", &f.heightFalloff, 0.0f, 0.5f,  "%.3f");
            pg::Float      ("基準高さ(Y)",   &f.heightRef,     0.1f, -500.0f, 500.0f, "%.1f");

            pg::Group("ボリューム");
            pg::SliderFloat("到達距離(m)",   &f.distance,        10.0f, 500.0f, "%.0f");
            pg::SliderFloat("深度分布 k",    &f.depthDistribution, 1.0f, 4.0f, "%.2f", nullptr,
                            "z = 距離 * w^k。1=線形 / 大きいほど手前が細かい");
            pg::Checkbox("解析フォグで延長", &f.extendBeyondRange, "到達距離の外を解析フォグで延長する");

            pg::Group("ライティング");
            pg::Color3     ("環境散乱",      &f.ambient.x);
            pg::SliderFloat("太陽の寄与",    &f.sunIntensity, 0.0f, 5.0f, "%.2f");
            pg::Checkbox("点光源/スポット", &f.lightScattering,
                         "点光源・スポットも散乱させる。クラスタライトリストを引く（クラスタード無効時はスキップ）");

            pg::Group("時間再投影");
            pg::Checkbox("再投影を使う", &f.temporal);
            ImGui::BeginDisabled(!f.temporal);
            pg::SliderFloat("現フレーム比率", &f.temporalBlend, 0.01f, 1.0f, "%.3f", nullptr,
                            "小さいほど滑らかだがゴーストが増える（既定 0.08）");
            ImGui::EndDisabled();

            pg::Group("デバッグ表示（保存されない）");
            PgCombo("表示", &f.debugMode, "オフ\0散乱だけ\0透過率だけ\0froxel スライス\0\0");
            ImGui::EndDisabled();
            pg::End();
        }
        ImGui::End();
    }
}

} // namespace dx12e
