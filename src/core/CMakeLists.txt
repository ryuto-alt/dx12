find_package(imgui CONFIG REQUIRED)

# 配布アセット暗号鍵（ビルド毎にランダム生成、gitignore）。無ければ即エラー。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/AssetKey.h")
    message(FATAL_ERROR
        "src/core/generated/AssetKey.h is missing. "
        "Run: powershell -ExecutionPolicy Bypass -File tools/gen_asset_key.ps1 "
        "(build_debug.bat / build_release.bat do this automatically).")
endif()

add_library(Core STATIC
    Version.cpp
    # Application の実装は 1 ファイル 18k 行のゴッドファイルだったので、クラス定義
    # (Application.h) を一切変えずにテーマ別 TU へ分割してある。担当を分けて同時に
    # 触れるようにするのが目的なので、新しい実装は該当テーマのファイルへ足すこと。
    # 分割の全体像と共有ヘルパは ApplicationInternal.h を参照。
    Application.cpp                 # ctor / Initialize / Run / Update / Shutdown
    ApplicationInternal.cpp         # 複数 TU 共有ヘルパのうち本文が大きいもの
    ApplicationPipeline.cpp         # PSO 再生成 / レンダー解像度 / PSO・SRV キャッシュ
    ApplicationRender.cpp           # Render() と描画の下請け
    ApplicationPostWindows.cpp      # ポストプロセス / Skybox / SSAO / SSR / Fog の設定窓（pg:: 2 カラム。フェーズ 1b）
    ApplicationScene.cpp            # シーンロード / Play⇔Editor 遷移 / 永続化
    ApplicationProject.cpp          # プロジェクト / バージョン管理 / ゲームビルド
    ApplicationVg.cpp               # 仮想ジオメトリ P2: インスタンス収集 + GPU カリング呼び出し + 統計（既定 OFF）
    ApplicationAtmosphere.cpp       # 物理ベース大気 A1: 時刻系 / AtmosphereRenderer の遅延確保 / IBL 増分再ベイク / 空・AP の呼び出し（enabled=false なら何もしない）
    ApplicationWater.cpp            # 水面 W1: WaterRenderer の遅延確保 / フレーム開始 / 統計（WaterBody が無ければ何もしない）
    ApplicationFoliage.cpp          # 植生 F1: FoliageSystem の遅延確保 / GPU カリング呼び出し / 描画呼び出し / 統計（FoliageLayer が無ければ何もしない）
    ApplicationOffscreenShot.cpp    # Q2(校正): ライティング単位の切替 / 任意解像度のオフスクリーン撮影 / 線形 HDR スクショの進行
    ApplicationPathTracer.cpp       # DXR パストレーサー(リファレンスレンダー): スナップショット + ジョブ + 出力（既定 OFF・遅延確保）
    ApplicationSequencer.cpp        # シーケンサー S1b: ホストの生成・フレーム更新・アクティブカメラ(カット)の選択
    SequencerBinding.cpp            # シーケンサー S1b: バインディング解決(guid → 階層パス → 名前)
    SequencerApply.cpp              # シーケンサー S1b: 適用層(Transform / プロパティ / ポスト・DoF / シェイク)+ PreAnimatedState
    SequencerHost.cpp               # シーケンサー S1b: 文書・エディタの非破壊スクラブ・Play の再生(Application 非依存)
    mcp/ApplicationMcp.cpp          # ディスパッチ表の土台 + スクショ / 読み戻し / 診断
    mcp/ApplicationMcpEntity.cpp
    mcp/ApplicationMcpEditor.cpp
    mcp/ApplicationMcpRender.cpp
    mcp/ApplicationMcpTooling.cpp
    mcp/ApplicationMcpAsset.cpp
    mcp/ApplicationMcpTerrain.cpp
    mcp/ApplicationMcpLighting.cpp
    mcp/ApplicationMcpNav.cpp
    mcp/ApplicationMcpGit.cpp
    mcp/ApplicationMcpValidate.cpp   # 配置検査（埋まり / ちらつき / 二重 / 当たり無し）
    mcp/ApplicationMcpPerceive.cpp   # 知覚層（dx12_perceive: プレイヤーの目から見た事実を数値で）
    mcp/ApplicationMcpUndo.cpp       # Undo / Redo / トランザクション（MCP の編集を Undo に積む）
    mcp/ApplicationMcpAi.cpp         # ゲーム AI の観測（brain_state）
    mcp/ApplicationMcpPathTracer.cpp # DXR パストレーサー（render_reference / render_reference_status / render_reference_cancel）
    mcp/ApplicationMcpWater.cpp      # 水面 W1（water_apply_preset）
    mcp/ApplicationMcpFoliage.cpp    # 植生 F1（foliage_scatter / foliage_paint / foliage_stats / set_wind ほか）
    mcp/ApplicationMcpImGui.cpp      # 仮想入力モード（AI が OS の入力を奪わずエディタ UI を操作: dx12_imgui_*）
    mcp/ApplicationMcpEditorUi.cpp   # M7: エディタ操作の全面公開（editor_command_list / run / editor_state / editor_notify / editor_select）
    mcp/McpUndoTrack.cpp             # MCP の編集の値スナップショット（McpUndo().Track<T>）
    mcp/ApplicationMcpAudio.cpp      # 音の観測（audio_state）
    mcp/ApplicationMcpSequence.cpp   # シーケンサー(sequence_list / load / save / get / eval / scrub / play / stop / apply_op)
    mcp/ApplicationMcpMatGraph.cpp   # マテリアルグラフ G2b(material_graph_get / edit / validate / compile / status / graphize / set_param / apply / nodes)
    mcp/ApplicationMcpManifest.cpp   # マニフェスト（method ごとのメタ情報）と引数の中央検査（データ表は ApplicationMcpManifestData.inc）
    Window.cpp
    SplashScreen.cpp                # 起動画面の窓 + スレッド + API（D2D 描画 / GDI+ 縮退）
    SplashRenderer.cpp              # Direct2D + DirectWrite の描画器（実窓とプレビューが共有）
    SplashAudio.cpp                 # 起動音（winmm waveOut の薄いラッパー。ミキサーは SplashMixer.h）
    SplashCommon.cpp                # 起動画面の共有部品（Tips / 最近のプロジェクト / ビルド日時 / DPI）
    SplashPreview.cpp               # --splash-preview（窓なしで PNG 連番を書く検証用の軽量経路）
    Logger.cpp
    LogRing.cpp
    ErrorLog.cpp
    CrashReport.cpp
    CrashHandler.cpp
    GameClock.cpp
    PlaySession.cpp                 # Play 1 回ぶんの記録（人間の操作を後から読む）
    PathResolver.cpp
    Updater.cpp
    UpdateWindow.cpp                # 更新の案内・進捗窓（起動画面と同じ Direct2D。MessageBox を使わない）と --preview-update-ui
    UpdateLogic.cpp                 # 案内窓の純ロジック（GitHub 本文の整形 / この版を飛ばす / 速度と残り時間）
    ReleaseNotes.cpp                # 更新内容の唯一の正（データは ReleaseNotesData.inc。画面・ランチャー・GitHub 本文）
    Http.cpp
    vfs/Crypto.cpp
    vfs/Compression.cpp
    vfs/PakWriter.cpp
    vfs/PakArchive.cpp
    vfs/Vfs.cpp
    mcp/McpBridge.cpp
    mcp/McpJournal.cpp               # MCP ファイル書き込みジャーナル（M5。標準ライブラリだけ。tests/mcp_safety_test.cpp が単体検査）
    save/UserData.cpp                # ユーザー領域（%LOCALAPPDATA%/<ゲーム名> / <プロジェクト>/.dx12）
    save/SaveFile.cpp                # セーブファイル 1 本の読み書き（純粋な部分。tests/save_file_test.cpp）
    save/SaveService.cpp             # Lua の save.* の窓口（置き場・プレイ時間）
)

target_include_directories(Core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/include   # 封印ランタイムの中立公開ヘッダ（engine/...）EventBus 等
)

find_package(spdlog CONFIG REQUIRED)

target_link_libraries(Core
    # Dx12Profiler は Tracy 無効時は空の INTERFACE ターゲット。ここ 1 箇所で
    # 全ライブラリへ伝播する（全部が直接/間接に Core を PUBLIC リンクしているため）。
    PUBLIC spdlog::spdlog Dx12Profiler
           SequencerCore   # シーケンサー(.dxseq)の純ロジック(src/sequencer/)。S1b の SequencerHost / Apply / MCP が使う
    PRIVATE imgui::imgui bcrypt.lib Cabinet.lib ws2_32.lib gdiplus.lib  # gdiplus はスプラッシュの縮退描画用
            # 起動画面: Direct2D + DirectWrite（D3D11 上）/ WIC（PNG ロゴ・プレビュー出力）/ winmm（起動音）
            d2d1.lib d3d11.lib dwrite.lib windowscodecs.lib dxguid.lib winmm.lib ole32.lib shell32.lib
)

target_compile_options(Core PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Core PRIVATE ${DX12_COMPILE_DEFS})

# Application*.cpp は MCP ハンドラ + nlohmann::json のテンプレート実体化が多く、Debug(未最適化)では
# COFF セクション上限(C1128)を超える。/bigobj で回避（Release は最適化で収まるが両構成へ付けて統一）。
target_compile_options(Core PRIVATE /bigobj)
