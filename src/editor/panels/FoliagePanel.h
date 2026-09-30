#pragma once

// 植生ツール窓（F1: FoliageLayer への散布 / 円ブラシ / シーンの風）。
//
// - 独立フローティング窓（TerrainPanel と同じ扱い。ドックしない）。
// - ブラシは EditorContext::viewportToolHandlers に登録して割り込む（ピッキングと排他）。
// - 散布 / ブラシの中身は editor/FoliageOps（MCP の foliage_scatter / foliage_paint と同じ関数）。
// - 状態は Render() 内の関数ローカル static が持つ（パネル 1 個で足りる）。
//
// 開き方: メニュー「ツール → 植生ツール」、または FoliageLayer を選んだ Inspector の「植生ツールを開く」。

#include <string>

namespace dx12e
{

class Scene;
class EditorContext;

namespace FoliagePanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。窓が閉じていれば何もしない。
void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir);

} // namespace FoliagePanel

} // namespace dx12e
