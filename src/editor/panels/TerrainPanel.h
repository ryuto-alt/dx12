#pragma once

// 地形ツール窓（ハイトフィールド地形の作成 / スカルプトブラシ / 浸食 / 山の一発生成）。
//
// - 独立フローティング窓（VfxEditorPanel / MaterialEditorPanel と同じ扱い。ドックしない）。
// - ビューポートでのブラシ操作は EditorContext::viewportToolHandlers に登録して割り込む
//   ＝SceneViewPanel を一切書き換えずにピッキングと排他になる。
// - 状態は Render() 内の関数ローカル static が持つ（パネル1個で足りる & Application 側の
//   追記を1行に抑えるため）。
//
// 開き方: ヒエラルキー「＋エンティティ追加 → 地形（Terrain）」、または Terrain を持つ
//         エンティティを選択すると自動で開く。

#include <string>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Scene;
class EditorContext;

namespace TerrainPanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。
// 初回呼び出しでビューポートツール（ブラシ）を EditorContext へ登録する。
// cmd = 記録中のコマンドリスト（地形メッシュの GPU 更新に使う）。
void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir,
            ID3D12GraphicsCommandList* cmd);

} // namespace TerrainPanel

} // namespace dx12e
