#pragma once

// シーンビューの精密ピッキング（レイキャスト）。SceneViewPanel のメンバに一切依存しない
// 自由関数なので、エディタの左クリック選択・マテリアル D&D・右クリックメニューだけでなく
// MCP 側からも同じ結果を得られる。
//
// 2 段構え:
//   1) ブロードフェーズ … Application の描画リスト(DrawItem)を借りて レイ vs バウンディング球。
//      ワールド行列も球も構築済みなので ComputeWorldMatrix の呼び直しがゼロになる（本命の高速化）。
//   2) ナローフェーズ  … 候補を近い順に、サブメッシュ単位で
//      「ワールド行列(+meshNodeTransforms)の逆行列でレイをローカルへ → メッシュAABB → 三角形」。
//
// 戻り値は距離(t)の昇順。単一選択は先頭を使い、重なりの循環選択は 2 番目以降へ進む。

#include <vector>
#include <entt/entt.hpp>
#include <DirectXMath.h>
#include "core/Types.h"
#include "renderer/DrawItem.h"

namespace dx12e
{

class Camera;

struct ScenePickHit
{
    entt::entity      entity       = entt::null;
    u32               submeshIndex = 0;       // MeshRenderer::meshes[] のインデックス
    f32               distance     = 0.0f;    // レイ原点(near平面)からのワールド距離
    DirectX::XMFLOAT3 worldPos{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 worldNormal{0.0f, 1.0f, 0.0f};
    // MeshRenderer を持たない Light/Camera/Empty のアイコンヒット
    // （三角形ではなくスクリーン上のピクセル半径で判定したもの）。
    bool              isIcon       = false;
};

struct ScenePickOptions
{
    // false にすると MeshRenderer 持ちだけを対象にする（マテリアル D&D / テクスチャメニュー用。
    // アイコンやワールドスプライトを拾わない）。
    bool includeNonMesh  = true;
    // 三角形単位の精密判定。false ならメッシュ AABB 止まり（軽い代わりに従来どおり大雑把）。
    bool trianglePrecise = true;
    // アイコン(Light/Camera/Empty)のスクリーン当たり半径(px)。EditorIconRenderer が描く
    // ビルボードは一定スクリーンサイズなので、当たりもスクリーン基準にするのが正しい。
    f32  iconPixelRadius = 18.0f;
    // ナローフェーズに掛けるブロードフェーズ候補の上限（病的なシーンでの最悪時間の歯止め）。
    u32  maxCandidates   = 64;
};

// スクリーン座標 → ワールドレイ。クリップ空間の near/far をアンプロジェクトするので
// 透視でも正射でも正しい（正射をカメラ位置基準で作ると平行レイにならず破綻する）。
// outDir は正規化済み。
void ScreenRay(const Camera& camera,
               f32 vpX, f32 vpY, f32 vpW, f32 vpH,
               f32 screenX, f32 screenY,
               DirectX::XMFLOAT3& outOrigin, DirectX::XMFLOAT3& outDir);

// screenX/screenY からレイを飛ばし、ヒットを距離昇順で返す（ヒット無しなら空）。
// drawItems が null / 空なら entt を直接走査するフォールバックに落ちる（描画リストがまだ
// 構築されていない初回フレームやヘッドレス経路向け。結果は同じ、速度だけが違う）。
std::vector<ScenePickHit> RaycastScene(entt::registry& reg,
                                       const std::vector<DrawItem>* drawItems,
                                       const Camera& camera,
                                       f32 vpX, f32 vpY, f32 vpW, f32 vpH,
                                       f32 screenX, f32 screenY,
                                       const ScenePickOptions& opt = ScenePickOptions{});

// ワールド空間のレイ版。上と【同じブロード/ナローフェーズ】を通るので結果の意味は完全に一致する
// （MCP の dx12_raycast_precise / dx12_snap_to_ground の接地計算がこれを使う）。
// カメラを取らないぶんアイコンとビルボードは対象外＝opt.includeNonMesh は無視される。
// direction は正規化されていなくてよい（内部で正規化するので distance は実距離）。
// maxDistance <= 0 で無制限。戻り値は距離(t)の昇順。
std::vector<ScenePickHit> RaycastSceneRay(entt::registry& reg,
                                          const std::vector<DrawItem>* drawItems,
                                          const DirectX::XMFLOAT3& origin,
                                          const DirectX::XMFLOAT3& direction,
                                          f32 maxDistance = 0.0f,
                                          const ScenePickOptions& opt = ScenePickOptions{});

} // namespace dx12e
