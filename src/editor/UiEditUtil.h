#pragma once

#include <entt/entt.hpp>
#include <DirectXMath.h>
#include <imgui.h>

#include "ui/UISystem.h"   // UiResolvedRect / UiXform2x3

struct ImDrawList;

namespace dx12e::uiedit
{

// 解決済み UI 矩形のアウトライン描画（選択枠/ホバー枠）。回転/スキュー（hasXform）の要素は
// 変形後 4 隅の四辺形（AddQuad）で描く。非変形は従来どおり AddRect。
void DrawResolvedOutline(ImDrawList* dl, const UiResolvedRect& rr, ImU32 col,
                         float thickness = 1.0f);

// マウス点が解決済み UI 矩形に入っているか。回転/スキューの要素は点を逆写像してから
// レイアウト空間の AABB で判定する（UiEditorPanel / SceneViewPanel のピッキング共通）。
bool ResolvedRectContains(const UiResolvedRect& rr, const ImVec2& p);

// クリック選択の対象解決（Figma / UMG 方式）。
// hit = 最前面ヒット要素。ボタンのラベル（子 UIText）を直接掴んで「文字だけ動く」のを防ぐため、
//   - シングルクリック: hit の「最外殻の祖先ウィジェット」（キャンバス直下）を返す。
//     ただし現在の選択が hit の祖先 or hit 自身ならそれを維持（深掘り済み状態を保つ）。
//   - ダブルクリック: 現在の選択から 1 段だけ hit 側へ深掘りする（選択が経路上に無ければ hit）。
// 返り値は必ず UIRect 持ち。hit が UIRect を持たなければ entt::null。
entt::entity ResolveClickTarget(entt::registry& reg, entt::entity hit,
                                entt::entity currentSelection, bool doubleClick);

// UIRect 用: エンティティの「親矩形」をキャンバス空間(px)で解決する。
// UISystem と同じ式（rect = parentMin + parentSize*anchor + offset）で、祖先の UICanvas の
// 基準解像度をルート矩形として e の親までのチェーンを順に適用する。ConstantPixel は
// 実行時のビューポート寸法に依存するため基準解像度で近似する（アンカープリセット/配置の
// 「見た目の位置を維持する」offset 補正用途にはこれで十分）。
// 祖先に UICanvas が無い（UI ツリー外）なら false を返し、out は変更しない。
bool ResolveUiParentRectPx(entt::registry& reg, entt::entity e,
                           DirectX::XMFLOAT2& outMin, DirectX::XMFLOAT2& outMax);

// 9 方位の配置テンプレ（Unity の Alt+アンカープリセット相当）。
// ax/ay ∈ {0,1,2}（0=左/上, 1=中央, 2=右/下）。現在の解決済みサイズを保ったまま、
// アンカーをその方位に設定し、要素をその位置へピッタリ寄せる（端はフラッシュ、中央は中央寄せ）。
// 解像度が変わってもその方位に追従する「正しい」配置になる。
// UIRect 無し / 親解決不可なら false（何も変更しない）。Undo は呼び出し側で before/after を取ること。
bool ApplyUiPlacement(entt::registry& reg, entt::entity e, int ax, int ay);

} // namespace dx12e::uiedit
