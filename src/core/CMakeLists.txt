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
    ApplicationScene.cpp            # シーンロード / Play⇔Editor 遷移 / 永続化
    ApplicationProject.cpp          # プロジェクト / バージョン管理 / ゲームビルド
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
    Window.cpp
    SplashScreen.cpp
    Logger.cpp
    CrashHandler.cpp
    GameClock.cpp
    PlaySession.cpp                 # Play 1 回ぶんの記録（人間の操作を後から読む）
    PathResolver.cpp
    Updater.cpp
    Http.cpp
    vfs/Crypto.cpp
    vfs/Compression.cpp
    vfs/PakWriter.cpp
    vfs/PakArchive.cpp
    vfs/Vfs.cpp
    mcp/McpBridge.cpp
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
    PRIVATE imgui::imgui bcrypt.lib Cabinet.lib ws2_32.lib gdiplus.lib  # gdiplus はスプラッシュのPNGロゴ用
)

target_compile_options(Core PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Core PRIVATE ${DX12_COMPILE_DEFS})

# Application*.cpp は MCP ハンドラ + nlohmann::json のテンプレート実体化が多く、Debug(未最適化)では
# COFF セクション上限(C1128)を超える。/bigobj で回避（Release は最適化で収まるが両構成へ付けて統一）。
target_compile_options(Core PRIVATE /bigobj)
