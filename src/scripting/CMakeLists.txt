add_library(Scripting STATIC
    ScriptEngine.cpp
    ScriptEngineAi.cpp      # nav / 群衆 / Brain の Lua API
    ScriptAudioBindings.cpp   # Lua の audio（音の担当。ScriptEngine.cpp から分けて衝突を避ける）
    ScriptSaveBindings.cpp    # Lua の save（セーブの担当。同上）
    LuaJson.cpp               # Lua の値 ⇔ JSON（セーブ本体。tests/lua_json_test.cpp）
)

target_include_directories(Scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(directxmath CONFIG REQUIRED)

target_link_libraries(Scripting PUBLIC
    Core
    Scene
    ECS
    Input
    Renderer
    Animation
    Audio
    Physics
    Network
    Nav
    AI
    ${LUA_LIBRARIES}
    sol2::sol2
    Microsoft::DirectXMath
)

target_include_directories(Scripting PRIVATE ${LUA_INCLUDE_DIR})

target_compile_options(Scripting PRIVATE ${DX12_COMPILE_OPTIONS} /bigobj)
target_compile_definitions(Scripting PRIVATE ${DX12_COMPILE_DEFS})
