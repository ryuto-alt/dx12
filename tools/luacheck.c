/* luacheck — game.lua 等の Lua スクリプトを構文チェックするだけの小ツール。
 * luaL_loadfile は実行せずにパースのみ行うので、構文エラー（括弧/end 抜け等）を確実に検出できる。
 * 使い方: luacheck <file.lua> [more.lua ...]   全部 OK なら exit 0、1 つでも構文エラーなら exit 1。
 * ビルド: vcvars64 後に
 *   cl /nologo /I<vcpkg>/include luacheck.c <vcpkg>/lib/lua.lib
 */
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: luacheck <file.lua> [...]\n"); return 2; }
    int bad = 0;
    for (int i = 1; i < argc; ++i)
    {
        lua_State* L = luaL_newstate();
        int r = luaL_loadfile(L, argv[i]);
        if (r != LUA_OK)
        {
            fprintf(stderr, "SYNTAX ERROR  %s\n  %s\n", argv[i], lua_tostring(L, -1));
            bad = 1;
        }
        else
        {
            printf("OK  %s\n", argv[i]);
        }
        lua_close(L);
    }
    return bad;
}
