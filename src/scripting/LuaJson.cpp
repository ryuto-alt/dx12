#include "scripting/LuaJson.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace dx12e::luajson
{

namespace
{
using nlohmann::json;

enum class Step { Ok, Skip, Fail };

struct EncodeCtx
{
    const Hooks*                    hooks = nullptr;
    int                             maxDepth = 64;
    std::unordered_set<const void*> onPath;   // いま辿っている途中のテーブル（循環検出）
    std::vector<std::string>        skipped;
    std::string                     error;
};

std::string Join(const std::string& where, const std::string& key)
{
    return where.empty() ? key : where + "." + key;
}

const char* LuaTypeName(sol::type t)
{
    switch (t)
    {
    case sol::type::function:      return "function";
    case sol::type::thread:        return "thread";
    case sol::type::userdata:      return "userdata";
    case sol::type::lightuserdata: return "lightuserdata";
    default:                       return "unsupported";
    }
}

std::string ToHex(const std::string& s)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s)
    {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 15]);
    }
    return out;
}

bool FromHex(const std::string& hex, std::string& out)
{
    if (hex.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

// 数値を JSON へ。整数型は整数のまま、NaN/inf はタグへ。
json EncodeNumber(const sol::object& v)
{
    lua_State* L = v.lua_state();
    v.push(L);
    const bool isInt = lua_isinteger(L, -1) != 0;
    const lua_Integer iv = isInt ? lua_tointeger(L, -1) : 0;
    const lua_Number  dv = isInt ? 0.0 : lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (isInt) return json(static_cast<std::int64_t>(iv));
    if (std::isnan(dv)) return json{{"$num", "nan"}};
    if (std::isinf(dv)) return json{{"$num", dv > 0 ? "inf" : "-inf"}};
    return json(static_cast<double>(dv));
}

// テーブルのキーが「1..n の隙間なし整数」なら n を返す（それ以外は 0）。
bool IsIntegerKey(const sol::object& k, lua_Integer& out)
{
    if (k.get_type() != sol::type::number) return false;
    lua_State* L = k.lua_state();
    k.push(L);
    const bool isInt = lua_isinteger(L, -1) != 0;
    if (isInt) out = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return isInt;
}

Step Encode(EncodeCtx& c, const sol::object& v, json& out, const std::string& where, int depth);

// $map 形式のキー。文字列 / 数値 / 真偽だけ（テーブルや関数をキーにしたものは運べない）
bool EncodeKey(const sol::object& k, json& out)
{
    switch (k.get_type())
    {
    case sol::type::string:
    {
        const std::string s = k.as<std::string>();
        if (!IsValidUtf8(s)) out = json{{"$bin", ToHex(s)}};
        else out = s;
        return true;
    }
    case sol::type::number:  out = EncodeNumber(k); return true;
    case sol::type::boolean: out = k.as<bool>();    return true;
    default:                 return false;
    }
}

std::string KeyLabel(const sol::object& k)
{
    switch (k.get_type())
    {
    case sol::type::string:  return k.as<std::string>();
    case sol::type::number:
    {
        lua_Integer i = 0;
        if (IsIntegerKey(k, i)) return "[" + std::to_string(i) + "]";
        return "[" + std::to_string(k.as<double>()) + "]";
    }
    case sol::type::boolean: return k.as<bool>() ? "[true]" : "[false]";
    default:                 return "[?]";
    }
}

Step EncodeTable(EncodeCtx& c, const sol::table& t, json& out, const std::string& where, int depth)
{
    const void* id = t.pointer();
    if (c.onPath.count(id))
    {
        c.error = (where.empty() ? std::string("(最上位)") : where) + ": 循環参照（自分を含むテーブルは保存できない）";
        return Step::Fail;
    }
    if (depth > c.maxDepth)
    {
        c.error = where + ": 入れ子が深すぎる（" + std::to_string(c.maxDepth) + " 段まで）";
        return Step::Fail;
    }
    c.onPath.insert(id);

    std::vector<std::pair<sol::object, sol::object>> entries;
    for (auto&& kv : t)
        entries.emplace_back(kv.first, kv.second);

    bool allStringKeys = true, anyDollar = false, allIntKeys = true;
    lua_Integer maxIndex = 0;
    for (const auto& [k, val] : entries)
    {
        (void)val;
        if (k.get_type() == sol::type::string)
        {
            const std::string s = k.as<std::string>();
            if (!s.empty() && s[0] == '$') anyDollar = true;
            if (!IsValidUtf8(s)) anyDollar = true;   // JSON のキーにできない＝$map へ逃がす
            allIntKeys = false;
        }
        else
        {
            allStringKeys = false;
            lua_Integer i = 0;
            if (IsIntegerKey(k, i) && i >= 1) maxIndex = std::max(maxIndex, i);
            else allIntKeys = false;
        }
    }
    const bool isArray = !entries.empty() && allIntKeys
                         && maxIndex == static_cast<lua_Integer>(entries.size());

    Step result = Step::Ok;
    if (entries.empty())
    {
        out = json::object();
    }
    else if (isArray)
    {
        out = json::array();
        std::vector<json> slots(entries.size());
        for (const auto& [k, val] : entries)
        {
            lua_Integer i = 0;
            IsIntegerKey(k, i);
            json ev;
            const std::string w = where + "[" + std::to_string(i) + "]";
            const Step s = Encode(c, val, ev, w, depth + 1);
            if (s == Step::Fail) { result = Step::Fail; break; }
            // 保存できない要素は null（＝nil）。飛ばすと後ろの添字がずれるので詰めない
            slots[static_cast<size_t>(i - 1)] = (s == Step::Ok) ? std::move(ev) : json();
        }
        if (result != Step::Fail)
            for (auto& s : slots) out.push_back(std::move(s));
    }
    else if (allStringKeys && !anyDollar)
    {
        out = json::object();
        for (const auto& [k, val] : entries)
        {
            const std::string key = k.as<std::string>();
            json ev;
            const Step s = Encode(c, val, ev, Join(where, key), depth + 1);
            if (s == Step::Fail) { result = Step::Fail; break; }
            if (s == Step::Ok) out[key] = std::move(ev);
        }
    }
    else
    {
        json pairs = json::array();
        for (const auto& [k, val] : entries)
        {
            json ek;
            const std::string w = Join(where, KeyLabel(k));
            if (!EncodeKey(k, ek))
            {
                c.skipped.push_back(w + ": キーが " + std::string(LuaTypeName(k.get_type()))
                                    + "（文字列・数値・真偽以外のキーは保存できない）");
                continue;
            }
            json ev;
            const Step s = Encode(c, val, ev, w, depth + 1);
            if (s == Step::Fail) { result = Step::Fail; break; }
            if (s == Step::Ok) pairs.push_back(json::array({std::move(ek), std::move(ev)}));
        }
        if (result != Step::Fail) out = json{{"$map", std::move(pairs)}};
    }

    c.onPath.erase(id);
    return result;
}

Step Encode(EncodeCtx& c, const sol::object& v, json& out, const std::string& where, int depth)
{
    switch (v.get_type())
    {
    case sol::type::lua_nil:
    case sol::type::none:
        out = nullptr;
        return Step::Ok;
    case sol::type::boolean:
        out = v.as<bool>();
        return Step::Ok;
    case sol::type::number:
        out = EncodeNumber(v);
        return Step::Ok;
    case sol::type::string:
    {
        const std::string s = v.as<std::string>();
        if (IsValidUtf8(s)) out = s;
        else out = json{{"$bin", ToHex(s)}};
        return Step::Ok;
    }
    case sol::type::table:
        return EncodeTable(c, v.as<sol::table>(), out, where, depth);
    case sol::type::userdata:
        if (v.is<DirectX::XMFLOAT3>())
        {
            const auto f = v.as<DirectX::XMFLOAT3>();
            out = json{{"$vec3", json::array({f.x, f.y, f.z})}};
            return Step::Ok;
        }
        if (c.hooks && c.hooks->encodeUserdata)
        {
            if (auto j = c.hooks->encodeUserdata(v))
            {
                out = std::move(*j);
                return Step::Ok;
            }
        }
        [[fallthrough]];
    default:
        if (depth == 0)
        {
            c.error = std::string("保存できない型: ") + LuaTypeName(v.get_type())
                      + "（テーブル / 数値 / 文字列 / 真偽 / Vec3 / Entity を渡すこと）";
            return Step::Fail;
        }
        c.skipped.push_back((where.empty() ? std::string("(最上位)") : where) + ": "
                            + LuaTypeName(v.get_type()));
        return Step::Skip;
    }
}

sol::object Decode(sol::state_view lua, const json& j, const Hooks* hooks,
                   std::vector<std::string>* warnings, const std::string& where);

sol::object DecodeTagged(sol::state_view lua, const std::string& tag, const json& payload,
                         const Hooks* hooks, std::vector<std::string>* warnings, const std::string& where)
{
    auto warn = [&](const std::string& msg) { if (warnings) warnings->push_back(where + ": " + msg); };
    if (tag == "$map")
    {
        sol::table t = lua.create_table();
        if (!payload.is_array()) { warn("$map が配列ではない"); return t; }
        for (const auto& pair : payload)
        {
            if (!pair.is_array() || pair.size() != 2) { warn("$map の要素が [キー, 値] ではない"); continue; }
            sol::object k = Decode(lua, pair[0], hooks, warnings, where);
            if (k.get_type() == sol::type::lua_nil) { warn("$map のキーが nil"); continue; }
            sol::object v = Decode(lua, pair[1], hooks, warnings, Join(where, pair[0].dump()));
            if (v.get_type() != sol::type::lua_nil) t[k] = v;
        }
        return t;
    }
    if (tag == "$vec3")
    {
        if (!payload.is_array() || payload.size() != 3 || !payload[0].is_number()
            || !payload[1].is_number() || !payload[2].is_number())
        {
            warn("$vec3 が [x, y, z] ではない");
            return sol::make_object(lua, sol::lua_nil);
        }
        return sol::make_object(lua, DirectX::XMFLOAT3{payload[0].get<float>(), payload[1].get<float>(),
                                                       payload[2].get<float>()});
    }
    if (tag == "$num")
    {
        const std::string s = payload.is_string() ? payload.get<std::string>() : std::string();
        if (s == "nan")  return sol::make_object(lua, std::numeric_limits<double>::quiet_NaN());
        if (s == "inf")  return sol::make_object(lua, std::numeric_limits<double>::infinity());
        if (s == "-inf") return sol::make_object(lua, -std::numeric_limits<double>::infinity());
        warn("$num の値が nan / inf / -inf ではない");
        return sol::make_object(lua, sol::lua_nil);
    }
    if (tag == "$bin")
    {
        std::string bytes;
        if (!payload.is_string() || !FromHex(payload.get<std::string>(), bytes))
        {
            warn("$bin が 16 進文字列ではない");
            return sol::make_object(lua, sol::lua_nil);
        }
        return sol::make_object(lua, bytes);
    }
    if (hooks && hooks->decodeTag)
    {
        sol::object o = hooks->decodeTag(lua, tag, payload);
        if (o.valid() && o.get_type() != sol::type::lua_nil) return o;
    }
    warn("知らないタグ " + tag + "（nil として読んだ）");
    return sol::make_object(lua, sol::lua_nil);
}

sol::object Decode(sol::state_view lua, const json& j, const Hooks* hooks,
                   std::vector<std::string>* warnings, const std::string& where)
{
    switch (j.type())
    {
    case json::value_t::null:
    case json::value_t::discarded:
        return sol::make_object(lua, sol::lua_nil);
    case json::value_t::boolean:
        return sol::make_object(lua, j.get<bool>());
    case json::value_t::number_integer:
        return sol::make_object(lua, static_cast<lua_Integer>(j.get<std::int64_t>()));
    case json::value_t::number_unsigned:
    {
        const std::uint64_t u = j.get<std::uint64_t>();
        if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return sol::make_object(lua, static_cast<lua_Integer>(u));
        return sol::make_object(lua, static_cast<double>(u));
    }
    case json::value_t::number_float:
        return sol::make_object(lua, j.get<double>());
    case json::value_t::string:
        return sol::make_object(lua, j.get<std::string>());
    case json::value_t::array:
    {
        sol::table t = lua.create_table(static_cast<int>(j.size()), 0);
        lua_Integer i = 1;
        for (const auto& e : j)
        {
            sol::object v = Decode(lua, e, hooks, warnings, where + "[" + std::to_string(i) + "]");
            if (v.get_type() != sol::type::lua_nil) t[i] = v;
            ++i;
        }
        return t;
    }
    case json::value_t::object:
    {
        if (j.size() == 1)
        {
            const auto it = j.begin();
            if (!it.key().empty() && it.key()[0] == '$')
                return DecodeTagged(lua, it.key(), it.value(), hooks, warnings, where);
        }
        sol::table t = lua.create_table(0, static_cast<int>(j.size()));
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            sol::object v = Decode(lua, it.value(), hooks, warnings, Join(where, it.key()));
            if (v.get_type() != sol::type::lua_nil) t[it.key()] = v;
        }
        return t;
    }
    default:
        return sol::make_object(lua, sol::lua_nil);
    }
}
} // namespace

bool IsValidUtf8(const std::string& s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n)
    {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k)
        {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        // 過長表現 / サロゲート / 範囲外
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        i += len;
    }
    return true;
}

EncodeResult ToJson(const sol::object& value, const Hooks* hooks, int maxDepth)
{
    EncodeCtx c;
    c.hooks = hooks;
    c.maxDepth = maxDepth;
    EncodeResult r;
    const Step s = Encode(c, value, r.value, "", 0);
    r.ok = (s == Step::Ok);
    r.error = std::move(c.error);
    r.skipped = std::move(c.skipped);
    if (!r.ok) r.value = nullptr;
    return r;
}

sol::object FromJson(sol::state_view lua, const nlohmann::json& j, const Hooks* hooks,
                     std::vector<std::string>* warnings)
{
    return Decode(lua, j, hooks, warnings, "");
}

} // namespace dx12e::luajson
