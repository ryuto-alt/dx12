#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Logger.h"

namespace dx12e
{
class ScriptEngine;
class EditorContext;

// エディタ内コンソール（Unity の Console + Unreal の Output Log 参考）。
// Logger のインメモリリングバッファ（全ログ = エンジン/Lua/ビルド/MCP）を表示する。
//  - 重大度トグル（情報/警告/エラー、件数バッジ付き）+ テキスト検索
//  - 折りたたみ（同一メッセージを xN 集約）/ 自動スクロール / Play時クリア / エラーで前面
//  - 行クリックで詳細ペイン（全文表示・コピー）/ 行を右クリックで全文コピー
//  - ★カスタムシェーダーのエラー行をクリックすると、そのシェーダーを使っている
//    エンティティを選び直し、Inspector の該当セクションを開いて全文をその場に出す
//    （EditorContext::revealShaderIssue 経由。詳細は resource/ShaderDiagnostics.h）
//  - 下部に Lua 即時実行行（Unreal のコンソール入力相当。scene/fx/camera 等がそのまま使える）
// アセットブラウザの隣タブに常設される（EditorLayer::BuildDefaultLayout）。
class ConsolePanel
{
public:
    // ctx は省略可（null なら Inspector へのジャンプが無効になるだけ）。
    void Render(ScriptEngine* scriptEngine, bool isPlaying, EditorContext* ctx = nullptr);

private:
    void PullNewEntries();   // Logger から新着を取り込み（新着エラーで前面化）
    void RebuildView();      // フィルタ/折りたたみを反映した表示リストを再構築
    void ClearLocal();       // 表示のクリア（ログファイル/リングは無傷）

    std::vector<LogEntry> m_entries;    // ローカルコピー（追記のみ）
    std::vector<int>      m_view;       // 表示する m_entries の添字
    std::vector<int>      m_viewCount;  // 折りたたみ時の同文件数（m_view と対）
    uint64_t              m_cursor = 0; // Logger 読み取りカーソル

    // 表示オプション（既定は警告+エラーのみ。情報は右上のトグルでいつでも表示できる）
    bool m_showInfo     = false;
    bool m_showWarn     = true;
    bool m_showError    = true;
    bool m_collapse     = false;
    bool m_autoScroll   = true;
    bool m_clearOnPlay  = true;
    bool m_focusOnError = true;
    char m_filter[128]  = "";
    char m_luaInput[512] = "";

    // Lua 予測変換（コンソール入力行）。候補は ScriptEngine::GetCompletions から。
    // Tab=確定 / ↑↓=候補選択 / クリック=挿入。m_lastQuery で入力変化を検知して再計算。
    std::vector<std::string> m_completions;
    int         m_completionSel = 0;
    std::string m_lastQuery;

    uint64_t m_selectedId   = 0;     // 詳細ペインに出すエントリ（0=なし）
    bool     m_prevPlaying  = false;
    bool     m_viewDirty    = true;
    bool     m_newThisFrame = false; // 新着あり（自動スクロール用）
    bool     m_wantFocus    = false; // 新着エラーでタブを前面へ
    int      m_cntInfo = 0, m_cntWarn = 0, m_cntError = 0;   // 全体件数（フィルタ非依存）
};

} // namespace dx12e
