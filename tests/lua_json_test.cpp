// Lua の値 ⇔ JSON（scripting/LuaJson.h）の往復テスト。セーブ本体の心臓部。
//
// 守りたいこと:
//  - 入れ子テーブル・配列・数値・文字列・真偽が 1 ビットも変わらず戻る
//  - 整数と小数の区別（3 と 3.0）が戻る（Lua 5.4 は math.type で見分けるので、崩れるとゲームの判定が変わる）
//  - JSON に素直に落ちない形（数値キー混在・疎な配列・"$" 始まりのキー・NaN・不正 UTF-8）も戻る
//  - 保存できないもの（関数）はテーブルの中なら飛ばして報告、最上位なら失敗
//  - 循環参照は無限ループせず失敗する
//
// 比べ方: Lua 側に「深い比較」関数を書いて、往復前後のテーブルを Lua 自身に比べさせる
// （C++ 側で比べると「比べ方のバグ」でテストが通ってしまう）。

#include "scripting/LuaJson.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace dx12e;
using nlohmann::json;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);      \
            ++g_failed;                                                        \
        }                                                                      \
    } while (0)

static const char* kDeepEq = R"(
function deepEq(a, b, path)
  path = path or "root"
  if type(a) ~= type(b) then return false, path .. ": type " .. type(a) .. " vs " .. type(b) end
  if type(a) == "number" then
    if math.type(a) ~= math.type(b) then return false, path .. ": " .. math.type(a) .. " vs " .. math.type(b) end
    if a ~= a and b ~= b then return true end   -- NaN
    if a ~= b then return false, path .. ": " .. tostring(a) .. " vs " .. tostring(b) end
    return true
  end
  if type(a) == "userdata" then
    if a.x ~= b.x or a.y ~= b.y or a.z ~= b.z then return false, path .. ": vec3" end
    return true
  end
  if type(a) ~= "table" then
    if a ~= b then return false, path .. ": " .. tostring(a) .. " vs " .. tostring(b) end
    return true
  end
  for k, v in pairs(a) do
    local ok, why = deepEq(v, b[k], path .. "." .. tostring(k))
    if not ok then return false, why end
  end
  for k, _ in pairs(b) do
    if a[k] == nil then return false, path .. "." .. tostring(k) .. ": 余計なキー" end
  end
  return true
end
)";

// src を Lua で評価したテーブルを JSON → 文字列 → JSON → Lua と往復させ、deepEq で比べる
static void RoundTrip(sol::state& lua, const char* label, const char* src)
{
    sol::object orig = lua.safe_script(std::string("return ") + src);
    auto enc = luajson::ToJson(orig);
    if (!enc.ok) { std::printf("  encode error: %s\n", enc.error.c_str()); }
    CHECK(enc.ok, label);
    const std::string text = enc.value.dump();                      // ファイルに書く形
    const json back = json::parse(text);
    sol::object restored = luajson::FromJson(lua, back);
    sol::protected_function eq = lua["deepEq"];
    auto r = eq(orig, restored);
    const bool same = r.valid() && r.get<bool>(0);
    if (!same && r.valid()) std::printf("  %s: %s\n  json=%s\n", label, r.get<std::string>(1).c_str(), text.c_str());
    CHECK(same, label);
}

int main()
{
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
    lua.new_usertype<DirectX::XMFLOAT3>("Vec3",
        sol::constructors<DirectX::XMFLOAT3(), DirectX::XMFLOAT3(float, float, float)>(),
        "x", &DirectX::XMFLOAT3::x, "y", &DirectX::XMFLOAT3::y, "z", &DirectX::XMFLOAT3::z);
    lua.safe_script(kDeepEq);

    // ---- 素直な形 ----
    RoundTrip(lua, "入れ子のテーブルと配列", R"({ hp = 3, name = "勇者", alive = true,
        inventory = { "sword", "potion", "key" },
        stats = { str = 10, dex = 7.5, tags = { "a", "b" } },
        grid = { {1,2,3}, {4,5,6} } })");
    RoundTrip(lua, "整数と小数の区別", R"({ i = 3, f = 3.0, neg = -2, big = 9007199254740993, tiny = 1e-300 })");
    RoundTrip(lua, "空テーブル", "{ empty = {}, list = { {} } }");
    RoundTrip(lua, "最上位が配列", "{ 10, 20, 30 }");
    RoundTrip(lua, "Vec3", "{ pos = Vec3.new(1.5, -2, 3.25), path = { Vec3.new(0,0,0), Vec3.new(1,1,1) } }");

    // ---- JSON に素直に落ちない形 ----
    RoundTrip(lua, "数値キーと文字列キーの混在", R"({ 1, 2, 3, mode = "x" })");
    RoundTrip(lua, "疎な配列", "{ [1] = 'a', [3] = 'c', [10] = 'j' }");
    RoundTrip(lua, "0 と負の数値キー", "{ [0] = 'zero', [-1] = 'neg' }");
    RoundTrip(lua, "小数キーと真偽キー", "{ [1.5] = 'half', [true] = 'yes' }");
    RoundTrip(lua, "$ で始まるキー", R"({ ["$map"] = 1, ["$vec3"] = "not a vec" })");
    RoundTrip(lua, "NaN と無限大", "{ nan = 0/0, inf = math.huge, ninf = -math.huge }");
    RoundTrip(lua, "UTF-8 として不正な文字列", R"({ bin = "\xff\xfe\x00abc", key = { ["\xc3\x28"] = 1 } })");
    RoundTrip(lua, "最上位が文字列", R"("hello")");

    // ---- 保存できないもの ----
    {
        sol::object o = lua.safe_script("return { hp = 3, onHit = function() end, list = { 1, function() end, 3 } }");
        auto enc = luajson::ToJson(o);
        CHECK(enc.ok, "関数を含んでも全体は成功する");
        CHECK(enc.skipped.size() == 2, "飛ばした 2 か所が報告される");
        CHECK(!enc.value.contains("onHit"), "関数のキーは消える");
        CHECK(enc.value["hp"] == 3, "他の値は残る");
        CHECK(enc.value["list"].is_array() && enc.value["list"].size() == 3 && enc.value["list"][1].is_null(),
              "配列の中の関数は null（後ろの添字がずれない）");
    }
    {
        sol::object f = lua.safe_script("return function() end");
        auto enc = luajson::ToJson(f);
        CHECK(!enc.ok && !enc.error.empty(), "最上位が関数なら失敗 + 理由");
    }
    {
        sol::object cyc = lua.safe_script("local t = { a = 1 }; t.self = t; return t");
        auto enc = luajson::ToJson(cyc);
        CHECK(!enc.ok, "循環参照は失敗する（無限ループしない）");
        CHECK(enc.error.find("self") != std::string::npos, "どこで回ったかが error に出る");
    }
    {
        sol::object shared = lua.safe_script("local s = { v = 1 }; return { a = s, b = s }");
        auto enc = luajson::ToJson(shared);
        CHECK(enc.ok, "同じテーブルを 2 か所から指すのは循環ではない（値として 2 回書く）");
    }
    {
        sol::object deep = lua.safe_script("local t = {}; local c = t; for i=1,100 do c.n = {}; c = c.n end; return t");
        auto enc = luajson::ToJson(deep, nullptr, 64);
        CHECK(!enc.ok, "64 段を超える入れ子は失敗");
    }

    // ---- 壊れた JSON 側からの読み戻し（落ちない）----
    {
        std::vector<std::string> warns;
        sol::object o = luajson::FromJson(lua, json::parse(R"({"a": {"$unknownTag": 1}, "b": {"$vec3": [1, 2]}})"),
                                          nullptr, &warns);
        CHECK(o.get_type() == sol::type::table, "知らないタグがあってもテーブルは返る");
        CHECK(warns.size() == 2, "知らないタグと壊れた $vec3 の 2 件が警告になる");
    }

    CHECK(luajson::IsValidUtf8("abc\xE3\x81\x82"), "正しい UTF-8");
    CHECK(!luajson::IsValidUtf8("\xC0\x80"), "過長表現は不正");
    CHECK(!luajson::IsValidUtf8("\xED\xA0\x80"), "サロゲートは不正");
    CHECK(!luajson::IsValidUtf8("\xE3\x81"), "途中で切れた列は不正");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
