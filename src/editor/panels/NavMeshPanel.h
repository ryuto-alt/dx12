#pragma once

// ナビメッシュ窓（追いかける AI 用の経路探索メッシュを焼く / 確認する）。
//
// - 独立フローティング窓（TerrainPanel / SculptPanel と同じ扱い。ドックしない）。
// - 状態は Render() 内の関数ローカル static が持つ（Application 側の追記を 1 行に抑えるため）。
// - 焼いた実体はシーンの隣の `<シーン>.nav`。シーンを保存すると一緒に書かれる。
//
// 開き方: メニュー「ツール > ナビメッシュ」。

#include <string>

namespace dx12e
{

class Scene;
class EditorContext;

namespace NavMeshPanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。
void Render(Scene& scene, EditorContext& ctx);

// パネルの外（MCP）からもナビメッシュを焼くための共通入口。
// シーンのメッシュを集めて Scene::GetNavConfig() の設定でビルドし、Scene へ載せる。
// 戻り値 false で outError に理由。outLog は段階ごとの要約（UI/MCP がそのまま出す）。
bool BuildForScene(Scene& scene, std::string& outLog, std::string& outError);

} // namespace NavMeshPanel

} // namespace dx12e
