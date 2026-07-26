#pragma once

// ===== ライティング・プリセット（太陽 + ポストの組み合わせ）=====
//
// エディタの「ライティング」窓（editor/panels/LightingPanel.cpp）と MCP の
// dx12_apply_lighting_preset が【同じテーブル・同じ適用関数】を使うためのヘッダオンリー。
// 二重実装すると「エディタの夕暮れと AI の夕暮れが違う」という最悪の事故になるので、
// 値も式もここ 1 箇所にしか置かない。
//
// ImGui / entt / GPU に依存しない（DirectionalLight と PostProcessSettings という
// ただのデータ構造しか触らない）＝ヘッドレスからも呼べる。

#include <cstring>

#include "core/Types.h"
#include "ecs/Components.h"
#include "editor/LightMath.h"
#include "renderer/PostProcessSettings.h"

namespace dx12e
{

struct LightingPreset
{
    const char* id;          // MCP / 保存用の安定 ID（英小文字）
    const char* label;       // エディタのボタン表示（日本語）
    const char* tip;         // ひとことの説明
    f32         hour;        // >= 0 なら lightmath::SampleTimeOfDay(hour) の値を使う（Lua と同じカーブ）
    DirectX::XMFLOAT3 dir;   // hour < 0 のときの向き（光が進む向き）
    f32         kelvin;      // hour < 0 のときの色温度（<= 0 なら白）
    f32         intensity;
    f32         ambient;
    bool        exposureOn;   f32 exposure;
    bool        bloomOn;      f32 bloom;  f32 bloomThreshold;
    bool        vignetteOn;   f32 vignette;
    bool        saturationOn; f32 saturation;
};

// constexpr にはしない（DirectXMath の XMFLOAT3 コンストラクタが constexpr でない版があるため）。
inline const LightingPreset kLightingPresets[] = {
    {"day",    "昼",        "真上から白い光。まずここから作り始めるやつ",
     12.0f, {0.0f, -1.0f, 0.0f},    0.0f, 0.0f, 0.0f,
     false, 1.00f,  true, 0.20f, 1.40f,  false, 0.00f,  false, 1.00f},

    {"dusk",   "夕暮れ",    "低いオレンジの光 + 強めのブルーム。逆光が映える",
     17.5f, {0.0f, -1.0f, 0.0f},    0.0f, 0.0f, 0.0f,
     true,  1.05f,  true, 0.45f, 1.00f,  true,  0.35f,  true,  1.12f},

    {"night",  "夜 / 月明かり", "青白い弱い光 + 低い環境光。暗所の質感を見る用",
     1.0f,  {0.0f, -1.0f, 0.0f},    0.0f, 0.0f, 0.0f,
     true,  0.90f,  true, 0.35f, 1.10f,  true,  0.50f,  true,  0.85f},

    {"indoor", "屋内（暖色）", "電球色の斜め光 + 環境光多め。室内の見た目",
     -1.0f, {-0.35f, -1.00f, -0.25f}, 2900.0f, 1.2f, 0.22f,
     true,  1.00f,  true, 0.18f, 1.50f,  true,  0.20f,  false, 1.00f},

    {"horror", "ホラー",    "ほぼ真っ暗 + 冷たい薄明かり + 強いビネット",
     -1.0f, {0.20f, -1.00f, 0.35f},   7800.0f, 0.25f, 0.02f,
     true,  0.80f,  true, 0.50f, 1.20f,  true,  0.75f,  true,  0.60f},

    {"studio", "スタジオ",  "均一なニュートラル光。アセットの素の色を確認する用",
     -1.0f, {-0.40f, -0.85f, -0.35f}, 5600.0f, 2.2f, 0.42f,
     false, 1.00f,  false, 0.20f, 1.40f, false, 0.00f,  false, 1.00f},
};

inline constexpr int kLightingPresetCount =
    static_cast<int>(sizeof(kLightingPresets) / sizeof(kLightingPresets[0]));

// id（"day" 等）でプリセットを引く。未知なら nullptr。
inline const LightingPreset* FindLightingPreset(const char* id)
{
    if (!id) return nullptr;
    for (const LightingPreset& p : kLightingPresets)
        if (std::strcmp(p.id, id) == 0) return &p;
    return nullptr;
}

// プリセットを「太陽の DirectionalLight」へ適用した結果を返す（元の値は書き換えない）。
// base には現在の太陽を渡す（direction/color/intensity/ambient 以外のフィールドを保つため）。
inline DirectionalLight ApplyLightingPresetToSun(const LightingPreset& p, const DirectionalLight& base)
{
    namespace lm = lightmath;
    DirectionalLight out = base;
    if (p.hour >= 0.0f)
    {
        const lm::TimeOfDaySample s = lm::SampleTimeOfDay(p.hour);
        out.direction = s.direction;
        out.color     = s.color;
        out.intensity = s.intensity;
        out.ambient   = s.ambient;
    }
    else
    {
        const lm::SunAngles a = lm::DirectionToSunAngles(p.dir);
        out.direction = lm::SunAnglesToDirection(a);   // 正規化して入れる
        out.color     = (p.kelvin > 0.0f) ? lm::KelvinToRGB(p.kelvin)
                                          : DirectX::XMFLOAT3{1.0f, 1.0f, 1.0f};
        out.intensity = p.intensity;
        out.ambient   = p.ambient;
    }
    out._prevRotInit = false;   // Transform 回転の差分追従をリセット（次フレームで取り直す）
    return out;
}

// プリセットのポスト設定を適用した結果を返す（触らないエフェクトは base のまま）。
inline PostProcessSettings ApplyLightingPresetToPost(const LightingPreset& p,
                                                     const PostProcessSettings& base)
{
    PostProcessSettings out = base;
    out.exposureOn     = p.exposureOn;
    out.exposure       = p.exposure;
    out.bloomOn        = p.bloomOn;
    out.bloom          = p.bloom;
    out.bloomThreshold = p.bloomThreshold;
    out.vignetteOn     = p.vignetteOn;
    out.vignette       = p.vignette;
    out.saturationOn   = p.saturationOn;
    out.saturation     = p.saturation;
    return out;
}

// GPU へ送れる灯数の上限。超えたぶんは【無言で描画されない】ので、エディタの警告表示と
// MCP の dx12_list_lights が同じ数字を出せるようここに置く。
//
// ★クラスタードライティング（Forward+）化で「点 8 / スポット 8」の個別上限は撤廃された。
//   今の上限は point + spot の合計 1024 灯（src/renderer/ClusterMath.h の kMaxSceneLights）。
//   ただし 1 クラスタ（16x9x24 グリッドの 1 マス）で評価できるのは 128 灯までで、
//   ここを超えた分は【無言で消える】。これは CPU 側からは検出できないので、
//   エディタのクラスタデバッグ表示（ライト複雑度ヒートマップ）で目視するしかない。
//
//   影マップの本数（spot 4 / point 2）は据え置き＝クラスタード化しても
//   シャドウパスの描画回数は 1 回も減らないため（別問題として切り離してある）。
inline constexpr int kLightBudgetTotal      = 1024;  // point + spot の合計（kMaxSceneLights）
inline constexpr int kLightBudgetPerCluster = 128;   // 1 クラスタ内で評価できる灯数（kMaxLightsPerCluster）
inline constexpr int kLightBudgetShadowSpot = 4;     // MAX_SHADOW_SPOT（Application::kMaxShadowSpot）
inline constexpr int kLightBudgetShadowPoint = 2;    // Application::kMaxShadowPoint（6面/灯）
// 後方互換のエイリアス（呼び出し側を段階的に移行するため。どちらも合計上限を指す）。
inline constexpr int kLightBudgetPoint = kLightBudgetTotal;
inline constexpr int kLightBudgetSpot  = kLightBudgetTotal;

} // namespace dx12e
