#pragma once

// ===== エディタのコマンド表（ショートカットの「唯一の正」）=====
// メニューのキー表記・ヘルプ「ショートカット一覧」・コマンドパレット(Ctrl+K)・実際のキー処理は
// すべてこの表から作る。**キーを足す / 変える時はここだけを直す**。
//   ・表示ラベルとキー処理が別管理だったせいで「メニューに Ctrl+O と書いてあるのに効かない」が
//     起きていた。表を 1 つにすれば構造的に食い違わない。
//   ・純データ（ImGui にも EditorContext にも依存しない）。tests/editor_ux_test.cpp が
//     「id の重複無し / キーの衝突無し / 全コマンドにラベル」を単体で検査する。
//   ・実行内容は editor/EditorCommands.cpp の Execute(id)。ウィンドウの開閉コマンド(window.*)と
//     エンティティ作成コマンド(create.*)は ToolWindows.h / EditorCreateTable.h の表から自動生成する。
//
// キー表記: "Ctrl+Shift+S" のように 修飾(Ctrl / Shift / Alt) と キー名を + でつなぐ。
//   キー名: A-Z 0-9 F1-F12 Esc Del Enter Tab Space Grave(`) Up Down Left Right
// chord2 は別名（Ctrl+Y と Ctrl+Shift+Z のように 2 通りで動かしたいとき）。

#include <cstdint>
#include <string>
#include <string_view>
#include <cctype>

namespace dx12e::cmd
{

// どの状態で有効か
enum class Scope : uint8_t
{
    Always,   // Editor でも Play 中でも（Play の開始/停止など）
    Editor,   // Play 中は無効（シーンを触る操作）
};

// キー入力の扱い
enum class KeyMode : uint8_t
{
    Global,   // テキスト入力中も効く（Ctrl+S / F5 など。確認モーダルが開いている間は効かない）
    Typing,   // テキスト入力中は効かない（W/E/R/T・F・Esc・Ctrl+Z など）
    Panel,    // フォーカス中のパネルが自前で処理する（Del / Ctrl+G）。ヘルプとパレットには載る
    External, // エンジンの別の場所で処理する（F1 一時停止 / F11 全画面）。ヘルプとパレットには載る
};

struct Def
{
    const char* id;        // "file.save"
    const char* label;     // 日本語ラベル（メニュー / パレット）
    const char* labelEn;   // 検索用の英語別名（空文字可。ファジー検索の対象）
    const char* category;  // "ファイル" "編集" "表示" "再生" "コマンド"
    const char* chord;     // "Ctrl+S"（無ければ ""）
    const char* chord2;    // 別名（無ければ ""）
    const char* help;      // ヘルプ一覧に出す説明（空なら label）
    Scope       scope;
    KeyMode     mode;
};

// アイコンはこの表に持たない（ヘッダを ImGui 非依存に保つため）。EditorCommands.cpp の IconFor(id) が引く。

inline constexpr Def kCommands[] = {
    // ---- ファイル ----
    {"file.new",         "新規シーン",                       "New Scene",         "ファイル", "Ctrl+N",       "", "新規シーンを作る",                     Scope::Editor, KeyMode::Global},
    {"file.open",        "シーンを開く",                     "Open Scene",        "ファイル", "Ctrl+O",       "", "シーンを開く（ファイルを選ぶ）",       Scope::Editor, KeyMode::Global},
    {"file.save",        "保存",                             "Save",              "ファイル", "Ctrl+S",       "", "シーンを保存（Play 中は保存せず通知だけ出す）", Scope::Always, KeyMode::Global},
    {"file.saveAs",      "名前を付けて保存",                 "Save As",           "ファイル", "Ctrl+Shift+S", "", "名前を付けてシーンを保存",             Scope::Editor, KeyMode::Global},
    {"file.newScript",   "新規スクリプト",                   "New Script Lua",    "ファイル", "Ctrl+L",       "", "新しい Lua スクリプトを作る",           Scope::Editor, KeyMode::Global},
    {"file.newShader",   "新規シェーダー",                   "New Shader HLSL",   "ファイル", "",             "", "",                                     Scope::Editor, KeyMode::Global},
    {"file.closeProject","プロジェクトを閉じる（ランチャーに戻る）", "Close Project", "ファイル", "",        "", "",                                     Scope::Editor, KeyMode::Global},

    // ---- 編集 ----
    {"edit.undo",        "元に戻す",                         "Undo",              "編集",     "Ctrl+Z",       "", "元に戻す",                             Scope::Editor, KeyMode::Typing},
    {"edit.redo",        "やり直す",                         "Redo",              "編集",     "Ctrl+Y",       "Ctrl+Shift+Z", "やり直す",                 Scope::Editor, KeyMode::Typing},
    {"edit.copy",        "コピー",                           "Copy",              "編集",     "Ctrl+C",       "", "選択をコピー",                         Scope::Editor, KeyMode::Typing},
    {"edit.paste",       "貼り付け",                         "Paste",             "編集",     "Ctrl+V",       "", "貼り付け",                             Scope::Editor, KeyMode::Typing},
    {"edit.duplicate",   "複製",                             "Duplicate",         "編集",     "Ctrl+D",       "", "選択を複製",                           Scope::Editor, KeyMode::Typing},
    {"edit.delete",      "削除",                             "Delete",            "編集",     "Del",          "", "削除（フォーカス中のパネルだけが反応）", Scope::Editor, KeyMode::Panel},
    {"edit.rename",      "名前を変更",                       "Rename",            "編集",     "F2",           "", "選択エンティティの名前を変更",         Scope::Editor, KeyMode::Typing},
    {"edit.group",       "選択をグループ化",                 "Group",             "編集",     "Ctrl+G",       "", "選択をグループ化（ヒエラルキーにフォーカス時）", Scope::Editor, KeyMode::Panel},
    {"edit.selectNone",  "選択を解除",                       "Deselect",          "編集",     "Esc",          "", "選択解除（フライ中は解除 / Play 一時停止中は停止）", Scope::Always, KeyMode::Typing},
    {"edit.focus",       "選択にフォーカス",                 "Focus Frame",       "編集",     "F",            "", "選択エンティティへカメラを寄せる",     Scope::Editor, KeyMode::Typing},

    // ---- 表示 ----
    {"view.gizmoMove",   "移動ギズモ",                       "Translate Gizmo",   "表示",     "W",            "", "ギズモ: 移動",                         Scope::Editor, KeyMode::Typing},
    {"view.gizmoRotate", "回転ギズモ",                       "Rotate Gizmo",      "表示",     "E",            "", "ギズモ: 回転",                         Scope::Editor, KeyMode::Typing},
    {"view.gizmoScale",  "拡大縮小ギズモ",                   "Scale Gizmo",       "表示",     "R",            "", "ギズモ: 拡大縮小",                     Scope::Editor, KeyMode::Typing},
    {"view.gizmoSpace",  "ローカル / ワールド空間を切り替え", "Local World Space", "表示",     "T",            "", "ギズモの空間を切り替え",               Scope::Editor, KeyMode::Typing},
    {"view.fill",        "編集用の照らし込み",               "Viewport Fill Light", "表示",   "Shift+F2",     "", "暗いシーンを見るための光（ゲームには影響なし）", Scope::Editor, KeyMode::Typing},
    {"view.flyMode",     "キーボードフライの切り替え",       "Keyboard Fly",      "表示",     "Grave",        "", "` キーでキーボードフライ（WASD / Q E / 矢印）", Scope::Editor, KeyMode::Typing},
    {"view.toggle2D",    "2D / 3D ビューを切り替え",         "2D 3D View",        "表示",     "",             "", "",                                     Scope::Editor, KeyMode::Global},
    {"view.resetLayout", "レイアウトをリセット",             "Reset Layout",      "表示",     "",             "", "",                                     Scope::Always, KeyMode::Global},
    {"view.closeTools",  "ツール窓をすべて閉じる",           "Close All Tool Windows", "表示", "",            "", "",                                     Scope::Always, KeyMode::Global},
    // 開発用: エディタのアイデンティティ案の切替（ThemeVariants.h）。パレット(Ctrl+K)からだけ。案が決まったら表ごと消す。
    {"view.theme.default","テーマ案: 現行",                 "Theme Variant Default", "表示", "",           "", "",                                     Scope::Always, KeyMode::Global},
    {"view.theme.a",     "テーマ案 A: ネオン・エッジ",       "Theme Variant A Neon Edge",   "表示", "",     "", "",                                     Scope::Always, KeyMode::Global},
    {"view.theme.b",     "テーマ案 B: グラス・レイヤー",     "Theme Variant B Glass Layer", "表示", "",     "", "",                                     Scope::Always, KeyMode::Global},
    {"view.theme.c",     "テーマ案 C: インク・アンド・シグナル", "Theme Variant C Ink Signal", "表示", "",  "", "",                                     Scope::Always, KeyMode::Global},
    {"view.fullscreen",  "ボーダレスフルスクリーン",         "Fullscreen",        "表示",     "F11",          "", "ボーダレスフルスクリーン切り替え",     Scope::Always, KeyMode::External},

    // ---- 再生 ----
    {"play.toggle",      "再生 / 停止",                      "Play Stop",         "再生",     "F5",           "", "Play の開始 / 停止",                   Scope::Always, KeyMode::Global},
    {"play.stop",        "停止",                             "Stop",              "再生",     "Shift+F5",     "", "Play を停止してエディタへ戻る",        Scope::Always, KeyMode::Global},
    {"play.pause",       "一時停止 / 再開",                  "Pause Resume",      "再生",     "F1",           "", "Play 中に時間だけ止めてシーンビューを動かす", Scope::Always, KeyMode::External},

    // ---- コマンド ----
    {"palette.commands", "コマンドパレット",                 "Command Palette",   "コマンド", "Ctrl+K",       "", "コマンドを検索して実行",               Scope::Editor, KeyMode::Global},
    {"palette.quickOpen","クイックオープン（エンティティ / アセット）", "Quick Open Go To", "コマンド", "Ctrl+P", "", "エンティティ / アセットへジャンプ", Scope::Editor, KeyMode::Global},
};
inline constexpr size_t kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

// マウス操作など「コマンドではないがヘルプに載せたい」もの（ショートカット一覧の下半分）。
struct MouseHelp { const char* key; const char* desc; };
inline constexpr MouseHelp kMouseHelp[] = {
    {"左クリック",        "エンティティ選択（Ctrl+クリックで複数選択）"},
    {"右クリック",        "ビューポート: 選択対象のメニュー / ヒエラルキー: 行のメニュー・空白で作成"},
    {"右ドラッグ + WASD", "フライカメラ移動（Space / Shift で上下、ホイールで速度）"},
    {"Alt + 左ドラッグ",  "選択を中心にオービット"},
    {"L + マウス移動",    "太陽（DirectionalLight）の向きを直接回す"},
};

// ---- キー表記のパース（純関数）----
struct ChordSpec
{
    bool ctrl = false, shift = false, alt = false;
    std::string key;     // 正規化したキー名（大文字。"S" "F5" "ESC" "DEL" "GRAVE"）
    bool valid = false;

    // 重複検査用の正規形 "CTRL+SHIFT+S"
    std::string Normalized() const
    {
        std::string s;
        if (ctrl)  s += "CTRL+";
        if (shift) s += "SHIFT+";
        if (alt)   s += "ALT+";
        return s + key;
    }
};

inline ChordSpec ParseChordSpec(std::string_view text)
{
    ChordSpec c;
    if (text.empty()) return c;
    size_t i = 0;
    while (i <= text.size())
    {
        size_t j = text.find('+', i);
        if (j == std::string_view::npos) j = text.size();
        std::string tok(text.substr(i, j - i));
        for (auto& ch : tok) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        i = j + 1;
        if (tok.empty()) { if (j >= text.size()) break; continue; }
        if (tok == "CTRL" || tok == "CONTROL") c.ctrl = true;
        else if (tok == "SHIFT") c.shift = true;
        else if (tok == "ALT") c.alt = true;
        else if (c.key.empty()) c.key = tok;
        else return ChordSpec{};   // キー名が 2 つ
        if (j >= text.size()) break;
    }
    // "Esc" "Escape" "Delete" "Del" の別名を寄せる
    if (c.key == "ESCAPE") c.key = "ESC";
    if (c.key == "DELETE") c.key = "DEL";
    c.valid = !c.key.empty();
    return c;
}

// メニュー右側に出すキー表記（chord と chord2 を " / " でつなぐ。無ければ空）。
inline std::string ChordLabel(const Def& d)
{
    std::string s = d.chord ? d.chord : "";
    if (d.chord2 && d.chord2[0]) { if (!s.empty()) s += " / "; s += d.chord2; }
    // 表示は "Grave" ではなく "`" に
    const auto pos = s.find("Grave");
    if (pos != std::string::npos) s.replace(pos, 5, "`");
    return s;
}

inline const Def* FindCommand(std::string_view id)
{
    for (const Def& d : kCommands)
        if (id == d.id) return &d;
    return nullptr;
}

} // namespace dx12e::cmd
