add_library(Editor STATIC
    EditorLayer.cpp
    UndoSystem.cpp
    UiEditUtil.cpp
    ScenePick.cpp
    ModelThumbnailRenderer.cpp
    EditorIconRenderer.cpp
    panels/ToolbarPanel.cpp
    panels/HierarchyPanel.cpp
    panels/InspectorPanel.cpp
    panels/SceneViewPanel.cpp
    panels/AssetBrowserPanel.cpp
    panels/ConsolePanel.cpp
    panels/McpBridgePanel.cpp
    panels/VfxEditorPanel.cpp
    panels/UiEditorPanel.cpp
    panels/AnimationEditorPanel.cpp
    panels/SpriteSheetEditorPanel.cpp
    panels/NetworkPanel.cpp
    panels/PathTracerPanel.cpp   # リファレンスレンダー窓（DXR パストレーサー。パリティ基盤 Q1a）
    panels/MaterialEditorPanel.cpp
    panels/MaterialLibraryPanel.cpp
    panels/MaterialPreviewRenderer.cpp
    panels/TerrainPanel.cpp
    panels/NavMeshPanel.cpp
)

target_include_directories(Editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Ui: UiAnimRuntime（.uianim/.spranim のロードと ECS への適用）をアニメ系パネルが使う
target_link_libraries(Editor PUBLIC Core Scene ECS Renderer Gui Resource Audio Physics Ui Nav AI)

# MCP / AI Bridge パネルの接続コマンド表示用フォールバックパス。
# 通常はパネルが exe を基準に tools/mcp-server を「実行時」解決する（別 PC・フォルダ移動でも有効）。
# これはビルドしたマシンの絶対パスで、その実行時解決が失敗した場合の最終手段としてのみ使う。
target_compile_definitions(Editor PRIVATE MCP_SERVER_DIR="${CMAKE_SOURCE_DIR}/tools/mcp-server")

if(MSVC)
    target_compile_options(Editor PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(Editor PRIVATE ${DX12_COMPILE_DEFS})
endif()

# シーンビューのライティング編集（太陽ドラッグ / ライトのハンドル / ライティング・パネル）。
# 上の add_library のリストへ挿し込まず target_sources で足しているのは、同じファイルを
# 同時に触る作業とぶつからないようにするため（末尾追記だけで済む）。
target_sources(Editor PRIVATE
    LightHandles.cpp
    panels/LightingPanel.cpp
)

# 選択中の Brain（ゲーム AI）のデバッグ表示。ImGui の背景ドローリストへ描くだけ。
target_sources(Editor PRIVATE
    AiDebugOverlay.cpp
)

# 「更新内容」画面（更新後の初回起動 / コマンド「更新内容を表示」）。データは core/ReleaseNotes.h。
target_sources(Editor PRIVATE
    WhatsNewScreen.cpp
)

# 任意メッシュの頂点スカルプト窓（洞窟・アーチ・岩などの異形。地形ツールの相棒）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/SculptPanel.cpp
)

# オーディオミキサー窓（バスのメーター/フェーダー・スナップショット・鳴っている音）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/AudioMixerPanel.cpp
)

# トランジション専用プレビュー窓（架空のゲーム画面 2 枚 + プリセット全件のサムネイルアトラス）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    panels/TransitionPreviewPanel.cpp
)

# エディタ共通の見た目部品（ui:: 名前空間。PropertyGrid.h の pg:: が内部で呼ぶ）。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    UiWidgets.cpp
)

# 第2波（操作性の基盤）: コマンド表 + ショートカット処理 / コマンドパレット / トースト通知。
# 表そのもの（EditorCommandTable.h / ToolWindows.h / EditorCreateTable.h）と純ロジック
# （FuzzyMatch.h / ToastQueue.h / TransformMath.h）はヘッダオンリー。tests/editor_ux_test.cpp・
# tests/reparent_test.cpp が単体で検証する。
target_sources(Editor PRIVATE
    EditorCommands.cpp
    CommandPalette.cpp
    Toast.cpp
)

# プロジェクトランチャー（UE5 プロジェクトブラウザ風の起動後画面）。純ロジックは project/LauncherLogic.h・LauncherMotion.h。
# 上のリストへ挿し込まず target_sources の末尾追記にしている（同時編集とぶつからないように）。
target_sources(Editor PRIVATE
    LauncherScreen.cpp
)

# 第 3 波（フェーズ 1a）: エディタ専用の選択 / ホバー輪郭の後処理・ビューポート専用ツールバー・ビューキューブ。
# 純ロジックは ViewportLogic.h（tests/viewport_logic_test.cpp）。上と同じ理由で target_sources の末尾追記。
target_sources(Editor PRIVATE
    SelectionOutline.cpp
    panels/ViewportToolbar.cpp
)

# マテリアルグラフ G0: 汎用ノードグラフ UI（自前 ImDrawList 実装）+ 開発用サンドボックス窓。
# 純ロジック層（GraphLayout / GraphDocument / GraphIO / GraphPalette / Sandbox*）は ImGui 非依存で、tests/nodegraph_test.cpp が
# 同じソースを直接ビルドして検証する。上の add_library へ挿し込まず target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    nodegraph/GraphLayout.cpp
    nodegraph/GraphDocument.cpp
    nodegraph/GraphIO.cpp
    nodegraph/GraphPalette.cpp
    nodegraph/SandboxGraphModel.cpp
    nodegraph/SandboxSamples.cpp
)

# 上の純ロジック層に加え、ImGui ビュー（描画・入力・ポップアップ）とサンドボックス窓。
target_sources(Editor PRIVATE
    nodegraph/GraphView.cpp
    nodegraph/GraphViewDraw.cpp
    nodegraph/GraphViewUi.cpp
    panels/NodeGraphSandboxPanel.cpp
)

# マテリアルグラフ G3（UI 接続）: G1 の MaterialGraph を G0 の IGraphModel へ適合させるアダプタ・エディタ文書（保存 / 読込 / 診断 / コンパイル）・
# HLSL の色分け（純ロジック。ImGui 非依存。tests/matgraph_editor_test.cpp が同じソースを直接ビルドして検証する）と、マテリアルグラフ窓。
target_sources(Editor PRIVATE
    matgraph/MatGraphModel.cpp
    matgraph/MatGraphEditor.cpp
    matgraph/HlslColorize.cpp
    matgraph/NodeThumbs.cpp   # マテリアルグラフ G2c: ノード内サムネイルの管理（可視のみ・上限・変更ノードとその下流だけ更新）
)
target_link_libraries(Editor PUBLIC MatGraph)
target_sources(Editor PRIVATE
    panels/MaterialGraphPanel.cpp
    panels/MaterialGraphPanelSide.cpp
    panels/MaterialGraphPanelBottom.cpp
    panels/MaterialGraphPanelPreview.cpp   # G2c: ライブプレビュー / ノード内サムネイル / 非同期コンパイルの仕上げ
)

# フェーズ 1b W: エディタ設定の永続化（editor_state.json の "prefs"。純ロジックは EditorPrefsStore.h）。
target_sources(Editor PRIVATE
    EditorPrefs.cpp
    WorkspaceManager.cpp   # ドックの構築 / レイアウト保存 / ワークスペース / 下部ドック
)

# フェーズ 1b A: アセットブラウザ（非同期の一覧走査 + 変更監視 / テクスチャのサムネイル非同期デコード + ディスクキャッシュ）。純ロジックは AssetBrowserLogic.h。
target_sources(Editor PRIVATE
    AssetIndex.cpp
    ThumbnailCache.cpp
)

# M7: エディタ操作の全面公開（dx12_editor_command / dx12_editor_state）。表から公開用の分類・可否・実行を作る層と、状態を JSON で読む層。
# 純ロジック（分類 TraitsFor など）は tests/mcp_editor_command_test.cpp が検査する。上と同じ理由で target_sources の末尾追記。
target_sources(Editor PRIVATE
    EditorCommandMcp.cpp
    EditorStateMcp.cpp
)

# 植生 F1（FoliageLayer）の編集操作（散布 / ブラシ。MCP とツール窓が共用）+ ツール窓。
# 上と同じ理由で target_sources の末尾追記にしている。
target_sources(Editor PRIVATE
    FoliageOps.cpp
    panels/FoliagePanel.cpp
)
