cmake_minimum_required(VERSION 3.28)
project(DX12Engine VERSION 0.1.0 LANGUAGES CXX RC)  # RC = .rc リソース（アプリアイコン）

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DX12_BUILD_TESTS "Build unit tests" ON)

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

# Sub-projects
add_subdirectory(src/core)
add_subdirectory(src/graphics)
add_subdirectory(src/renderer)
add_subdirectory(src/resource)
add_subdirectory(src/animation)
add_subdirectory(src/ecs)
add_subdirectory(src/input)
add_subdirectory(src/audio)
add_subdirectory(src/terrain)   # ハイトフィールド地形（Scene / Physics / Editor が使う）
add_subdirectory(src/scene)
add_subdirectory(src/scripting)
add_subdirectory(src/physics)
add_subdirectory(src/network)
add_subdirectory(src/gui)
add_subdirectory(src/ui)
add_subdirectory(src/editor)
add_subdirectory(src/project)

# Unit tests (GPU 非依存。ctest で実行可能)
if(DX12_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# アプリアイコン（exe 埋め込み + ウィンドウアイコン）。両 exe に同じリソースを付ける。
set(DX12_APP_ICON_RC "${CMAKE_SOURCE_DIR}/resources/app.rc")

# Main executable
add_executable(DX12Engine WIN32 src/main.cpp ${DX12_APP_ICON_RC})

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
        Terrain
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
add_executable(GameRuntime WIN32 src/main.cpp ${DX12_APP_ICON_RC})
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
        Terrain
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
        Terrain Scene Scripting Physics Network Gui Ui Editor ProjectLib
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
add_custom_command(TARGET DX12Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "${CMAKE_BINARY_DIR}/tests"
    COMMENT "Bundling meshoptimizer.dll next to tests")

# --- DXC Shader Compilation ---
find_program(DXC_EXECUTABLE dxc
    PATHS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
)

if(DXC_EXECUTABLE)
    message(STATUS "Found DXC: ${DXC_EXECUTABLE}")

    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

    set(SHADER_SOURCES
        "${CMAKE_SOURCE_DIR}/shaders/forward/Forward.hlsl"
        "${CMAKE_SOURCE_DIR}/shaders/forward/ForwardSkinned.hlsl"
    )

    set(SHADER_FORWARD "${CMAKE_SOURCE_DIR}/shaders/forward/Forward.hlsl")
    set(SHADER_SKINNED "${CMAKE_SOURCE_DIR}/shaders/forward/ForwardSkinned.hlsl")
    set(SHADER_EMISSIVE "${CMAKE_SOURCE_DIR}/shaders/forward/Emissive.hlsl")
    # Forward/Skinned が include する共有ヘッダ（編集時に再コンパイルさせる）
    # ★ClusterCommon.hlsli は Lighting.hlsli が include している。ここに入れておかないと
    #   クラスタ定数を変えたときにフォワード系の .cso が古いまま残る。
    set(SHADER_FWD_INC
        "${CMAKE_SOURCE_DIR}/shaders/forward/PBR.hlsli"
        "${CMAKE_SOURCE_DIR}/shaders/forward/ClusterCommon.hlsli"
        "${CMAKE_SOURCE_DIR}/shaders/forward/DecalCommon.hlsli"
        "${CMAKE_SOURCE_DIR}/shaders/forward/DecalApply.hlsli"
        "${CMAKE_SOURCE_DIR}/shaders/forward/Lighting.hlsli")
    # クラスタードライティング（Forward+）のライトカリング compute 2 本
    set(SHADER_CLUSTER_BUILD "${CMAKE_SOURCE_DIR}/shaders/forward/ClusterBuild.hlsl")
    set(SHADER_CLUSTER_CULL  "${CMAKE_SOURCE_DIR}/shaders/forward/ClusterCull.hlsl")
    set(SHADER_CLUSTER_INC   "${CMAKE_SOURCE_DIR}/shaders/forward/ClusterCommon.hlsli")
    # デカール（クラスタードフォワードデカール）。カリング compute 1 本 + 共有 .hlsli 2 本。
    set(SHADER_DECAL_CULL    "${CMAKE_SOURCE_DIR}/shaders/forward/ClusterCullDecals.hlsl")
    set(SHADER_DECAL_INC     "${CMAKE_SOURCE_DIR}/shaders/forward/DecalCommon.hlsli"
                             "${CMAKE_SOURCE_DIR}/shaders/forward/DecalApply.hlsli")
    set(SHADER_GRID          "${CMAKE_SOURCE_DIR}/shaders/forward/ForwardGrid.hlsl")
    # 地形マテリアル（4 レイヤースプラット + 高さブレンド + トライプラナー）。
    # Lighting.hlsli / PBR.hlsli / ClusterCommon.hlsli / DecalApply.hlsli を include するので
    # DEPENDS に ${SHADER_FWD_INC} を必ず入れる（入れ忘れると古い .cso が残ってレイアウトがズレる）。
    set(SHADER_TERRAIN       "${CMAKE_SOURCE_DIR}/shaders/forward/Terrain.hlsl")
    set(SHADER_FWD_INST      "${CMAKE_SOURCE_DIR}/shaders/forward/ForwardInstanced.hlsl")
    set(SHADER_SHADOW_INST   "${CMAKE_SOURCE_DIR}/shaders/shadow/ShadowPassInstanced.hlsl")
    set(SHADER_SHADOW        "${CMAKE_SOURCE_DIR}/shaders/shadow/ShadowPass.hlsl")
    set(SHADER_SHADOW_SKIN   "${CMAKE_SOURCE_DIR}/shaders/shadow/ShadowPassSkinned.hlsl")
    set(SHADER_DEPTHPRE_SKIN "${CMAKE_SOURCE_DIR}/shaders/shadow/DepthPrepassSkinned.hlsl")
    set(SHADER_DEBUG_LINE    "${CMAKE_SOURCE_DIR}/shaders/debug/DebugLine.hlsl")
    set(SHADER_POST          "${CMAKE_SOURCE_DIR}/shaders/post/PostProcess.hlsl")
    set(SHADER_POST_INC      "${CMAKE_SOURCE_DIR}/shaders/post/FullscreenTri.hlsli")
    set(SHADER_BLOOM         "${CMAKE_SOURCE_DIR}/shaders/post/Bloom.hlsl")
    set(SHADER_AUTOEXPOSURE  "${CMAKE_SOURCE_DIR}/shaders/post/AutoExposure.hlsl")
    set(SHADER_GODRAYS       "${CMAKE_SOURCE_DIR}/shaders/post/GodRays.hlsl")
    set(SHADER_LENSFLARE     "${CMAKE_SOURCE_DIR}/shaders/post/LensFlare.hlsl")
    set(SHADER_DOF           "${CMAKE_SOURCE_DIR}/shaders/post/DoF.hlsl")
    set(SHADER_MOTIONBLUR    "${CMAKE_SOURCE_DIR}/shaders/post/MotionBlur.hlsl")
    set(SHADER_SPRITE        "${CMAKE_SOURCE_DIR}/shaders/sprite/Sprite.hlsl")
    set(SHADER_ICON_BB       "${CMAKE_SOURCE_DIR}/shaders/editor/IconBillboard.hlsl")
    set(SHADER_TRANSITION    "${CMAKE_SOURCE_DIR}/shaders/post/Transition.hlsl")
    set(SHADER_PARTICLE      "${CMAKE_SOURCE_DIR}/shaders/particle/Particle.hlsl")
    set(SHADER_BEAM          "${CMAKE_SOURCE_DIR}/shaders/particle/Beam.hlsl")
    set(SHADER_TRAIL         "${CMAKE_SOURCE_DIR}/shaders/particle/Trail.hlsl")
    set(SHADER_GPU_PARTICLE_SIM  "${CMAKE_SOURCE_DIR}/shaders/particle/GpuParticleSim.hlsl")
    set(SHADER_GPU_PARTICLE_DRAW "${CMAKE_SOURCE_DIR}/shaders/particle/GpuParticleDraw.hlsl")
    set(SHADER_PARTICLE_INC  "${CMAKE_SOURCE_DIR}/shaders/particle/ParticleCommon.hlsli")
    set(SHADER_IBL_IRR       "${CMAKE_SOURCE_DIR}/shaders/ibl/IrradianceConvolution.hlsl")
    set(SHADER_IBL_PRE       "${CMAKE_SOURCE_DIR}/shaders/ibl/PrefilterEnv.hlsl")
    set(SHADER_IBL_BRDF      "${CMAKE_SOURCE_DIR}/shaders/ibl/IntegrateBRDF.hlsl")
    set(SHADER_SKYBOX        "${CMAKE_SOURCE_DIR}/shaders/ibl/Skybox.hlsl")
    set(SHADER_IBL_INC       "${CMAKE_SOURCE_DIR}/shaders/ibl/IBLCommon.hlsli")
    set(SHADER_SSAO          "${CMAKE_SOURCE_DIR}/shaders/ssao/SSAO.hlsl")
    set(SHADER_SSAO_BLUR     "${CMAKE_SOURCE_DIR}/shaders/ssao/SSAOBlur.hlsl")
    set(SHADER_CONTACT_SHADOW "${CMAKE_SOURCE_DIR}/shaders/shadow/ContactShadow.hlsl")
    set(SHADER_VELOCITY      "${CMAKE_SOURCE_DIR}/shaders/velocity/VelocityPrepass.hlsl")
    set(SHADER_VELOCITY_INST "${CMAKE_SOURCE_DIR}/shaders/velocity/VelocityPrepassInstanced.hlsl")
    set(SHADER_VELOCITY_SKIN "${CMAKE_SOURCE_DIR}/shaders/velocity/VelocityPrepassSkinned.hlsl")
    # スクリーン空間共通ヘッダ（oct 法線 / ビューポート UV↔NDC / レイマーチ）。
    # 深度プリパスの PS（G-Buffer 書き出し）と SSR/SSGI の両方が include する。
    set(SHADER_SS_INC        "${CMAKE_SOURCE_DIR}/shaders/screenspace/ScreenSpaceCommon.hlsli")
    # SSR / SSGI（計画04）。VS は SSR.hlsl の FSTriVS を全パスで共有する。
    set(SHADER_SSR           "${CMAKE_SOURCE_DIR}/shaders/screenspace/SSR.hlsl")
    set(SHADER_SSGI          "${CMAKE_SOURCE_DIR}/shaders/screenspace/SSGI.hlsl")
    set(SHADER_SSGI_INC      "${SHADER_SS_INC}"
                             "${CMAKE_SOURCE_DIR}/shaders/screenspace/ScreenSpaceParams.hlsli"
                             "${CMAKE_SOURCE_DIR}/shaders/post/FullscreenTri.hlsli")
    set(SHADER_VELOCITY_INC  "${CMAKE_SOURCE_DIR}/shaders/velocity/VelocityCommon.hlsli"
                             "${SHADER_SS_INC}")
    # ボリュメトリックフォグ（froxel）。compute 3 本 + 合成 VS/PS。
    set(SHADER_FOG_INC       "${CMAKE_SOURCE_DIR}/shaders/fog/FogCommon.hlsli")
    set(SHADER_FOG_INJECT    "${CMAKE_SOURCE_DIR}/shaders/fog/FogInject.hlsl")
    set(SHADER_FOG_SCATTER   "${CMAKE_SOURCE_DIR}/shaders/fog/FogScatter.hlsl")
    set(SHADER_FOG_INTEGRATE "${CMAKE_SOURCE_DIR}/shaders/fog/FogIntegrate.hlsl")
    set(SHADER_FOG_COMPOSITE "${CMAKE_SOURCE_DIR}/shaders/fog/FogComposite.hlsl")
    set(SHADER_TAA           "${CMAKE_SOURCE_DIR}/shaders/post/TAA.hlsl")
    # ★ shaders/debug/ は .gitignore の "Debug/" に食われて新規ファイルが追跡されない。
    #    velocity/ 配下に置くこと（DebugLine.hlsl は昔から追跡済みなので生き残っているだけ）。
    set(SHADER_VEL_DEBUG     "${CMAKE_SOURCE_DIR}/shaders/velocity/VelocityDebug.hlsl")

    set(SHADER_OUTPUTS
        "${SHADER_OUTPUT_DIR}/Forward_VS.cso"
        "${SHADER_OUTPUT_DIR}/Forward_PS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardLdr_PS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardSkinned_PS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardGrid_VS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardGrid_PS.cso"
        "${SHADER_OUTPUT_DIR}/Terrain_VS.cso"
        "${SHADER_OUTPUT_DIR}/Terrain_PS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardInstanced_VS.cso"
        "${SHADER_OUTPUT_DIR}/ShadowPassInstanced_VS.cso"
        "${SHADER_OUTPUT_DIR}/ShadowPass_VS.cso"
        "${SHADER_OUTPUT_DIR}/ShadowPassSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/DepthPrepassSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/DebugLine_VS.cso"
        "${SHADER_OUTPUT_DIR}/DebugLine_PS.cso"
        "${SHADER_OUTPUT_DIR}/PostProcess_VS.cso"
        "${SHADER_OUTPUT_DIR}/PostProcess_PS.cso"
        "${SHADER_OUTPUT_DIR}/Bloom_VS.cso"
        "${SHADER_OUTPUT_DIR}/BloomDown_PS.cso"
        "${SHADER_OUTPUT_DIR}/BloomUp_PS.cso"
        "${SHADER_OUTPUT_DIR}/ExposureHistogram_CS.cso"
        "${SHADER_OUTPUT_DIR}/ExposureAdapt_CS.cso"
        "${SHADER_OUTPUT_DIR}/GodRaysMask_PS.cso"
        "${SHADER_OUTPUT_DIR}/GodRaysBlur_PS.cso"
        "${SHADER_OUTPUT_DIR}/LensFlare_PS.cso"
        "${SHADER_OUTPUT_DIR}/DofCoc_PS.cso"
        "${SHADER_OUTPUT_DIR}/DofGather_PS.cso"
        "${SHADER_OUTPUT_DIR}/DofComposite_PS.cso"
        "${SHADER_OUTPUT_DIR}/MotionBlur_PS.cso"
        "${SHADER_OUTPUT_DIR}/Sprite_VS.cso"
        "${SHADER_OUTPUT_DIR}/Sprite_PS.cso"
        "${SHADER_OUTPUT_DIR}/IconBillboard_VS.cso"
        "${SHADER_OUTPUT_DIR}/IconBillboard_PS.cso"
        "${SHADER_OUTPUT_DIR}/Transition_VS.cso"
        "${SHADER_OUTPUT_DIR}/Transition_PS.cso"
        "${SHADER_OUTPUT_DIR}/Emissive_VS.cso"
        "${SHADER_OUTPUT_DIR}/Emissive_PS.cso"
        "${SHADER_OUTPUT_DIR}/Particle_VS.cso"
        "${SHADER_OUTPUT_DIR}/Particle_PS.cso"
        "${SHADER_OUTPUT_DIR}/ParticleDistort_PS.cso"
        "${SHADER_OUTPUT_DIR}/Trail_VS.cso"
        "${SHADER_OUTPUT_DIR}/Trail_PS.cso"
        "${SHADER_OUTPUT_DIR}/Beam_VS.cso"
        "${SHADER_OUTPUT_DIR}/Beam_PS.cso"
        "${SHADER_OUTPUT_DIR}/GpuParticleInit_CS.cso"
        "${SHADER_OUTPUT_DIR}/GpuParticlePrepare_CS.cso"
        "${SHADER_OUTPUT_DIR}/GpuParticleEmit_CS.cso"
        "${SHADER_OUTPUT_DIR}/GpuParticleKickoff_CS.cso"
        "${SHADER_OUTPUT_DIR}/GpuParticleSimulate_CS.cso"
        "${SHADER_OUTPUT_DIR}/GpuParticleDraw_VS.cso"
        "${SHADER_OUTPUT_DIR}/IrradianceConvolution_CS.cso"
        "${SHADER_OUTPUT_DIR}/PrefilterEnv_CS.cso"
        "${SHADER_OUTPUT_DIR}/IntegrateBRDF_CS.cso"
        "${SHADER_OUTPUT_DIR}/Skybox_VS.cso"
        "${SHADER_OUTPUT_DIR}/Skybox_PS.cso"
        "${SHADER_OUTPUT_DIR}/SSAO_VS.cso"
        "${SHADER_OUTPUT_DIR}/SSAO_PS.cso"
        "${SHADER_OUTPUT_DIR}/SSAOBlur_PS.cso"
        "${SHADER_OUTPUT_DIR}/ContactShadow_VS.cso"
        "${SHADER_OUTPUT_DIR}/ContactShadow_PS.cso"
        "${SHADER_OUTPUT_DIR}/VelocityPrepass_VS.cso"
        "${SHADER_OUTPUT_DIR}/VelocityPrepass_PS.cso"
        "${SHADER_OUTPUT_DIR}/VelocityPrepassInstanced_VS.cso"
        "${SHADER_OUTPUT_DIR}/VelocityPrepassSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/TaaResolve_PS.cso"
        "${SHADER_OUTPUT_DIR}/VelocityDebug_PS.cso"
        "${SHADER_OUTPUT_DIR}/ClusterBuild_CS.cso"
        "${SHADER_OUTPUT_DIR}/ClusterCull_CS.cso"
        "${SHADER_OUTPUT_DIR}/ScreenSpace_VS.cso"
        "${SHADER_OUTPUT_DIR}/SsrTrace_PS.cso"
        "${SHADER_OUTPUT_DIR}/SsrUpsample_PS.cso"
        "${SHADER_OUTPUT_DIR}/SsgiTrace_PS.cso"
        "${SHADER_OUTPUT_DIR}/SsgiTemporal_PS.cso"
        "${SHADER_OUTPUT_DIR}/SsgiUpsample_PS.cso"
        "${SHADER_OUTPUT_DIR}/ColorDownsample_PS.cso"
        "${SHADER_OUTPUT_DIR}/FogInject_CS.cso"
        "${SHADER_OUTPUT_DIR}/FogScatter_CS.cso"
        "${SHADER_OUTPUT_DIR}/FogIntegrate_CS.cso"
        "${SHADER_OUTPUT_DIR}/FogComposite_VS.cso"
        "${SHADER_OUTPUT_DIR}/FogComposite_PS.cso"
        "${SHADER_OUTPUT_DIR}/ClusterCullDecals_CS.cso"
    )

    # --- デカールのクラスタカリング ---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ClusterCullDecals_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain -Fo "${SHADER_OUTPUT_DIR}/ClusterCullDecals_CS.cso" "${SHADER_DECAL_CULL}"
        DEPENDS "${SHADER_DECAL_CULL}" "${SHADER_CLUSTER_INC}" ${SHADER_DECAL_INC}
        COMMENT "Compiling ClusterCullDecals.hlsl -> ClusterCullDecals_CS.cso (cs_6_0)"
    )

    # --- ボリュメトリックフォグ（froxel）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/FogInject_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSInject -Fo "${SHADER_OUTPUT_DIR}/FogInject_CS.cso" "${SHADER_FOG_INJECT}"
        DEPENDS "${SHADER_FOG_INJECT}" "${SHADER_FOG_INC}"
        COMMENT "Compiling FogInject.hlsl -> FogInject_CS.cso (cs_6_0)"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/FogScatter_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSScatter -Fo "${SHADER_OUTPUT_DIR}/FogScatter_CS.cso" "${SHADER_FOG_SCATTER}"
        DEPENDS "${SHADER_FOG_SCATTER}" "${SHADER_FOG_INC}" "${SHADER_CLUSTER_INC}"
        COMMENT "Compiling FogScatter.hlsl -> FogScatter_CS.cso (cs_6_0)"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/FogIntegrate_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSIntegrate -Fo "${SHADER_OUTPUT_DIR}/FogIntegrate_CS.cso" "${SHADER_FOG_INTEGRATE}"
        DEPENDS "${SHADER_FOG_INTEGRATE}" "${SHADER_FOG_INC}"
        COMMENT "Compiling FogIntegrate.hlsl -> FogIntegrate_CS.cso (cs_6_0)"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/FogComposite_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/FogComposite_VS.cso" "${SHADER_FOG_COMPOSITE}"
        DEPENDS "${SHADER_FOG_COMPOSITE}" "${SHADER_FOG_INC}" "${SHADER_POST_INC}"
        COMMENT "Compiling FogComposite.hlsl -> FogComposite_VS.cso (vs_6_0)"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/FogComposite_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/FogComposite_PS.cso" "${SHADER_FOG_COMPOSITE}"
        DEPENDS "${SHADER_FOG_COMPOSITE}" "${SHADER_FOG_INC}" "${SHADER_POST_INC}"
        COMMENT "Compiling FogComposite.hlsl -> FogComposite_PS.cso (ps_6_0)"
        VERBATIM
    )

    # --- SSR / SSGI（スクリーン空間反射 + スクリーン空間GI）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ScreenSpace_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/ScreenSpace_VS.cso" "${SHADER_SSR}"
        DEPENDS "${SHADER_SSR}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSR.hlsl -> ScreenSpace_VS.cso (vs_6_0)"
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SsrTrace_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E TracePS -Fo "${SHADER_OUTPUT_DIR}/SsrTrace_PS.cso" "${SHADER_SSR}"
        DEPENDS "${SHADER_SSR}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSR.hlsl -> SsrTrace_PS.cso (ps_6_0)"
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SsrUpsample_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E UpsamplePS -Fo "${SHADER_OUTPUT_DIR}/SsrUpsample_PS.cso" "${SHADER_SSR}"
        DEPENDS "${SHADER_SSR}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSR.hlsl -> SsrUpsample_PS.cso (ps_6_0)"
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SsgiTrace_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E TracePS -Fo "${SHADER_OUTPUT_DIR}/SsgiTrace_PS.cso" "${SHADER_SSGI}"
        DEPENDS "${SHADER_SSGI}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSGI.hlsl -> SsgiTrace_PS.cso (ps_6_0)"
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SsgiTemporal_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E TemporalPS -Fo "${SHADER_OUTPUT_DIR}/SsgiTemporal_PS.cso" "${SHADER_SSGI}"
        DEPENDS "${SHADER_SSGI}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSGI.hlsl -> SsgiTemporal_PS.cso (ps_6_0)"
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SsgiUpsample_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E UpsamplePS -Fo "${SHADER_OUTPUT_DIR}/SsgiUpsample_PS.cso" "${SHADER_SSGI}"
        DEPENDS "${SHADER_SSGI}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSGI.hlsl -> SsgiUpsample_PS.cso (ps_6_0)"
    )
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ColorDownsample_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E DownsamplePS -Fo "${SHADER_OUTPUT_DIR}/ColorDownsample_PS.cso" "${SHADER_SSGI}"
        DEPENDS "${SHADER_SSGI}" ${SHADER_SSGI_INC}
        COMMENT "Compiling SSGI.hlsl -> ColorDownsample_PS.cso (ps_6_0)"
    )

    # --- クラスタードライティング（Forward+）のライトカリング compute 2 パス ---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ClusterBuild_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain -Fo "${SHADER_OUTPUT_DIR}/ClusterBuild_CS.cso" "${SHADER_CLUSTER_BUILD}"
        DEPENDS "${SHADER_CLUSTER_BUILD}" "${SHADER_CLUSTER_INC}"
        COMMENT "Compiling ClusterBuild.hlsl -> ClusterBuild_CS.cso (cs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ClusterCull_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain -Fo "${SHADER_OUTPUT_DIR}/ClusterCull_CS.cso" "${SHADER_CLUSTER_CULL}"
        DEPENDS "${SHADER_CLUSTER_CULL}" "${SHADER_CLUSTER_INC}"
        COMMENT "Compiling ClusterCull.hlsl -> ClusterCull_CS.cso (cs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Forward_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Forward_VS.cso" "${SHADER_FORWARD}"
        DEPENDS "${SHADER_FORWARD}" ${SHADER_FWD_INC}
        COMMENT "Compiling Forward.hlsl -> Forward_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Forward_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Forward_PS.cso" "${SHADER_FORWARD}"
        DEPENDS "${SHADER_FORWARD}" ${SHADER_FWD_INC}
        COMMENT "Compiling Forward.hlsl -> Forward_PS.cso (ps_6_0)"
    )

    # インスタンシング版 forward VS（PS は Forward_PS.cso を共用）
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardInstanced_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/ForwardInstanced_VS.cso" "${SHADER_FWD_INST}"
        DEPENDS "${SHADER_FWD_INST}" ${SHADER_FWD_INC}
        COMMENT "Compiling ForwardInstanced.hlsl -> ForwardInstanced_VS.cso (vs_6_0)"
    )

    # インスタンシング版 深度onlyパス VS（シャドウ/深度プリパス共用）
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ShadowPassInstanced_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/ShadowPassInstanced_VS.cso" "${SHADER_SHADOW_INST}"
        DEPENDS "${SHADER_SHADOW_INST}"
        COMMENT "Compiling ShadowPassInstanced.hlsl -> ShadowPassInstanced_VS.cso (vs_6_0)"
    )

    # LDR 直出力バリアント（サムネイル等、ポストプロセスを通らない R8G8B8A8 RT 用）
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardLdr_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -D LDR_OUTPUT=1 -Fo "${SHADER_OUTPUT_DIR}/ForwardLdr_PS.cso" "${SHADER_FORWARD}"
        DEPENDS "${SHADER_FORWARD}" ${SHADER_FWD_INC}
        COMMENT "Compiling Forward.hlsl -> ForwardLdr_PS.cso (ps_6_0, LDR_OUTPUT)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardSkinned_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/ForwardSkinned_VS.cso" "${SHADER_SKINNED}"
        DEPENDS "${SHADER_SKINNED}" ${SHADER_FWD_INC}
        COMMENT "Compiling ForwardSkinned.hlsl -> ForwardSkinned_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardSkinned_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/ForwardSkinned_PS.cso" "${SHADER_SKINNED}"
        DEPENDS "${SHADER_SKINNED}" ${SHADER_FWD_INC}
        COMMENT "Compiling ForwardSkinned.hlsl -> ForwardSkinned_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardGrid_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/ForwardGrid_VS.cso" "${SHADER_GRID}"
        DEPENDS "${SHADER_GRID}" ${SHADER_FWD_INC}   # Lighting.hlsli/PBR.hlsli を include している
        COMMENT "Compiling ForwardGrid.hlsl -> ForwardGrid_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardGrid_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/ForwardGrid_PS.cso" "${SHADER_GRID}"
        DEPENDS "${SHADER_GRID}" ${SHADER_FWD_INC}   # Lighting.hlsli/PBR.hlsli を include している
        COMMENT "Compiling ForwardGrid.hlsl -> ForwardGrid_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Terrain_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Terrain_VS.cso" "${SHADER_TERRAIN}"
        DEPENDS "${SHADER_TERRAIN}" ${SHADER_FWD_INC}
        COMMENT "Compiling Terrain.hlsl -> Terrain_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Terrain_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Terrain_PS.cso" "${SHADER_TERRAIN}"
        DEPENDS "${SHADER_TERRAIN}" ${SHADER_FWD_INC}
        COMMENT "Compiling Terrain.hlsl -> Terrain_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ShadowPass_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/ShadowPass_VS.cso" "${SHADER_SHADOW}"
        DEPENDS "${SHADER_SHADOW}"
        COMMENT "Compiling ShadowPass.hlsl -> ShadowPass_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ShadowPassSkinned_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/ShadowPassSkinned_VS.cso" "${SHADER_SHADOW_SKIN}"
        DEPENDS "${SHADER_SHADOW_SKIN}"
        COMMENT "Compiling ShadowPassSkinned.hlsl -> ShadowPassSkinned_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/DepthPrepassSkinned_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/DepthPrepassSkinned_VS.cso" "${SHADER_DEPTHPRE_SKIN}"
        DEPENDS "${SHADER_DEPTHPRE_SKIN}"
        COMMENT "Compiling DepthPrepassSkinned.hlsl -> DepthPrepassSkinned_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/DebugLine_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/DebugLine_VS.cso" "${SHADER_DEBUG_LINE}"
        DEPENDS "${SHADER_DEBUG_LINE}"
        COMMENT "Compiling DebugLine.hlsl -> DebugLine_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/DebugLine_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/DebugLine_PS.cso" "${SHADER_DEBUG_LINE}"
        DEPENDS "${SHADER_DEBUG_LINE}"
        COMMENT "Compiling DebugLine.hlsl -> DebugLine_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/PostProcess_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/PostProcess_VS.cso" "${SHADER_POST}"
        DEPENDS "${SHADER_POST}" "${SHADER_POST_INC}"
        COMMENT "Compiling PostProcess.hlsl -> PostProcess_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/PostProcess_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PostPS -Fo "${SHADER_OUTPUT_DIR}/PostProcess_PS.cso" "${SHADER_POST}"
        DEPENDS "${SHADER_POST}" "${SHADER_POST_INC}"
        COMMENT "Compiling PostProcess.hlsl -> PostProcess_PS.cso (ps_6_0)"
    )

    # --- 物理ベースブルーム（VS は FSTriVS / ダウン・アップの 2 PS）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Bloom_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/Bloom_VS.cso" "${SHADER_BLOOM}"
        DEPENDS "${SHADER_BLOOM}" "${SHADER_POST_INC}"
        COMMENT "Compiling Bloom.hlsl -> Bloom_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/BloomDown_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E BloomDownPS -Fo "${SHADER_OUTPUT_DIR}/BloomDown_PS.cso" "${SHADER_BLOOM}"
        DEPENDS "${SHADER_BLOOM}" "${SHADER_POST_INC}"
        COMMENT "Compiling Bloom.hlsl -> BloomDown_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/BloomUp_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E BloomUpPS -Fo "${SHADER_OUTPUT_DIR}/BloomUp_PS.cso" "${SHADER_BLOOM}"
        DEPENDS "${SHADER_BLOOM}" "${SHADER_POST_INC}"
        COMMENT "Compiling Bloom.hlsl -> BloomUp_PS.cso (ps_6_0)"
    )

    # --- 自動露出（compute 2 パス）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ExposureHistogram_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSHistogram -Fo "${SHADER_OUTPUT_DIR}/ExposureHistogram_CS.cso" "${SHADER_AUTOEXPOSURE}"
        DEPENDS "${SHADER_AUTOEXPOSURE}"
        COMMENT "Compiling AutoExposure.hlsl -> ExposureHistogram_CS.cso (cs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ExposureAdapt_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSAdapt -Fo "${SHADER_OUTPUT_DIR}/ExposureAdapt_CS.cso" "${SHADER_AUTOEXPOSURE}"
        DEPENDS "${SHADER_AUTOEXPOSURE}"
        COMMENT "Compiling AutoExposure.hlsl -> ExposureAdapt_CS.cso (cs_6_0)"
    )

    # --- ゴッドレイ（マスク + 放射ブラー）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/GodRaysMask_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E GodRaysMaskPS -Fo "${SHADER_OUTPUT_DIR}/GodRaysMask_PS.cso" "${SHADER_GODRAYS}"
        DEPENDS "${SHADER_GODRAYS}" "${SHADER_POST_INC}"
        COMMENT "Compiling GodRays.hlsl -> GodRaysMask_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/GodRaysBlur_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E GodRaysBlurPS -Fo "${SHADER_OUTPUT_DIR}/GodRaysBlur_PS.cso" "${SHADER_GODRAYS}"
        DEPENDS "${SHADER_GODRAYS}" "${SHADER_POST_INC}"
        COMMENT "Compiling GodRays.hlsl -> GodRaysBlur_PS.cso (ps_6_0)"
    )

    # --- レンズフレア ---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/LensFlare_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E LensFlarePS -Fo "${SHADER_OUTPUT_DIR}/LensFlare_PS.cso" "${SHADER_LENSFLARE}"
        DEPENDS "${SHADER_LENSFLARE}" "${SHADER_POST_INC}"
        COMMENT "Compiling LensFlare.hlsl -> LensFlare_PS.cso (ps_6_0)"
    )

    # --- 被写界深度（CoC / gather / 合成）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/DofCoc_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E DofCocPS -Fo "${SHADER_OUTPUT_DIR}/DofCoc_PS.cso" "${SHADER_DOF}"
        DEPENDS "${SHADER_DOF}" "${SHADER_POST_INC}"
        COMMENT "Compiling DoF.hlsl -> DofCoc_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/DofGather_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E DofGatherPS -Fo "${SHADER_OUTPUT_DIR}/DofGather_PS.cso" "${SHADER_DOF}"
        DEPENDS "${SHADER_DOF}" "${SHADER_POST_INC}"
        COMMENT "Compiling DoF.hlsl -> DofGather_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/DofComposite_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E DofCompositePS -Fo "${SHADER_OUTPUT_DIR}/DofComposite_PS.cso" "${SHADER_DOF}"
        DEPENDS "${SHADER_DOF}" "${SHADER_POST_INC}"
        COMMENT "Compiling DoF.hlsl -> DofComposite_PS.cso (ps_6_0)"
    )

    # --- カメラモーションブラー ---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/MotionBlur_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E MotionBlurPS -Fo "${SHADER_OUTPUT_DIR}/MotionBlur_PS.cso" "${SHADER_MOTIONBLUR}"
        DEPENDS "${SHADER_MOTIONBLUR}" "${SHADER_POST_INC}"
        COMMENT "Compiling MotionBlur.hlsl -> MotionBlur_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Sprite_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Sprite_VS.cso" "${SHADER_SPRITE}"
        DEPENDS "${SHADER_SPRITE}"
        COMMENT "Compiling Sprite.hlsl -> Sprite_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Sprite_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Sprite_PS.cso" "${SHADER_SPRITE}"
        DEPENDS "${SHADER_SPRITE}"
        COMMENT "Compiling Sprite.hlsl -> Sprite_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/IconBillboard_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/IconBillboard_VS.cso" "${SHADER_ICON_BB}"
        DEPENDS "${SHADER_ICON_BB}"
        COMMENT "Compiling IconBillboard.hlsl -> IconBillboard_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/IconBillboard_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/IconBillboard_PS.cso" "${SHADER_ICON_BB}"
        DEPENDS "${SHADER_ICON_BB}"
        COMMENT "Compiling IconBillboard.hlsl -> IconBillboard_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Transition_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/Transition_VS.cso" "${SHADER_TRANSITION}"
        DEPENDS "${SHADER_TRANSITION}" "${SHADER_POST_INC}"
        COMMENT "Compiling Transition.hlsl -> Transition_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Transition_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E TransPS -Fo "${SHADER_OUTPUT_DIR}/Transition_PS.cso" "${SHADER_TRANSITION}"
        DEPENDS "${SHADER_TRANSITION}" "${SHADER_POST_INC}"
        COMMENT "Compiling Transition.hlsl -> Transition_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Emissive_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Emissive_VS.cso" "${SHADER_EMISSIVE}"
        DEPENDS "${SHADER_EMISSIVE}"
        COMMENT "Compiling Emissive.hlsl -> Emissive_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Emissive_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Emissive_PS.cso" "${SHADER_EMISSIVE}"
        DEPENDS "${SHADER_EMISSIVE}"
        COMMENT "Compiling Emissive.hlsl -> Emissive_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Particle_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Particle_VS.cso" "${SHADER_PARTICLE}"
        DEPENDS "${SHADER_PARTICLE}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Particle.hlsl -> Particle_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Particle_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Particle_PS.cso" "${SHADER_PARTICLE}"
        DEPENDS "${SHADER_PARTICLE}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Particle.hlsl -> Particle_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ParticleDistort_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSDistort -Fo "${SHADER_OUTPUT_DIR}/ParticleDistort_PS.cso" "${SHADER_PARTICLE}"
        DEPENDS "${SHADER_PARTICLE}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Particle.hlsl -> ParticleDistort_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Trail_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Trail_VS.cso" "${SHADER_TRAIL}"
        DEPENDS "${SHADER_TRAIL}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Trail.hlsl -> Trail_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Trail_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Trail_PS.cso" "${SHADER_TRAIL}"
        DEPENDS "${SHADER_TRAIL}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Trail.hlsl -> Trail_PS.cso (ps_6_0)"
    )

    # --- GPUパーティクル（compute シム 5 本 + 描画 VS。PS は Particle_PS を流用）---
    foreach(GPCS IN ITEMS Init Prepare Emit Kickoff Simulate)
        add_custom_command(
            OUTPUT "${SHADER_OUTPUT_DIR}/GpuParticle${GPCS}_CS.cso"
            COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CS${GPCS} -Fo "${SHADER_OUTPUT_DIR}/GpuParticle${GPCS}_CS.cso" "${SHADER_GPU_PARTICLE_SIM}"
            DEPENDS "${SHADER_GPU_PARTICLE_SIM}"
            COMMENT "Compiling GpuParticleSim.hlsl -> GpuParticle${GPCS}_CS.cso (cs_6_0)"
        )
    endforeach()

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/GpuParticleDraw_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/GpuParticleDraw_VS.cso" "${SHADER_GPU_PARTICLE_DRAW}"
        DEPENDS "${SHADER_GPU_PARTICLE_DRAW}"
        COMMENT "Compiling GpuParticleDraw.hlsl -> GpuParticleDraw_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Beam_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Beam_VS.cso" "${SHADER_BEAM}"
        DEPENDS "${SHADER_BEAM}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Beam.hlsl -> Beam_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Beam_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Beam_PS.cso" "${SHADER_BEAM}"
        DEPENDS "${SHADER_BEAM}" "${SHADER_PARTICLE_INC}"
        COMMENT "Compiling Beam.hlsl -> Beam_PS.cso (ps_6_0)"
    )

    # --- IBL compute shaders ---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/IrradianceConvolution_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain -Fo "${SHADER_OUTPUT_DIR}/IrradianceConvolution_CS.cso" "${SHADER_IBL_IRR}"
        DEPENDS "${SHADER_IBL_IRR}" "${SHADER_IBL_INC}"
        COMMENT "Compiling IrradianceConvolution.hlsl -> IrradianceConvolution_CS.cso (cs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/PrefilterEnv_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain -Fo "${SHADER_OUTPUT_DIR}/PrefilterEnv_CS.cso" "${SHADER_IBL_PRE}"
        DEPENDS "${SHADER_IBL_PRE}" "${SHADER_IBL_INC}"
        COMMENT "Compiling PrefilterEnv.hlsl -> PrefilterEnv_CS.cso (cs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/IntegrateBRDF_CS.cso"
        COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E CSMain -Fo "${SHADER_OUTPUT_DIR}/IntegrateBRDF_CS.cso" "${SHADER_IBL_BRDF}"
        DEPENDS "${SHADER_IBL_BRDF}" "${SHADER_IBL_INC}"
        COMMENT "Compiling IntegrateBRDF.hlsl -> IntegrateBRDF_CS.cso (cs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Skybox_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/Skybox_VS.cso" "${SHADER_SKYBOX}"
        DEPENDS "${SHADER_SKYBOX}"
        COMMENT "Compiling Skybox.hlsl -> Skybox_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/Skybox_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/Skybox_PS.cso" "${SHADER_SKYBOX}"
        DEPENDS "${SHADER_SKYBOX}"
        COMMENT "Compiling Skybox.hlsl -> Skybox_PS.cso (ps_6_0)"
    )

    # --- SSAO（VS は FSTriVS 流用 / Blur は SSAO_VS.cso を共有）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SSAO_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/SSAO_VS.cso" "${SHADER_SSAO}"
        DEPENDS "${SHADER_SSAO}" "${SHADER_POST_INC}"
        COMMENT "Compiling SSAO.hlsl -> SSAO_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SSAO_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/SSAO_PS.cso" "${SHADER_SSAO}"
        DEPENDS "${SHADER_SSAO}" "${SHADER_POST_INC}"
        COMMENT "Compiling SSAO.hlsl -> SSAO_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/SSAOBlur_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/SSAOBlur_PS.cso" "${SHADER_SSAO_BLUR}"
        DEPENDS "${SHADER_SSAO_BLUR}" "${SHADER_POST_INC}"
        COMMENT "Compiling SSAOBlur.hlsl -> SSAOBlur_PS.cso (ps_6_0)"
    )

    # --- コンタクトシャドウ（VS は FSTriVS 流用。SSAO と同じフルスクリーン 1 パス）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ContactShadow_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E FSTriVS -Fo "${SHADER_OUTPUT_DIR}/ContactShadow_VS.cso" "${SHADER_CONTACT_SHADOW}"
        DEPENDS "${SHADER_CONTACT_SHADOW}" "${SHADER_POST_INC}"
        COMMENT "Compiling ContactShadow.hlsl -> ContactShadow_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ContactShadow_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/ContactShadow_PS.cso" "${SHADER_CONTACT_SHADOW}"
        DEPENDS "${SHADER_CONTACT_SHADOW}" "${SHADER_POST_INC}"
        COMMENT "Compiling ContactShadow.hlsl -> ContactShadow_PS.cso (ps_6_0)"
    )

    # --- 速度バッファ（モーションベクター）生成パス: 静的 / インスタンシング / スキンド ---
    # PS は 3 経路で共有（VelocityCommon.hlsli の VelocityPS）。VelocityPrepass.hlsl からだけ出す。
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/VelocityPrepass_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/VelocityPrepass_VS.cso" "${SHADER_VELOCITY}"
        DEPENDS "${SHADER_VELOCITY}" ${SHADER_VELOCITY_INC}
        COMMENT "Compiling VelocityPrepass.hlsl -> VelocityPrepass_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/VelocityPrepass_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E VelocityPS -Fo "${SHADER_OUTPUT_DIR}/VelocityPrepass_PS.cso" "${SHADER_VELOCITY}"
        DEPENDS "${SHADER_VELOCITY}" ${SHADER_VELOCITY_INC}
        COMMENT "Compiling VelocityPrepass.hlsl -> VelocityPrepass_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/VelocityPrepassInstanced_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/VelocityPrepassInstanced_VS.cso" "${SHADER_VELOCITY_INST}"
        DEPENDS "${SHADER_VELOCITY_INST}" ${SHADER_VELOCITY_INC}
        COMMENT "Compiling VelocityPrepassInstanced.hlsl -> VelocityPrepassInstanced_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/VelocityPrepassSkinned_VS.cso"
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo "${SHADER_OUTPUT_DIR}/VelocityPrepassSkinned_VS.cso" "${SHADER_VELOCITY_SKIN}"
        DEPENDS "${SHADER_VELOCITY_SKIN}" ${SHADER_VELOCITY_INC}
        COMMENT "Compiling VelocityPrepassSkinned.hlsl -> VelocityPrepassSkinned_VS.cso (vs_6_0)"
    )

    # --- TAA 解決 / 速度バッファのデバッグ可視化（VS は PostProcess_VS.cso を流用）---
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/TaaResolve_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E TaaResolvePS -Fo "${SHADER_OUTPUT_DIR}/TaaResolve_PS.cso" "${SHADER_TAA}"
        DEPENDS "${SHADER_TAA}" "${SHADER_POST_INC}"
        COMMENT "Compiling TAA.hlsl -> TaaResolve_PS.cso (ps_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/VelocityDebug_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E VelocityDebugPS -Fo "${SHADER_OUTPUT_DIR}/VelocityDebug_PS.cso" "${SHADER_VEL_DEBUG}"
        DEPENDS "${SHADER_VEL_DEBUG}" "${SHADER_POST_INC}"
        COMMENT "Compiling VelocityDebug.hlsl -> VelocityDebug_PS.cso (ps_6_0)"
    )

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
