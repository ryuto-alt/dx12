add_library(Resource STATIC
    ShaderCompiler.cpp
)
target_include_directories(Resource PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(Resource PUBLIC Core)
