#pragma once
// シーン設定の保存・読み込みの lane-C 用サイドカー（W1-FU-E-27）。
//   shadingRevision・材質 V2 の欄（"shading"）と LS-02a の欄（"shadowPcss"）を、SceneSerializer.cpp の本体を
//   増やさずに保存 / 読み込みする。SceneSerializer.cpp からは保存 1 か所・読み込み 1 か所の呼び出しだけ。
//   既定と違う値だけ書く＝既定のシーンの保存結果は 1 バイトも変わらない。読み込みは範囲を丸める（rev は 1 / 2 のみ）。
#include <nlohmann/json_fwd.hpp>

namespace dx12e
{
class Scene;

// root["shading"] / root["shadowPcss"] へ、既定と違う拡張欄を足す（BuildSceneJson の後ろの方から呼ぶ）。
void SaveSceneShadingLaneC(const Scene& scene, nlohmann::json& root);

// LoadShadingSettings / LoadShadowPcssSettings の後に呼ぶ。scene の設定へ拡張欄だけを上書きする。
void LoadSceneShadingLaneC(Scene& scene, const nlohmann::json& root);
} // namespace dx12e
