#pragma once

#include <entt/entt.hpp>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include "editor/HierarchyLogic.h"

namespace dx12e
{

class EditorContext;
class Scene;

// ヒエラルキー（フェーズ 1b で全面改修）。
//   ・行: 開閉 / 種別アイコン / 名前 / 子の数 / 目（非表示）と鍵（ロック）。目と鍵は行にホバーした時か、状態が立っている時だけ出る。
//   ・上部: 件数（隠れている数・ロック数）/ 検索（名前 + コンポーネント名 + タグ。tag: c: t: n: is:）/ 型チップ。
//   ・並べ替え: 行の上 25%・下 25% へのドロップ = 兄弟の前・後ろ（親替えも兼ねる）/ 中央 = 子にする。挿入位置をラインで表示。
//   ・大量エンティティ: 親→子の索引・表示行・検索結果は、構造が変わらない間は使い回す（10 万体でも毎フレーム O(N) にしない）。
class HierarchyPanel
{
public:
    void Render(entt::registry& reg, EditorContext& ctx);
    void SetAssetsDir(const std::string& assetsDir) { m_assetsDir = assetsDir; }
    // コピー / 貼り付けのシリアライズに使う（EditorLayer が毎フレーム渡す）
    void SetScene(Scene* scene) { m_scene = scene; }

    // ---- テスト / 診断用 ----
    size_t VisibleRowCount() const { return m_filterActive ? m_filterRows.size() : m_rows.size(); }
    const hier::Index& IndexForTest() const { return m_index; }

private:
    // depth: ツリーの深さ（フィルタ中は 0）。flat=true: フィルタ中の行（子の開閉はしない。選択 / 右クリック / D&D / 改名は通常行と同じ）。
    void DrawEntityNode(entt::registry& reg, EditorContext& ctx, entt::entity e, bool flat);
    void DrawHeader(entt::registry& reg, EditorContext& ctx, size_t objCount);
    void DrawTypeChips(EditorContext& ctx);
    void DrawEmptyState(entt::registry& reg, EditorContext& ctx);
    void DrawEntityContextMenu(entt::registry& reg, EditorContext& ctx, entt::entity e, const std::string& name);
    void DrawBackgroundArea(entt::registry& reg, EditorContext& ctx);
    // 行の右端の目 / 鍵（rowMin/rowMax は行の矩形）。押されたら状態を切り替える。
    void DrawFlagButtons(entt::registry& reg, EditorContext& ctx, entt::entity e, ImVec2 rowMin, ImVec2 rowMax, bool rowHovered);

    void StartRename(entt::entity e, const std::string& currentName);
    // クリックの選択処理（通常 / Ctrl=トグル / Shift=範囲）。ツリー表示とフィルタ表示で共用。
    void HandleRowClick(EditorContext& ctx, entt::entity e);
    // 見えている行の上で a〜b の行をまとめて選択する
    void SelectRange(EditorContext& ctx, entt::entity a, entt::entity b, bool additive);
    // 索引 / 表示行 / 検索結果の更新（構造が変わった時だけ作り直す）
    void RefreshModel(entt::registry& reg, EditorContext& ctx);
    const std::vector<hier::Row>& VisibleRows() const { return m_rows; }
    // 選択（複数）の子孫まで含めて開く / 閉じる
    void SetSubtreeOpen(entt::entity root, bool open);

    // 展開状態は自前で持つ。ImGui 内部の storage だけだと「開いている行だけを
    // 平坦化した行リスト」が作れず、ListClipper で間引けないため。
    std::unordered_set<entt::entity> m_openNodes;
    uint32_t m_openVersion = 0;   // m_openNodes が変わったら進める（表示行のキャッシュ鍵）

    // ---- モデル（キャッシュ）----
    hier::Index m_index;                       // 親→子の索引（CSR）
    uint32_t    m_structVersion = 0;           // 索引を作り直すたびに進める
    uint32_t    m_localVersion  = 0;           // このパネルの操作（親替え・並べ替え・改名）で進める（指紋に混ぜる）
    uint32_t    m_frame = 0;
    bool        m_dirty = false;               // 描画中に古い行（無効なエンティティ）を見つけた → 次のフレームで作り直す
    bool        m_prefsLoaded = false;         // 型チップの選択（prefs）を読み込んだか
    std::vector<hier::Row> m_rows;             // ツリー表示の可視行（開いてる親の子だけ）
    uint64_t    m_rowsKey = 0;
    std::vector<hier::Row> m_allRows;          // フィルタ用: 全展開した並び（検索結果の元）
    std::vector<hier::Row> m_filterRows;       // フィルタ中の一致行（ツリー順。depth は 0）
    uint64_t    m_filterKey = 0;
    bool        m_filterActive = false;

    // 検索
    char        m_filterBuf[128] = {};
    std::string m_filterPrev;
    hier::Query m_query;

    // Shift+クリックの範囲選択の起点（通常クリック / Ctrl+クリックで更新、Shift では動かさない）
    entt::entity m_selectAnchor = entt::null;
    // ビューポートや MCP で選択が変わったとき、その行を「見える状態」にするための追跡。
    // 祖先を開いてスクロールする＝グループの中に入っている物を探し回らなくて済む。
    entt::entity m_lastRevealed   = entt::null;
    entt::entity m_scrollToEntity = entt::null;
    // 複数選択の1行を押した状態。ドラッグせずに離したらこの行の単独選択に確定する
    // （押した瞬間に潰すと、まとめてドラッグ移動できなくなるため）
    entt::entity m_clickPendingEntity = entt::null;
    // ゆっくり 2 回クリック = 改名（エクスプローラ流）
    entt::entity m_lastClickEntity = entt::null;
    double       m_lastClickTime   = -10.0;
    entt::entity m_slowRenameEntity = entt::null;   // 離した時に改名を始める行（ドラッグしなかった場合だけ）

    // リネーム
    entt::entity m_renamingEntity = entt::null;
    char m_renameBuf[128] = {};
    int  m_renameWarmup = 0;  // フォーカス安定まで数フレーム待つ

    Scene*      m_scene = nullptr;
    std::string m_assetsDir;
};

} // namespace dx12e
