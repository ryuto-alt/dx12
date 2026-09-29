#pragma once

// ===== 起動画面（スプラッシュ）に流す Tips =====
// すべて「実在するエディタ機能」だけ。キーは src/editor/EditorCommandTable.h の表と一致させてある
// （キーを変えたらここの文言も直すこと）。文体は標準語の丁寧語または体言止め。1 行 40 文字前後（UTF-8）。
// 各行の直前コメントは安定 ID（並べ替え・削除しても他の Tips を指さない）。
//
// 裏取り元（要点）:
//   ショートカット全般 ... src/editor/EditorCommandTable.h（kCommands / kMouseHelp）
//   ギズモのスナップ   ... src/editor/panels/SceneViewPanel.cpp（Ctrl 押下 or snapAlways）
//   太陽を回す L       ... src/editor/LightHandles.cpp（L 単独。Ctrl/Alt/Shift を握っていると無効）
//   ライティング       ... src/editor/LightingPresets.h（day / dusk / night / indoor / horror / studio）
//   地形 / スカルプト  ... src/editor/panels/TerrainPanel.cpp / SculptPanel.cpp
//   ツール窓の一覧     ... src/editor/ToolWindows.h
//   MCP                ... src/editor/panels/McpBridgePanel.cpp / docs/MCP.md
//   Lua                ... src/scripting/ScriptEngine.cpp（ReloadChangedScripts）/ src/editor/panels/ConsolePanel.cpp
//   自動退避           ... src/core/Application.cpp（UpdateAutosave: 60 秒 / 未保存のときだけ）

#include <cstddef>

inline constexpr const char* kSplashTips[] = {
    // tip.palette
    "Ctrl+K でコマンドパレット。機能を名前で検索して実行できます",
    // tip.quickopen
    "Ctrl+P でエンティティやアセットへ素早くジャンプできます",
    // tip.save
    "Ctrl+S で保存、Ctrl+Shift+S で名前を付けて保存します",
    // tip.undo
    "Ctrl+Z で元に戻す、Ctrl+Y か Ctrl+Shift+Z でやり直し",
    // tip.rename
    "F2 で選択中のエンティティの名前を変更できます",
    // tip.play
    "F5 で Play の開始と停止、Shift+F5 で停止だけ行えます",
    // tip.pause
    "Play 中に F1 を押すと、時間だけ止めてシーンを動かせます",
    // tip.fill
    "暗いシーンは Shift+F2 の照らし込みで見えます。ゲームには影響なし",
    // tip.gizmo
    "W / E / R で移動・回転・拡大縮小のギズモを切り替え",
    // tip.gizmospace
    "T でギズモのローカル / ワールド空間を切り替えられます",
    // tip.snap
    "ギズモは Ctrl を押しながらドラッグするとスナップします",
    // tip.focus
    "F で選択したエンティティへカメラを寄せられます",
    // tip.fly
    "` キーでキーボードフライ。WASD と Q E で飛び回れます",
    // tip.rightdrag
    "右ドラッグ + WASD でフライカメラ。ホイールで速度を調整",
    // tip.orbit
    "Alt + 左ドラッグで、選択を中心にカメラを回せます",
    // tip.sun
    "L を押しながらマウスを動かすと、太陽の向きを直接回せます",
    // tip.duplicate
    "Ctrl+D で複製、Ctrl+C / Ctrl+V でコピーと貼り付け",
    // tip.group
    "ヒエラルキーで Ctrl+G を押すと、選択をグループ化します",
    // tip.lightingPresets
    "ライティング窓のプリセットは昼・夕暮れ・夜・屋内・ホラー・スタジオ",
    // tip.terrainBrush
    "地形ツールのブラシで、山を盛る・削る・ならす・浸食できます",
    // tip.terrainDice
    "地形ツールの「サイコロ」で、シードを変えて山を一発生成",
    // tip.brushMods
    "ブラシは Shift で逆方向、Ctrl で一時的にならす操作です",
    // tip.sculpt
    "スカルプトで洞窟・アーチ・岩など、ブロック以外の形を彫れます",
    // tip.tools
    "ナビメッシュやパーティクルなどの制作ツールは「ツール」メニューから",
    // tip.mcp
    "MCP で Claude Code や Codex からエディタを操作できます",
    // tip.build
    "メニュー「ツール」の「ビルド」で、ゲームを配布用に書き出せます",
    // tip.luaReload
    "Lua スクリプトは保存するだけで再読み込み。Play も止まりません",
    // tip.luaConsole
    "コンソール下部の入力欄で、Lua を 1 行その場で実行できます",
    // tip.autosave
    "変更のあるシーンは 60 秒ごとに自動退避され、次回に復旧を確認します",
    // tip.resetLayout
    "配置が崩れたら、コマンドパレットの「レイアウトをリセット」で戻せます",
};

inline constexpr std::size_t kSplashTipCount = sizeof(kSplashTips) / sizeof(kSplashTips[0]);
