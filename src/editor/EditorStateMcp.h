#pragma once

// ===========================================================================
// エディタの状態を JSON で読む（dx12_editor_state の部品。M7）
// ---------------------------------------------------------------------------
// EditorContext と ImGui のコンテキストだけを読む（何も書かない）。エンジン全体（Application）が要る項目
// （Play モード・シーン・カメラ・perf）は core/mcp/ApplicationMcpEditorUi.cpp が足す。
// ImGui のコンテキストが無い（ヘッドレス / テスト）ときは、ImGui 由来の項目が空で返る。
// ===========================================================================

#include <nlohmann/json.hpp>

#include <entt/entt.hpp>

#include <string>
#include <vector>

namespace dx12e
{
class EditorContext;
}

namespace dx12e::edstate
{

struct ModalEntry
{
    std::string kind;        // "imgui-modal" | "editor-dialog" | "palette" | "popup"
    std::string id;          // new_scene / save_as / new_script / new_shader / unsaved_confirm / autosave_recovery / command_palette …
    std::string title;
    std::string source;      // "imgui" | "editor"
    bool        blocking = true;
    bool        canDismiss = true;
    std::string dismissHint;
};

// 開いているモーダル / ダイアログ / パレット / ポップアップ（ImGui のポップアップスタック + EditorContext のダイアログ要求）。
std::vector<ModalEntry> CollectModals(const EditorContext& ctx);
// 入力を奪っているもの（モーダル / ダイアログ / パレット）が 1 つでもあるか。ポップアップ（メニュー等）は数えない。
bool IsBlocking(const std::vector<ModalEntry>& modals);
bool IsBlocking(const EditorContext& ctx);

nlohmann::json ModalJson(const EditorContext& ctx);

// いちばん上の「安全に閉じられる」モーダルを閉じる（キャンセルと同じ）。ImGui のモーダルは Esc では閉じない（ImGui の仕様）ので、AI が詰まったときの脱出口。
// 閉じてよいのは、キャンセルしても副作用が無いダイアログ（新規シーン / 名前を付けて保存 / 新規スクリプト / 新規シェーダー / ショートカット一覧 / バージョン情報）。
// 「保存 / 破棄 / 復元」の選択が要るもの・パレット・マテリアルグラフのダイアログは閉じない（reason に理由）。
struct DismissResult
{
    bool        ok = false;
    std::string id, title, reason;
};
DismissResult DismissTopModal(EditorContext& ctx);
nlohmann::json SelectionJson(const EditorContext& ctx, const entt::registry& reg);
nlohmann::json WindowsJson(const EditorContext& ctx);
nlohmann::json LayoutJson(const EditorContext& ctx, float dpiScale);
nlohmann::json UndoJson(const EditorContext& ctx);
nlohmann::json ToastsJson(size_t limit);

} // namespace dx12e::edstate
