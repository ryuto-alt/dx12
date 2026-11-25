// Lua の post.* / ssao.*（ポストプロセスと SSAO を文字列キーで読み書きする束縛）。
// ScriptEngine.cpp から無変更で移した（W05-15 / R-00b）。関数名・引数・既定値は 1 つも変えていない。
// 登録は LuaBindList.inc の UNO_LUA_BIND(BindPost)（自己登録はしない）。
#include "scripting/LuaBindingRegistry.h"

#include "core/Logger.h"
#include "renderer/PostProcessSettings.h"
#include "renderer/SSAOSettings.h"
#include "scene/Scene.h"

#include <sol/sol.hpp>
#include <DirectXMath.h>
#include <string>

namespace dx12e
{

namespace
{
// ScriptEngine.cpp の同名の部品の写し（あちらは他の束縛も使うので残してある）。
bool ReadVec3(const sol::object& v, DirectX::XMFLOAT3& dst)
{
    if (v.is<DirectX::XMFLOAT3>()) { dst = v.as<DirectX::XMFLOAT3>(); return true; }
    if (!v.is<sol::table>()) return false;
    sol::table t = v.as<sol::table>();
    sol::optional<float> nx = t["x"], ny = t["y"], nz = t["z"];   // {x=,y=,z=} / Vec3 風
    sol::optional<float> i1 = t[1],   i2 = t[2],   i3 = t[3];     // {r,g,b} の配列書き
    dst.x = nx ? *nx : (i1 ? *i1 : dst.x);
    dst.y = ny ? *ny : (i2 ? *i2 : dst.y);
    dst.z = nz ? *nz : (i3 ? *i3 : dst.z);
    return true;
}

bool ToBool(const sol::object& v, bool dflt)
{
    if (v.is<bool>())   return v.as<bool>();
    if (v.is<double>()) return v.as<double>() != 0.0;
    return dflt;
}
float ToNum(const sol::object& v, float dflt)
{
    if (v.is<double>()) return static_cast<float>(v.as<double>());
    if (v.is<bool>())   return v.as<bool>() ? 1.0f : 0.0f;
    return dflt;
}

// ── ポストプロセス / SSAO の「文字列キー」アクセス ───────────────────────
// 90 個近いフィールドを個別バインドすると API が爆発するので、名前表
// (renderer/PostProcessSettings.h の DX12E_POST_FIELDS / DX12E_SSAO_FIELDS) を
// 1 本回して get/set/names を生成する。項目を足すときの修正箇所はあの表だけ。
// 名前は MCP の get_post_process / set_post_process と同一。
sol::object PostGetField(sol::state_view lua, const PostProcessSettings& p, const std::string& name)
{
#define DX12E_PP_G(f) if (name == #f) return sol::make_object(lua, p.f);
    DX12E_POST_FIELDS(DX12E_PP_G, DX12E_PP_G, DX12E_PP_G, DX12E_PP_G, DX12E_PP_G)
    DX12E_POST_FIELDS_V2(DX12E_PP_G, DX12E_PP_G, DX12E_PP_G, DX12E_PP_G, DX12E_PP_G)
#undef DX12E_PP_G
    return sol::make_object(lua, sol::lua_nil);
}

bool PostSetField(PostProcessSettings& p, const std::string& name, const sol::object& v)
{
#define DX12E_PP_B(f) if (name == #f) { p.f = ToBool(v, p.f); return true; }
#define DX12E_PP_F(f) if (name == #f) { p.f = ToNum(v, p.f); return true; }
#define DX12E_PP_I(f) if (name == #f) { p.f = static_cast<int>(ToNum(v, static_cast<float>(p.f))); return true; }
#define DX12E_PP_V(f) if (name == #f) { return ReadVec3(v, p.f); }
#define DX12E_PP_S(f) if (name == #f) { if (!v.is<std::string>()) return false; p.f = v.as<std::string>(); return true; }
    DX12E_POST_FIELDS(DX12E_PP_B, DX12E_PP_F, DX12E_PP_I, DX12E_PP_V, DX12E_PP_S)
    DX12E_POST_FIELDS_V2(DX12E_PP_B, DX12E_PP_F, DX12E_PP_I, DX12E_PP_V, DX12E_PP_S)
#undef DX12E_PP_B
#undef DX12E_PP_F
#undef DX12E_PP_I
#undef DX12E_PP_V
#undef DX12E_PP_S
    return false;
}

sol::table PostFieldNames(sol::state_view lua)
{
    sol::table t = lua.create_table();
    int i = 1;
#define DX12E_PP_N(f) t[i++] = #f;
    DX12E_POST_FIELDS(DX12E_PP_N, DX12E_PP_N, DX12E_PP_N, DX12E_PP_N, DX12E_PP_N)
    DX12E_POST_FIELDS_V2(DX12E_PP_N, DX12E_PP_N, DX12E_PP_N, DX12E_PP_N, DX12E_PP_N)
#undef DX12E_PP_N
    return t;
}

sol::object SsaoGetField(sol::state_view lua, const SSAOSettings& s, const std::string& name)
{
#define DX12E_SS_G(f) if (name == #f) return sol::make_object(lua, s.f);
    DX12E_SSAO_FIELDS(DX12E_SS_G, DX12E_SS_G, DX12E_SS_G)
#undef DX12E_SS_G
    return sol::make_object(lua, sol::lua_nil);
}

bool SsaoSetField(SSAOSettings& s, const std::string& name, const sol::object& v)
{
#define DX12E_SS_B(f) if (name == #f) { s.f = ToBool(v, s.f); return true; }
#define DX12E_SS_F(f) if (name == #f) { s.f = ToNum(v, s.f); return true; }
#define DX12E_SS_I(f) if (name == #f) { s.f = static_cast<int>(ToNum(v, static_cast<float>(s.f))); return true; }
    DX12E_SSAO_FIELDS(DX12E_SS_B, DX12E_SS_F, DX12E_SS_I)
#undef DX12E_SS_B
#undef DX12E_SS_F
#undef DX12E_SS_I
    return false;
}

sol::table SsaoFieldNames(sol::state_view lua)
{
    sol::table t = lua.create_table();
    int i = 1;
#define DX12E_SS_N(f) t[i++] = #f;
    DX12E_SSAO_FIELDS(DX12E_SS_N, DX12E_SS_N, DX12E_SS_N)
#undef DX12E_SS_N
    return t;
}
} // namespace

void BindPost(sol::state& lua, ScriptContext& ctx)
{
    Scene* const* sceneSlot = ctx.scene;   // ScriptEngine::m_scene を指す（エンジンの寿命内。差し替えにも追従する）
    // --- post / ssao: ポストプロセス・SSAO を文字列キーで読み書き（'.' で呼ぶ）---
    // 項目名は MCP の get_post_process / set_post_process と同一（名前表は
    // renderer/PostProcessSettings.h に 1 つだけ）。post.names() で一覧できる。
    // Play 中の変更は Stop でシーンJSONごと巻き戻る（EnterPlayMode のスナップショット）。
    {
        sol::table po = lua.create_named_table("post");
        po.set_function("get", [sceneSlot](const std::string& name, sol::this_state ts) -> sol::object {
            sol::state_view sv(ts);
            if (!*sceneSlot) return sol::make_object(sv, sol::lua_nil);
            return PostGetField(sv, (*sceneSlot)->GetPostSettings(), name);
        });
        po.set_function("set", [sceneSlot](const std::string& name, sol::object v) -> bool {
            if (!*sceneSlot) return false;
            if (PostSetField((*sceneSlot)->GetPostSettings(), name, v)) return true;
            Logger::Warn("post.set: 不明または型違いの項目 \"{}\"（post.names() で一覧）", name);
            return false;
        });
        // まとめて設定: post.setMany{ bloomOn=true, bloom=0.8, vignetteOn=true }
        po.set_function("setMany", [sceneSlot](sol::table t) -> int {
            if (!*sceneSlot) return 0;
            auto& p = (*sceneSlot)->GetPostSettings();
            int n = 0;
            for (auto& kv : t)
            {
                if (!kv.first.is<std::string>()) continue;
                const std::string key = kv.first.as<std::string>();
                if (PostSetField(p, key, kv.second)) ++n;
                else Logger::Warn("post.setMany: 不明または型違いの項目 \"{}\"", key);
            }
            return n;
        });
        po.set_function("names", [](sol::this_state ts) -> sol::table {
            sol::state_view sv(ts);
            return PostFieldNames(sv);
        });

        sol::table so = lua.create_named_table("ssao");
        so.set_function("get", [sceneSlot](const std::string& name, sol::this_state ts) -> sol::object {
            sol::state_view sv(ts);
            if (!*sceneSlot) return sol::make_object(sv, sol::lua_nil);
            return SsaoGetField(sv, (*sceneSlot)->GetSSAOSettings(), name);
        });
        so.set_function("set", [sceneSlot](const std::string& name, sol::object v) -> bool {
            if (!*sceneSlot) return false;
            if (SsaoSetField((*sceneSlot)->GetSSAOSettings(), name, v)) return true;
            Logger::Warn("ssao.set: 不明または型違いの項目 \"{}\"（ssao.names() で一覧）", name);
            return false;
        });
        so.set_function("setMany", [sceneSlot](sol::table t) -> int {
            if (!*sceneSlot) return 0;
            auto& s = (*sceneSlot)->GetSSAOSettings();
            int n = 0;
            for (auto& kv : t)
            {
                if (!kv.first.is<std::string>()) continue;
                const std::string key = kv.first.as<std::string>();
                if (SsaoSetField(s, key, kv.second)) ++n;
                else Logger::Warn("ssao.setMany: 不明または型違いの項目 \"{}\"", key);
            }
            return n;
        });
        so.set_function("names", [](sol::this_state ts) -> sol::table {
            sol::state_view sv(ts);
            return SsaoFieldNames(sv);
        });
    }
}

} // namespace dx12e
