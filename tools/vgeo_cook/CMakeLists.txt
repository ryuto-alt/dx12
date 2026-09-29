# vgeo_cook ― 仮想ジオメトリ（Nanite 風）のオフライン cooker（P1）。VGSRC / OBJ → 階層 LOD の完全な .vgeo。
#
# ★既定ビルドには CLI を入れない（EXCLUDE_FROM_ALL）。必要なときだけ:
#     pwsh tools/build.ps1 -Target vgeo_cook          → build/release/tools/vgeo_cook/vgeo_cook.exe
# ライブラリ VgeoCookLib（cooker 本体）はテスト VgeoCookTests が使うので、テストのビルドで自動的に建つ。
# src/renderer/CMakeLists.txt から add_subdirectory される（meshoptimizer::meshoptimizer が見える文脈で）。
# ヘッダオンリーの形式実装は src/renderer/vg/（VgeoFormat.h / VgeoBvh.h / VgsrcFormat.h）。
add_library(VgeoCookLib STATIC EXCLUDE_FROM_ALL VgeoCook.cpp VgeoCook.h CookGroup.h CookUtil.h CookBench.h CookObj.h)
target_include_directories(VgeoCookLib PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VgeoCookLib PUBLIC meshoptimizer::meshoptimizer)
target_compile_features(VgeoCookLib PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(VgeoCookLib PRIVATE /W4 /WX /permissive- /utf-8 /external:anglebrackets /external:W0)
    target_compile_definitions(VgeoCookLib PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(VgeoCookLib PUBLIC psapi)
endif()

add_executable(vgeo_cook EXCLUDE_FROM_ALL main.cpp)
target_link_libraries(vgeo_cook PRIVATE VgeoCookLib)
if(MSVC)
    target_compile_options(vgeo_cook PRIVATE /W4 /WX /permissive- /utf-8 /external:anglebrackets /external:W0)
    target_compile_definitions(vgeo_cook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# meshoptimizer.dll を exe の隣へ（vcpkg の applocal に頼らない。vgeo_stub と同じ流儀）
add_custom_command(TARGET vgeo_cook POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:vgeo_cook>"
    COMMENT "Bundling meshoptimizer.dll next to vgeo_cook.exe")
