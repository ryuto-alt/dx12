#pragma once

#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dx12e
{

class EditorContext;
class Scene;

class HierarchyPanel
{
public:
    void Render(entt::registry& reg, EditorContext& ctx);
    void SetAssetsDir(const std::string& assetsDir) { m_assetsDir = assetsDir; }

private:
    void DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e);
    // クリックの選択処理（通常 / Ctrl=トグル / Shift=範囲）。ツリー表示とフィルタ表示で共用。
    void HandleRowClick(EditorContext& ctx, entt::entity e);
    // m_rows（今フレームの可視行）上で a〜b の行をまとめて選択する
    void SelectRange(EditorContext& ctx, entt::entity a, entt::entity b, bool additive);

    // 展開状態は自前で持つ。ImGui 内部の storage だけだと「開いている行だけを
    // 平坦化した行リスト」が作れず、ListClipper で間引けないため。
    std::unordered_set<entt::entity> m_openNodes;
    struct Row { entt::entity e; int depth; };
    std::vector<Row> m_rows;   // このフレームの可視行（開いてる親の子だけ含む）

    // 親→子の索引。Render() 先頭で1パスだけ作る。
    // これが無いと「ノード1つ描くたびに全 Transform を走査」= O(N^2) になり、
    // 1万体シーンでヒエラルキーだけで数十 ms 溶ける（実測 10000体で ~50ms）。
    std::unordered_map<entt::entity, std::vector<entt::entity>> m_childIndex;
    static const std::vector<entt::entity> s_noChildren;

    // Shift+クリックの範囲選択の起点（通常クリック / Ctrl+クリックで更新、Shift では動かさない）
    entt::entity m_selectAnchor = entt::null;
    // ビューポートや MCP で選択が変わったとき、その行を「見える状態」にするための追跡。
    // 祖先を開いてスクロールする＝グループの中に入っている物を探し回らなくて済む。
    entt::entity m_lastRevealed   = entt::null;
    entt::entity m_scrollToEntity = entt::null;
    // 複数選択の1行を押した状態。ドラッグせずに離したらこの行の単独選択に確定する
    // （押した瞬間に潰すと、まとめてドラッグ移動できなくなるため）
    entt::entity m_clickPendingEntity = entt::null;

    // リネーム
    entt::entity m_renamingEntity = entt::null;
    char m_renameBuf[128] = {};
    int  m_renameWarmup = 0;  // フォーカス安定まで数フレーム待つ

    std::string m_assetsDir;
};

} // namespace dx12e
