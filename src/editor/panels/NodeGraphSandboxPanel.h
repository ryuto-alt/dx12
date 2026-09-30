#pragma once

// ノードグラフ サンドボックス窓（開発用）。汎用ノードグラフ UI（src/editor/nodegraph/）の全機能を、
// マテリアルとは無関係のダミーの型・ノード（SandboxGraphModel）で手（= 仮想入力）で確認するための窓。
// マテリアルグラフ G0（docs/MATERIAL_GRAPH_DESIGN.md §7）。
//
// 開き方: メニュー「ツール > ノードグラフ サンドボックス」/ コマンドパレット。
// 状態は Render() 内の関数ローカル static が持つ（Application 側の追記を 1 行に抑えるため）。

namespace dx12e
{

class EditorContext;

namespace NodeGraphSandboxPanel
{

// Application が毎フレーム呼ぶ唯一の入口。窓が閉じていれば即 return。
void Render(EditorContext& ctx);

// EditorCommandTable の graph.* コマンド（コマンドパレット / メニューから）を、開いている窓へ渡す。
// 窓が開いていなければ false。
bool ExecuteCommand(EditorContext& ctx, const char* commandId);

} // namespace NodeGraphSandboxPanel

} // namespace dx12e
