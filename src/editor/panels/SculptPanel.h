#pragma once

// スカルプト窓（任意メッシュの頂点スカルプト。洞窟・アーチ・岩みたいな異形を作る）。
//
// - 地形ツール（TerrainPanel）と同じ作法。独立フローティング窓（ドックしない）。
// - ビューポートでのブラシ操作は EditorContext::viewportToolHandlers に登録して割り込む
//   ＝SceneViewPanel を一切書き換えずにピッキングと排他になる。
// - 状態は Render() 内の関数ローカル static が持つ（パネル1個で足りる & Application 側の
//   追記を1行に抑えるため）。
//
// 開き方: ヒエラルキー「＋エンティティ追加 → スカルプト（異形）」、または SculptMesh を持つ
//         エンティティを選択すると自動で開く。

#include <functional>
#include <string>
#include <entt/entt.hpp>

struct ID3D12GraphicsCommandList;

namespace dx12e
{

class Scene;
class EditorContext;
class SculptMeshData;

namespace SculptPanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。
// 初回呼び出しでビューポートツール（ブラシ）を EditorContext へ登録する。
// cmd = 記録中のコマンドリスト（スカルプトメッシュの GPU 更新に使う）。
void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir,
            ID3D12GraphicsCommandList* cmd);

// src の MeshRenderer（CPU 頂点キャッシュ）から編集可能なコピーを作る。
// 全サブメッシュを 1 つに畳み込み、meshNodeTransforms も焼き込む。元アセットは読むだけ。
// CPU キャッシュを持たないモデルでは false（＝「編集可能にできない」）。
// パネルの「選択中のモデルを編集可能にする」ボタンと MCP の dx12_sculpt_make_editable が
// 同じ実装を通るように公開している。
bool MakeEditable(entt::registry& reg, entt::entity src, SculptMeshData& out);

// パネルの外（MCP）からスカルプトを彫るときの Undo 入口。TerrainPanel 側と同じ理由:
// これを通さないと MCP のブラシが Undo に一切積まれず、Ctrl+Z が利用者自身の
// 無関係な 1 手を巻き戻す（しかも MarkDirty が .smsh の自動保存まで走らせる）。
// op の前後で全ルート頂点をスナップショットし、動いたものだけを Undo に積む。
bool RunUndoableSculptEdit(entt::registry& reg, EditorContext& ctx, entt::entity e,
                           const char* label, const std::function<void(SculptMeshData&)>& op);

} // namespace SculptPanel

} // namespace dx12e
