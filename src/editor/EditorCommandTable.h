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
    // ---- ヒエラルキー: 表示 / ロック / フォルダ（フェーズ 1b。エディタ専用。ゲームには影響しない）----
    {"edit.toggleHidden","選択の表示 / 非表示を切り替え",   "Toggle Hidden Hide Show", "編集", "H",            "", "選択をエディタのビューポートで隠す / 表示する（ゲームには影響しない）", Scope::Editor, KeyMode::Typing},
    {"edit.isolate",     "選択だけを表示（他を隠す）",       "Isolate Selection Hide Others", "編集", "Shift+H", "", "選択以外をすべて隠す（もう一度で全部表示）", Scope::Editor, KeyMode::Typing},
    {"edit.showAll",     "すべて表示",                       "Show All Unhide",   "編集",     "Alt+H",        "", "非表示にした物をすべて表示する",       Scope::Editor, KeyMode::Typing},
    {"edit.toggleLocked","選択のロック / ロック解除",        "Toggle Lock",       "編集",     "Ctrl+Shift+L", "", "選択をビューポートで選べなく / 動かせなくする（ヒエラルキーからは選べる）", Scope::Editor, KeyMode::Typing},
    {"edit.newFolder",   "選択をフォルダに入れる",           "New Folder",        "編集",     "Ctrl+Shift+G", "", "選択（無ければ空）を整理用のフォルダにまとめる", Scope::Editor, KeyMode::Typing},

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
    {"view.viewportBar", "ビューポートの帯（ツールバー）を表示 / 隠す", "Viewport Toolbar Bar", "表示", "", "", "ビューポート上端のツールバーとビューキューブ（隠すと従来の 3D の矩形に戻る）", Scope::Always, KeyMode::Global},
    {"view.outline", "選択アウトラインの切り替え", "Selection Outline Highlight", "表示", "", "", "選択 / ホバーした物の輪郭（エディタ専用。ゲームの絵には写らない）", Scope::Always, KeyMode::Global},
    {"view.bookmark.jump.1", "カメラブックマーク 1 へ移動", "Camera Bookmark 1 Jump", "表示", "1", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.2", "カメラブックマーク 2 へ移動", "Camera Bookmark 2 Jump", "表示", "2", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.3", "カメラブックマーク 3 へ移動", "Camera Bookmark 3 Jump", "表示", "3", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.4", "カメラブックマーク 4 へ移動", "Camera Bookmark 4 Jump", "表示", "4", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.5", "カメラブックマーク 5 へ移動", "Camera Bookmark 5 Jump", "表示", "5", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.6", "カメラブックマーク 6 へ移動", "Camera Bookmark 6 Jump", "表示", "6", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.7", "カメラブックマーク 7 へ移動", "Camera Bookmark 7 Jump", "表示", "7", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.8", "カメラブックマーク 8 へ移動", "Camera Bookmark 8 Jump", "表示", "8", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.jump.9", "カメラブックマーク 9 へ移動", "Camera Bookmark 9 Jump", "表示", "9", "", "保存したカメラ位置へなめらかに移動", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.1", "カメラブックマーク 1 に現在の視点を保存", "Camera Bookmark 1 Save", "表示", "Ctrl+1", "", "今のカメラ位置と向きを 1 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.2", "カメラブックマーク 2 に現在の視点を保存", "Camera Bookmark 2 Save", "表示", "Ctrl+2", "", "今のカメラ位置と向きを 2 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.3", "カメラブックマーク 3 に現在の視点を保存", "Camera Bookmark 3 Save", "表示", "Ctrl+3", "", "今のカメラ位置と向きを 3 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.4", "カメラブックマーク 4 に現在の視点を保存", "Camera Bookmark 4 Save", "表示", "Ctrl+4", "", "今のカメラ位置と向きを 4 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.5", "カメラブックマーク 5 に現在の視点を保存", "Camera Bookmark 5 Save", "表示", "Ctrl+5", "", "今のカメラ位置と向きを 5 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.6", "カメラブックマーク 6 に現在の視点を保存", "Camera Bookmark 6 Save", "表示", "Ctrl+6", "", "今のカメラ位置と向きを 6 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.7", "カメラブックマーク 7 に現在の視点を保存", "Camera Bookmark 7 Save", "表示", "Ctrl+7", "", "今のカメラ位置と向きを 7 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.8", "カメラブックマーク 8 に現在の視点を保存", "Camera Bookmark 8 Save", "表示", "Ctrl+8", "", "今のカメラ位置と向きを 8 番に保存", Scope::Editor, KeyMode::Typing},
    {"view.bookmark.set.9", "カメラブックマーク 9 に現在の視点を保存", "Camera Bookmark 9 Save", "表示", "Ctrl+9", "", "今のカメラ位置と向きを 9 番に保存", Scope::Editor, KeyMode::Typing},
    // 別テーマ（B / C）への切替（ThemeVariants.h）。--theme-variant 付きで起動した時だけパレット(Ctrl+K)に出る（将来のテーマ切替機能の下地）。
    {"view.theme.default","テーマ: 既定（ネオン・エッジ）",     "Theme Variant Default Neon Edge", "表示", "",           "", "",                                     Scope::Always, KeyMode::Global},
    {"view.theme.b",     "テーマ: グラス・レイヤー（別テーマ）", "Theme Variant B Glass Layer", "表示", "",     "", "",                                     Scope::Always, KeyMode::Global},
    {"view.theme.c",     "テーマ: インク・アンド・シグナル（別テーマ）", "Theme Variant C Ink Signal", "表示", "",  "", "",                                     Scope::Always, KeyMode::Global},
    {"view.fullscreen",  "ボーダレスフルスクリーン",         "Fullscreen",        "表示",     "F11",          "", "ボーダレスフルスクリーン切り替え",     Scope::Always, KeyMode::External},

    // ---- ワークスペース / レイアウト（フェーズ 1b。実処理は editor/WorkspaceManager。ツールバーの「ワークスペース ▾」/ 表示メニュー / パレット共通）----
    {"workspace.level",     "ワークスペース: レベル編集",           "Workspace Level Editing",        "ワークスペース", "", "", "既定のレイアウト（ツール窓を閉じてビューポートを広く）",     Scope::Always, KeyMode::Global},
    {"workspace.material",  "ワークスペース: マテリアル",           "Workspace Material Shader Graph", "ワークスペース", "", "", "マテリアルエディタとマテリアルグラフを開く",           Scope::Always, KeyMode::Global},
    {"workspace.lighting",  "ワークスペース: ライティング",         "Workspace Lighting Post Process", "ワークスペース", "", "", "ライティング / ポストプロセス / 空 / SSAO を開く",         Scope::Always, KeyMode::Global},
    {"workspace.animation", "ワークスペース: アニメ・シーケンサー", "Workspace Animation Sequencer Timeline", "ワークスペース", "", "", "下部ドックを広げてタイムラインのタブを出す", Scope::Always, KeyMode::Global},
    {"workspace.vfx",       "ワークスペース: VFX",                  "Workspace VFX Particle Effect",  "ワークスペース", "", "", "パーティクルエディタを開く",                             Scope::Always, KeyMode::Global},
    {"workspace.ui",        "ワークスペース: UI",                   "Workspace UI Canvas",            "ワークスペース", "", "", "UI エディタを開く",                          Scope::Always, KeyMode::Global},
    {"layout.save",         "レイアウトを保存…",                    "Save Layout",                    "表示", "", "", "今のレイアウト（分割比・開いている窓・配置先）に名前を付けて保存", Scope::Always, KeyMode::Global},
    {"layout.slots",        "ツール窓の配置先…",                    "Tool Window Placement Dock Split Bottom Float", "表示", "", "", "ツール窓を右カラム / 右分割 / 下部ドック / フローティングのどこに開くかを選ぶ", Scope::Always, KeyMode::Global},
    {"layout.bottomMaximize","下部ドックを最大化 / 元に戻す",      "Maximize Bottom Dock",           "表示", "", "", "下部ドックのタブ帯のダブルクリックでも切り替わる",       Scope::Always, KeyMode::Global},

    // ---- 再生 ----
    {"play.toggle",      "再生 / 停止",                      "Play Stop",         "再生",     "F5",           "", "Play の開始 / 停止",                   Scope::Always, KeyMode::Global},
    {"play.stop",        "停止",                             "Stop",              "再生",     "Shift+F5",     "", "Play を停止してエディタへ戻る",        Scope::Always, KeyMode::Global},
    {"play.pause",       "一時停止 / 再開",                  "Pause Resume",      "再生",     "F1",           "", "Play 中に時間だけ止めてシーンビューを動かす", Scope::Always, KeyMode::External},

    // ---- コマンド ----
    {"palette.commands", "コマンドパレット",                 "Command Palette",   "コマンド", "Ctrl+K",       "", "コマンドを検索して実行",               Scope::Editor, KeyMode::Global},
    {"palette.quickOpen","クイックオープン（エンティティ / アセット）", "Quick Open Go To", "コマンド", "Ctrl+P", "", "エンティティ / アセットへジャンプ", Scope::Editor, KeyMode::Global},

    // ---- ノードグラフ（サンドボックス窓 = マテリアルグラフ G0 が開いているとき。edit.* の Ctrl+Z/C/V/D/Del/F は同じ表を窓が使う）----
    // Panel = 窓がフォーカスされているときに窓自身が処理する（ProcessShortcuts は見ない）。パレット(Ctrl+K)からは常に実行できる。
    {"graph.selectAll",   "ノード: すべて選択",           "Graph Select All",       "ノードグラフ", "Ctrl+A",       "", "ノードとコメントをすべて選択",   Scope::Always, KeyMode::Panel},
    {"graph.frameAll",    "ノード: 全体を表示",           "Graph Frame All",        "ノードグラフ", "Home",         "", "グラフ全体が収まるように表示",   Scope::Always, KeyMode::Panel},
    {"graph.palette",     "ノード: 検索パレットを開く",   "Graph Node Palette",     "ノードグラフ", "Space",        "", "ノード検索パレット（右クリックでも開く）", Scope::Always, KeyMode::Panel},
    {"graph.comment",     "ノード: コメントで囲む",       "Graph Comment Box",      "ノードグラフ", "C",            "", "選択ノードをコメントで囲む（未選択なら空のコメント）", Scope::Always, KeyMode::Panel},
    {"graph.selectUpstream",  "ノード: 上流を選択",       "Graph Select Upstream",  "ノードグラフ", "Alt+Left",     "", "選択ノードの上流をすべて選択",   Scope::Always, KeyMode::Panel},
    {"graph.selectDownstream","ノード: 下流を選択",       "Graph Select Downstream","ノードグラフ", "Alt+Right",    "", "選択ノードの下流をすべて選択",   Scope::Always, KeyMode::Panel},
    {"graph.alignLeft",   "ノード: 左揃え",               "Graph Align Left",       "ノードグラフ", "Shift+Alt+A",  "", "",                               Scope::Always, KeyMode::Panel},
    {"graph.alignRight",  "ノード: 右揃え",               "Graph Align Right",      "ノードグラフ", "Shift+Alt+D",  "", "",                               Scope::Always, KeyMode::Panel},
    {"graph.alignTop",    "ノード: 上揃え",               "Graph Align Top",        "ノードグラフ", "Shift+Alt+W",  "", "",                               Scope::Always, KeyMode::Panel},
    {"graph.alignBottom", "ノード: 下揃え",               "Graph Align Bottom",     "ノードグラフ", "Shift+Alt+S",  "", "",                               Scope::Always, KeyMode::Panel},
    {"graph.alignCenterH","ノード: 水平中央揃え",         "Graph Align Center H",   "ノードグラフ", "Shift+Alt+Z",  "", "",                               Scope::Always, KeyMode::Panel},
    {"graph.alignCenterV","ノード: 垂直中央揃え",         "Graph Align Center V",   "ノードグラフ", "Shift+Alt+X",  "", "",                               Scope::Always, KeyMode::Panel},
    {"graph.distributeH", "ノード: 水平に等間隔",         "Graph Distribute H",     "ノードグラフ", "Shift+Alt+H",  "", "3 個以上を等間隔に",             Scope::Always, KeyMode::Panel},
    {"graph.distributeV", "ノード: 垂直に等間隔",         "Graph Distribute V",     "ノードグラフ", "Shift+Alt+V",  "", "3 個以上を等間隔に",             Scope::Always, KeyMode::Panel},
    {"graph.resetZoom",   "ノード: 表示を 100% に",       "Graph Reset Zoom",       "ノードグラフ", "Ctrl+0",       "", "ズームを 100% に戻す",           Scope::Always, KeyMode::Panel},
    {"graph.disconnectSelected", "ノード: 選択ノードのワイヤを切断", "Graph Disconnect Selected", "ノードグラフ", "", "", "", Scope::Always, KeyMode::Global},
    {"graph.toggleSnap",  "ノード: グリッドスナップの切替", "Graph Toggle Snap",    "ノードグラフ", "",             "", "",                               Scope::Always, KeyMode::Global},
    {"graph.toggleMinimap","ノード: ミニマップの切替",    "Graph Toggle Minimap",   "ノードグラフ", "",             "", "",                               Scope::Always, KeyMode::Global},
    {"graph.toggleGrid",  "ノード: グリッドの切替",       "Graph Toggle Grid",      "ノードグラフ", "",             "", "",                               Scope::Always, KeyMode::Global},
    {"graph.toggleFlow",  "ノード: ワイヤの流れの切替",   "Graph Toggle Wire Flow", "ノードグラフ", "",             "", "",                               Scope::Always, KeyMode::Global},

    // ---- マテリアルグラフ窓（G3。窓が開いているときだけ有効）----
    // Ctrl+S / Ctrl+O / Ctrl+N / Ctrl+Shift+S は上の file.* の同じキー。マテリアルグラフ窓にフォーカスがあるときだけ、
    // シーンではなくグラフの保存 / 開く / 新規 / 名前を付けて保存へ振り向ける（EditorCommands.cpp の Execute。キーは重複させない）。
    {"matgraph.new",      "マテリアルグラフ: 新規（空）",         "Material Graph New",        "マテリアルグラフ", "",           "", "出力ノードだけの空のグラフを作る（Ctrl+N）", Scope::Always, KeyMode::Global},
    {"matgraph.newPbr",   "マテリアルグラフ: 新規（標準 PBR）",   "Material Graph New PBR",    "マテリアルグラフ", "",           "", "ベースカラー・ラフネス・ノーマルを持つ標準構成で新規作成", Scope::Always, KeyMode::Global},
    {"matgraph.open",     "マテリアルグラフ: 開く…",              "Material Graph Open",       "マテリアルグラフ", "",           "", ".dxmg を開く（Ctrl+O）",                  Scope::Always, KeyMode::Global},
    {"matgraph.save",     "マテリアルグラフ: 保存",               "Material Graph Save",       "マテリアルグラフ", "",           "", "グラフを保存（Ctrl+S）",                  Scope::Always, KeyMode::Global},
    {"matgraph.saveAs",   "マテリアルグラフ: 名前を付けて保存…",  "Material Graph Save As",    "マテリアルグラフ", "",           "", "グラフを別名で保存（Ctrl+Shift+S）",      Scope::Always, KeyMode::Global},
    {"matgraph.recompile","マテリアルグラフ: HLSL を再生成",      "Material Graph Recompile",  "マテリアルグラフ", "Ctrl+Enter", "", "グラフを検証して HLSL を作り直す",         Scope::Always, KeyMode::Panel},
    {"matgraph.nextDiag", "マテリアルグラフ: 次の診断へ移動",     "Material Graph Next Diagnostic", "マテリアルグラフ", "F8",   "", "診断リストの次の項目のノードへ移動",      Scope::Always, KeyMode::Panel},
    {"matgraph.copyHlsl", "マテリアルグラフ: 生成 HLSL をコピー", "Material Graph Copy HLSL",  "マテリアルグラフ", "",           "", "生成された HLSL をクリップボードへ",       Scope::Always, KeyMode::Global},
    {"matgraph.promote",  "マテリアルグラフ: 選択をパラメータに昇格", "Material Graph Promote Parameter", "マテリアルグラフ", "", "", "定数 / テクスチャサンプルをパラメータへ", Scope::Always, KeyMode::Global},
    {"matgraph.sample",   "マテリアルグラフ: 見本を読み込む",     "Material Graph Sample",     "マテリアルグラフ", "",           "", "テクスチャ x 色 / ノイズのラフネス / ノーマル / フレネルの見本グラフ", Scope::Always, KeyMode::Global},
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
