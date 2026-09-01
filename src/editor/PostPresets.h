#pragma once

// ===== ポストプロセスの「見た目プリセット」=====
//
// ライティング・プリセット（editor/LightingPresets.h）と同じ流儀のヘッダオンリー。
// 太陽を触らず、ポストの組み合わせだけで画面の質感を決め打ちする。
//
// ★プリセットは「積み増し」ではなく「置き換え」。
//   適用すると【見た目を作るエフェクト】は全部いったん既定値へ戻してからプリセットの値を入れる。
//   前に触ったグリッチやセピアが残って「プリセットを選んだのに違う絵になる」のを防ぐため。
//   逆に【シーンの露出・カメラ・画質の設定】は保存する（下の kept 一覧）。
//     保持: enabled / tonemapper / exposure* / autoExposure* / lut* / dof* / motionBlur* /
//           bloomKnee / bloomRadius / fxaaOn / debandOn / godrays* / lensflare*
//
// ImGui にも GPU にも依存しない（PostProcessSettings というただのデータしか触らない）＝
// ヘッドレスからも MCP からも同じ関数を呼べる。

#include <cstring>

#include "renderer/PostProcessSettings.h"

namespace dx12e
{

struct PostPreset
{
    const char* id;     // MCP / 保存用の安定 ID（英小文字）
    const char* label;  // エディタのボタン表示（日本語）
    const char* tip;    // ひとことの説明
    void (*apply)(PostProcessSettings& pp);  // 既定へ戻したあとに呼ばれる
};

// 「見た目を作るエフェクト」を既定へ戻し、シーン側の設定は base から引き継ぐ。
inline PostProcessSettings PostPresetBaseline(const PostProcessSettings& base)
{
    PostProcessSettings out{};  // 既定値（＝全スタイライズ OFF）

    // ── シーン/カメラ側の設定は引き継ぐ（プリセットは「絵の味付け」だけを担当する）──
    out.enabled        = base.enabled;
    out.tonemapper     = base.tonemapper;
    out.exposureOn     = base.exposureOn;     out.exposure     = base.exposure;
    out.autoExposureOn = base.autoExposureOn; out.aeSpeed      = base.aeSpeed;
    out.aeEvComp       = base.aeEvComp;       out.aeLogMin     = base.aeLogMin;
    out.aeLogMax       = base.aeLogMax;
    out.lutOn          = base.lutOn;          out.lutPath      = base.lutPath;
    out.lutAmount      = base.lutAmount;
    out.dofOn          = base.dofOn;          out.dofFocusDist = base.dofFocusDist;
    out.dofFocusRange  = base.dofFocusRange;  out.dofBlurSize  = base.dofBlurSize;
    out.dofFocusName   = base.dofFocusName;   out.dofAperture  = base.dofAperture;
    out.dofFocalLength = base.dofFocalLength;
    out.motionBlurOn   = base.motionBlurOn;   out.mbStrength   = base.mbStrength;
    out.mbSamples      = base.mbSamples;
    out.bloomKnee      = base.bloomKnee;      out.bloomRadius  = base.bloomRadius;
    out.fxaaOn         = base.fxaaOn;         out.debandOn     = base.debandOn;
    out.godraysOn      = base.godraysOn;      out.grIntensity  = base.grIntensity;
    out.grDensity      = base.grDensity;      out.grDecay      = base.grDecay;
    out.lensflareOn    = base.lensflareOn;    out.lfIntensity  = base.lfIntensity;
    out.lfGhosts       = base.lfGhosts;       out.lfDispersal  = base.lfDispersal;
    out.lfHalo         = base.lfHalo;         out.lfChroma     = base.lfChroma;
    return out;
}

inline const PostPreset kPostPresets[] = {
    {"none", "素の絵", "味付けを全部外す（露出・DoF・ブルームの品質設定は残る）",
     [](PostProcessSettings&) {}},

    {"cinematic", "シネマ", "淡いブルーム + 四隅を落として少しコントラスト。まず迷ったらこれ",
     [](PostProcessSettings& p) {
         p.bloomOn = true;   p.bloom = 0.35f; p.bloomThreshold = 1.2f;
         p.contrastOn = true; p.contrast = 1.08f;
         p.saturationOn = true; p.saturation = 1.06f;
         p.vignetteOn = true; p.vignette = 0.45f;
         p.vignetteRadius = 0.68f; p.vignetteSoftness = 0.5f;
         p.chromaticOn = true; p.chromatic = 0.12f; p.chromaMode = 0;
         p.grainOn = true;   p.grain = 0.10f; p.grainSize = 1.5f;
     }},

    {"crt", "レトロ CRT", "走査線 + 画面湾曲 + 色ズレ。ブラウン管のゲーム画面",
     [](PostProcessSettings& p) {
         p.scanlineOn = true; p.scanline = 0.55f; p.scanCount = 240.0f; p.scanCurve = 0.22f;
         p.chromaticOn = true; p.chromatic = 0.30f; p.chromaMode = 1;
         p.vignetteOn = true; p.vignette = 0.55f;
         p.vignetteRadius = 0.62f; p.vignetteSoftness = 0.45f;
         p.contrastOn = true; p.contrast = 1.12f;
     }},

    {"pixel8bit", "8bit ドット", "ピクセル化 + 色数を落とす + ディザ。レトロ機の画面",
     [](PostProcessSettings& p) {
         p.pixelizeOn = true;  p.pixelSize = 5.0f;
         p.posterizeOn = true; p.posterize = 6;
         p.ditherOn = true;    p.ditherLevels = 6;
         p.saturationOn = true; p.saturation = 1.25f;
     }},

    {"fisheye", "魚眼レンズ", "等距離射影の魚眼 + 倍率色収差。周りは黒く落ちる",
     [](PostProcessSettings& p) {
         p.lensOn = true; p.lens = 0.75f; p.lensMode = 1;
         p.lensZoom = 1.0f; p.lensCircular = true; p.lensEdge = 1;
         p.lensChroma = 0.35f;
         p.vignetteOn = true; p.vignette = 0.5f; p.vignetteRadius = 0.7f;
     }},

    {"underwater", "水中", "青く沈めて揺らす。水面下 / 潜水",
     [](PostProcessSettings& p) {
         p.waveOn = true; p.waveAmp = 0.006f; p.waveFreq = 14.0f; p.waveSpeed = 1.6f;
         p.tintOn = true; p.tint = {0.72f, 0.92f, 1.0f};
         p.saturationOn = true; p.saturation = 0.85f;
         p.chromaticOn = true;  p.chromatic = 0.18f;
         p.vignetteOn = true;   p.vignette = 0.6f; p.vignetteRadius = 0.55f;
         p.bloomOn = true; p.bloom = 0.3f; p.bloomThreshold = 1.0f;
     }},

    {"horror", "ホラー", "彩度を抜いて粒子を乗せ、四隅を強く潰す",
     [](PostProcessSettings& p) {
         p.saturationOn = true; p.saturation = 0.45f;
         p.contrastOn = true;   p.contrast = 1.22f;
         p.grainOn = true;      p.grain = 0.42f; p.grainSize = 1.8f;
         p.vignetteOn = true;   p.vignette = 0.85f;
         p.vignetteRadius = 0.45f; p.vignetteSoftness = 0.5f;
         p.chromaticOn = true;  p.chromatic = 0.22f;
     }},

    {"noir", "白黒フィルム", "モノクロ + 粒子 + 強めのコントラスト",
     [](PostProcessSettings& p) {
         p.grayscaleOn = true; p.grayscale = 1.0f;
         p.contrastOn = true;  p.contrast = 1.25f;
         p.grainOn = true;     p.grain = 0.38f; p.grainSize = 1.6f;
         p.vignetteOn = true;  p.vignette = 0.6f; p.vignetteRadius = 0.6f;
     }},

    {"sepia", "セピア写真", "古い写真。褪せた色 + 粒子 + 周辺減光",
     [](PostProcessSettings& p) {
         p.sepiaOn = true;      p.sepia = 0.9f;
         p.saturationOn = true; p.saturation = 0.6f;
         p.grainOn = true;      p.grain = 0.28f; p.grainSize = 2.0f;
         p.vignetteOn = true;   p.vignette = 0.6f; p.vignetteRadius = 0.6f;
     }},

    {"toon", "線画コミック", "輪郭だけを白地に描く（絵の色は捨てる）",
     [](PostProcessSettings& p) {
         p.outlineOn = true; p.outline = 2.0f; p.outlineThickness = 1.4f;
         p.outlineThreshold = 0.03f; p.outlineOnly = true;
         p.outlineColor = {0.05f, 0.05f, 0.08f};
         p.outlineBg    = {1.0f, 1.0f, 1.0f};
     }},

    {"dream", "ドリーム", "強いブルームで滲ませる。回想 / 幻覚",
     [](PostProcessSettings& p) {
         p.bloomOn = true; p.bloom = 0.95f; p.bloomThreshold = 0.55f;
         p.saturationOn = true; p.saturation = 1.18f;
         p.warmthOn = true; p.warmth = 0.25f;
         p.vignetteOn = true; p.vignette = 0.35f; p.vignetteRadius = 0.8f;
     }},

    {"nightvision", "暗視ゴーグル", "緑一色 + 走査線 + 粒子",
     [](PostProcessSettings& p) {
         p.grayscaleOn = true; p.grayscale = 1.0f;
         p.tintOn = true;      p.tint = {0.25f, 1.35f, 0.35f};
         p.contrastOn = true;  p.contrast = 1.3f;
         p.scanlineOn = true;  p.scanline = 0.35f; p.scanCount = 300.0f; p.scanCurve = 0.0f;
         p.grainOn = true;     p.grain = 0.45f;
         p.vignetteOn = true;  p.vignette = 0.9f; p.vignetteRadius = 0.5f;
     }},

    {"glitch", "グリッチ", "デジタル崩れ + RGB 分離。ダメージ演出 / バグった画面",
     [](PostProcessSettings& p) {
         p.glitchOn = true; p.glitch = 0.55f; p.glitchBlocks = 32.0f;
         p.glitchSpeed = 14.0f; p.glitchColor = 0.8f;
         p.chromaticOn = true; p.chromatic = 0.5f;
         p.scanlineOn = true;  p.scanline = 0.25f; p.scanCount = 200.0f; p.scanCurve = 0.0f;
     }},
};

inline constexpr int kPostPresetCount =
    static_cast<int>(sizeof(kPostPresets) / sizeof(kPostPresets[0]));

inline const PostPreset* FindPostPreset(const char* id)
{
    if (!id) return nullptr;
    for (const PostPreset& p : kPostPresets)
        if (std::strcmp(p.id, id) == 0) return &p;
    return nullptr;
}

// プリセットを適用した結果を返す（base は書き換えない）。
inline PostProcessSettings ApplyPostPreset(const PostPreset& p, const PostProcessSettings& base)
{
    PostProcessSettings out = PostPresetBaseline(base);
    p.apply(out);
    return out;
}

} // namespace dx12e
