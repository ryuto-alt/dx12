cmake_minimum_required(VERSION 3.28)
project(DX12Engine VERSION 0.1.0 LANGUAGES CXX RC)  # RC = .rc リソース（アプリアイコン）

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DX12_BUILD_TESTS "Build unit tests" ON)

# MSVC compile options (not global - applied per-target to avoid conflicts with FetchContent deps)
set(DX12_COMPILE_OPTIONS /W4 /WX /MP /permissive- /utf-8 /external:anglebrackets /external:W0)
set(DX12_COMPILE_DEFS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# Dependencies
find_package(spdlog CONFIG REQUIRED)

# Sub-projects
add_subdirectory(src/core)
add_subdirectory(src/graphics)
add_subdirectory(src/renderer)
add_subdirectory(src/resource)
add_subdirectory(src/animation)
add_subdirectory(src/ecs)
add_subdirectory(src/input)
add_subdirectory(src/audio)
add_subdirectory(src/scene)
add_subdirectory(src/scripting)
add_subdirectory(src/physics)
add_subdirectory(src/gui)
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
        Scene
        Scripting
        Physics
        Gui
        Editor
        ProjectLib
)

if(MSVC)
    target_compile_options(DX12Engine PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(DX12Engine PRIVATE ${DX12_COMPILE_DEFS})
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
        Scene
        Scripting
        Physics
        Gui
        Editor
        ProjectLib
)
if(MSVC)
    target_compile_options(GameRuntime PRIVATE ${DX12_COMPILE_OPTIONS})
    target_compile_definitions(GameRuntime PRIVATE ${DX12_COMPILE_DEFS} DX12_GAME_RUNTIME)
endif()

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
    set(SHADER_FWD_INC
        "${CMAKE_SOURCE_DIR}/shaders/forward/PBR.hlsli"
        "${CMAKE_SOURCE_DIR}/shaders/forward/Lighting.hlsli")
    set(SHADER_GRID          "${CMAKE_SOURCE_DIR}/shaders/forward/ForwardGrid.hlsl")
    set(SHADER_SHADOW        "${CMAKE_SOURCE_DIR}/shaders/shadow/ShadowPass.hlsl")
    set(SHADER_SHADOW_SKIN   "${CMAKE_SOURCE_DIR}/shaders/shadow/ShadowPassSkinned.hlsl")
    set(SHADER_DEPTHPRE_SKIN "${CMAKE_SOURCE_DIR}/shaders/shadow/DepthPrepassSkinned.hlsl")
    set(SHADER_DEBUG_LINE    "${CMAKE_SOURCE_DIR}/shaders/debug/DebugLine.hlsl")
    set(SHADER_POST          "${CMAKE_SOURCE_DIR}/shaders/post/PostProcess.hlsl")
    set(SHADER_POST_INC      "${CMAKE_SOURCE_DIR}/shaders/post/FullscreenTri.hlsli")
    set(SHADER_SPRITE        "${CMAKE_SOURCE_DIR}/shaders/sprite/Sprite.hlsl")
    set(SHADER_ICON_BB       "${CMAKE_SOURCE_DIR}/shaders/editor/IconBillboard.hlsl")
    set(SHADER_TRANSITION    "${CMAKE_SOURCE_DIR}/shaders/post/Transition.hlsl")
    set(SHADER_PARTICLE      "${CMAKE_SOURCE_DIR}/shaders/particle/Particle.hlsl")
    set(SHADER_BEAM          "${CMAKE_SOURCE_DIR}/shaders/particle/Beam.hlsl")
    set(SHADER_PARTICLE_INC  "${CMAKE_SOURCE_DIR}/shaders/particle/ParticleCommon.hlsli")
    set(SHADER_IBL_IRR       "${CMAKE_SOURCE_DIR}/shaders/ibl/IrradianceConvolution.hlsl")
    set(SHADER_IBL_PRE       "${CMAKE_SOURCE_DIR}/shaders/ibl/PrefilterEnv.hlsl")
    set(SHADER_IBL_BRDF      "${CMAKE_SOURCE_DIR}/shaders/ibl/IntegrateBRDF.hlsl")
    set(SHADER_SKYBOX        "${CMAKE_SOURCE_DIR}/shaders/ibl/Skybox.hlsl")
    set(SHADER_IBL_INC       "${CMAKE_SOURCE_DIR}/shaders/ibl/IBLCommon.hlsli")
    set(SHADER_SSAO          "${CMAKE_SOURCE_DIR}/shaders/ssao/SSAO.hlsl")
    set(SHADER_SSAO_BLUR     "${CMAKE_SOURCE_DIR}/shaders/ssao/SSAOBlur.hlsl")

    set(SHADER_OUTPUTS
        "${SHADER_OUTPUT_DIR}/Forward_VS.cso"
        "${SHADER_OUTPUT_DIR}/Forward_PS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardSkinned_PS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardGrid_VS.cso"
        "${SHADER_OUTPUT_DIR}/ForwardGrid_PS.cso"
        "${SHADER_OUTPUT_DIR}/ShadowPass_VS.cso"
        "${SHADER_OUTPUT_DIR}/ShadowPassSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/DepthPrepassSkinned_VS.cso"
        "${SHADER_OUTPUT_DIR}/DebugLine_VS.cso"
        "${SHADER_OUTPUT_DIR}/DebugLine_PS.cso"
        "${SHADER_OUTPUT_DIR}/PostProcess_VS.cso"
        "${SHADER_OUTPUT_DIR}/PostProcess_PS.cso"
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
        "${SHADER_OUTPUT_DIR}/Beam_VS.cso"
        "${SHADER_OUTPUT_DIR}/Beam_PS.cso"
        "${SHADER_OUTPUT_DIR}/IrradianceConvolution_CS.cso"
        "${SHADER_OUTPUT_DIR}/PrefilterEnv_CS.cso"
        "${SHADER_OUTPUT_DIR}/IntegrateBRDF_CS.cso"
        "${SHADER_OUTPUT_DIR}/Skybox_VS.cso"
        "${SHADER_OUTPUT_DIR}/Skybox_PS.cso"
        "${SHADER_OUTPUT_DIR}/SSAO_VS.cso"
        "${SHADER_OUTPUT_DIR}/SSAO_PS.cso"
        "${SHADER_OUTPUT_DIR}/SSAOBlur_PS.cso"
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
        DEPENDS "${SHADER_GRID}"
        COMMENT "Compiling ForwardGrid.hlsl -> ForwardGrid_VS.cso (vs_6_0)"
    )

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT_DIR}/ForwardGrid_PS.cso"
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo "${SHADER_OUTPUT_DIR}/ForwardGrid_PS.cso" "${SHADER_GRID}"
        DEPENDS "${SHADER_GRID}"
        COMMENT "Compiling ForwardGrid.hlsl -> ForwardGrid_PS.cso (ps_6_0)"
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

    add_custom_target(Shaders ALL DEPENDS ${SHADER_OUTPUTS})
    add_dependencies(DX12Engine Shaders)
    add_dependencies(GameRuntime Shaders)
else()
    message(WARNING "DXC not found -- shader compilation skipped")
endif()

# Shader directory and assets directory (Core needs it because Application.cpp loads shaders/assets)
target_compile_definitions(Core PRIVATE
    SHADER_DIR=L"${CMAKE_BINARY_DIR}/shaders/"
    ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/"
    SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts/"
)
