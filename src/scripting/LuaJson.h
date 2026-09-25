#pragma once

// Lua の値 ⇔ JSON の変換（セーブの本体に使う）。
//
// ★JSON に素直に落ちない Lua の形は、1 キーだけの「$タグ付きオブジェクト」で表す。
//   普通のオブジェクトに "$" で始まるキーは出てこない（出るときは $map 形式へ逃がす）ので、
//   読み戻しで取り違えない。
//     配列（キーが 1..n で隙間なし）       → JSON 配列
//     キーが全部文字列（"$" 始まり無し）   → JSON オブジェクト
//     それ以外（数値キー混在・疎な配列等） → {"$map": [[キー, 値], ...]}
//     Vec3                                 → {"$vec3": [x, y, z]}
//     NaN / ±inf                           → {"$num": "nan" | "inf" | "-inf"}
//     UTF-8 として不正な文字列             → {"$bin": "16進"}（JSON は UTF-8 しか運べない）
//     その他の userdata（Entity 等）       → Hooks.encodeUserdata が決める（例 {"$entity": …}）
//   整数と小数は区別して往復する（Lua 5.4 の整数型を保つ: 3 は 3、3.0 は 3.0 のまま）。
//   空テーブルは {}（読み戻すと空テーブル）。
//
// 保存できないもの（関数・コルーチン・知らない userdata）は、テーブルの中なら
// そのキーを飛ばして skipped に積む（セーブ全体は止めない）。最上位がそれなら失敗。
// 循環参照は表せないので失敗（どこで回ったかを error に書く）。
//
// sol2 の設定マクロは ScriptEngine.cpp と揃えること（TU ごとに違うと ODR 違反になる）。

#pragma warning(push)
#pragma warning(disable: 4100 4189 4244 4267 4996)
#ifndef SOL_ALL_SAFETIES_ON
#define SOL_ALL_SAFETIES_ON 1
#endif
#include <sol/sol.hpp>
#pragma warning(pop)

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dx12e::luajson
{

struct Hooks
{
    // userdata を JSON へ。対応しない型なら std::nullopt（→ 保存できない値として扱う）。
    std::function<std::optional<nlohmann::json>(const sol::object&)> encodeUserdata;
    // "$タグ" の値を Lua へ。対応しないタグなら sol::lua_nil を返す（→ 警告を積んで nil）。
    std::function<sol::object(sol::state_view, const std::string& tag, const nlohmann::json& payload)> decodeTag;
};

struct EncodeResult
{
    bool                     ok = false;
    nlohmann::json           value;
    std::string              error;     // ok=false のときの理由（どのキーで何が起きたか）
    std::vector<std::string> skipped;   // 飛ばしたキーのパス（"data.inventory[3].onUse: function" の形）
};

// maxDepth を超える入れ子は失敗（循環検出とは別の安全弁）
EncodeResult ToJson(const sol::object& value, const Hooks* hooks = nullptr, int maxDepth = 64);

// warnings: 読み戻せなかったもの（知らないタグ等）。nullptr なら捨てる。
sol::object FromJson(sol::state_view lua, const nlohmann::json& j, const Hooks* hooks = nullptr,
                     std::vector<std::string>* warnings = nullptr);

// UTF-8 として正しいか（JSON に文字列のまま載せてよいか）。過長表現・サロゲートも不正扱い。
bool IsValidUtf8(const std::string& s);

} // namespace dx12e::luajson
