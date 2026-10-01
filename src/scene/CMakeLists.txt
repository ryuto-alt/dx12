add_library(Scene STATIC
    Entity.cpp
    Scene.cpp
    SceneSerializer.cpp
    SceneFormatV2.cpp
    SceneFlow.cpp
    ComponentRegistry.cpp
    NavSceneGather.cpp
    GiMigration.cpp
    ShaderParamTween.cpp
)

# シーンファイル形式 v2 の既定値表（凍結データ）をバイト配列のヘッダへ埋め込む。
# 実行時にファイルを探さない＝Game.exe（pak 配布）でも同じ表が使える。表が変わったら再構成が走る。
set(SCENE_DEFAULTS_V2_JSON ${CMAKE_CURRENT_SOURCE_DIR}/scene_defaults_v2.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SCENE_DEFAULTS_V2_JSON})
file(READ ${SCENE_DEFAULTS_V2_JSON} _scene_defaults_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _scene_defaults_bytes "${_scene_defaults_hex}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/scene_defaults_v2_data.h
     CONTENT "// generated from scene_defaults_v2.json (do not edit)
static const unsigned char kSceneDefaultsV2Bytes[] = { ${_scene_defaults_bytes} 0x20 };
"
     @ONLY NEWLINE_STYLE UNIX)

target_include_directories(Scene PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(Scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/include   # 封印ランタイムの中立公開ヘッダ（engine/...）
)

find_package(directxmath CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(Scene PUBLIC
    Core
    Graphics
    Renderer
    Animation
    Resource
    ECS
    Terrain
    Nav
    Microsoft::DirectXMath
    nlohmann_json::nlohmann_json
)

target_compile_options(Scene PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Scene PRIVATE ${DX12_COMPILE_DEFS})
