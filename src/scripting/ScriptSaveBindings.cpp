#include "scripting/ScriptSaveBindings.h"

#include "scripting/LuaJson.h"   // sol2 の設定マクロもここで揃う（ScriptEngine.cpp と同じ）
#include "scripting/ScriptEngine.h"

#include "core/Logger.h"
#include "core/save/SaveService.h"
#include "core/save/UserData.h"
#include "ecs/Components.h"
#include "physics/PhysicsSystem.h"
#include "scene/Entity.h"
#include "scene/Scene.h"

#include <DirectXMath.h>

#include <ctime>
#include <string>
#include <tuple>
#include <vector>

namespace dx12e
{

namespace
{
using nlohmann::json;
namespace sv = save;

constexpr int kEngineFormat = 1;   // 本体 JSON の並び（data / entities / meta …）の版

// self のうち「エンジンが毎回入れ直すもの」。保存しても意味が無い（読み戻すと壊す）ので除外する。
bool IsReservedSelfKey(const std::string& k)
{
    return k == "entity" || k == "name" || k == "transform" || k == "enabled";
}

entt::entity FindByName(entt::registry& reg, const std::string& name)
{
    if (name.empty()) return entt::null;
    for (auto [e, tag] : reg.view<NameTag>().each())
        if (tag.name == name) return e;
    return entt::null;
}

// save.write の opts.entities の 1 要素 → エンティティ。Entity / 数値 id / 名前 を受ける。
entt::entity ResolveEntityArg(entt::registry& reg, const sol::object& o)
{
    if (o.is<Entity>())
    {
        const Entity en = o.as<Entity>();
        return (en.IsValid() && reg.valid(en.GetHandle())) ? en.GetHandle() : entt::null;
    }
    if (o.get_type() == sol::type::number)
    {
        const auto id = static_cast<entt::entity>(o.as<std::uint32_t>());
        return reg.valid(id) ? id : entt::null;
    }
    if (o.get_type() == sol::type::string)
        return FindByName(reg, o.as<std::string>());
    return entt::null;
}

std::string GuidOf(const entt::registry& reg, entt::entity e)
{
    const auto* g = reg.try_get<EntityGuid>(e);
    return (g && g->value != 0) ? FormatEntityGuidHex(g->value) : std::string();
}

std::string NameOf(const entt::registry& reg, entt::entity e)
{
    const auto* t = reg.try_get<NameTag>(e);
    return t ? t->name : std::string();
}

// Entity は guid（無ければ名前）で運ぶ。読み戻しは ResolveEntityRef と同じ「guid が正・名前は予備」。
luajson::Hooks MakeHooks(entt::registry& reg)
{
    luajson::Hooks h;
    h.encodeUserdata = [&reg](const sol::object& o) -> std::optional<json> {
        if (!o.is<Entity>()) return std::nullopt;
        const Entity en = o.as<Entity>();
        const entt::entity e = en.GetHandle();
        if (!en.IsValid() || !reg.valid(e)) return json{{"$entity", {{"guid", ""}, {"name", ""}}}};
        return json{{"$entity", {{"guid", GuidOf(reg, e)}, {"name", NameOf(reg, e)}}}};
    };
    h.decodeTag = [&reg](sol::state_view lua, const std::string& tag, const json& p) -> sol::object {
        if (tag != "$entity" || !p.is_object()) return sol::make_object(lua, sol::lua_nil);
        const entt::entity e = ResolveEntityRef(reg, ParseEntityGuidHex(p.value("guid", std::string())),
                                                p.value("name", std::string()));
        return sol::make_object(lua, Entity(e, &reg));   // 見つからなくても invalid な Entity（isValid()=false）
    };
    return h;
}

json F3(const DirectX::XMFLOAT3& v) { return json::array({v.x, v.y, v.z}); }
json F4(const DirectX::XMFLOAT4& v) { return json::array({v.x, v.y, v.z, v.w}); }
bool ReadF3(const json& j, DirectX::XMFLOAT3& out)
{
    if (!j.is_array() || j.size() != 3) return false;
    for (const auto& v : j) if (!v.is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    return true;
}
bool ReadF4(const json& j, DirectX::XMFLOAT4& out)
{
    if (!j.is_array() || j.size() != 4) return false;
    for (const auto& v : j) if (!v.is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
    return true;
}

json ScriptPropToJson(const ScriptProp& p)
{
    switch (p.type)
    {
    case ScriptPropType::Float:  return p.num;
    case ScriptPropType::Int:    return static_cast<std::int64_t>(p.num);
    case ScriptPropType::Bool:   return p.b;
    case ScriptPropType::String: return p.str;
    case ScriptPropType::Vec3:
    case ScriptPropType::Color:  return json{{"$vec3", F3(p.vec)}};
    case ScriptPropType::Entity:
        return json{{"$entity", {{"guid", p.guid ? FormatEntityGuidHex(p.guid) : std::string()},
                                 {"name", p.str}}}};
    }
    return nullptr;
}

// 1 体ぶんの状態（Transform / DataComponent / Lua コンポーネントのフィールド）。
json CaptureEntity(entt::registry& reg, entt::entity e, const luajson::Hooks& hooks,
                   std::vector<std::string>& skipped)
{
    json j;
    j["guid"] = GuidOf(reg, e);
    j["name"] = NameOf(reg, e);
    const std::string who = j["name"].get<std::string>().empty() ? std::to_string(static_cast<std::uint32_t>(e))
                                                                  : j["name"].get<std::string>();

    if (const auto* t = reg.try_get<Transform>(e))
    {
        json tj{{"position", F3(t->position)}, {"rotation", F3(t->rotation)}, {"scale", F3(t->scale)}};
        if (t->useQuaternion) tj["quaternion"] = F4(t->quaternion);
        j["transform"] = std::move(tj);
    }

    if (const auto* dc = reg.try_get<DataComponent>(e))
    {
        json dj = json::object();
        for (const auto& [k, v] : dc->values)
        {
            switch (v.type)
            {
            case DataValue::Type::Number: dj[k] = v.num; break;
            case DataValue::Type::Bool:   dj[k] = v.b;   break;
            case DataValue::Type::String: dj[k] = v.str; break;
            case DataValue::Type::Vec3:   dj[k] = json{{"$vec3", F3(v.vec)}}; break;
            }
        }
        j["data"] = std::move(dj);
    }

    if (const auto* ls = reg.try_get<LuaScript>(e))
    {
        json lj{{"script", ls->scriptPath}, {"enabled", ls->enabled}};
        json fields = json::object();
        if (ls->self)
        {
            // ★宣言したプロパティだけでなく self に積んだ実行時の値（self.hp など）も運ぶ。
            //   ゲームの状態はほとんどそちらにあるため。保存できないもの（関数など）は飛ばす。
            sol::table self = *std::static_pointer_cast<sol::table>(ls->self);
            for (auto&& kv : self)
            {
                if (kv.first.get_type() != sol::type::string) continue;
                const std::string key = kv.first.as<std::string>();
                if (IsReservedSelfKey(key)) continue;
                const sol::type vt = kv.second.get_type();
                if (vt == sol::type::function) continue;   // メソッドは状態ではない（黙って飛ばす）
                luajson::EncodeResult r = luajson::ToJson(kv.second, &hooks);
                for (auto& s : r.skipped) skipped.push_back(who + ".self." + key + " " + s);
                if (!r.ok)
                {
                    skipped.push_back(who + ".self." + key + ": " + r.error);
                    continue;
                }
                fields[key] = std::move(r.value);
            }
        }
        else
        {
            // まだ OnStart 前（または読み込みエラー）: インスタンス値だけを持っていく
            for (const auto& p : ls->props) fields[p.name] = ScriptPropToJson(p);
        }
        lj["fields"] = std::move(fields);
        j["lua"] = std::move(lj);
    }
    return j;
}

// 物理の実体もテレポートする（Transform だけ書くと次の物理ステップで元の位置へ戻される）。
void TeleportPhysics(entt::registry& reg, entt::entity e, PhysicsSystem* physics)
{
    if (!physics) return;
    DirectX::XMFLOAT3 world{};
    DirectX::XMStoreFloat3(&world, ComputeWorldMatrix(reg, e).r[3]);
    if (auto* rb = reg.try_get<RigidBody>(e); rb && rb->bodyId != kInvalidBodyId)
    {
        physics->SetPosition(rb->bodyId, world);
        physics->SetLinearVelocity(rb->bodyId, {0.0f, 0.0f, 0.0f});
    }
    else if (auto* cc = reg.try_get<CharacterController>(e))
    {
        cc->_verticalVel = 0.0f;
        cc->_desiredVel  = {0.0f, 0.0f, 0.0f};
        physics->SetCharacterPosition(e, world);
    }
}

// 宣言プロパティのインスタンス値も揃える（まだ OnStart 前のスクリプトが注入で拾えるように）
void SyncScriptProp(LuaScript& ls, const std::vector<ScriptPropDef>& schema, const std::string& key,
                    const json& v, entt::registry& reg)
{
    const ScriptPropDef* def = nullptr;
    for (const auto& d : schema) if (d.name == key) { def = &d; break; }
    if (!def) return;
    ScriptProp* p = nullptr;
    for (auto& q : ls.props) if (q.name == key) { p = &q; break; }
    ScriptProp tmp;
    tmp.name = key;
    tmp.type = def->type;
    bool ok = false;
    switch (def->type)
    {
    case ScriptPropType::Float:
    case ScriptPropType::Int:
        if (v.is_number()) { tmp.num = v.get<double>(); ok = true; }
        break;
    case ScriptPropType::Bool:
        if (v.is_boolean()) { tmp.b = v.get<bool>(); ok = true; }
        break;
    case ScriptPropType::String:
        if (v.is_string()) { tmp.str = v.get<std::string>(); ok = true; }
        break;
    case ScriptPropType::Vec3:
    case ScriptPropType::Color:
        ok = v.is_object() && v.contains("$vec3") && ReadF3(v["$vec3"], tmp.vec);
        break;
    case ScriptPropType::Entity:
        if (v.is_object() && v.contains("$entity") && v["$entity"].is_object())
        {
            tmp.guid = ParseEntityGuidHex(v["$entity"].value("guid", std::string()));
            tmp.str  = v["$entity"].value("name", std::string());
            if (tmp.guid != 0)
                if (const entt::entity re = FindEntityByGuid(reg, tmp.guid); re != entt::null)
                    tmp.str = NameOf(reg, re);
            ok = true;
        }
        break;
    }
    if (!ok) return;
    if (p) *p = tmp;
    else   ls.props.push_back(tmp);
}

// 1 体ぶんを戻す。見つからなければ false（呼び出し側が missing に積む）。
bool ApplyEntity(sol::state_view lua, entt::registry& reg, const json& j, ScriptEngine* engine,
                 PhysicsSystem* physics, const luajson::Hooks& hooks, std::vector<std::string>& warnings)
{
    const std::string guidHex = j.value("guid", std::string());
    const std::string name    = j.value("name", std::string());
    const entt::entity e = ResolveEntityRef(reg, ParseEntityGuidHex(guidHex), name);
    if (e == entt::null || !reg.valid(e)) return false;

    if (j.contains("transform") && j["transform"].is_object())
    {
        const json& tj = j["transform"];
        auto& t = reg.get_or_emplace<Transform>(e);
        ReadF3(tj.value("position", json()), t.position);
        ReadF3(tj.value("rotation", json()), t.rotation);
        ReadF3(tj.value("scale", json()), t.scale);
        DirectX::XMFLOAT4 q{};
        if (ReadF4(tj.value("quaternion", json()), q)) { t.quaternion = q; t.useQuaternion = true; }
        else t.useQuaternion = false;
        TeleportPhysics(reg, e, physics);
    }

    if (j.contains("data") && j["data"].is_object())
    {
        auto& dc = reg.get_or_emplace<DataComponent>(e);
        for (auto it = j["data"].begin(); it != j["data"].end(); ++it)
        {
            DataValue dv;
            const json& v = it.value();
            if (v.is_number())       { dv.type = DataValue::Type::Number; dv.num = v.get<double>(); }
            else if (v.is_boolean()) { dv.type = DataValue::Type::Bool;   dv.b = v.get<bool>(); }
            else if (v.is_string())  { dv.type = DataValue::Type::String; dv.str = v.get<std::string>(); }
            else if (v.is_object() && v.contains("$vec3") && ReadF3(v["$vec3"], dv.vec))
                dv.type = DataValue::Type::Vec3;
            else { warnings.push_back(name + ".data." + it.key() + ": 読めない値"); continue; }
            dc.values[it.key()] = std::move(dv);
        }
    }

    if (j.contains("lua") && j["lua"].is_object())
    {
        auto* ls = reg.try_get<LuaScript>(e);
        const json& lj = j["lua"];
        if (!ls)
        {
            warnings.push_back(name + ": セーブには Lua コンポーネントの値があるが、今のエンティティに LuaScript が無い");
        }
        else
        {
            const std::string savedScript = lj.value("script", std::string());
            if (!savedScript.empty() && savedScript != ls->scriptPath)
                warnings.push_back(name + ": スクリプトが変わっている（セーブ " + savedScript + " / 今 "
                                   + ls->scriptPath + "）。同名のフィールドだけ戻す");
            if (lj.contains("enabled") && lj["enabled"].is_boolean())
                ls->enabled = lj["enabled"].get<bool>();
            static const std::vector<ScriptPropDef> kNoSchema;
            const std::vector<ScriptPropDef>& schema =
                engine ? engine->GetPropertySchema(ls->scriptPath) : kNoSchema;
            if (lj.contains("fields") && lj["fields"].is_object())
            {
                sol::table* self = ls->self ? std::static_pointer_cast<sol::table>(ls->self).get() : nullptr;
                if (self) (*self)["enabled"] = ls->enabled;
                for (auto it = lj["fields"].begin(); it != lj["fields"].end(); ++it)
                {
                    if (IsReservedSelfKey(it.key())) continue;
                    SyncScriptProp(*ls, schema, it.key(), it.value(), reg);
                    if (self)
                        (*self)[it.key()] = luajson::FromJson(lua, it.value(), &hooks, &warnings);
                }
            }
        }
    }
    return true;
}

// 数値 / 文字列 → スロット名。整数以外の数（1.5）や不正な名前は空文字 + why。
std::string SlotFromLua(const sol::object& o, std::string& why)
{
    std::string slot;
    if (o.get_type() == sol::type::number)
    {
        const double d = o.as<double>();
        if (d != static_cast<double>(static_cast<long long>(d)) || d < 0)
        {
            why = "数値のスロットは 0 以上の整数（例 save.write(1, data)）";
            return {};
        }
        slot = std::to_string(static_cast<long long>(d));
    }
    else if (o.get_type() == sol::type::string)
    {
        slot = o.as<std::string>();
    }
    else
    {
        why = "スロットは数値か文字列（例 save.write(1, data) / save.write(\"auto\", data)）";
        return {};
    }
    if (!sv::IsValidSlotName(slot, &why)) return {};
    return slot;
}

std::string NowIsoUtc(std::time_t t)
{
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string NowLocal(std::time_t t)
{
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

// 一覧 / info 用の要約（data と entities の中身は載せない）
sol::table SummaryTable(sol::state_view lua, const std::string& slot, const std::string& status,
                        const json& body, entt::registry& reg)
{
    sol::table t = lua.create_table();
    t["slot"]   = slot;
    t["ok"]     = (status == "ok");
    t["status"] = status;
    if (body.is_object())
    {
        t["savedAt"]      = body.value("savedAt", std::string());
        t["savedAtLocal"] = body.value("savedAtLocal", std::string());
        t["savedAtUnix"]  = body.value("savedAtUnix", 0LL);
        t["playTime"]     = body.value("playTime", 0.0);
        t["scene"]        = body.value("scene", std::string());
        t["entities"]     = body.contains("entities") && body["entities"].is_array()
                                ? static_cast<int>(body["entities"].size()) : 0;
        if (body.contains("meta"))
        {
            const luajson::Hooks hooks = MakeHooks(reg);
            t["meta"] = luajson::FromJson(lua, body["meta"], &hooks);
        }
    }
    return t;
}

void LogSkipped(const char* what, const std::vector<std::string>& skipped)
{
    if (skipped.empty()) return;
    std::string list;
    for (size_t i = 0; i < skipped.size() && i < 8; ++i) list += "\n  - " + skipped[i];
    if (skipped.size() > 8) list += "\n  ...ほか " + std::to_string(skipped.size() - 8) + " 件";
    Logger::Warn("セーブ: {} のうち保存できない値を飛ばした（{} 件）:{}", what, skipped.size(), list);
}
} // namespace

void RegisterSaveBindings(sol::state& lua, Scene* scene, ScriptEngine* engine)
{
    sol::table save = lua.create_named_table("save");

    // ---- 書く ----
    // save.write(slot, data, opts?) -> true | false, err
    //   opts.entities = { Entity | 名前 | 数値 id, ... }  状態を一緒に保存するエンティティ
    //   opts.meta     = { ... }                           一覧に出す任意の情報（章・場所・レベル等）
    save.set_function("write", [scene](sol::object slotObj, sol::object data, sol::optional<sol::table> opts,
                                       sol::this_state ts) -> std::tuple<bool, sol::object> {
        sol::state_view L(ts);
        auto fail = [&L](const std::string& msg) {
            Logger::Warn("save.write: {}", msg);
            return std::make_tuple(false, sol::make_object(L, msg));
        };
        std::string why;
        const std::string slot = SlotFromLua(slotObj, why);
        if (slot.empty()) return fail(why);
        if (!scene) return fail("シーンが無い");
        entt::registry& reg = scene->GetRegistry();
        const luajson::Hooks hooks = MakeHooks(reg);

        std::vector<std::string> skipped;
        luajson::EncodeResult d = luajson::ToJson(data, &hooks);
        if (!d.ok) return fail("data: " + d.error);
        skipped.insert(skipped.end(), d.skipped.begin(), d.skipped.end());

        json body;
        const std::time_t now = std::time(nullptr);
        body["engineFormat"] = kEngineFormat;
        body["slot"]         = slot;
        body["savedAt"]      = NowIsoUtc(now);
        body["savedAtLocal"] = NowLocal(now);
        body["savedAtUnix"]  = static_cast<long long>(now);
        body["playTime"]     = sv::SaveService::Get().PlayTime();
        body["scene"]        = sv::SaveService::Get().CurrentScene();
        body["data"]         = std::move(d.value);

        json ents = json::array();
        if (opts)
        {
            sol::object meta = (*opts)["meta"];
            if (meta.valid() && meta.get_type() != sol::type::lua_nil)
            {
                luajson::EncodeResult m = luajson::ToJson(meta, &hooks);
                if (!m.ok) return fail("meta: " + m.error);
                body["meta"] = std::move(m.value);
            }
            sol::object list = (*opts)["entities"];
            if (list.get_type() == sol::type::table)
            {
                for (auto&& kv : list.as<sol::table>())
                {
                    const entt::entity e = ResolveEntityArg(reg, kv.second);
                    if (e == entt::null)
                    {
                        skipped.push_back("entities: 見つからないエンティティを飛ばした");
                        continue;
                    }
                    if (GuidOf(reg, e).empty())
                        skipped.push_back("entities: " + NameOf(reg, e)
                                          + " は guid が無い（実行中に作った物）。名前で戻すことになる");
                    ents.push_back(CaptureEntity(reg, e, hooks, skipped));
                }
            }
            else if (list.valid() && list.get_type() != sol::type::lua_nil)
            {
                return fail("opts.entities はテーブル（{player, \"Door1\"} の形）");
            }
        }
        body["entities"] = std::move(ents);
        LogSkipped(("スロット " + slot).c_str(), skipped);

        const sv::WriteResult w = sv::SaveService::Get().Write(slot, body);
        if (!w.ok) return std::make_tuple(false, sol::make_object(L, w.error));
        return std::make_tuple(true, sol::make_object(L, sol::lua_nil));
    });

    // ---- 読む ----
    // save.read(slot, opts?) -> data, info | nil, err, info
    //   opts.applyEntities = false  エンティティの状態を今のシーンへ戻さない（既定 true）
    //   opts.adoptPlayTime = false  プレイ時間をセーブの値へ合わせない（既定 true）
    save.set_function("read", [scene, engine](sol::object slotObj, sol::optional<sol::table> opts,
                                              sol::this_state ts) -> std::tuple<sol::object, sol::object, sol::object> {
        sol::state_view L(ts);
        std::string why;
        const std::string slot = SlotFromLua(slotObj, why);
        auto nil = sol::make_object(L, sol::lua_nil);
        if (slot.empty()) return {nil, sol::make_object(L, why), nil};
        if (!scene) return {nil, sol::make_object(L, std::string("シーンが無い")), nil};
        entt::registry& reg = scene->GetRegistry();

        sv::ReadResult rr = sv::SaveService::Get().Read(slot);
        if (!rr.ok())
        {
            const std::string status = sv::DecodeStatusName(rr.decoded.status);
            sol::table info = SummaryTable(L, slot, status, json(), reg);
            std::string err = status;
            if (!rr.decoded.detail.empty()) err += ": " + rr.decoded.detail;
            if (rr.decoded.status != sv::DecodeStatus::Missing)
                Logger::Warn("save.read: スロット '{}' が読めない（{}）", slot, err);
            return {nil, sol::make_object(L, err), sol::make_object(L, info)};
        }

        const json& body = rr.decoded.body;
        const luajson::Hooks hooks = MakeHooks(reg);
        std::vector<std::string> warnings;
        sol::object data = luajson::FromJson(L, body.value("data", json()), &hooks, &warnings);
        sol::table info = SummaryTable(L, slot, "ok", body, reg);

        const bool applyEntities = !opts || (*opts)["applyEntities"].get_or(true);
        int applied = 0;
        sol::table missing = L.create_table();
        if (applyEntities && body.contains("entities") && body["entities"].is_array())
        {
            PhysicsSystem* physics = nullptr;
            sol::object po = L["physics"];
            if (po.is<PhysicsSystem*>()) physics = po.as<PhysicsSystem*>();
            int mi = 1;
            for (const auto& ej : body["entities"])
            {
                if (ApplyEntity(L, reg, ej, engine, physics, hooks, warnings)) ++applied;
                else
                {
                    const std::string g = ej.value("guid", std::string());
                    missing[mi++] = g.empty() ? ej.value("name", std::string()) : g;
                }
            }
        }
        info["entitiesApplied"] = applied;
        info["entitiesMissing"] = missing;

        const bool adopt = !opts || (*opts)["adoptPlayTime"].get_or(true);
        if (adopt) sv::SaveService::Get().ResetSession(body.value("playTime", 0.0));

        if (!warnings.empty())
        {
            std::string list;
            for (size_t i = 0; i < warnings.size() && i < 8; ++i) list += "\n  - " + warnings[i];
            Logger::Warn("save.read: スロット '{}' の読み戻しで注意 {} 件:{}", slot, warnings.size(), list);
        }
        Logger::Info("セーブ: スロット '{}' を読んだ（エンティティ {} 体を戻した / 見つからない {} 体）",
                     slot, applied, missing.size());
        return {data, sol::make_object(L, info), nil};
    });

    // ---- 一覧・情報 ----
    save.set_function("list", [scene](sol::this_state ts) -> sol::table {
        sol::state_view L(ts);
        sol::table out = L.create_table();
        if (!scene) return out;
        int i = 1;
        for (const auto& s : sv::SaveService::Get().List())
            out[i++] = SummaryTable(L, s.slot, sv::DecodeStatusName(s.status), s.body, scene->GetRegistry());
        return out;
    });
    save.set_function("info", [scene](sol::object slotObj, sol::this_state ts) -> sol::object {
        sol::state_view L(ts);
        std::string why;
        const std::string slot = SlotFromLua(slotObj, why);
        if (slot.empty() || !scene) return sol::make_object(L, sol::lua_nil);
        sv::ReadResult rr = sv::SaveService::Get().Read(slot);
        if (rr.decoded.status == sv::DecodeStatus::Missing) return sol::make_object(L, sol::lua_nil);
        return sol::make_object(L, SummaryTable(L, slot, sv::DecodeStatusName(rr.decoded.status),
                                                rr.decoded.body, scene->GetRegistry()));
    });
    save.set_function("exists", [](sol::object slotObj) -> bool {
        std::string why;
        const std::string slot = SlotFromLua(slotObj, why);
        return !slot.empty() && sv::SaveService::Get().Exists(slot);
    });
    save.set_function("delete", [](sol::object slotObj) -> bool {
        std::string why;
        const std::string slot = SlotFromLua(slotObj, why);
        if (slot.empty()) { Logger::Warn("save.delete: {}", why); return false; }
        return sv::SaveService::Get().Delete(slot);
    });

    // ---- プレイ時間 / 置き場 ----
    save.set_function("playTime", []() -> double { return sv::SaveService::Get().PlayTime(); });
    save.set_function("resetPlayTime", [](sol::optional<double> base) {
        sv::SaveService::Get().ResetSession(base.value_or(0.0));
    });
    save.set_function("dir", []() -> std::string {
        return sv::PathToUtf8(sv::SaveService::Get().Dir());
    });
}

} // namespace dx12e
