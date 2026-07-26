#pragma once

#include <DirectXMath.h>

namespace dx12e
{

// froxel ボリュメトリックフォグのシーン単位設定。
// SSAOSettings / ContactShadowSettings / TaaSettings / SsrSettings と同じ流儀
// （ECS コンポーネントではないレンダ設定）。シリアライズ対象。既定 OFF。
//
// なぜ PostProcessSettings に入れないか:
//   PostProcessSettings は uber パス 1 枚へ渡す「マスク付きエフェクト」の集合だが、
//   ボリュメトリックフォグはシャドウパス直後の compute 3 パス + フォワード直後の合成パスという
//   フレーム構造そのものを変える。性質が違うので SSAO と同じく独立させる。
struct VolumetricFogSettings
{
    bool  enabled           = false;  // 既定 OFF（有効時のみ 3D テクスチャ 28MB を確保する）
    float density           = 0.02f;  // 基準の消散係数 σ_t（1/m 相当）
    DirectX::XMFLOAT3 albedo = {1.0f, 1.0f, 1.0f};  // 散乱アルベド（σ_s = density * albedo）
    float anisotropy        = 0.3f;   // Henyey-Greenstein の g。0=等方 / 0.6-0.8=強い太陽シャフト
    float heightFalloff     = 0.1f;   // 高さ方向の指数減衰（1/m）。0 で高さ無依存
    float heightRef         = 0.0f;   // 減衰の基準高さ（world Y）
    float distance          = 150.0f; // froxel ボリュームの far。ここから先は解析フォグへ引き継ぐ
    float depthDistribution = 2.0f;   // z = far * w^k の k。1=線形 / 2=既定 / 大きいほど近距離が細かい
    DirectX::XMFLOAT3 ambient = {0.20f, 0.24f, 0.32f};  // 環境散乱（等方）
    float sunIntensity      = 1.0f;   // 太陽の散乱寄与スケール
    bool  lightScattering   = true;   // 点光源 / スポットの散乱（クラスタライトリストを引く）
    bool  temporal          = true;   // 時間再投影（OFF にするとジッタも自動で切れる）
    float temporalBlend     = 0.08f;  // 現フレームの比率。小さいほど滑らかだがゴーストが増える
    bool  extendBeyondRange = true;   // distance より遠方を解析的な指数フォグで延長する
    // 目視検証用の一時トグル（シリアライズしない）:
    //   0=off / 1=in-scattering のみ / 2=transmittance のみ / 3=froxel スライスの縞
    int   debugMode         = 0;
};

} // namespace dx12e
