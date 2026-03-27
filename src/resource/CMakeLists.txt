find_package(directxtex CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
    GIT_SHALLOW    TRUE
)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(assimp)

add_library(Resource STATIC
    ShaderCompiler.cpp
    TextureLoader.cpp
    ModelLoader.cpp
    ResourceManager.cpp
)
target_include_directories(Resource PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Resource PUBLIC
    Core
    Graphics
    Animation
    assimp
    Microsoft::DirectXTex
)

target_compile_options(Resource PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Resource PRIVATE ${DX12_COMPILE_DEFS})
