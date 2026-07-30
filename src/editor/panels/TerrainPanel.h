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

#include <functional>
#include <string>

#include <entt/entt.hpp>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Scene;
class EditorContext;
class HeightField;
class TerrainSplatMap;

namespace TerrainPanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。
// 初回呼び出しでビューポートツール（ブラシ）を EditorContext へ登録する。
// cmd = 記録中のコマンドリスト（地形メッシュの GPU 更新に使う）。
void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir,
            ID3D12GraphicsCommandList* cmd);

// ---- パネルの外（MCP）から地形を書き換えるときの Undo 入口 -------------------
// op の前後で全面をスナップショットし、変化があれば Undo に 1 エントリ積んで
// MarkDirty（＝メッシュ/コライダー作り直し + 自動保存）まで行う。
//
// ★これを通さないと MCP の地形編集が Undo に一切積まれない。ブラシは共有の
//   HeightField を直に書き換えたうえ MarkDirty が .hf の自動保存まで走らせるので、
//   「AI に山を作らせて、気に入らないので Ctrl+Z」で山は残ったまま
//   **利用者自身の無関係な 1 手が巻き戻る**。
// 全面スナップショットなので 1 回の呼び出しにつき 512²×4B ≒ 1MB を Undo に積む。
// ponytail: 押しっぱなしのストロークと違い MCP は一発ずつなので全面で許容する
//           （GUI 側の RunWholeTerrainEdit と同じ判断）。
bool RunUndoableHeightEdit(entt::registry& reg, EditorContext& ctx, entt::entity e,
                           const char* label, const std::function<void(HeightField&)>& op);

// スプラット（テクスチャペイント）版。高さ側と同じ流儀。
bool RunUndoableSplatEdit(entt::registry& reg, EditorContext& ctx, entt::entity e,
                          const std::function<void(TerrainSplatMap&)>& op);

} // namespace TerrainPanel

} // namespace dx12e
