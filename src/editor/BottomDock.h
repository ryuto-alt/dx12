#pragma once

// ===== 下部ドックのスロット（中央下。アセットブラウザ / コンソールと同じタブ群）=====
// 「後からタブを足せる」ための登録口。シーケンサーのタイムラインなど、下部に常駐したいパネルは
// 自分の場所（例 src/sequencer 側の初期化）から Register を 1 回呼ぶだけでよい:
//
//     #include "editor/BottomDock.h"
//     dx12e::bottomdock::Register({
//         "timeline",                              // id（一意。ワークスペース / レイアウトの保存キー）
//         "タイムライン",                          // タブの表示名
//         ICON_FILM,                               // アイコン（空文字可）
//         [](dx12e::EditorContext& ctx) {          // 中身の描画（開いている間だけ毎フレーム呼ばれる。Begin/End は不要）
//             ImGui::TextUnformatted("...");
//         }});
//     dx12e::bottomdock::SetOpen("timeline", true);   // 開く（既に登録済みで閉じている場合）
//
//   ・同じ id を再登録すると置き換わる（開閉状態は保つ）。EditorLayer が「タイムライン（ダミー）」を登録済み＝
//     本物を同じ id "timeline" で登録すれば、ダミーがそのまま本物に入れ替わる。
//   ・窓の実体は EditorLayer が Begin/End する（ImGui 名 "<title>###bottom.<id>"）。既定レイアウトが中央下ノードへドックする。
//   ・タブの並び順・最後に選んだタブ・開閉はレイアウト（WorkspaceLogic.h の Layout::bottomTabs / bottomActive）として
//     ワークスペース / 名前つきレイアウトに保存される。
//   ・タブ帯のダブルクリックで下部ドックを最大化 / 元に戻す（ビューポートには下限が残る）。
// 依存: EditorContext は前方宣言だけ（ヘッダは軽い）。

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dx12e
{
class EditorContext;

namespace bottomdock
{

struct Tab
{
    std::string id;
    std::string title;
    std::string icon;                                   // ICON_*（空なら無し）
    std::function<void(EditorContext&)> draw;
    bool open = false;                                  // 開いているか（登録時は閉じた状態）
};

inline std::vector<Tab>& Tabs()
{
    static std::vector<Tab> t;
    return t;
}

inline Tab* Find(const std::string& id)
{
    for (Tab& t : Tabs()) if (t.id == id) return &t;
    return nullptr;
}

// 登録（同じ id は置き換え。開閉状態は保つ）。
inline void Register(Tab tab)
{
    if (tab.id.empty()) return;
    if (Tab* old = Find(tab.id))
    {
        tab.open = old->open;
        *old = std::move(tab);
        return;
    }
    Tabs().push_back(std::move(tab));
}

inline bool IsOpen(const std::string& id)
{
    const Tab* t = Find(id);
    return t && t->open;
}

inline void SetOpen(const std::string& id, bool on)
{
    if (Tab* t = Find(id)) t->open = on;
}

// ImGui 上の窓名。### の後ろが固定 ID なので、表示名を変えても保存位置 / ドック割り当てが壊れない。
inline std::string WindowName(const Tab& t)
{
    return (t.icon.empty() ? std::string() : t.icon + " ") + t.title + "###bottom." + t.id;
}

} // namespace bottomdock
} // namespace dx12e
