#pragma once

// ===== シーンビュー上でライトを直接いじるツール =====
//
// 1) 太陽ドラッグ … L キーを押しっぱなしでマウスを動かすと、シーンの最初の
//    DirectionalLight（＝太陽）の向きがそのまま回る（UE の Ctrl+L 相当。
//    Ctrl+L は「新規スクリプト」に取られているので L 単独にしてある）。
//    方位/高度をビューポート上にオーバーレイ表示し、キーを離した時に Undo を 1 エントリ積む。
//
// 2) ライトのハンドル … 選択中のライトに丸ハンドルを出し、ドラッグで直接パラメータを変える。
//      SpotLight        : 外コーン角 / 内コーン角 / 距離(range)
//      PointLight       : 距離(range)
//      DirectionalLight : 向き（矢印の先を掴んで回す）
//    Undo はドラッグ確定（マウスを離した瞬間）に 1 エントリだけ積む。
//
// 3) 影響範囲の一望 … EditorContext::lightWireAll が ON なら、選択中でない
//    全ライトの range / コーンも薄いワイヤで描く。
//
// 描画は全部 ImGui の背景ドローリストなので、シェーダ／定数バッファには一切触らない。
//
// 【呼び出し規約】EditorLayer が 3D ビューポート操作の一連（RenderGizmo の後・
// RunViewportTools の前）で毎フレーム 1 回呼ぶこと。当たり判定とドラッグ処理を
// 「今フレームのマウス位置」で済ませてから、EditorContext::viewportToolHandlers 経由で
// 「このクリックは食った」を SceneViewPanel へ伝える（＝ピッキングに横取りされない）。

#include <entt/entt.hpp>

namespace dx12e
{

class Camera;
class EditorContext;

// ライトのハンドル操作 + オーバーレイ描画を 1 フレーム分進める。
// 初回呼び出しで EditorContext::viewportToolHandlers へ自分を登録する。
void LightHandlesFrame(entt::registry& reg, EditorContext& ctx, Camera* camera);

} // namespace dx12e
