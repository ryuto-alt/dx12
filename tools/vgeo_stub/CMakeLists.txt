# vgeo_stub ― 合成メッシュから LOD0 のみの .vgeo を書く CLI（仮想ジオメトリ P0）。
#
# ★既定ビルドには入れない（EXCLUDE_FROM_ALL）。必要なときだけ:
#     pwsh tools/build.ps1 -Target vgeo_stub          → build/release/vgeo_stub.exe
# src/renderer/CMakeLists.txt から add_subdirectory される（meshoptimizer::meshoptimizer が見える文脈で）。
# ヘッダオンリーの実装は src/renderer/vg/（VgeoFormat.h / VgeoBvh.h / VgsrcFormat.h）と VgeoStubGen.h。
add_executable(vgeo_stub EXCLUDE_FROM_ALL main.cpp VgeoStubGen.h)
target_include_directories(vgeo_stub PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vgeo_stub PRIVATE meshoptimizer::meshoptimizer)
target_compile_features(vgeo_stub PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(vgeo_stub PRIVATE /W4 /WX /permissive- /utf-8 /external:anglebrackets /external:W0)
    target_compile_definitions(vgeo_stub PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# meshoptimizer.dll を exe の隣へ（vcpkg の applocal に頼らない。ルートの DX12Engine と同じ流儀）
add_custom_command(TARGET vgeo_stub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:meshoptimizer::meshoptimizer>" "$<TARGET_FILE_DIR:vgeo_stub>"
    COMMENT "Bundling meshoptimizer.dll next to vgeo_stub.exe")
