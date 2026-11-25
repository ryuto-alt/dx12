cmake_minimum_required(VERSION 3.28)
project(DX12Engine VERSION 0.1.0 LANGUAGES CXX RC)  # RC = .rc リソース（アプリアイコン）

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# C++20 モジュールは使っていないので、CMake が全 TU に足すモジュール走査（CXX.dd の dyndep）を切る。
# ★これがあると ninja 1.13 が「Assertion failed: edge && !edge->outputs_ready()」で落ち、
#   「中止/再試行/無視」のダイアログが出てビルドが止まる（2026-10-01 に多発）。走査ぶん速くもなる。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(DX12_BUILD_TESTS "Build unit tests" ON)
# Tracy Profiler。既定 OFF ＝ 配布ビルドには 1 バイトも入らない。
# 計測したいときだけ  cmake --preset windows-tracy  で別ディレクトリへ建てる。
option(DX12_ENABLE_TRACY "Tracy Profiler を有効化（開発計測用。既定 OFF）" OFF)

# NVIDIA DLSS Super Resolution / DLAA（NGX）。SDK はリポジトリに入れず tools/fetch_dlss.ps1 で
# third_party/dlss へ取得する（NVIDIA RTX SDK ライセンス 4.e: SDK をオープンソースのライセンス条件に服させない）。
# ON でも SDK が見つからなければ自動で OFF に落ちてビルドは通る（実行時は TAA へ縮退）。
option(UNO_ENABLE_DLSS "NVIDIA DLSS（SDK が third_party/dlss にあれば有効）" ON)
set(UNO_DLSS_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/dlss" CACHE PATH "NVIDIA DLSS SDK のルート（tools/fetch_dlss.ps1 の取得先）")
set(UNO_DLSS_ACTIVE OFF)
if(UNO_ENABLE_DLSS)
    if(EXISTS "${UNO_DLSS_SDK_DIR}/include/nvsdk_ngx_helpers.h"
       AND EXISTS "${UNO_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
       AND EXISTS "${UNO_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
        set(UNO_DLSS_ACTIVE ON)
        message(STATUS "DLSS: ON（${UNO_DLSS_SDK_DIR}）")
    else()
        message(STATUS "DLSS: OFF（SDK が無い。pwsh tools/fetch_dlss.ps1 で取得してから構成し直すと有効になる）")
    endif()
endif()

# MSVC compile options (not global - applied per-target to avoid conflicts with FetchContent deps)
set(DX12_COMPILE_OPTIONS /W4 /WX /permissive- /utf-8 /external:anglebrackets /external:W0 /bigobj)
set(DX12_COMPILE_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# デバッグ情報は obj 埋め込み(/Z7)。/Zi と違い cl 同士が mspdbsrv 経由で PDB 書き込みを
# 取り合わないので、Ninja の高並列ビルドが詰まらない。PDB 自体はリンク時に生成されるため、
# クラッシュレポート(core/CrashHandler)の関数名/行番号解決は従来どおり有効。
# Release も /DEBUG + /OPT:REF,ICF で従来の /Zi 構成と同じ PDB が出る。
if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,Release>:Embedded>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>" "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
endif()

# Dependencies
find_package(spdlog CONFIG REQUIRED)
# ルートで find_package する（サブディレクトリで呼ぶと IMPORTED ターゲットがそのディレクトリ以下にしか
# 見えず、後段の POST_BUILD DLL コピー(DX12Engine/GameRuntime、ルート scope)から参照できないため）。
find_package(directx-dxc CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)   # 自動LOD生成（Renderer がリンク、DLL はルートで同梱）

# --- Tracy Profiler（任意・既定 OFF）---------------------------------------
# vcpkg の tracy は TRACY_ENABLE 込みでビルドされており、リンクした時点で有効になる。
# したがって「リンクするかしないか」で開発/配布を切り分ける。
# INTERFACE ターゲットへ畳んでおくと、各ライブラリ側は無条件に 1 行リンクするだけで済む。
#
# ★vcpkg 側で default-features:false にして crash-handler フィーチャを切っている。
#   Tracy の crash handler は VEH(AddVectoredExceptionHandler)を張り、SEH より先に走るので、
#   有効にすると src/core/CrashHandler.cpp の SetUnhandledExceptionFilter が
#   クラッシュレポートを奪われる（dx12_crash.log / .dmp が出なくなる）。
add_library(Dx12Profiler INTERFACE)
if(DX12_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)          # Tracy::TracyClient
    target_link_libraries(Dx12Profiler INTERFACE Tracy::TracyClient)
    target_compile_definitions(Dx12Profiler INTERFACE DX12_TRACY=1)
    message(STATUS "Tracy Profiler: ON （プロファイラUIは公式リリースの Tracy.exe を使う）")
endif()

# Sub-projects
add_subdirectory(src/core)
add_subdirectory(src/graphics)
add_subdirectory(src/renderer)
add_subdirectory(src/resource)
add_subdirectory(src/animation)
add_subdirectory(src/ecs)
add_subdirectory(src/input)
add_subdirectory(src/hardware)  # Arduino / ESP32 の USB シリアル連携（docs/HARDWARE.md）。Core だけに依存
add_subdirectory(src/audio)
add_subdirectory(src/terrain)   # ハイトフィールド地形（Scene / Physics / Editor が使う）
add_subdirectory(src/nav)       # 自作ナビメッシュ（Scene が保持し、Editor / MCP / Lua が叩く）
add_subdirectory(src/scene)
add_subdirectory(src/scripting)
add_subdirectory(src/physics)
add_subdirectory(src/ai)        # ゲーム AI（群衆とエンティティの対応 / Brain）。Scripting / Core / Editor が使う
add_subdirectory(src/network)
add_subdirectory(src/gui)
add_subdirectory(src/ui)
add_subdirectory(src/editor)
add_subdirectory(src/project)
add_subdirectory(src/sequencer)  # シーケンサー(.dxseq)の純ロジック(S0。GPU/ECS/ImGui 非依存。統合は S1b 以降)

# Unit tests (GPU 非依存。ctest で実行可能)
if(DX12_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# アプリアイコン（exe 埋め込み + ウィンドウアイコン）。両 exe に同じリソースを付ける。
set(DX12_APP_ICON_RC "${CMAKE_SOURCE_DIR}/resources/app.rc")
# エディタ専用リソース（DX12Engine だけ。GameRuntime には付けない。W05-17）
set(DX12_EDITOR_RC "${CMAKE_SOURCE_DIR}/resources/editor.rc")
# アプリケーションマニフェスト（Per-Monitor V2 DPI。MSVC のリンカ既定マニフェストへ CMake が /MANIFESTINPUT で併合する）。
set(DX12_APP_MANIFEST "${CMAKE_SOURCE_DIR}/resources/dx12.manifest")

# Main executable
add_executable(DX12Engine WIN32 src/main.cpp ${DX12_APP_ICON_RC} ${DX12_EDITOR_RC} ${DX12_APP_MANIFEST})

target_link_libraries(DX12Engine
    PRIVATE
        Core
        Graphics
        Renderer
        Resource
        Animation
        ECS
        Audio
        Input
        Hardware
        Terrain
        Nav
        AI
        Scene
        Scripting
        Physics
        Network
        Gui
        Ui
        Editor
        ProjectLib
)

if(MSVC)
    target_compile_options(DX12Engine PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(DX12Engine PRIVATE ${DX12_COMPILE_DEFS})
endif()

# --- GitHub CLI (gh) 同梱: エディタの GitHub 連携(リポジトリ作成/push/ログイン)は
#     GitIntegration.cpp が gh.exe を CreateProcess するだけなので、ユーザーに別途インストール
#     させなくて済むよう gh.exe を DX12Engine.exe の隣に置く。CreateProcess の exe 検索順位は
#     1番目が「読み込んだ exe 自身のフォルダ」なので、隣に置くだけで PATH 未設定でも自動的に
#     見つかる(GitIntegration.cpp は無改造)。初回 configure 時だけ公式リリースから取得しキャッシュ。
set(DX12_GH_CLI_VERSION "2.95.0")
set(DX12_GH_CLI_SHA256  "19a7154161ada9cfaa9e57edb752ecc679b75c391a62e4f7b586eea1df30b5bb")
set(DX12_GH_CLI_DIR "${CMAKE_SOURCE_DIR}/third_party/gh-cli")
set(DX12_GH_CLI_EXE "${DX12_GH_CLI_DIR}/gh.exe")
if(NOT EXISTS "${DX12_GH_CLI_EXE}")
    set(DX12_GH_CLI_ZIP "${CMAKE_BINARY_DIR}/gh-cli.zip")
    message(STATUS "gh.exe (GitHub CLI v${DX12_GH_CLI_VERSION}) を取得中...")
    file(DOWNLOAD
        "https://github.com/cli/cli/releases/download/v${DX12_GH_CLI_VERSION}/gh_${DX12_GH_CLI_VERSION}_windows_amd64.zip"
        "${DX12_GH_CLI_ZIP}"
        TLS_VERIFY ON
        EXPECTED_HASH SHA256=${DX12_GH_CLI_SHA256}
        STATUS DX12_GH_CLI_DL_STATUS)
    list(GET DX12_GH_CLI_DL_STATUS 0 DX12_GH_CLI_DL_CODE)
    if(DX12_GH_CLI_DL_CODE EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT "${DX12_GH_CLI_ZIP}" DESTINATION "${CMAKE_BINARY_DIR}/gh-cli-extract")
        if(EXISTS "${CMAKE_BINARY_DIR}/gh-cli-extract/bin/gh.exe")
            file(MAKE_DIRECTORY "${DX12_GH_CLI_DIR}")
            file(COPY "${CMAKE_BINARY_DIR}/gh-cli-extract/bin/gh.exe" DESTINATION "${DX12_GH_CLI_DIR}")
        endif()
    else()
        message(WARNING "gh.exe の取得に失敗（オフライン？）。GitHub連携はPATH上のghにフォールバックするで。"
                         "手動なら third_party/gh-cli/gh.exe に置いてもOK。")
    endif()
    file(REMOVE "${DX12_GH_CLI_ZIP}")
endif()
if(EXISTS "${DX12_GH_CLI_EXE}")
    add_custom_command(TARGET DX12Engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DX12_GH_CLI_EXE}" "$<TARGET_FILE_DIR:DX12Engine>/gh.exe"
        COMMENT "Bundling gh.exe next to DX12Engine.exe")
endif()

# --- GameRuntime: ゲーム専用実行ファイル（DX12_GAME_RUNTIME で常にゲームモード固定）---
add_executable(GameRuntime WIN32 src/main.cpp ${DX12_APP_ICON_RC} ${DX12_APP_MANIFEST})
target_link_libraries(GameRuntime
    PRIVATE
        Core
        Graphics
        Renderer
        Resource
        Animation
        ECS
        Audio
        Input
        Hardware
        Terrain
        Nav
        AI
        Scene
        Scripting
        Physics
        Network
        Gui
        Ui
        Editor
        ProjectLib
)
if(MSVC)
    target_compile_options(GameRuntime PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(GameRuntime PRIVATE ${DX12_COMPILE_DEFS} DX12_GAME_RUNTIME)

    # dxcompiler.dll を delay-load にする。Resource が Microsoft::DirectXShaderCompiler を
    # PUBLIC リンクしているため、素のリンクだと GameRuntime.exe の通常インポートテーブルに
    # dxcompiler.dll が hard dependency として載る。BuildGame() は配布ゲームから
    # dxcompiler.dll を除外する(エディタの実行時シェーダーコンパイル専用、ゲームは .cso を
    # 読むだけで不要、~25MB削減)設計なので、除外すると exe が起動時に
    # 「dxcompiler.dll が見つからないため、コードの実行を続行できません」で即死する。
    # ゲームモードは ShaderManager(=DxcCreateInstance の唯一の呼び出し元)を生成しない
    # (Application::Initialize、!m_isGameMode 時のみ生成)ため、delay-load にしておけば
    # 実行時に一切ロードされず、同梱しなくても安全に起動できる。
    target_link_options(GameRuntime PRIVATE "/DELAYLOAD:dxcompiler.dll")

    # 起動画面（エディタ専用: Direct2D / DirectWrite / D3D11 / WIC）の DLL も delay-load にする。SplashScreen は
    # Core に居るので GameRuntime にもリンクされるが、ゲームは Show() を一切呼ばない（呼び出し側が !gameMode）ので
    # 実行時にロードされない。素のリンクだと、ゲームの起動時に使わない DLL を 4 本ロードして起動が遅くなり、
    # 配布ゲームの依存も増える。
    target_link_options(GameRuntime PRIVATE
        "/DELAYLOAD:d2d1.dll" "/DELAYLOAD:dwrite.dll" "/DELAYLOAD:d3d11.dll" "/DELAYLOAD:windowscodecs.dll")
    target_link_libraries(GameRuntime PRIVATE delayimp.lib)
endif()

# --- プリコンパイル済みヘッダー(src/pch.h) ---
# windows.h / d3d12 / entt / spdlog / nlohmann-json / ImGui 等の重量級ヘッダーを
# TU ごとに毎回パースするのをやめる(111 TU × 数十万行 → ターゲットごとに 1 回)。
# 中身の方針は src/pch.h のコメント参照。ターゲットごとに固有フラグ(/bigobj や
# DX12_GAME_RUNTIME 等)が違うので REUSE_FROM は使わず各自でコンパイルする。
if(MSVC)
    foreach(DX12_PCH_TARGET IN ITEMS
        Core Graphics Renderer Resource Animation ECS Audio Input
        Terrain Nav AI Scene Scripting Physics Network Gui Ui Editor ProjectLib
        DX12Engine GameRuntime)
        target_precompile_headers(${DX12_PCH_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}/src/pch.h")
    endforeach()
endif()

# --- 実行時シェーダーコンパイル用 DLL 同梱 ---
# dxcompiler.dll は vcpkg の applocal 自動コピーで運ばれるが、依存の dxil.dll(DXIL署名用)は
# applocal が拾わないことがあるため明示コピーする(無いと開発者モード OFF 環境で
# CreatePipelineState が DXIL 未署名エラーになる)。
# 注意: vcpkg directx-dxc ポートの directx-dxc-config.cmake.in には
# `IMPORTED_LOCATION_RELASE`(RELEASEのタイポ)というバグがあり、Microsoft::DXIL の
# Release 構成の場所が正しく設定されない($<TARGET_FILE:Microsoft::DXIL> が Release で解決できない)。
# DIRECTX_DXC_TOOL(ポートが設定する dxc.exe のパス、tools/directx-dxc/ 配下でconfig非依存)を
# 基準に dxil.dll を直接指定することでこのポートのバグを回避する。
get_filename_component(DX12_DXC_TOOL_DIR "${DIRECTX_DXC_TOOL}" DIRECTORY)
set(DX12_DXIL_DLL "${DX12_DXC_TOOL_DIR}/dxil.dll")
foreach(DX12_SHADERC_TARGET DX12Engine GameRuntime)
    add_custom_command(TARGET ${DX12_SHADERC_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Microsoft::DirectXShaderCompiler>"
            "$<TARGET_FILE_DIR:${DX12_SHADERC_TARGET}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DX12_DXIL_DLL}"
            "$<TARGET_FILE_DIR:${DX12_SHADERC_TARGET}>"
        COMMENT "Bundling dxcompiler.dll / dxil.dll next to ${DX12_SHADERC_TARGET}.exe")
endforeach()

# --- meshoptimizer.dll 同梱（自動LOD）---
# vcpkg applocal が拾わない環境があるため dxil.dll と同様に明示コピーする。
# tests/ 配下の Renderer リンクなテスト exe 用にも1部コピーしておく。
foreach(DX12_MESHOPT_TARGET DX12Engine GameRuntime)
    add_custom_command(TARGET ${DX12_MESHOPT_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:meshoptimizer::meshoptimizer>"
            "$<TARGET_FILE_DIR:${DX12_MESHOPT_TARGET}>"
        COMMENT "Bundling meshoptimizer.dll next to ${DX12_MESHOPT_TARGET}.exe")
endforeach()
# nvngx_dlss.dll（DLSS 有効ビルドのみ）。NGX は exe のフォルダから探すので隣へ置く。
# 再配布は NVIDIA RTX SDK ライセンスで許可されている（商用リリース前の NVIDIA への通知義務あり。LICENSE.txt の補足 4）。
if(UNO_DLSS_ACTIVE)
    foreach(DX12_DLSS_TARGET DX12Engine GameRuntime)
        add_custom_command(TARGET ${DX12_DLSS_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UNO_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:${DX12_DLSS_TARGET}>"
            COMMENT "Bundling nvngx_dlss.dll next to ${DX12_DLSS_TARGET}.exe")
    endforeach()
endif()
# TracyClient.dll（Tracy 有効ビルドのみ）。dxil.dll / meshoptimizer.dll と同じ理由で明示コピー。
if(DX12_ENABLE_TRACY)
    foreach(DX12_TRACY_TARGET DX12Engine GameRuntime)
        add_custom_command(TARGET ${DX12_TRACY_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Tracy::TracyClient>" "$<TARGET_FILE_DIR:${DX12_TRACY_TARGET}>"
            COMMENT "Bundling TracyClient.dll next to ${DX12_TRACY_TARGET}.exe")
    endforeach()
endif()

# テスト実行ファイルの隣へ、メイン exe と同じ実行時 DLL を全部配る。
# vcpkg の applocal デプロイはメイン exe の分しか置かないので、これが無いと
# Core をリンクするテスト(spdlog.dll / fmt.dll 等が要る)が 0xC0000135 で
# **起動すらできない**。ctest では「失敗」に見えるが実際には 1 行も走っておらず、
# 壊れていることに気付けない（実際 SerializeRoundtrip / TextureCompress / Vfs /
# EnetLoopback の 4 本がずっとこの状態だった）。
# 以前は meshoptimizer.dll だけを個別にコピーしていたが、必要な DLL が増えるたびに
# 同じ症状を踏むので、ディレクトリごと配る形にした。
add_custom_command(TARGET DX12Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=$<TARGET_FILE_DIR:DX12Engine>
        -DDST=${CMAKE_BINARY_DIR}/tests
        -P ${CMAKE_SOURCE_DIR}/tools/copy_runtime_dlls.cmake
    COMMENT "Bundling runtime DLLs next to tests")

# --- DXC Shader Compilation ---
# シェーダは宣言表で管理する（W05-16）。.cso の一覧は shaders/ShaderManifest.cmake、
# 新しい .cso は各レーンが shaders/manifest/lane_{a,b,c,d}.cmake の自分のブロックへ足す。
# 書式と依存の張り方は cmake/UnoShaders.cmake の先頭を参照。
find_program(DXC_EXECUTABLE dxc
    PATHS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)

if(DXC_EXECUTABLE)
    message(STATUS "Found DXC: ${DXC_EXECUTABLE}")

    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

    include(${CMAKE_SOURCE_DIR}/cmake/UnoShaders.cmake)
    include(${CMAKE_SOURCE_DIR}/shaders/ShaderManifest.cmake)
    include(${CMAKE_SOURCE_DIR}/shaders/manifest/lane_a.cmake)
    include(${CMAKE_SOURCE_DIR}/shaders/manifest/lane_b.cmake)
    include(${CMAKE_SOURCE_DIR}/shaders/manifest/lane_c.cmake)
    include(${CMAKE_SOURCE_DIR}/shaders/manifest/lane_d.cmake)

    get_property(SHADER_OUTPUTS GLOBAL PROPERTY UNO_SHADER_OUTPUTS)
    add_custom_target(Shaders ALL DEPENDS ${SHADER_OUTPUTS})
    add_dependencies(DX12Engine Shaders)
    add_dependencies(GameRuntime Shaders)
else()
    message(WARNING "DXC not found -- shader compilation skipped")
endif()

# Shader directory and assets directory (Core needs it because Application.cpp loads shaders/assets)
target_compile_definitions(Core PRIVATE
    SHADER_DIR=L"${CMAKE_BINARY_DIR}/shaders/"
    SHADER_SRC_DIR=L"${CMAKE_SOURCE_DIR}/shaders/"
    ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/"
    SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts/"
)
