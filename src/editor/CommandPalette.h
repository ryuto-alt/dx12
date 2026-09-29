#pragma once

// ===== コマンドパレット (Ctrl+K) とクイックオープン (Ctrl+P) =====
// 画面中央上に出るモーダルの検索窓（UE5 / VS Code 風）。
//   Ctrl+K : コマンドを検索して実行（メニュー全項目・ウィンドウの開閉・エンティティ作成・再生など）。
//   Ctrl+P : シーン内のエンティティ / プロジェクトのアセットへジャンプ。
//   先頭の記号で対象を切り替え: ">" コマンド / "@" エンティティ / "#" アセット。
//   ↑↓ で選択、Enter で実行、Esc で閉じる。検索はファジー（日本語 / 英語・ひらがな⇔カタカナ・
//   全角半角を区別しない）で、空欄のときは「最近使った順」に並べる。
//
// コマンドの正は EditorCommandTable.h / ToolWindows.h / EditorCreateTable.h。ここは表を検索して
// cmd::Execute を呼ぶだけ（実行内容は持たない）。

#include "editor/EditorCommands.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <entt/entt.hpp>
#pragma warning(pop)

namespace dx12e
{

class EditorContext;
class AssetBrowserPanel;

class CommandPalette
{
public:
    // 毎フレーム。ctx.paletteRequest が立っていれば開き、開いている間は自前で描いてキーを処理する。
    void Render(EditorContext& ctx, const cmd::Env& env, entt::registry& reg, AssetBrowserPanel* assets);

    bool IsOpen() const { return m_open; }

private:
    enum class Mode : uint8_t { Commands, QuickOpen };
    enum class Source : uint8_t { Commands, Entities, Assets, All };   // 記号で切り替わる検索対象

    struct Item
    {
        enum class Kind : uint8_t { Command, Entity, Asset } kind = Kind::Command;
        std::string       id;         // Command: コマンド id / Asset: 絶対パス
        entt::entity      entity = entt::null;
        std::string       label;      // 主表示
        std::string       sub;        // 補足（カテゴリ / 親階層 / 相対パス）
        std::string       chord;      // 右端のキー表記
        const char*       icon = "";
        int               tint = 0;   // 0=dim 1=Light 2=Camera 3=Script 4=Audio 5=Folder 6=Scene 7=UI 8=Physics
        int               score = 0;
        bool              enabled = true;
        std::vector<uint32_t> hl;     // label の一致位置（UTF-8 バイト）
    };
    struct AssetRec { std::string abs, rel, name; int type = 0; bool isScene = false; };

    void Open(Mode mode, EditorContext& ctx);
    void Close(EditorContext& ctx);
    void Rebuild(EditorContext& ctx, entt::registry& reg);
    void BuildCommandItems(EditorContext& ctx, const std::string& q, std::vector<Item>& out);
    void BuildEntityItems(entt::registry& reg, const std::string& q, std::vector<Item>& out);
    void BuildAssetItems(const std::string& q, std::vector<Item>& out);
    void EnsureAssetIndex(AssetBrowserPanel* assets);
    void Activate(EditorContext& ctx, const cmd::Env& env, entt::registry& reg, AssetBrowserPanel* assets, const Item& it);
    void Touch(std::vector<std::string>& mru, const std::string& key);
    int  RecentRank(const std::vector<std::string>& mru, const std::string& key) const;

    bool  m_open = false;
    bool  m_justOpened = false;
    Mode  m_mode = Mode::Commands;
    char  m_buf[256] = {};
    std::string m_lastQuery = "\x01";      // 初回は必ず作り直す
    Source m_lastSource = Source::All;
    int   m_sel = 0;
    bool  m_scrollToSel = false;
    std::vector<Item> m_items;

    std::vector<std::string> m_recentCommands;   // 新しい順
    std::vector<std::string> m_recentAssets;     // 新しい順（絶対パス）

    std::vector<AssetRec> m_assetIndex;
    std::chrono::steady_clock::time_point m_assetIndexTime{};
    bool m_assetIndexBuilt = false;
    std::vector<std::filesystem::path> m_assetRoots;
};

} // namespace dx12e
