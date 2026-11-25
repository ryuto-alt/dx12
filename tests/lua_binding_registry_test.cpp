// Lua 束縛の登録表（W05-15）の単体テスト。GPU 不要。
// post.* / ssao.* が scripting/bind/Post.cpp へ移った後も、関数名・引数・戻り値・警告時の挙動が同じであること。
#include "scripting/LuaBindingRegistry.h"
#include "scene/Scene.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <sol/sol.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace dx12e;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)
}

int main()
{
    Scene scene;
    Scene* slot = &scene;
    ScriptContext ctx;
    ctx.scene = &slot;

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
    RunAllLuaBindings(lua, ctx);

    // post の 5 関数と ssao の 5 関数が揃っている
    for (const char* t : { "post", "ssao" })
        for (const char* f : { "get", "set", "setMany", "names" })
        {
            const std::string code = std::string("return type(") + t + "." + f + ")";
            CHECK(lua.script(code).get<std::string>() == "function");
        }

    // set / get の往復（bool / float / int / vec3 / string）
    CHECK(lua.script("return post.set('bloomOn', true)").get<bool>());
    CHECK(scene.GetPostSettings().bloomOn);
    CHECK(lua.script("return post.set('bloom', 0.8)").get<bool>());
    CHECK(std::abs(scene.GetPostSettings().bloom - 0.8f) < 1e-6f);
    CHECK(lua.script("return post.get('bloom')").get<float>() > 0.79f);
    CHECK(lua.script("return post.set('tonemapper', 3)").get<bool>());
    CHECK(scene.GetPostSettings().tonemapper == 3);
    CHECK(lua.script("return post.set('tint', {0.5, 0.25, 0.125})").get<bool>());
    CHECK(std::abs(scene.GetPostSettings().tint.y - 0.25f) < 1e-6f);
    CHECK(lua.script("return post.set('lutPath', 'a/b.png')").get<bool>());
    CHECK(scene.GetPostSettings().lutPath == "a/b.png");
    CHECK(lua.script("return post.get('lutPath')").get<std::string>() == "a/b.png");

    // 不明なキー: get は nil、set は false
    CHECK(lua.script("return post.get('noSuchField') == nil").get<bool>());
    CHECK(!lua.script("return post.set('noSuchField', 1)").get<bool>());
    // 型違い（文字列の項目へ数値）は false
    CHECK(!lua.script("return post.set('lutPath', 3)").get<bool>());

    // setMany は成功した個数を返す（不明キーは数えない）
    CHECK(lua.script("return post.setMany{ vignetteOn=true, vignette=0.9, bogus=1 }").get<int>() == 2);
    CHECK(scene.GetPostSettings().vignetteOn);

    // names は 80 個以上の文字列の表
    CHECK(lua.script("return #post.names()").get<int>() >= 80);
    CHECK(lua.script("return post.names()[1]").get<std::string>() == "enabled");

    // ssao
    CHECK(lua.script("return ssao.set('radius', 1.5)").get<bool>());
    CHECK(std::abs(scene.GetSSAOSettings().radius - 1.5f) < 1e-6f);
    CHECK(lua.script("return ssao.setMany{ intensity=2.0, power=3.0 }").get<int>() == 2);
    CHECK(lua.script("return #ssao.names()").get<int>() >= 8);

    // シーンが無いとき（ScriptEngine::m_scene が null）は、get は nil / set は false / setMany は 0
    slot = nullptr;
    CHECK(lua.script("return post.get('bloom') == nil").get<bool>());
    CHECK(!lua.script("return post.set('bloom', 1)").get<bool>());
    CHECK(lua.script("return post.setMany{ bloom=1 }").get<int>() == 0);
    // シーンが差し替わっても、同じ束縛が新しい方を見る
    Scene scene2;
    slot = &scene2;
    CHECK(lua.script("return post.set('bloom', 0.123)").get<bool>());
    CHECK(std::abs(scene2.GetPostSettings().bloom - 0.123f) < 1e-6f);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
