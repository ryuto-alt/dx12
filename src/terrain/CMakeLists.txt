add_library(Terrain STATIC
    HeightField.cpp
    TerrainBrush.cpp
    TerrainMeshBuilder.cpp
    TerrainIO.cpp
)
target_include_directories(Terrain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
# Renderer: TerrainMeshBuilder が renderer/Mesh.h の Vertex をそのまま吐く（地形を
# 「普通の Mesh を持つ普通のエンティティ」にするため）。Core: .hf の読み書きが vfs / Logger を使う。
# Renderer は Terrain に依存しないのでサイクルにならない。
target_link_libraries(Terrain PUBLIC Core Renderer Microsoft::DirectXMath)

target_compile_options(Terrain PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Terrain PRIVATE ${DX12_COMPILE_DEFS})

# 任意メッシュの頂点スカルプト（ハイトフィールドでは作れないオーバーハング/異形の担当）。
# 上の add_library のリストへ挿し込まず target_sources で足しているのは、同じファイルを
# 同時に触る作業とぶつからないようにするため（末尾追記だけで済む）。
# SculptMesh.cpp は純ロジック（TerrainBrush の重み関数とノイズだけを共有）で、
# SculptIO.cpp が .smsh の vfs 読み / ディスク書きを担当する。
target_sources(Terrain PRIVATE
    SculptMesh.cpp
    SculptIO.cpp
)
