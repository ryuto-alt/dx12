#include "scripting/ScriptEngine.h"

#include "ui/UISystem.h"   // input:isUiCapturing* が WantsMouse/WantsNav を読む
#include "core/Logger.h"
#include "core/vfs/Vfs.h"

#include <set>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4244 4267 4996)
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#pragma warning(pop)

#include "scene/Scene.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "ui/UiRichText.h"   // isUiTypewriterDone: rich=true のタグ除去後文字数
#include "renderer/Mesh.h"
#include "renderer/SpriteAnim.h"   // isSpriteAnimDone/isUiAnimDone: 連番の単発終了判定
#include "renderer/ParticleSystem.h"
#include "renderer/GpuParticleSystem.h"
#include "input/InputSystem.h"
#include "engine/input/ActionMap.h"
#include "renderer/Camera.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsSystem.h"
#include "network/NetworkSystem.h"
#include "animation/Skeleton.h"
#include "animation/Animator.h"
#include "animation/AnimationClip.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/NodeGraph.h"
#include "animation/AnimGraphRuntime.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cctype>
#include <tuple>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dx12e
{

namespace {

// AnimatorController の実行状態を取り出す（未ロード / 無効なら nullptr）。
// アニメ FSM の Lua API は全部これを通す。無ければ黙って no-op / 既定値を返すのが
// 既存の playAnim 等と揃った流儀（エラーは上げない）。
AnimGraphRuntimeState* AnimStateOf(Entity& e)
{
    if (!e.HasComponent<AnimatorController>()) return nullptr;
    AnimatorController& ctrl = e.GetComponent<AnimatorController>();
    if (!ctrl._state || !ctrl._state->valid) return nullptr;
    return ctrl._state.get();
}

// 名前からエンティティを引く（NameTag 一致。先頭一致を返す）。見つからなければ entt::null。
entt::entity FindEntityByName(entt::registry& reg, const std::string& name)
{
    if (name.empty()) return entt::null;
    auto view = reg.view<NameTag>();
    for (auto e : view)
        if (view.get<NameTag>(e).name == name) return e;
    return entt::null;
}

// Lua から来た値を XMFLOAT3 へ読む。Vec3 usertype でも {r,g,b} / {x=,y=,z=} テーブルでも可。
// 読めなかったら false（呼び出し側は「未指定」として無視する）。
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

// Lua の値をゆるく bool / float へ読む（数値↔真偽の取り違えでスクリプトが黙って死なないように）。
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

// ── ライトのプロキシ（Lua: entity:light() / entity:addLight(kind) / scene:sun()）──
// PointLight / DirectionalLight / SpotLight を 1 個の usertype にまとめる。
// 型ごとに 3 つ生やすと演出側（Tween / Flicker）が型分岐だらけになるので、
// 「持っている型に書く・無い項目は読むと既定値／書くと無視」の薄いプロキシへ寄せた。
// color / direction は必ず**値コピー**で返す。参照を返すと Lua が掴んだ「開始値」が
// 補間中に一緒に動いてしまい、Tween の from が壊れる。
struct LuaLight
{
    entt::registry* reg = nullptr;
    entt::entity    e   = entt::null;

    bool Valid() const { return reg != nullptr && reg->valid(e); }
    PointLight*       P() const { return Valid() ? reg->try_get<PointLight>(e) : nullptr; }
    DirectionalLight* D() const { return Valid() ? reg->try_get<DirectionalLight>(e) : nullptr; }
    SpotLight*        S() const { return Valid() ? reg->try_get<SpotLight>(e) : nullptr; }
    bool Any() const { return P() != nullptr || D() != nullptr || S() != nullptr; }
};

std::string LightTypeName(const LuaLight& l)
{
    if (l.P()) return "point";
    if (l.D()) return "directional";
    if (l.S()) return "spot";
    return "none";
}

float LightGetIntensity(const LuaLight& l)
{
    if (auto* p = l.P()) return p->intensity;
    if (auto* d = l.D()) return d->intensity;
    if (auto* s = l.S()) return s->intensity;
    return 0.0f;
}
void LightSetIntensity(LuaLight& l, float v)
{
    if (auto* p = l.P()) p->intensity = v;
    if (auto* d = l.D()) d->intensity = v;
    if (auto* s = l.S()) s->intensity = v;
}

DirectX::XMFLOAT3 LightGetColor(const LuaLight& l)
{
    if (auto* p = l.P()) return p->color;
    if (auto* d = l.D()) return d->color;
    if (auto* s = l.S()) return s->color;
    return DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f};
}
void LightSetColor(LuaLight& l, const sol::object& v)
{
    DirectX::XMFLOAT3 c = LightGetColor(l);
    if (!ReadVec3(v, c)) return;
    if (auto* p = l.P()) p->color = c;
    if (auto* d = l.D()) d->color = c;
    if (auto* s = l.S()) s->color = c;
}

DirectX::XMFLOAT3 LightGetDirection(const LuaLight& l)
{
    if (auto* d = l.D()) return d->direction;
    if (auto* s = l.S()) return s->direction;
    return DirectX::XMFLOAT3{0.0f, -1.0f, 0.0f};   // PointLight は無指向
}
void LightSetDirection(LuaLight& l, const sol::object& v)
{
    DirectX::XMFLOAT3 dir = LightGetDirection(l);
    if (!ReadVec3(v, dir)) return;
    // 正規化しておく（Lua 側で任意ベクトルを投げても破綻しないように）
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
    if (auto* d = l.D()) d->direction = dir;
    if (auto* s = l.S()) s->direction = dir;
}

float LightGetRange(const LuaLight& l)
{
    if (auto* p = l.P()) return p->range;
    if (auto* s = l.S()) return s->range;
    return 0.0f;   // DirectionalLight は無限遠
}
void LightSetRange(LuaLight& l, float v)
{
    if (auto* p = l.P()) p->range = v;
    if (auto* s = l.S()) s->range = v;
}

// ── 型固有（持っていない型では読むと既定値・書くと無視）──
float LightGetAmbient(const LuaLight& l) { auto* d = l.D(); return d ? d->ambient : 0.0f; }
void  LightSetAmbient(LuaLight& l, float v) { if (auto* d = l.D()) d->ambient = v; }

float LightGetInner(const LuaLight& l) { auto* s = l.S(); return s ? s->innerConeDeg : 0.0f; }
void  LightSetInner(LuaLight& l, float v) { if (auto* s = l.S()) s->innerConeDeg = v; }
float LightGetOuter(const LuaLight& l) { auto* s = l.S(); return s ? s->outerConeDeg : 0.0f; }
void  LightSetOuter(LuaLight& l, float v) { if (auto* s = l.S()) s->outerConeDeg = v; }

bool LightGetShadows(const LuaLight& l)
{
    if (auto* p = l.P()) return p->castShadows;
    if (auto* s = l.S()) return s->castShadows;
    return l.D() != nullptr;   // 平行光源は CSM が常時担当
}
void LightSetShadows(LuaLight& l, bool v)
{
    if (auto* p = l.P()) p->castShadows = v;
    if (auto* s = l.S()) s->castShadows = v;
}

// 宣言スキーマ + インスタンス値を self テーブルへ注入する。
// インスタンスに override が無いプロパティは既定値を使う。これで
// スクリプト側は self.<name> でプロパティを読める（Unity の serialized field 相当）。
void InjectScriptProps(sol::table& self,
                       entt::registry& reg,
                       const std::vector<ScriptPropDef>* schema,
                       const LuaScript& ls)
{
    if (!schema) return;
    for (const auto& def : *schema)
    {
        // インスタンスの override を名前で探す
        const ScriptProp* ov = nullptr;
        for (const auto& p : ls.props)
            if (p.name == def.name) { ov = &p; break; }

        switch (def.type)
        {
        case ScriptPropType::Float:
            self[def.name] = ov ? ov->num : def.def.num; break;
        case ScriptPropType::Int:
            self[def.name] = static_cast<int>(ov ? ov->num : def.def.num); break;
        case ScriptPropType::Bool:
            self[def.name] = ov ? ov->b : def.def.b; break;
        case ScriptPropType::String:
            self[def.name] = ov ? ov->str : def.def.str; break;
        case ScriptPropType::Vec3:
        case ScriptPropType::Color:
            self[def.name] = ov ? ov->vec : def.def.vec; break;
        case ScriptPropType::Entity:
        {
            // 参照先を解決して Entity を注入（self.<name>:isValid() で確認できる）。
            // ★guid が正、名前はフォールバック。スクリプトから見えるものは変わらない
            //   （注入されるのは今までどおり Entity ハンドルで、名前は返らない）。
            const ScriptProp& src = ov ? *ov : def.def;
            entt::entity re = ResolveEntityRef(reg, src.guid, src.str);
            self[def.name] = Entity(re, &reg);   // 未解決でも invalid な Entity を入れる
            break;
        }
        }
    }
}

// LuaScript コンポーネントに env / self を構築し、OnStart を呼ぶ。
// 失敗時は loadError を true に、Logger::Error を出す。
// 戻り値: 成功 true
bool InitializeLuaScriptInstance(sol::state& lua,
                                  entt::registry& reg,
                                  entt::entity e,
                                  LuaScript& ls,
                                  const std::string& assetsDir,
                                  std::string& lastError,
                                  const std::vector<ScriptPropDef>* schema)
{
    namespace fs = std::filesystem;
    fs::path abs = fs::path(assetsDir) / ls.scriptPath;

    auto env = std::make_shared<sol::environment>(lua, sol::create, lua.globals());

    // self テーブルを作る
    auto self = std::make_shared<sol::table>(lua.create_table());
    (*self)["entity"]  = static_cast<u32>(e);
    const auto* tag = reg.try_get<NameTag>(e);
    (*self)["name"]   = tag ? tag->name : std::string{};
    auto* tf = reg.try_get<Transform>(e);
    if (tf) (*self)["transform"] = tf;
    (*self)["enabled"] = ls.enabled;

    // 公開プロパティを注入（スクリプト本体実行前 → OnStart/トップレベルから参照可）
    InjectScriptProps(*self, reg, schema, ls);

    (*env)["self"] = *self;

    // VFS 経由で読む（ゲームモード: pak から復号。エディタ: ディスクから読む）。
    // 空のときはディスクの safe_script_file にフォールバック。
    auto vfsBytes = vfs::ReadAsset(ls.scriptPath);
    auto result = vfsBytes.empty()
        ? lua.safe_script_file(abs.string(), *env, sol::script_pass_on_error)
        : lua.safe_script(std::string(vfsBytes.begin(), vfsBytes.end()),
                          *env, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        lastError = err.what();
        Logger::Error("Luaエラー（スクリプト読み込み, entity={} path={}）: {}",
                      static_cast<u32>(e), ls.scriptPath, lastError);
        ls.loadError = true; ls.errorMessage = lastError;
        return false;
    }

    ls.env       = env;
    ls.self      = self;
    ls.loadError = false; ls.errorMessage.clear();

    // OnStart(self) を呼ぶ。
    // 必ず raw_get（フォールバック無し）で「このスクリプト自身が定義した関数」だけを見る。
    // (*env)["OnStart"] だと env の __index フォールバック経由で lua.globals() まで辿り、
    // game.lua のグローバル OnStart/OnUpdate を self 引数で誤呼び出ししてしまう。
    sol::object fnObj = env->raw_get<sol::object>("OnStart");

    // ★どちらも無いコンポーネントは「読めてはいるが何もしない」。
    //   よくある原因は module テーブル形式（local M = {} ... function M.OnUpdate() ... return M）で、
    //   この形式だと raw_get が env 直下を見るので関数が見つからず、エラーも出ないまま
    //   黙って動かないコンポーネントが出来上がる。気付けないので必ず警告を出す。
    if (fnObj.get_type() != sol::type::function
        && env->raw_get<sol::object>("OnUpdate").get_type() != sol::type::function)
    {
        Logger::Warn("LuaScript [{}] に OnStart / OnUpdate がありません（このコンポーネントは"
                     "何もしません）。module テーブルを return する形式ではなく、"
                     "トップレベルに function OnUpdate(self, dt) と書いてください",
                     ls.scriptPath);
    }

    if (fnObj.get_type() == sol::type::function)
    {
        sol::protected_function fn = fnObj;
        auto r = fn(*self);
        if (!r.valid())
        {
            sol::error err = r;
            lastError = err.what();
            Logger::Error("Luaエラー（OnStart, entity={}）: {}",
                          static_cast<u32>(e), lastError);
            ls.loadError = true; ls.errorMessage = lastError;
            return false;
        }
    }
    ls.started = true;
    return true;
}

} // namespace

ScriptEngine::ScriptEngine() = default;
ScriptEngine::~ScriptEngine() { Shutdown(); }

void ScriptEngine::Initialize(Scene* scene, InputSystem* input, Camera* camera,
                               AudioSystem* audio, PhysicsSystem* physics,
                               const std::string& assetsDir)
{
    m_scene     = scene;
    m_input     = input;
    m_camera    = camera;
    m_audio     = audio;
    m_physics   = physics;
    m_assetsDir = assetsDir;

    m_lua = std::make_unique<sol::state>();
    // coroutine: Lua 5.4 では独立ライブラリなので明示的に開かないと
    // coroutine.create / yield が一切使えない。これが無いと「n 秒待ってから次」を
    // 素直に書けず、time.after のネスト地獄になる（カットシーン・敵の行動シーケンス）。
    // prelude の task.spawn / wait() がこの上に乗っている。
    m_lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::io, sol::lib::coroutine);

    RegisterBindings();
    LoadPrelude();

    Logger::Info("ScriptEngine initialized");
}

void ScriptEngine::RegisterBindings()
{
    auto& lua = *m_lua;

    // --- Vec3 (XMFLOAT3 wrapper) ---
    lua.new_usertype<DirectX::XMFLOAT3>("Vec3",
        sol::constructors<DirectX::XMFLOAT3(), DirectX::XMFLOAT3(float, float, float)>(),
        "x", &DirectX::XMFLOAT3::x,
        "y", &DirectX::XMFLOAT3::y,
        "z", &DirectX::XMFLOAT3::z
    );

    // --- Transform ---
    lua.new_usertype<Transform>("Transform",
        "position", &Transform::position,
        "rotation", &Transform::rotation,
        "scale",    &Transform::scale
    );

    // --- Light（PointLight / DirectionalLight / SpotLight の統一プロキシ）---
    // entity:light() / entity:addLight(kind) / scene:sun() が返す。
    // プロパティは素直に読み書きする（Unity/Godot と同じ流儀）。時間変化は
    // prelude の Tween / Flicker が同じプロパティを叩くだけ＝専用 API を増やさない。
    lua.new_usertype<LuaLight>("Light",
        "isValid",   [](const LuaLight& l) { return l.Any(); },
        "id",        sol::property([](const LuaLight& l) { return static_cast<u32>(l.e); }),
        "type",      sol::property(&LightTypeName),
        "intensity", sol::property(&LightGetIntensity, &LightSetIntensity),
        "color",     sol::property(&LightGetColor,     &LightSetColor),
        "direction", sol::property(&LightGetDirection, &LightSetDirection),
        "range",     sol::property(&LightGetRange,     &LightSetRange),
        "ambient",   sol::property(&LightGetAmbient,   &LightSetAmbient),
        "innerAngle", sol::property(&LightGetInner,    &LightSetInner),
        "outerAngle", sol::property(&LightGetOuter,    &LightSetOuter),
        "castShadows", sol::property(&LightGetShadows, &LightSetShadows),
        // 色を 3 数値で書く近道（Tween/Flicker から Vec3 を作らずに済む）
        "setColor", [](LuaLight& l, float r, float g, float b) {
            DirectX::XMFLOAT3 c{r, g, b};
            if (auto* p = l.P()) p->color = c;
            if (auto* d = l.D()) d->color = c;
            if (auto* s = l.S()) s->color = c;
        },
        "setDirection", [](LuaLight& l, float x, float y, float z) {
            DirectX::XMFLOAT3 dir{x, y, z};
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len > 1e-6f) { dir.x /= len; dir.y /= len; dir.z /= len; }
            if (auto* d = l.D()) d->direction = dir;
            if (auto* s = l.S()) s->direction = dir;
        }
    );

    // --- Entity ---
    lua.new_usertype<Entity>("Entity",
        "isValid", &Entity::IsValid,

        // Name access
        "name", sol::property(
            [](const Entity& e) -> std::string {
                return e.HasComponent<NameTag>() ? e.GetComponent<NameTag>().name : "";
            }
        ),

        // Transform access
        "transform", sol::property(
            [](Entity& e) -> Transform& { return e.GetComponent<Transform>(); }
        ),

        // Component query（全コンポーネント型を網羅。未知の型名は黙って false にせず警告を出す）
        "hasComponent", [](const Entity& e, const std::string& type) -> bool {
            if (type == "Transform")          return e.HasComponent<Transform>();
            if (type == "NameTag")            return e.HasComponent<NameTag>();
            if (type == "Tag")                return e.HasComponent<Tag>();
            if (type == "DataComponent")      return e.HasComponent<DataComponent>();
            if (type == "MeshRenderer")       return e.HasComponent<MeshRenderer>();
            if (type == "SkeletalAnimation")  return e.HasComponent<SkeletalAnimation>();
            if (type == "NodeAnimation")      return e.HasComponent<NodeAnimationComp>();
            if (type == "GridPlane")          return e.HasComponent<GridPlane>();
            if (type == "PointLight")         return e.HasComponent<PointLight>();
            if (type == "DirectionalLight")   return e.HasComponent<DirectionalLight>();
            if (type == "SpotLight")          return e.HasComponent<SpotLight>();
            if (type == "Camera")             return e.HasComponent<CameraComponent>();
            if (type == "Sprite2D")           return e.HasComponent<Sprite2D>();
            if (type == "AudioSource")        return e.HasComponent<AudioSource>();
            if (type == "Gimmick")            return e.HasComponent<Gimmick>();
            if (type == "RigidBody")          return e.HasComponent<RigidBody>();
            if (type == "BoxCollider")        return e.HasComponent<BoxCollider>();
            if (type == "SphereCollider")     return e.HasComponent<SphereCollider>();
            if (type == "CapsuleCollider")    return e.HasComponent<CapsuleCollider>();
            if (type == "ConvexHullCollider") return e.HasComponent<ConvexHullCollider>();
            if (type == "CharacterController") return e.HasComponent<CharacterController>();
            if (type == "LuaScript")          return e.HasComponent<LuaScript>();
            if (type == "ParticleEmitter")    return e.HasComponent<ParticleEmitter>();
            if (type == "TrailRenderer")      return e.HasComponent<TrailRenderer>();
            if (type == "DecalComponent")     return e.HasComponent<DecalComponent>();
            if (type == "Trigger")            return e.HasComponent<Trigger>();
            if (type == "UICanvas")           return e.HasComponent<UICanvas>();
            if (type == "UIRect")             return e.HasComponent<UIRect>();
            if (type == "UIImage")            return e.HasComponent<UIImage>();
            if (type == "UIText")             return e.HasComponent<UIText>();
            if (type == "UIButton")           return e.HasComponent<UIButton>();
            if (type == "UISlider")           return e.HasComponent<UISlider>();
            if (type == "UIToggle")           return e.HasComponent<UIToggle>();
            if (type == "UIScrollView")       return e.HasComponent<UIScrollView>();
            if (type == "UILayout")           return e.HasComponent<UILayout>();
            if (type == "UIAnimator")         return e.HasComponent<UIAnimator>();
            if (type == "AnimatorController") return e.HasComponent<AnimatorController>();
            if (type == "FootIK")             return e.HasComponent<FootIK>();
            // タイプミスや未対応型を「持ってない」と誤認させない（デバッグ困難の元）。
            // 毎フレーム呼ばれてもスパムしないよう型名ごとに1回だけ警告する。
            {
                static std::set<std::string> warned;
                if (warned.insert(type).second)
                    Logger::Warn("hasComponent: 不明なコンポーネント型 \"{}\"", type);
            }
            return false;
        },

        // Skeletal animation playback (backward compatible)
        "playAnim", [](Entity& e, int clipIndex, float blendDuration) {
            if (!e.HasComponent<SkeletalAnimation>()) return;
            auto& skelAnim = e.GetComponent<SkeletalAnimation>();
            if (clipIndex >= 0 && clipIndex < static_cast<int>(skelAnim.clips.size()))
            {
                skelAnim.animator->CrossFadeTo(skelAnim.clips[clipIndex].get(), blendDuration);
            }
        },

        "playAnimByName", [](Entity& e, const std::string& name, float blendDuration) {
            if (!e.HasComponent<SkeletalAnimation>()) return;
            auto& skelAnim = e.GetComponent<SkeletalAnimation>();
            for (const auto& clip : skelAnim.clips)
            {
                if (clip->GetName() == name)
                {
                    skelAnim.animator->CrossFadeTo(clip.get(), blendDuration);
                    return;
                }
            }
        },

        "setLooping", [](Entity& e, bool loop) {
            if (!e.HasComponent<SkeletalAnimation>()) return;
            e.GetComponent<SkeletalAnimation>().animator->SetLooping(loop);
        },

        "setAnimSpeed", [](Entity& e, float speed) {
            if (!e.HasComponent<SkeletalAnimation>()) return;
            e.GetComponent<SkeletalAnimation>().animator->SetSpeed(speed);
        },

        // --- アニメーションステートマシン(.animfsm / AnimatorController) ---
        // FSM の「構造」はアセット側にあり、Lua が触るのは**パラメータだけ**。
        // AnimatorController が無いときは黙って no-op / 既定値（既存 API と同じ流儀）。
        "setAnimFloat", [](Entity& e, const std::string& name, float value) {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return;
            auto it = st->params.find(name);
            if (it == st->params.end()) return;
            it->second.f = value;
        },

        "setAnimBool", [](Entity& e, const std::string& name, bool value) {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return;
            auto it = st->params.find(name);
            if (it == st->params.end()) return;
            it->second.b = value;
        },

        "setAnimTrigger", [](Entity& e, const std::string& name) {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return;
            auto it = st->params.find(name);
            if (it == st->params.end()) return;
            it->second.b = true;   // 次に条件を満たした遷移が消費する
        },

        "getAnimFloat", [](Entity& e, const std::string& name) -> float {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return 0.0f;
            auto it = st->params.find(name);
            return (it == st->params.end()) ? 0.0f : it->second.f;
        },

        "getAnimBool", [](Entity& e, const std::string& name) -> bool {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return false;
            auto it = st->params.find(name);
            return (it == st->params.end()) ? false : it->second.b;
        },

        "getAnimStateName", [](Entity& e, sol::optional<int> layer) -> std::string {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            const u32 li = static_cast<u32>((std::max)(0, layer.value_or(0)));
            if (!st || li >= st->layers.size()) return std::string();
            const i32 s = st->layers[li].curState;
            const auto& states = st->asset.layers[li].states;
            if (s < 0 || s >= static_cast<i32>(states.size())) return std::string();
            return states[static_cast<size_t>(s)].name;
        },

        "getAnimNormalizedTime", [](Entity& e, sol::optional<int> layer) -> float {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st || !e.HasComponent<SkeletalAnimation>()) return 0.0f;
            const u32 li = static_cast<u32>((std::max)(0, layer.value_or(0)));
            return anim_graph::NormalizedTime(*st, li, e.GetComponent<SkeletalAnimation>().clips);
        },

        "playAnimState", [](Entity& e, const std::string& stateName, sol::optional<float> blend) {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return;
            anim_graph::PlayState(*st, 0, stateName, blend.value_or(0.2f));
        },

        "setAnimLayerWeight", [](Entity& e, int layer, float w) {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            if (!st) return;
            const u32 li = static_cast<u32>((std::max)(0, layer));
            if (li >= st->layers.size()) return;
            st->layers[li].weight = std::clamp(w, 0.0f, 1.0f);
        },

        "getAnimLayerWeight", [](Entity& e, int layer) -> float {
            AnimGraphRuntimeState* st = AnimStateOf(e);
            const u32 li = static_cast<u32>((std::max)(0, layer));
            if (!st || li >= st->layers.size()) return 0.0f;
            return st->layers[li].weight;
        },

        // --- フット IK（接地補正）---
        "setFootIKWeight", [](Entity& e, float w) {
            if (!e.HasComponent<FootIK>()) return;
            e.GetComponent<FootIK>().weight = std::clamp(w, 0.0f, 1.0f);
        },

        "getFootIKWeight", [](Entity& e) -> float {
            if (!e.HasComponent<FootIK>()) return 0.0f;
            return e.GetComponent<FootIK>().weight;
        },

        "isFootGrounded", [](Entity& e, sol::optional<bool> rightFoot) -> bool {
            if (!e.HasComponent<FootIK>()) return false;
            const FootIK& ik = e.GetComponent<FootIK>();
            return rightFoot.value_or(false) ? ik._rContact : ik._lContact;
        },

        // --- タイムライン製 UI アニメ(.uianim) ---
        // 実体の評価は UiAnimRuntime（Application::Update）が行う。ここは再生状態を
        // 立てるだけなので、UiAnimRuntime への参照を ScriptEngine に持たせる必要がない。
        // clipPath 省略時は現在割り当てられているクリップを頭から再生する。
        "playUiAnim", [](Entity& e, sol::optional<std::string> clipPath) {
            auto& pl = e.GetOrAddComponent<UIAnimPlayer>();
            if (clipPath && !clipPath->empty()) pl.clipPath = *clipPath;
            if (pl.clipPath.empty()) return;
            pl._time = pl._prevTime = 0.0f;
            pl._finished = false;
            pl._playing  = true;
        },

        "stopUiAnim", [](Entity& e) {
            if (!e.HasComponent<UIAnimPlayer>()) return;
            e.GetComponent<UIAnimPlayer>()._playing = false;
        },

        // 任意時刻へシーク（0..duration）。再生中かどうかは変えない = 一時停止したまま
        // スクラブしてポーズ絵を作る、といった使い方ができる。
        "setUiAnimTime", [](Entity& e, float t) {
            if (!e.HasComponent<UIAnimPlayer>()) return;
            auto& pl = e.GetComponent<UIAnimPlayer>();
            pl._time = pl._prevTime = (t > 0.0f) ? t : 0.0f;
            pl._finished = false;
        },

        "setUiAnimSpeed", [](Entity& e, float speed) {
            if (!e.HasComponent<UIAnimPlayer>()) return;
            e.GetComponent<UIAnimPlayer>().speed = speed;
        },

        // --- スプライトシート連番アニメ(.spranim) ---
        // 同じシーケンスを再指定しても頭から再生し直す（被弾モーションの連打など、
        // 「もう一度頭から」が欲しい場面の方が多い）。
        "playSprite", [](Entity& e, const std::string& seqName) {
            auto& sa = e.GetOrAddComponent<SpriteAnimator>();
            sa.currentSeq = seqName;
            sa._time = 0.0f;
            sa._finished = false;
            sa._playing  = true;
        },

        "stopSprite", [](Entity& e) {
            if (!e.HasComponent<SpriteAnimator>()) return;
            e.GetComponent<SpriteAnimator>()._playing = false;
        },

        "setSpriteSheet", [](Entity& e, const std::string& sheetPath) {
            auto& sa = e.GetOrAddComponent<SpriteAnimator>();
            sa.sheetPath = sheetPath;
            sa._time = 0.0f;
            sa._finished = false;
        },

        "getAnimCount", [](const Entity& e) -> int {
            if (!e.HasComponent<SkeletalAnimation>()) return 0;
            return static_cast<int>(e.GetComponent<SkeletalAnimation>().clips.size());
        },

        "getAnimName", [](const Entity& e, int index) -> std::string {
            if (!e.HasComponent<SkeletalAnimation>()) return "";
            const auto& clips = e.GetComponent<SkeletalAnimation>().clips;
            if (index >= 0 && index < static_cast<int>(clips.size()))
                return clips[index]->GetName();
            return "";
        },

        // --- ライト（統一プロキシ Light を返す）---
        // ライトを持っていなければ nil。 例: local l = e:light(); if l then l.intensity = 3 end
        "light", [this](Entity& e) -> sol::optional<LuaLight> {
            if (!e.IsValid() || !m_scene) return sol::nullopt;
            LuaLight l{&m_scene->GetRegistry(), e.GetHandle()};
            if (!l.Any()) return sol::nullopt;
            return l;
        },
        // ライトを後付けする。kind: "point"(既定) / "directional"("dir"/"sun") / "spot"
        // 既にその型があればそれを返す（重ね掛けしない）。戻り値は Light プロキシ。
        "addLight", [this](Entity& e, sol::optional<std::string> kind) -> sol::optional<LuaLight> {
            if (!e.IsValid() || !m_scene) return sol::nullopt;
            const std::string k = kind.value_or(std::string("point"));
            if (k == "directional" || k == "dir" || k == "sun") e.GetOrAddComponent<DirectionalLight>();
            else if (k == "spot")                               e.GetOrAddComponent<SpotLight>();
            else                                                e.GetOrAddComponent<PointLight>();
            return LuaLight{&m_scene->GetRegistry(), e.GetHandle()};
        },
        // 付いているライト成分を全部外す（消灯ではなく削除。CB 枠を空けたいとき用）
        "removeLight", [](Entity& e) {
            e.RemoveComponent<PointLight>();
            e.RemoveComponent<DirectionalLight>();
            e.RemoveComponent<SpotLight>();
        }
    );

    // --- Scene ---
    lua.new_usertype<Scene>("Scene",
        "spawn", [](Scene& s, const std::string& name, const std::string& modelPath,
                     DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 rot, DirectX::XMFLOAT3 scale) -> Entity {
            return s.Spawn(name, modelPath, pos, rot, scale);
        },
        "spawnPlane", [](Scene& s, const std::string& name, DirectX::XMFLOAT3 pos,
                         float size, bool grid) -> Entity {
            return s.SpawnPlane(name, pos, size, grid);
        },
        "spawnBox", [](Scene& s, const std::string& name, DirectX::XMFLOAT3 pos,
                       DirectX::XMFLOAT3 rot, DirectX::XMFLOAT3 scale) -> Entity {
            return s.SpawnBox(name, pos, rot, scale);
        },
        "spawnSphere", [](Scene& s, const std::string& name, DirectX::XMFLOAT3 pos,
                          float radius) -> Entity {
            return s.SpawnSphere(name, pos, radius);
        },
        "remove", [](Scene& s, Entity entity) { s.Remove(entity); },
        "getEntityCount", &Scene::GetEntityCount,
        "findEntity", &Scene::FindEntity,
        "setUVScale", [](Scene& s, Entity& e, float u, float v) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            auto& mr = reg.get<MeshRenderer>(e.GetHandle());
            auto* device = s.GetDevice();
            if (!device) return;
            // ★コンポーネントにも記録する。焼き込むだけだとシーン保存に残らず、
            //   開き直すと 1.0 に戻る（すぐ下の setColor が同じ理由で colorTint を書いている）。
            mr.uvScaleU = u;
            mr.uvScaleV = v;
            for (auto* mesh : mr.meshes)
            {
                if (mesh) mesh->ApplyUVScale(*device, u, v);
            }
        },
        // 頂点カラーで色付け（生成時に1度だけ。GPU 同期を伴うため毎フレーム禁止）
        "setColor", [](Scene& s, Entity& e, float r, float g, float b) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            auto& mr = reg.get<MeshRenderer>(e.GetHandle());
            mr.colorTint    = {r, g, b, 1.0f};   // シーン保存で色指定が消えないよう記録
            mr.hasColorTint = true;
            // 共有メッシュ(instanced)は VB を焼かず per-instance 色へ。発光弾はこちら＝
            // setColor が VB 再生成しない＝大量の弾でも GPU 同期ゼロ。
            if (mr.instanced)
            {
                mr.instanceColor = {r, g, b, 1.0f};
                return;
            }
            auto* device = s.GetDevice();
            if (!device) return;
            for (auto* mesh : mr.meshes)
            {
                if (mesh) mesh->SetVertexColor(*device, r, g, b, 1.0f);
            }
        },
        // Sprite2D::effectValue を書き換える(カスタムシェーダーへ渡す汎用の進捗/強度値)。
        // 頂点属性として補間されるだけなので毎フレーム呼んでも安価(GPU同期・VB再生成なし)。
        "setSpriteEffect", [](Scene& s, Entity& e, float value) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            reg.get<Sprite2D>(e.GetHandle()).effectValue = value;
        },
        // Sprite2D::color.w を書き換える(不透明度0..1、半透明演出用)。setSpriteEffect同様、
        // 頂点属性として補間されるだけなので毎フレーム呼んでも安価(GPU同期・VB再生成なし)。
        "setSpriteAlpha", [](Scene& s, Entity& e, float alpha) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            reg.get<Sprite2D>(e.GetHandle()).color.w = alpha;
        },
        // MeshRenderer::effectValue を書き換える(カスタムシェーダーへ渡す汎用の進捗/強度値、
        // Sprite2D::effectValue のメッシュ版)。ルート定数なので毎フレーム呼んでも安価
        // (GPU同期・VB再生成なし、マテリアル/テクスチャには一切影響しない)。
        "setMeshEffect", [](Scene& s, Entity& e, float value) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            reg.get<MeshRenderer>(e.GetHandle()).effectValue = value;
        },
        // MeshRenderer::shaderParams(カスタムシェーダーへ渡す汎用 float4)を書き換える。
        // effectValue と同じルート定数経路なので毎フレーム呼んでも安価。
        "setMeshParams", [](Scene& s, Entity& e, float x, float y, float z, float w) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            reg.get<MeshRenderer>(e.GetHandle()).shaderParams = {x, y, z, w};
        },
        // Sprite2D::shaderParams(カスタムシェーダーへ渡す汎用 float4、TEXCOORD2)を書き換える。
        // effectValue と同じ頂点属性経路なので毎フレーム呼んでも安価(GPU同期・VB再生成なし)。
        "setSpriteParams", [](Scene& s, Entity& e, float x, float y, float z, float w) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            reg.get<Sprite2D>(e.GetHandle()).shaderParams = {x, y, z, w};
        },
        // Sprite2D::uvMin/uvMax を直接指定(アトラス切り出しの実行時切替。フリップブック
        // (animFrames>0)/スクロール中は描画時に上書きされる点に注意)。
        "setSpriteUV", [](Scene& s, Entity& e, float u0, float v0, float u1, float v1) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            auto& sp = reg.get<Sprite2D>(e.GetHandle());
            sp.uvMin = {u0, v0};
            sp.uvMax = {u1, v1};
        },
        // Sprite2D の UVスクロール速度(単位/秒)を設定(溶岩表面・滝・背景ループ用)。
        // animFrames>0 のときはフリップブック優先で無視される。
        "setSpriteScroll", [](Scene& s, Entity& e, float su, float sv) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            auto& sp = reg.get<Sprite2D>(e.GetHandle());
            sp.scrollU = su;
            sp.scrollV = sv;
        },
        // Sprite2D のフリップブックアニメを設定(frames=0 で停止し uvMin/uvMax 指定に戻る)。
        // cols=0 は frames と同じ(横1行ストリップ)、row はシート内の行(walk行/jump行等)。
        // 呼ぶたび再生位置が頭に戻る = 攻撃モーション等の切替がこれ1発で書ける。
        "setSpriteAnim", [](Scene& s, Entity& e, int frames, float fps, int cols, int row) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            auto& sp = reg.get<Sprite2D>(e.GetHandle());
            sp.animFrames = frames;
            sp.animFps    = fps;
            sp.animCols   = cols;
            sp.animRow    = row;
            sp._animT     = 0.0f;
        },
        // Sprite2D の再生モードを設定(0=ループ 1=単発(最終フレームで停止) 2=往復)。
        // 再生位置は頭に戻る。
        "setSpriteAnimMode", [](Scene& s, Entity& e, int mode) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            auto& sp = reg.get<Sprite2D>(e.GetHandle());
            sp.animMode = mode;
            sp._animT   = 0.0f;
        },
        // Sprite2D の連番アニメを頭から再生し直す(設定は変えない)。
        "restartSpriteAnim", [](Scene& s, Entity& e) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return;
            reg.get<Sprite2D>(e.GetHandle())._animT = 0.0f;
        },
        // 単発(animMode=1)の再生が終わったか。ループ/往復は常に false。
        // 爆発スプライトを消す/次の行動へ進む判定に使う。
        "isSpriteAnimDone", [](Scene& s, Entity& e) -> bool {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<Sprite2D>(e.GetHandle())) return false;
            const auto& sp = reg.get<Sprite2D>(e.GetHandle());
            return IsFlipbookFinished(sp.animFrames, sp.animFps, sp.animMode, sp._animT);
        },
        // --- メッシュ(3D)の UVスクロール / 連番アニメ ---
        // MeshRenderer の UVスクロール速度(uv/秒)。滝・溶岩・コンベア・流れる雲。
        // 頂点は触らないので毎フレーム呼んでも安価(VB再生成なし)。
        "setMeshUvScroll", [](Scene& s, Entity& e, float su, float sv) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            auto& mr = reg.get<MeshRenderer>(e.GetHandle());
            mr.uvScrollU = su;
            mr.uvScrollV = sv;
        },
        // MeshRenderer の連番アニメを設定(frames=0 で停止)。呼ぶたび再生位置が頭に戻る。
        // cols=0 は frames と同じ(横1行ストリップ)、row はシート内の行。
        "setMeshAnim", [](Scene& s, Entity& e, int frames, float fps, int cols, int row) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            auto& mr = reg.get<MeshRenderer>(e.GetHandle());
            mr.animFrames = frames;
            mr.animFps    = fps;
            mr.animCols   = cols;
            mr.animRow    = row;
            mr._animT     = 0.0f;
        },
        // MeshRenderer の再生モード(0=ループ 1=単発 2=往復)。再生位置は頭に戻る。
        "setMeshAnimMode", [](Scene& s, Entity& e, int mode) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return;
            auto& mr = reg.get<MeshRenderer>(e.GetHandle());
            mr.animMode = mode;
            mr._animT   = 0.0f;
        },
        // MeshRenderer の連番アニメが単発再生を終えたか。
        "isMeshAnimDone", [](Scene& s, Entity& e) -> bool {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<MeshRenderer>(e.GetHandle())) return false;
            const auto& mr = reg.get<MeshRenderer>(e.GetHandle());
            return IsFlipbookFinished(mr.animFrames, mr.animFps, mr.animMode, mr._animT);
        },
        // --- ゲーム内UI（retained-mode）: スコア表示・HPバー等をスクリプトから書き換える ---
        // UIText::text を書き換える(スコア・残機・メッセージ)。UIText が無ければ何もしない。
        // タイプライター(typewriterSpeed>0)は先頭から再生し直す=会話送りがこれ1発で書ける。
        "setUiText", [](Scene& s, Entity& e, const std::string& text) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIText>(e.GetHandle())) return;
            auto& t = reg.get<UIText>(e.GetHandle());
            // ★同じ文字列でも頭出しする。以前は != のときだけ戻していたので、
            //   会話の「……」のように同じ台詞を続けて出すと 2 回目が一瞬で全文表示になった。
            //   このバインドのコメント自身が「set はタイプライターを先頭から再生し直す」と
            //   謳っているので、その約束に合わせる。
            t._twT = 0.0f;
            t.text = text;
        },
        // UIText::text を読む。UIText が無ければ空文字列。
        "getUiText", [](Scene& s, Entity& e) -> std::string {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIText>(e.GetHandle())) return std::string();
            return reg.get<UIText>(e.GetHandle()).text;
        },
        // タイプライター速度(文字/秒)を設定して先頭から再生。0=即全表示(スキップに使える)。
        "setUiTypewriter", [](Scene& s, Entity& e, float speed) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIText>(e.GetHandle())) return;
            auto& t = reg.get<UIText>(e.GetHandle());
            t.typewriterSpeed = (std::max)(0.0f, speed);
            t._twT = 0.0f;
        },
        // タイプライターが全文まで到達したか(会話の「クリックで次へ」判定用)。
        // typewriterSpeed=0 や UIText 無しは true。
        "isUiTypewriterDone", [](Scene& s, Entity& e) -> bool {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIText>(e.GetHandle())) return true;
            const auto& t = reg.get<UIText>(e.GetHandle());
            if (t.typewriterSpeed <= 0.0f) return true;
            // UTF-8 コードポイント数（描画側と同じ数え方。rich=true はタグ除去後 = タグは0文字）
            std::size_t chars = 0;
            if (t.rich && !t.wrap)
            {
                chars = static_cast<std::size_t>(UiRichStrippedCodepoints(t.text));
            }
            else
            {
                for (std::size_t i = 0; i < t.text.size(); ++chars)
                {
                    const auto c = static_cast<unsigned char>(t.text[i]);
                    i += (c < 0xC0) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
                }
            }
            return t._twT * t.typewriterSpeed >= static_cast<float>(chars);
        },
        // 色を書き換える(0..1)。UIImage 優先、無ければ UIText。どちらも無ければ何もしない。
        "setUiColor", [](Scene& s, Entity& e, float r, float g, float b, float a) {
            auto& reg = s.GetRegistry();
            auto h = e.GetHandle();
            if (reg.all_of<UIImage>(h))
                reg.get<UIImage>(h).color = {r, g, b, a};
            else if (reg.all_of<UIText>(h))
                reg.get<UIText>(h).color = {r, g, b, a};
        },
        // 表示/非表示を切り替える。UIRect.visible を優先し(自身と子孫を丸ごと隠す)、
        // UIRect が無く UICanvas のみ持つエンティティ(キャンバス自身)なら UICanvas.visible を切り替える。
        "setUiVisible", [](Scene& s, Entity& e, bool visible) {
            auto& reg = s.GetRegistry();
            auto h = e.GetHandle();
            if (reg.all_of<UIRect>(h))
                reg.get<UIRect>(h).visible = visible;
            else if (reg.all_of<UICanvas>(h))
                reg.get<UICanvas>(h).visible = visible;
        },
        // UIImage::texturePath を差し替える(assets 相対)。UIImage が無ければ何もしない。
        "setUiTexture", [](Scene& s, Entity& e, const std::string& path) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return;
            reg.get<UIImage>(e.GetHandle()).texturePath = path;
        },
        // UIImage の UVスクロール速度(uv/秒)。タイル(uvMax>1)と併用で流れるパターンになる。
        // 連番アニメ(animFrames>0)中は無視される。
        "setUiUvScroll", [](Scene& s, Entity& e, float su, float sv) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return;
            reg.get<UIImage>(e.GetHandle()).uvScroll = {su, sv};
        },
        // UIImage の連番アニメを設定(frames=0 で停止し uvMin/uvMax 指定に戻る)。
        // 呼ぶたび再生位置が頭に戻る。cols=0 は frames と同じ(横1行ストリップ)。
        "setUiAnim", [](Scene& s, Entity& e, int frames, float fps, int cols, int row) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return;
            auto& img = reg.get<UIImage>(e.GetHandle());
            img.animFrames = frames;
            img.animFps    = fps;
            img.animCols   = cols;
            img.animRow    = row;
            img._animT     = 0.0f;
        },
        // UIImage の再生モード(0=ループ 1=単発 2=往復)。再生位置は頭に戻る。
        "setUiAnimMode", [](Scene& s, Entity& e, int mode) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return;
            auto& img = reg.get<UIImage>(e.GetHandle());
            img.animMode = mode;
            img._animT   = 0.0f;
        },
        // UIImage の連番アニメを頭から再生し直す(設定は変えない)。
        "restartUiAnim", [](Scene& s, Entity& e) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return;
            reg.get<UIImage>(e.GetHandle())._animT = 0.0f;
        },
        // UIImage の連番アニメが単発再生を終えたか(ループ/往復は常に false)。
        "isUiAnimDone", [](Scene& s, Entity& e) -> bool {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return false;
            const auto& img = reg.get<UIImage>(e.GetHandle());
            return IsFlipbookFinished(img.animFrames, img.animFps, img.animMode, img._animT);
        },
        // UIImage::fillAmount を設定する(0..1 にクランプ。HPバー/ゲージ用)。UIImage が無ければ何もしない。
        "setUiFill", [](Scene& s, Entity& e, float amount) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return;
            reg.get<UIImage>(e.GetHandle()).fillAmount = std::clamp(amount, 0.0f, 1.0f);
        },
        // UIImage::fillAmount を読む。UIImage が無ければ 0。
        "getUiFill", [](Scene& s, Entity& e) -> float {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIImage>(e.GetHandle())) return 0.0f;
            return reg.get<UIImage>(e.GetHandle()).fillAmount;
        },
        // UIRect::rotation(視覚回転・度)を設定する。UIRect が無ければ何もしない。
        "setUiRotation", [](Scene& s, Entity& e, float deg) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIRect>(e.GetHandle())) return;
            reg.get<UIRect>(e.GetHandle()).rotation = deg;
        },
        // UIRect::rotation を読む。UIRect が無ければ 0。
        "getUiRotation", [](Scene& s, Entity& e) -> float {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIRect>(e.GetHandle())) return 0.0f;
            return reg.get<UIRect>(e.GetHandle()).rotation;
        },
        // UISlider の現在値(実値)を読む/書く。書き込みは min..max へクランプ。onChangeEvent は
        // 発火しない(スクリプト起因の変更でハンドラが再帰しないように。UIToggle も同じ)。
        "getUiSlider", [](Scene& s, Entity& e) -> float {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UISlider>(e.GetHandle())) return 0.0f;
            return reg.get<UISlider>(e.GetHandle()).value;
        },
        "setUiSlider", [](Scene& s, Entity& e, float v) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UISlider>(e.GetHandle())) return;
            auto& sld = reg.get<UISlider>(e.GetHandle());
            sld.value = std::clamp(v, std::min(sld.minValue, sld.maxValue),
                                   std::max(sld.minValue, sld.maxValue));
        },
        // UIScrollView のスクロール量(px)を読む/書く。書き込みは翌フレームの描画で
        // 0..(コンテンツ−ビュー) にクランプされる。「一番下へ」は大きい値を入れれば良い。
        "getUiScroll", [](Scene& s, Entity& e) -> std::tuple<float, float> {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIScrollView>(e.GetHandle())) return {0.0f, 0.0f};
            const auto& sv = reg.get<UIScrollView>(e.GetHandle());
            return {sv.scrollX, sv.scrollY};
        },
        "setUiScroll", [](Scene& s, Entity& e, float x, float y) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIScrollView>(e.GetHandle())) return;
            auto& sv = reg.get<UIScrollView>(e.GetHandle());
            sv.scrollX = std::max(0.0f, x);
            sv.scrollY = std::max(0.0f, y);
        },
        // UIToggle の状態を読む/書く。
        "getUiToggle", [](Scene& s, Entity& e) -> bool {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIToggle>(e.GetHandle())) return false;
            return reg.get<UIToggle>(e.GetHandle()).isOn;
        },
        "setUiToggle", [](Scene& s, Entity& e, bool on) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIToggle>(e.GetHandle())) return;
            reg.get<UIToggle>(e.GetHandle()).isOn = on;
        },
        // --- UI アニメーション / トゥイーン ---
        // 対象は Entity か エンティティID(数値。ボタンクリックの data.source をそのまま渡せる)。
        // params: { dx=, dy=(相対移動px), scale=(視覚拡縮。scaleX=/scaleY= で非等方=
        //           スカッシュ&ストレッチ/フリップ風), alpha=(視覚透明度0..1),
        //           rotate=(視覚回転・度・絶対目標値。UIRect.rotation へ加算合成),
        //           color={r,g,b}(視覚カラー乗数。1超えで白フラッシュ。完了後も持続),
        //           shake=(振動振幅px。duration で 0 へ減衰), shakeFreq=24(Hz),
        //           fill=(UIImage.fillAmount を絶対目標値へ=ゲージのなめらか増減),
        //           countTo=(UIText へ数字ロール。countFrom= 省略時は現在テキストの数値,
        //           countFmt="%d"(printf 書式。"%05d"=ゼロ埋め,"%.1f"=小数)),
        //           onComplete=function()(完了時に 1 回呼ばれる。SE 同期/演出チェーン用),
        //           duration=0.3, delay=0, easing="out" }
        // easing: "linear"/"in"/"out"/"inOut"/"back"(勢い)/"bounce"/"elastic"/"expo"(鋭い減速)/
        //         "inBack"(溜め)/"inOutBack"/"quint"(強い減速)/"sine"(ゆったり)(または 0..11)
        "tweenUi", [](Scene& s, sol::object target, sol::table params) {
            auto& reg = s.GetRegistry();
            entt::entity h = entt::null;
            if (target.is<Entity>()) h = target.as<Entity>().GetHandle();
            else if (target.is<double>())
                h = static_cast<entt::entity>(static_cast<std::uint32_t>(target.as<double>()));
            if (h == entt::null || !reg.valid(h) || !reg.all_of<UIRect>(h)) return;

            UiTween t;
            t.duration = params.get_or("duration", 0.3f);
            t.delay    = params.get_or("delay", 0.0f);
            if (sol::object eo = params["easing"]; eo.valid())
            {
                if (eo.is<std::string>())
                {
                    const std::string es = eo.as<std::string>();
                    if      (es == "linear")                    t.easing = 0;
                    else if (es == "in")                        t.easing = 1;
                    else if (es == "out")                       t.easing = 2;
                    else if (es == "inOut" || es == "inout")    t.easing = 3;
                    else if (es == "back")                      t.easing = 4;
                    else if (es == "bounce")                    t.easing = 5;
                    else if (es == "elastic")                   t.easing = 6;
                    else if (es == "expo")                      t.easing = 7;
                    else if (es == "inBack")                    t.easing = 8;
                    else if (es == "inOutBack")                 t.easing = 9;
                    else if (es == "quint")                     t.easing = 10;
                    else if (es == "sine")                      t.easing = 11;
                }
                else if (eo.is<int>())
                {
                    t.easing = std::clamp(eo.as<int>(), 0, 11);
                }
            }
            const float dx = params.get_or("dx", 0.0f);
            const float dy = params.get_or("dy", 0.0f);
            if (dx != 0.0f || dy != 0.0f) { t.hasMove = true; t.moveDelta = {dx, dy}; }
            if (sol::object v = params["scale"]; v.is<float>())
            {
                const float sc = (std::max)(0.0f, v.as<float>());
                t.hasScaleX = t.hasScaleY = true;
                t.scaleXTo = t.scaleYTo = sc;
            }
            if (sol::object v = params["scaleX"]; v.is<float>())
            { t.hasScaleX = true; t.scaleXTo = (std::max)(0.0f, v.as<float>()); }
            if (sol::object v = params["scaleY"]; v.is<float>())
            { t.hasScaleY = true; t.scaleYTo = (std::max)(0.0f, v.as<float>()); }
            if (sol::object v = params["alpha"]; v.is<float>())
            { t.hasAlpha = true; t.alphaTo = std::clamp(v.as<float>(), 0.0f, 1.0f); }
            if (sol::object v = params["rotate"]; v.is<float>())
            { t.hasRotate = true; t.rotTo = v.as<float>(); }
            if (sol::object v = params["color"]; v.is<sol::table>())
            {
                sol::table ct = v.as<sol::table>();
                t.hasColor = true;
                t.colTo = {(std::max)(0.0f, ct.get_or(1, 1.0f)),
                           (std::max)(0.0f, ct.get_or(2, 1.0f)),
                           (std::max)(0.0f, ct.get_or(3, 1.0f))};
            }
            if (sol::object v = params["shake"]; v.is<float>())
            {
                t.hasShake  = true;
                t.shakeAmp  = (std::max)(0.0f, v.as<float>());
                t.shakeFreq = (std::max)(0.1f, params.get_or("shakeFreq", 24.0f));
            }
            if (sol::object v = params["fill"]; v.is<float>())
            { t.hasFill = true; t.fillTo = std::clamp(v.as<float>(), 0.0f, 1.0f); }
            if (sol::object v = params["countTo"]; v.is<double>())
            {
                t.hasCount = true;
                t.countTo  = v.as<double>();
                if (sol::object f = params["countFrom"]; f.is<double>())
                { t.countFromSet = true; t.countFrom = f.as<double>(); }
                // printf 書式は [%][フラグ/幅/.精度][d|f] だけ許可（%s 等の書式事故防止）
                std::string fmt = params.get_or("countFmt", std::string("%d"));
                bool ok = fmt.size() >= 2 && fmt.front() == '%'
                          && (fmt.back() == 'd' || fmt.back() == 'f');
                for (size_t i = 1; ok && i + 1 < fmt.size(); ++i)
                {
                    const char ch = fmt[i];
                    ok = (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+'
                         || ch == ' ' || ch == '#';
                }
                t.countFmt = ok ? fmt : std::string("%d");
            }
            if (sol::object v = params["onComplete"]; v.is<sol::function>())
            {
                // 完了フレームの UI 更新後に 1 回呼ばれる（SE 同期・演出チェーン用）。
                // UITweenState は Stop/ランタイムシーン切替の ResetRuntimeState で
                // Lua ステート破棄より先に消えるため、参照の残留はない
                sol::function fn = v.as<sol::function>();
                t.onComplete = [fn]() {
                    sol::protected_function pf = fn;
                    auto r = pf();
                    if (!r.valid())
                    {
                        sol::error err = r;
                        Logger::Warn("tweenUi onComplete エラー: {}", err.what());
                    }
                };
            }
            if (!t.hasMove && !t.hasScaleX && !t.hasScaleY && !t.hasAlpha && !t.hasRotate
                && !t.hasColor && !t.hasShake && !t.hasFill && !t.hasCount)
                return;
            reg.get_or_emplace<UITweenState>(h).tweens.push_back(t);
        },
        // 進行中の tween を全部打ち切る（連打対策。DOTween の Kill 相当）。視覚値は
        // 既定(等倍/不透明)へ戻す。UIAnimator の出現/ループには影響しない
        "stopUiTweens", [](Scene& s, sol::object target) {
            auto& reg = s.GetRegistry();
            entt::entity h = entt::null;
            if (target.is<Entity>()) h = target.as<Entity>().GetHandle();
            else if (target.is<double>())
                h = static_cast<entt::entity>(static_cast<std::uint32_t>(target.as<double>()));
            if (h == entt::null || !reg.valid(h)) return;
            if (auto* tw = reg.try_get<UITweenState>(h))
                *tw = UITweenState{};
        },
        // 表示して出現アニメを最初から再生（UIAnimator 無しなら visible=true だけ）。
        "showUi", [](Scene& s, sol::object target) {
            auto& reg = s.GetRegistry();
            entt::entity h = entt::null;
            if (target.is<Entity>()) h = target.as<Entity>().GetHandle();
            else if (target.is<double>())
                h = static_cast<entt::entity>(static_cast<std::uint32_t>(target.as<double>()));
            if (h == entt::null || !reg.valid(h)) return;
            if (reg.all_of<UIRect>(h))          reg.get<UIRect>(h).visible = true;
            else if (reg.all_of<UICanvas>(h))   reg.get<UICanvas>(h).visible = true;
            if (auto* an = reg.try_get<UIAnimator>(h)) { an->_t = 0.0f; an->_mode = 0; }
            // ★タイプライターも頭出しする。_twT は可視/不可視に関係なく進み続けるので、
            //   hideUi → showUi で開き直した会話ウィンドウは全文表示で出ていた
            //   （エディタで visible=false のまま置いたものは Play 開始からの経過ぶん進む）。
            if (auto* tx = reg.try_get<UIText>(h)) tx->_twT = 0.0f;
            for (auto [ce, ct, ctx2] : reg.view<Transform, UIText>().each())
                if (ct.parent == h) ctx2._twT = 0.0f;   // 直下の子も
        },
        // 出現アニメの逆再生で消す（UIAnimator 無し/出現アニメ無しなら即 visible=false）。
        // 消えた後に戻すのは showUi（setUiVisible では戻らない）。
        "hideUi", [](Scene& s, sol::object target) {
            auto& reg = s.GetRegistry();
            entt::entity h = entt::null;
            if (target.is<Entity>()) h = target.as<Entity>().GetHandle();
            else if (target.is<double>())
                h = static_cast<entt::entity>(static_cast<std::uint32_t>(target.as<double>()));
            if (h == entt::null || !reg.valid(h)) return;
            auto* an = reg.try_get<UIAnimator>(h);
            if (an && an->showAnim != 0)
            {
                if (an->_mode != 4) { an->_t = 0.0f; an->_mode = 3; }
            }
            else if (reg.all_of<UIRect>(h))     reg.get<UIRect>(h).visible = false;
            else if (reg.all_of<UICanvas>(h))   reg.get<UICanvas>(h).visible = false;
        },
        // 配置済み Gimmick コンポーネントを持つ全エンティティを列挙し、
        // パラメータ付きの配列(1始まり)で返す。ゲームスクリプトが動き/当たり判定を駆動する。
        // 各要素: { e=Entity, name=, kind=, period=, phase=, amplitude=, threshold=, solid=, deadly= }
        "gimmicks", [this](Scene& s) -> sol::table {
            auto& reg = s.GetRegistry();
            sol::table arr = m_lua->create_table();
            int idx = 1;
            auto view = reg.view<Gimmick, Transform, NameTag>();
            for (auto e : view)
            {
                const auto& g  = view.get<Gimmick>(e);
                const auto& nm = view.get<NameTag>(e);
                sol::table t = m_lua->create_table();
                t["e"]         = Entity(e, &reg);
                t["name"]      = nm.name;
                t["kind"]      = g.kind;
                t["period"]    = g.period;
                t["phase"]     = g.phase;
                t["amplitude"] = g.amplitude;
                t["threshold"] = g.threshold;
                t["solid"]     = g.solid;
                t["deadly"]    = g.deadly;
                arr[idx++] = t;
            }
            return arr;
        },
        // タグで列挙（filter汎用化・RTS群選択）。エンティティ名(string)の配列(1始まり)を返す。
        // 例: for _, name in ipairs(scene:queryByTag("enemy")) do local a = actor(name) ... end
        "queryByTag", [this](Scene& s, const std::string& tag) -> sol::table {
            auto& reg = s.GetRegistry();
            sol::table arr = m_lua->create_table();
            int idx = 1;
            for (auto e : s.QueryByTag(tag))
                if (reg.all_of<NameTag>(e))
                    arr[idx++] = reg.get<NameTag>(e).name;
            return arr;
        },
        // XZ矩形(+任意タグ)で列挙。エンティティ名(string)の配列を返す。RTS の矩形選択向け。
        // 例: scene:queryInBox(minX,minZ,maxX,maxZ) / scene:queryInBox(...,"unit")
        "queryInBox", [this](Scene& s, float minX, float minZ, float maxX, float maxZ,
                             sol::optional<std::string> tag) -> sol::table {
            auto& reg = s.GetRegistry();
            sol::table arr = m_lua->create_table();
            int idx = 1;
            for (auto e : s.QueryInBox(minX, minZ, maxX, maxZ, tag.value_or(std::string{})))
                if (reg.all_of<NameTag>(e))
                    arr[idx++] = reg.get<NameTag>(e).name;
            return arr;
        },

        // ── シーン照明 ──────────────────────────────────────────────
        // 太陽（最初の DirectionalLight）を Light プロキシで返す。無ければ nil。
        // Lighting.setTimeOfDay 等の時間帯演出はこれを掴んで駆動する。
        "sun", [](Scene& s) -> sol::optional<LuaLight> {
            auto& reg = s.GetRegistry();
            auto view = reg.view<DirectionalLight>();
            for (auto e : view) return LuaLight{&reg, e};
            return sol::nullopt;
        },
        // ライト本数と上限。演出で光を増やしすぎて「無言で消える」のを Lua から
        // 検知できるようにする（描画側と同じ view の数え方）。
        // クラスタードライティング（Forward+）化で点/スポットの個別上限は撤廃され、
        // 今は point + spot の合計 1024 灯（1 クラスタあたりは 128 灯）。
        // 戻り: { point=, spot=, directional=, total=, maxTotal=1024, maxPerCluster=128,
        //         maxPoint=1024, maxSpot=1024 }  ※maxPoint/maxSpot は後方互換の別名
        "lightCount", [this](Scene& s) -> sol::table {
            auto& reg = s.GetRegistry();
            int np = 0, ns = 0, nd = 0;
            for (auto e : reg.view<PointLight, Transform>())       { (void)e; ++np; }
            for (auto e : reg.view<SpotLight, Transform>())        { (void)e; ++ns; }
            for (auto e : reg.view<DirectionalLight>())            { (void)e; ++nd; }
            sol::table t = m_lua->create_table();
            t["point"]       = np;
            t["spot"]        = ns;
            t["directional"] = nd;
            t["total"]         = np + ns;
            t["maxTotal"]      = 1024;  // = ClusterMath.h の kMaxSceneLights
            t["maxPerCluster"] = 128;   // = ClusterMath.h の kMaxLightsPerCluster
            t["maxPoint"]      = 1024;  // 後方互換の別名（個別上限は撤廃済み）
            t["maxSpot"]       = 1024;
            return t;
        },
        // 環境光（影の部分の明るさ）。実体は DirectionalLight.ambient なので
        // 読みは最初の太陽から、書きは全 DirectionalLight へ（シーン単位の値として扱う）。
        "getAmbient", [](Scene& s) -> float {
            auto view = s.GetRegistry().view<const DirectionalLight>();
            for (auto e : view) return view.get<const DirectionalLight>(e).ambient;
            return 0.0f;
        },
        "setAmbient", [](Scene& s, float v) {
            auto view = s.GetRegistry().view<DirectionalLight>();
            for (auto e : view) view.get<DirectionalLight>(e).ambient = v;
        },
        // リアルタイム影(CSM)の ON/OFF。false で影パスを丸ごとスキップ（軽量化/演出）。
        "getShadowsEnabled", [](Scene& s) { return s.GetShadowsEnabled(); },
        "setShadowsEnabled", [](Scene& s, bool v) { s.SetShadowsEnabled(v); },
        // スカイボックス / IBL。get は { envMapPath=, iblIntensity=, skyboxIntensity=, drawSkybox= }。
        // set は渡したキーだけ上書き（envMapPath は差し替えに再ロードが要るので実行時変更は非対応）。
        "getSkybox", [this](Scene& s) -> sol::table {
            const auto& sk = s.GetSkyboxSettings();
            sol::table t = m_lua->create_table();
            t["envMapPath"]      = sk.envMapPath;
            t["iblIntensity"]    = sk.iblIntensity;
            t["skyboxIntensity"] = sk.skyboxIntensity;
            t["drawSkybox"]      = sk.drawSkybox;
            return t;
        },
        "setSkybox", [](Scene& s, sol::table t) {
            auto& sk = s.GetSkyboxSettings();
            if (sol::optional<float> v = t["iblIntensity"])    sk.iblIntensity    = *v;
            if (sol::optional<float> v = t["skyboxIntensity"]) sk.skyboxIntensity = *v;
            if (sol::optional<bool>  v = t["drawSkybox"])      sk.drawSkybox      = *v;
        }
    );

    // --- Input ---
    lua.new_usertype<InputSystem>("Input",
        "isKeyDown",      &InputSystem::IsKeyDown,
        "isKeyPressed",   &InputSystem::IsKeyPressed,
        "isAsyncKeyDown", &InputSystem::IsAsyncKeyDown,
        "isMouseCaptured", &InputSystem::IsMouseCaptured,
        "setMouseCapture", &InputSystem::SetMouseCapture,
        "isRightMouseDown", &InputSystem::IsRightMouseDown,
        "getMouseDeltaX",  &InputSystem::GetMouseDeltaX,
        "getMouseDeltaY",  &InputSystem::GetMouseDeltaY,
        // --- ゲームパッド(XInput / Xbox コントローラー、pad = 0..3) ---
        "isPadConnected",      &InputSystem::IsPadConnected,
        "getConnectedPadCount", &InputSystem::GetConnectedPadCount,
        "isPadButtonDown",     &InputSystem::IsPadButtonDown,
        "isPadButtonPressed",  &InputSystem::IsPadButtonPressed,
        "isPadButtonReleased", &InputSystem::IsPadButtonReleased,
        "getPadLeftStickX",   &InputSystem::GetPadLeftStickX,
        "getPadLeftStickY",   &InputSystem::GetPadLeftStickY,
        "getPadRightStickX",  &InputSystem::GetPadRightStickX,
        "getPadRightStickY",  &InputSystem::GetPadRightStickY,
        "getPadLeftTrigger",  &InputSystem::GetPadLeftTrigger,
        "getPadRightTrigger", &InputSystem::GetPadRightTrigger,
        "setPadVibration",      &InputSystem::SetPadVibration,
        "setPadVibrationTimed", &InputSystem::SetPadVibrationTimed,
        // --- ゲーム UI が入力を食ったか（自分の入力を止める判断用）---
        // ★HUD にボタンを 1 つ置くだけで、スティック移動がメニュー移動と二重に効き、
        //   ジャンプ(A/Space)が onClick も撃つ。エンジンは自動で抑止しない
        //   （既存プロジェクトの挙動を勝手に変えないため）ので、ゲーム側がこれで判断する。
        "isUiCapturingMouse", [this](InputSystem&) -> bool {
            return m_uiSystem && m_uiSystem->WantsMouse();
        },
        "isUiCapturingNav", [this](InputSystem&) -> bool {
            return m_uiSystem && m_uiSystem->WantsNav();
        }
    );

    // --- actions（アクションマップ）---
    // 「W キーで前進」ではなく「move アクションで前進」と書くための層。
    // キー割り当てを設定として差し替えられる＝リバインドの土台になる。
    // ★ActionMap は「キーが押されているか」を述語で受け取る設計（InputSystem 非依存）なので、
    //   ここで InputSystem を捕まえたラムダを渡す。ActionMap 側は入力の実体を知らないままでいる。
    {
        sol::table act = lua.create_named_table("actions");

        // bind(name, key, [x, y, z])  x,y,z 省略時は (1,0,0)＝ボタン用
        act.set_function("bind",
            [this](const std::string& name, int key, sol::optional<f32> x,
                   sol::optional<f32> y, sol::optional<f32> z) {
                if (!m_actionMap) return;
                m_actionMap->Bind(name, key,
                    DirectX::XMFLOAT3{ x.value_or(1.0f), y.value_or(0.0f), z.value_or(0.0f) });
            });

        // get(name) -> x, y, z（押されているキーの寄与を合算）
        act.set_function("get", [this](const std::string& name) {
            if (!m_actionMap || !m_input) return std::make_tuple(0.0f, 0.0f, 0.0f);
            const auto v = m_actionMap->Evaluate(name,
                [this](int k) { return m_input->IsKeyDown(k); });
            return std::make_tuple(v.x, v.y, v.z);
        });

        // down(name) -> bool（いずれかのキーが押されている）
        act.set_function("down", [this](const std::string& name) {
            if (!m_actionMap || !m_input) return false;
            return m_actionMap->Active(name, [this](int k) { return m_input->IsKeyDown(k); });
        });

        // pressed(name) -> bool（このフレームで押された。連打の判定用）
        act.set_function("pressed", [this](const std::string& name) {
            if (!m_actionMap || !m_input) return false;
            return m_actionMap->Active(name, [this](int k) { return m_input->IsKeyPressed(k); });
        });

        act.set_function("clear", [this](const std::string& name) {
            if (m_actionMap) m_actionMap->ClearAction(name);
        });
        act.set_function("clearAll", [this]() { if (m_actionMap) m_actionMap->Clear(); });
        act.set_function("count", [this](const std::string& name) {
            return m_actionMap ? static_cast<int>(m_actionMap->BindingCount(name)) : 0;
        });
        // 設定画面から呼ぶ。プロジェクト直下の input_bindings.json へ書く。
        act.set_function("save", [this]() { if (m_actionSaveCb) m_actionSaveCb(); });
    }

    // --- Camera ---
    lua.new_usertype<Camera>("Camera",
        "moveForward",  &Camera::MoveForward,
        "moveRight",    &Camera::MoveRight,
        "moveUp",       &Camera::MoveUp,
        "rotate",       &Camera::Rotate,
        "getPosition",  &Camera::GetPosition,
        "setPosition",  &Camera::SetPosition,
        "getYaw",       &Camera::GetYaw,
        "getPitch",     &Camera::GetPitch,
        "setYaw",       &Camera::SetYaw,
        "setPitch",     &Camera::SetPitch,
        "getMoveSpeed",  &Camera::GetMoveSpeed,
        "setMoveSpeed",  &Camera::SetMoveSpeed,
        "getMouseSensitivity", &Camera::GetMouseSensitivity,
        "setMouseSensitivity", &Camera::SetMouseSensitivity,
        // ワールド座標→正規化スクリーン座標(u,v∈[0,1], 左上原点)。戻り: u, v, visible
        // スクリプトの頭上ダメージ数値などが使う。w<=0(背面)や画面外は visible=false。
        "project", [](Camera& cam, f32 x, f32 y, f32 z) {
            using namespace DirectX;
            XMVECTOR clip = XMVector4Transform(XMVectorSet(x, y, z, 1.0f), cam.GetViewProjMatrix());
            f32 w = XMVectorGetW(clip);
            if (w <= 1e-5f) return std::make_tuple(0.0f, 0.0f, false);
            f32 u = XMVectorGetX(clip) / w * 0.5f + 0.5f;
            f32 v = 0.5f - XMVectorGetY(clip) / w * 0.5f;
            bool vis = (u > -0.2f && u < 1.2f && v > -0.2f && v < 1.2f);
            return std::make_tuple(u, v, vis);
        }
    );

    // --- Audio ---
    lua.new_usertype<AudioSystem>("AudioSystem",
        // メンバ関数ポインタ直バインドだと C++ 側のデフォルト引数(loop)が効かず
        // 1引数呼びでエラーになるので、sol::optional で loop を省略可にする
        "playBGM",         [](AudioSystem& a, const std::string& path, sol::optional<bool> loop) {
                               a.PlayBGM(path, loop.value_or(true));
                           },
        "stopBGM",         &AudioSystem::StopBGM,
        "pauseBGM",        &AudioSystem::PauseBGM,
        "resumeBGM",       &AudioSystem::ResumeBGM,
        "seekBGM",         &AudioSystem::SeekBGM,
        "setBGMRate",      &AudioSystem::SetBGMRate,
        "setListener",     &AudioSystem::SetListenerPos,
        "playSFX",         [](AudioSystem& a, const std::string& path, sol::optional<bool> loop) {
                               a.PlaySFX(path, loop.value_or(false));
                           },
        "playSpatial",     [](AudioSystem& a, const std::string& path, float x, float y, float z,
                              float minD, float maxD, sol::optional<float> vol, sol::optional<bool> loop) {
                               a.PlaySFXSpatial(path, x, y, z, minD, maxD,
                                                vol.value_or(1.0f), loop.value_or(false));
                           },
        "stopAllSFX",      &AudioSystem::StopAllSFX,
        "setMasterVolume",  &AudioSystem::SetMasterVolume,
        "setBGMVolume",     &AudioSystem::SetBGMVolume,
        "setSFXVolume",     &AudioSystem::SetSFXVolume,
        "getMasterVolume",  &AudioSystem::GetMasterVolume,
        "getBGMVolume",     &AudioSystem::GetBGMVolume,
        "getSFXVolume",     &AudioSystem::GetSFXVolume,
        // 「今なにが鳴っているか」。playBGM は同じパスでも頭出しするので、
        // シーンをまたいで同じ曲を流し続けたいときはこれで判定して呼ばない。
        "getCurrentBGM",    &AudioSystem::GetCurrentBGM,
        "isBGMPlaying",     &AudioSystem::IsBGMPlaying,
        "getBGMList",       &AudioSystem::GetBGMList,
        "getSFXList",       &AudioSystem::GetSFXList,
        "rescan",           &AudioSystem::ScanAudioFiles
    );

    // --- グローバル変数 ---
    lua["scene"]  = m_scene;
    lua["input"]  = m_input;
    lua["camera"] = m_camera;
    lua["audio"]  = m_audio;
    lua["ASSETS"] = m_assetsDir;

    // --- キーコード定数 ---
    lua["KEY_W"]     = static_cast<int>('W');
    lua["KEY_A"]     = static_cast<int>('A');
    lua["KEY_S"]     = static_cast<int>('S');
    lua["KEY_D"]     = static_cast<int>('D');
    lua["KEY_E"]     = static_cast<int>('E');
    lua["KEY_Q"]     = static_cast<int>('Q');
    lua["KEY_SPACE"] = static_cast<int>(VK_SPACE);
    lua["KEY_SHIFT"] = static_cast<int>(VK_SHIFT);
    lua["KEY_TAB"]   = static_cast<int>(VK_TAB);
    lua["KEY_ESCAPE"] = static_cast<int>(VK_ESCAPE);
    lua["KEY_F1"]    = static_cast<int>(VK_F1);
    lua["KEY_F2"]    = static_cast<int>(VK_F2);
    lua["KEY_F3"]    = static_cast<int>(VK_F3);
    lua["KEY_RBUTTON"] = static_cast<int>(VK_RBUTTON);
    lua["KEY_UP"]    = static_cast<int>(VK_UP);
    lua["KEY_DOWN"]  = static_cast<int>(VK_DOWN);
    lua["KEY_LEFT"]  = static_cast<int>(VK_LEFT);
    lua["KEY_RIGHT"] = static_cast<int>(VK_RIGHT);
    lua["KEY_ENTER"] = static_cast<int>(VK_RETURN);

    // 全英字 KEY_A..KEY_Z / 数字 KEY_0..KEY_9 を公開（Win32 VK は ASCII 大文字/数字と一致）。
    // これで Lua から任意のキーを KEY_X のように参照できる（W/A/S/D/E/Q は上書きだが同値）。
    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
        char name[8]; std::snprintf(name, sizeof(name), "KEY_%c", ch);
        lua[name] = static_cast<int>(ch);
    }
    for (char ch = '0'; ch <= '9'; ++ch)
    {
        char name[8]; std::snprintf(name, sizeof(name), "KEY_%c", ch);
        lua[name] = static_cast<int>(ch);
    }

    // --- ゲームパッドボタン定数（isPadButtonDown/Pressed/Released に渡す。値は XINPUT_GAMEPAD_* と同一）---
    lua["PAD_DPAD_UP"]    = static_cast<int>(XINPUT_GAMEPAD_DPAD_UP);
    lua["PAD_DPAD_DOWN"]  = static_cast<int>(XINPUT_GAMEPAD_DPAD_DOWN);
    lua["PAD_DPAD_LEFT"]  = static_cast<int>(XINPUT_GAMEPAD_DPAD_LEFT);
    lua["PAD_DPAD_RIGHT"] = static_cast<int>(XINPUT_GAMEPAD_DPAD_RIGHT);
    lua["PAD_START"]      = static_cast<int>(XINPUT_GAMEPAD_START);
    lua["PAD_BACK"]       = static_cast<int>(XINPUT_GAMEPAD_BACK);
    lua["PAD_LSTICK"]     = static_cast<int>(XINPUT_GAMEPAD_LEFT_THUMB);
    lua["PAD_RSTICK"]     = static_cast<int>(XINPUT_GAMEPAD_RIGHT_THUMB);
    lua["PAD_LB"]         = static_cast<int>(XINPUT_GAMEPAD_LEFT_SHOULDER);
    lua["PAD_RB"]         = static_cast<int>(XINPUT_GAMEPAD_RIGHT_SHOULDER);
    lua["PAD_A"]          = static_cast<int>(XINPUT_GAMEPAD_A);
    lua["PAD_B"]          = static_cast<int>(XINPUT_GAMEPAD_B);
    lua["PAD_X"]          = static_cast<int>(XINPUT_GAMEPAD_X);
    lua["PAD_Y"]          = static_cast<int>(XINPUT_GAMEPAD_Y);

    // --- ユーティリティ / デバッグログ（エディタのコンソールパネルに出る）---
    // 任意個・任意型の引数を tostring でつないで出す（Unity の Debug.Log 相当）。
    //   log("hp:", hp)  /  logWarn("弾切れ")  /  logError("想定外:", state)
    // print() も同じ経路へ差し替え（素の print は捕捉されずどこにも出ないため）。
    auto joinArgs = [](sol::variadic_args va, sol::this_state ts) -> std::string {
        sol::state_view L(ts);
        sol::protected_function tostr = L["tostring"];
        std::string line;
        for (auto v : va)
        {
            if (!line.empty()) line += "\t";
            auto r = tostr(v);
            line += r.valid() ? r.get<std::string>() : std::string("?");
        }
        return line;
    };
    lua["log"] = [joinArgs](sol::variadic_args va, sol::this_state ts) {
        Logger::Info("[Lua] {}", joinArgs(va, ts));
    };
    lua["logWarn"] = [joinArgs](sol::variadic_args va, sol::this_state ts) {
        Logger::Warn("[Lua] {}", joinArgs(va, ts));
    };
    lua["logError"] = [joinArgs](sol::variadic_args va, sol::this_state ts) {
        Logger::Error("[Lua] {}", joinArgs(va, ts));
    };
    lua["print"] = [joinArgs](sol::variadic_args va, sol::this_state ts) {
        Logger::Info("[Lua] {}", joinArgs(va, ts));
    };

    // --- シーンをまたいで残る数値ストレージ（スコア受け渡し等）---
    lua["saveNum"] = [this](const std::string& key, double v) { m_blackboard[key] = v; };
    lua["loadNum"] = [this](const std::string& key, sol::optional<double> def) -> double {
        auto it = m_blackboard.find(key);
        return (it != m_blackboard.end()) ? it->second : def.value_or(0.0);
    };

    // --- ディスク永続の数値ストア（settings.json。音量・映像設定などの保存用）---
    lua["savePersist"] = [this](const std::string& key, double v) {
        if (m_persistSaveCb) m_persistSaveCb(key, v);
    };
    lua["loadPersist"] = [this](const std::string& key, sol::optional<double> def) -> double {
        return m_persistLoadCb ? m_persistLoadCb(key, def.value_or(0.0)) : def.value_or(0.0);
    };

    // --- 映像設定（display:*。Application が注入したコールバック経由）---
    // set 系は即適用され settings.json に保存される。ゲームモード起動時に自動適用。
    {
        auto display = lua.create_named_table("display");
        display.set_function("setVSync", [this](sol::object, bool b) {
            if (m_displayCb.setVsync) m_displayCb.setVsync(b);
        });
        display.set_function("getVSync", [this](sol::object) -> bool {
            return m_displayCb.getVsync ? m_displayCb.getVsync() : false;
        });
        display.set_function("setFpsLimit", [this](sol::object, int v) {
            if (m_displayCb.setFpsLimit) m_displayCb.setFpsLimit(v);
        });
        display.set_function("getFpsLimit", [this](sol::object) -> int {
            return m_displayCb.getFpsLimit ? m_displayCb.getFpsLimit() : 0;
        });
        display.set_function("setWindowMode", [this](sol::object, const std::string& m) {
            if (m_displayCb.setWindowMode) m_displayCb.setWindowMode(m);
        });
        display.set_function("getWindowMode", [this](sol::object) -> std::string {
            return m_displayCb.getWindowMode ? m_displayCb.getWindowMode() : "windowed";
        });
        display.set_function("setResolution", [this](sol::object, int w, int h) {
            if (m_displayCb.setResolution) m_displayCb.setResolution(w, h);
        });
        display.set_function("getResolution", [this](sol::object) -> std::tuple<int, int> {
            int w = 0, h = 0;
            if (m_displayCb.getResolution) m_displayCb.getResolution(w, h);
            return { w, h };
        });
        display.set_function("getResolutions", [this](sol::object, sol::this_state ts) -> sol::table {
            sol::state_view sv(ts);
            sol::table list = sv.create_table();
            if (m_displayCb.getResolutions)
            {
                int i = 1;
                for (auto& [w, h] : m_displayCb.getResolutions())
                {
                    sol::table e = sv.create_table();
                    e["w"] = w;
                    e["h"] = h;
                    list[i++] = e;
                }
            }
            return list;
        });
    }

    // --- ゲーム制御（Application が注入したコールバック経由）---
    lua["loadScene"] = [this](const std::string& rel) { if (m_loadSceneCb) m_loadSceneCb(rel); };
    // シーンが参照するテクスチャ/モデルをキャッシュへ先読み(シーン切替時のカクつき対策)。
    // シーン自体は切り替えない。例: title の OnStart で preloadScene("scenes/stage_select.json")
    lua["preloadScene"] = [this](const std::string& rel) { if (m_preloadSceneCb) m_preloadSceneCb(rel); };
    lua["nextScene"] = [this]() { if (m_nextSceneCb) m_nextSceneCb(); };
    lua["quit"]      = [this]() { if (m_quitCb) m_quitCb(); };
    // フェード等のトランジション付きシーン切替（type: 0=Fade,1=Wipe,2=Circle,3=縦Wipe,4=シークバー早送り）
    lua["fadeToScene"] = [this](const std::string& rel, sol::optional<float> dur) {
        if (m_transitionCb) m_transitionCb(rel, 0, dur.value_or(0.6f));
    };
    lua["transitionToScene"] = [this](const std::string& rel, int type, sol::optional<float> dur) {
        if (m_transitionCb) m_transitionCb(rel, type, dur.value_or(0.6f));
    };
    // フォーカスナビ(矢印/D-pad + Enter/Space/A)へ初期フォーカスを与える。
    // Entity か数値 id を受ける。メニュー表示時に既定ボタンへ当ててパッド即操作可能にする用。
    lua["setUiFocus"] = [this](sol::object target) {
        if (!m_uiFocusCb) return;
        if (target.is<Entity>())
            m_uiFocusCb(static_cast<std::uint32_t>(target.as<Entity>().GetHandle()));
        else if (target.is<double>())
            m_uiFocusCb(static_cast<std::uint32_t>(target.as<double>()));
    };

    // --- ゲーム内 UI（即時モード）---
    // ui:text(x,y,text,[size,r,g,b,a]) / ui:button(x,y,w,h,label)->bool / ui:image(x,y,w,h,path)
    {
        auto ui = lua.create_named_table("ui");
        ui.set_function("text",
            [this](sol::object, float x, float y, const std::string& text,
                   sol::optional<float> size, sol::optional<float> r,
                   sol::optional<float> g, sol::optional<float> b, sol::optional<float> a)
            {
                if (m_uiTextCb)
                    m_uiTextCb(x, y, text, size.value_or(24.0f),
                               r.value_or(1.0f), g.value_or(1.0f), b.value_or(1.0f), a.value_or(1.0f));
            });
        ui.set_function("button",
            [this](sol::object, float x, float y, float w, float h, const std::string& label) -> bool
            {
                return m_uiButtonCb ? m_uiButtonCb(x, y, w, h, label) : false;
            });
        ui.set_function("image",
            [this](sol::object, float x, float y, float w, float h, const std::string& path)
            {
                if (m_uiImageCb) m_uiImageCb(x, y, w, h, path);
            });
        // ui:rect(x,y,w,h,[r,g,b,a,rounding]) 塗りつぶし矩形（バー/パネル背景に）
        ui.set_function("rect",
            [this](sol::object, float x, float y, float w, float h,
                   sol::optional<float> r, sol::optional<float> g, sol::optional<float> b,
                   sol::optional<float> a, sol::optional<float> rounding)
            {
                if (m_uiRectCb)
                    m_uiRectCb(x, y, w, h, r.value_or(1.0f), g.value_or(1.0f), b.value_or(1.0f),
                               a.value_or(1.0f), rounding.value_or(0.0f));
            });
    }

    // --- パーティクル / VFX（fx テーブル）---
    // fx:burst{...} 飛散, fx:ring{...} 衝撃波リング, fx:pulse(amt) 画面パルス, fx:clear()
    // テーブルのキー: x,y,z,count,spread,speed,speedVar,size,sizeEnd,life,lifeVar,
    //                r,g,b, rEnd,gEnd,bEnd, intensity,gravity,drag,up,dx,dy,dz
    {
        auto buildParams = [](sol::table t) -> ParticleSystem::EmitParams {
            ParticleSystem::EmitParams p;
            p.pos      = { t.get_or("x", 0.0f), t.get_or("y", 0.6f), t.get_or("z", 0.0f) };
            p.count    = t.get_or("count", 16);
            p.dir      = { t.get_or("dx", 0.0f), t.get_or("dy", 1.0f), t.get_or("dz", 0.0f) };
            p.spread   = t.get_or("spread", 1.0f);
            p.speed    = t.get_or("speed", 6.0f);
            p.speedVar = t.get_or("speedVar", 0.5f);
            p.size     = t.get_or("size", 0.4f);
            p.sizeEnd  = t.get_or("sizeEnd", 0.0f);
            p.life     = t.get_or("life", 0.6f);
            p.lifeVar  = t.get_or("lifeVar", 0.3f);
            p.color    = { t.get_or("r", 1.0f), t.get_or("g", 1.0f), t.get_or("b", 1.0f) };
            sol::optional<float> re = t["rEnd"], ge = t["gEnd"], be = t["bEnd"];
            if (re || ge || be) {
                p.hasColorEnd = true;
                p.colorEnd = { re.value_or(p.color.x), ge.value_or(p.color.y), be.value_or(p.color.z) };
            }
            p.intensity = t.get_or("intensity", 3.0f);
            p.gravity   = t.get_or("gravity", 0.0f);
            p.drag      = t.get_or("drag", 1.0f);
            p.up        = t.get_or("up", 0.0f);
            p.stretch      = t.get_or("stretch", 0.0f);       // 速度方向ストレッチ（火花/筋）
            p.turbStrength = t.get_or("turbStrength", 0.0f);  // カールノイズ乱流（煙/炎の有機的揺らぎ）
            p.turbFreq     = t.get_or("turbFreq", 1.0f);

            // 粒子の向き（任意・後方互換）: 数値 or 文字列 billboard(0)/horizontal(1)/vertical(2)
            {
                sol::object oo = t["orient"];
                if (oo.valid())
                {
                    if (oo.is<std::string>())
                    {
                        std::string s = oo.as<std::string>();
                        if      (s == "billboard")  p.orient = 0;
                        else if (s == "horizontal" || s == "ground") p.orient = 1;
                        else if (s == "vertical")   p.orient = 2;
                    }
                    else if (oo.is<double>())
                        p.orient = static_cast<int>(oo.as<double>());
                }
            }

            // --- プロシージャル質感(kind) / ブレンド / 3キー色 / 明滅（全て任意・後方互換）---
            //  kind は数値 or 文字列: glow/fire/smoke/spark/magic/electric(lightning)/ring(shockwave)/star(flare)
            int kind = 0;
            sol::object ko = t["kind"];
            if (ko.valid())
            {
                if (ko.is<std::string>())
                {
                    std::string s = ko.as<std::string>();
                    if      (s == "glow")     kind = 0;
                    else if (s == "fire")     kind = 1;
                    else if (s == "smoke")    kind = 2;
                    else if (s == "spark")    kind = 3;
                    else if (s == "magic")    kind = 4;
                    else if (s == "electric" || s == "lightning") kind = 5;
                    else if (s == "ring"     || s == "shockwave") kind = 6;
                    else if (s == "star"     || s == "flare")     kind = 7;
                }
                else if (ko.is<double>())
                {
                    kind = static_cast<int>(ko.as<double>());
                }
            }
            p.kind  = kind;
            p.blend = t.get_or("blend", kind == 2 ? 1 : 0);   // 煙は既定で α 前乗算（遮蔽）、他は加算
            sol::optional<float> rm = t["rMid"], gm = t["gMid"], bm = t["bMid"];
            if (rm || gm || bm)
            {
                p.hasColorMid = true;
                p.colorMid = { rm.value_or(p.color.x), gm.value_or(p.color.y), bm.value_or(p.color.z) };
            }
            p.flicker     = t.get_or("flicker", 0.0f);
            p.flickerFreq = t.get_or("flickerFreq", 18.0f);
            p.sizeMid     = t.get_or("sizeMid", -1.0f);     // >=0 で3キーサイズ（start→mid→end）
            p.distort     = t.get_or("distort", 0.0f);      // >0 で歪みパーティクル（熱ゆらぎ/衝撃波）
            p.light       = t.get_or("light", false);       // 明るい粒子上位をポイントライト化
            p.lightRange  = t.get_or("lightRange", 3.0f);
            // ★texture: パーティクルエディタも ParticleEmitter も texturePath を効かせるのに、
            //   fx:burst だけ読む口が無かった。エディタの「Lua コードをコピー」で貼っても
            //   画像が出ず、手で texture= を書き足しても無視される、という詰み方をしていた。
            p.texturePath = t.get_or("texture", std::string{});
            return p;
        };

        // onDeath = {…} で粒子の死亡位置に子バースト（サブエミッタ・1段のみ）。
        // 例: fx:burst{count=1, life=1.2, gravity=-9, onDeath={kind="spark", count=24, speed=5}}
        // gpu = true で GPUパーティクル（compute シム・最大 131072・加算専用）へルーティング。
        // 例: fx:burst{gpu=true, count=20000, kind="spark", speed=12, gravity=-9}
        auto emitWithChild = [this, buildParams](sol::table t, bool ring) {
            if (t.get_or("gpu", false) && m_gpuParticleSystem)
            {
                auto p = buildParams(t);
                GpuParticleSystem::EmitRequest r;
                r.pos    = p.pos;
                r.count  = static_cast<u32>((std::max)(p.count, 0));
                r.dir    = p.dir;
                r.spread = p.spread;
                r.col0   = { p.color.x * p.intensity, p.color.y * p.intensity, p.color.z * p.intensity };
                r.speed  = p.speed;
                const auto& ce = p.hasColorEnd ? p.colorEnd : p.color;
                r.col1   = { ce.x * p.intensity, ce.y * p.intensity, ce.z * p.intensity };
                r.speedVar = p.speedVar;
                r.size0 = p.size;    r.size1 = p.sizeEnd;
                r.life  = p.life;    r.lifeVar = p.lifeVar;
                r.gravity = p.gravity; r.drag = p.drag; r.up = p.up;
                r.turb  = p.turbStrength;
                r.kind  = p.kind;
                r.stretch = p.stretch;
                r.ring  = ring;   // ★これが無いと fx:ring{gpu=true} が球状バーストになる
                // GPU 経路は入れ子放出を持たない。黙って無視すると
                // 「gpu を外すと連鎖するのに付けると連鎖しない」になるので名指しで警告する。
                if (t["onDeath"].valid())
                {
                    static bool warned = false;
                    if (!warned) { warned = true;
                        Logger::Warn("fx: gpu=true では onDeath(入れ子放出)は使えません。無視します"
                                     "（gpu を外すか、死亡時に Lua 側で fx:burst してください）"); }
                }
                m_gpuParticleSystem->Emit(r);
                return;
            }
            if (!m_particleSystem) return;
            auto p = buildParams(t);
            p.ring = ring;
            sol::optional<sol::table> od = t["onDeath"];
            if (od)
            {
                auto child = buildParams(*od);
                child.ring = od->get_or("ring", false);
                m_particleSystem->Emit(p, &child);
            }
            else
            {
                m_particleSystem->Emit(p);
            }
        };

        auto fx = lua.create_named_table("fx");
        fx.set_function("burst", [emitWithChild](sol::object, sol::table t) {
            emitWithChild(t, false);
        });
        fx.set_function("ring", [emitWithChild](sol::object, sol::table t) {
            emitWithChild(t, true);
        });
        fx.set_function("pulse", [this](sol::object, sol::optional<float> amt) {
            if (m_particleSystem) m_particleSystem->AddPulse(amt.value_or(0.5f));
        });
        fx.set_function("clear", [this](sol::object) {
            if (m_particleSystem) m_particleSystem->Clear();
        });
        // fx:beam{ x0,y0,z0, x1,y1,z1, width, r,g,b, intensity, life, kind } 連続ビーム/火柱/稲妻
        fx.set_function("beam", [this](sol::object, sol::table t) {
            if (!m_particleSystem) return;
            ParticleSystem::BeamParams b;
            b.p0 = { t.get_or("x0", 0.0f), t.get_or("y0", 0.0f), t.get_or("z0", 0.0f) };
            b.p1 = { t.get_or("x1", 0.0f), t.get_or("y1", 1.0f), t.get_or("z1", 0.0f) };
            b.width     = t.get_or("width", 0.3f);
            b.color     = { t.get_or("r", 1.0f), t.get_or("g", 1.0f), t.get_or("b", 1.0f) };
            b.intensity = t.get_or("intensity", 6.0f);
            b.life      = t.get_or("life", 0.06f);
            int kind = 0;
            sol::object ko = t["kind"];
            if (ko.valid())
            {
                if (ko.is<std::string>())
                {
                    std::string s = ko.as<std::string>();
                    if      (s == "electric" || s == "lightning") kind = 1;
                    else if (s == "fire")                          kind = 2;
                    else                                           kind = 0;  // energy
                }
                else if (ko.is<double>()) kind = static_cast<int>(ko.as<double>());
            }
            b.kind = kind;
            m_particleSystem->EmitBeam(b);
        });
    }

    // --- time: 時間 API（'.' で呼ぶ）---
    // now/dt はタイムスケール適用済み。setScale(0)=ポーズ、0.5=スローモ、2=早送り。
    // スケールは OnUpdate に渡る dt 自体に掛かるので、既存スクリプトは無改修で追従する。
    // after/every/cancel タイマーは prelude(純Lua)側で time テーブルに追加される。
    {
        sol::table tm = lua.create_named_table("time");
        tm.set_function("now",      [this] { return m_timeElapsed; });
        tm.set_function("realtime", [this] { return m_timeUnscaled; });
        tm.set_function("dt",       [this] { return m_timeDt; });
        tm.set_function("realDt",   [this] { return m_timeUnscaledDt; });
        tm.set_function("frame",    [this] { return m_timeFrame; });
        tm.set_function("getScale", [this] { return m_timeScale; });
        tm.set_function("setScale", [this](float s) { m_timeScale = (s < 0.0f) ? 0.0f : s; });
    }

    // --- post / ssao: ポストプロセス・SSAO を文字列キーで読み書き（'.' で呼ぶ）---
    // 項目名は MCP の get_post_process / set_post_process と同一（名前表は
    // renderer/PostProcessSettings.h に 1 つだけ）。post.names() で一覧できる。
    // Play 中の変更は Stop でシーンJSONごと巻き戻る（EnterPlayMode のスナップショット）。
    {
        sol::table po = lua.create_named_table("post");
        po.set_function("get", [this](const std::string& name, sol::this_state ts) -> sol::object {
            sol::state_view sv(ts);
            if (!m_scene) return sol::make_object(sv, sol::lua_nil);
            return PostGetField(sv, m_scene->GetPostSettings(), name);
        });
        po.set_function("set", [this](const std::string& name, sol::object v) -> bool {
            if (!m_scene) return false;
            if (PostSetField(m_scene->GetPostSettings(), name, v)) return true;
            Logger::Warn("post.set: 不明または型違いの項目 \"{}\"（post.names() で一覧）", name);
            return false;
        });
        // まとめて設定: post.setMany{ bloomOn=true, bloom=0.8, vignetteOn=true }
        po.set_function("setMany", [this](sol::table t) -> int {
            if (!m_scene) return 0;
            auto& p = m_scene->GetPostSettings();
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
        so.set_function("get", [this](const std::string& name, sol::this_state ts) -> sol::object {
            sol::state_view sv(ts);
            if (!m_scene) return sol::make_object(sv, sol::lua_nil);
            return SsaoGetField(sv, m_scene->GetSSAOSettings(), name);
        });
        so.set_function("set", [this](const std::string& name, sol::object v) -> bool {
            if (!m_scene) return false;
            if (SsaoSetField(m_scene->GetSSAOSettings(), name, v)) return true;
            Logger::Warn("ssao.set: 不明または型違いの項目 \"{}\"（ssao.names() で一覧）", name);
            return false;
        });
        so.set_function("setMany", [this](sol::table t) -> int {
            if (!m_scene) return 0;
            auto& s = m_scene->GetSSAOSettings();
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

    RegisterPhysicsBindings();
    RegisterEventsBinding();
    RegisterNetworkBindings();

    Logger::Info("Lua bindings registered");
}

void ScriptEngine::RegisterPhysicsBindings()
{
    using namespace DirectX;
    auto& lua = *m_lua;

    // --- RaycastHit ---
    lua.new_usertype<RaycastHit>("RaycastHit",
        "hit",      &RaycastHit::hit,
        "distance", &RaycastHit::distance,
        "point",    &RaycastHit::point,
        "normal",   &RaycastHit::normal
    );

    // --- MotionType constants ---
    lua["MOTION_STATIC"]    = static_cast<int>(MotionType::Static);
    lua["MOTION_KINEMATIC"] = static_cast<int>(MotionType::Kinematic);
    lua["MOTION_DYNAMIC"]   = static_cast<int>(MotionType::Dynamic);

    // --- PhysicsSystem ---
    lua.new_usertype<PhysicsSystem>("PhysicsSystem",
        // メッシュ頂点から Convex Hull コライダーを自動生成（既にコライダーがあればスキップ）
        "autoCollider", [this](PhysicsSystem& /*ps*/, Entity& e) {
            auto& reg = m_scene->GetRegistry();
            // 既にいずれかのコライダーがあればスキップ（保存データを優先）
            if (reg.any_of<ConvexHullCollider, BoxCollider, SphereCollider, CapsuleCollider>(e.GetHandle()))
                return;
            auto* mr = reg.try_get<MeshRenderer>(e.GetHandle());
            auto* tf = reg.try_get<Transform>(e.GetHandle());
            if (!mr || !tf || mr->meshes.empty()) return;

            // 全メッシュの頂点を収集（スケール適用済み）
            std::vector<XMFLOAT3> allPoints;
            for (const auto* mesh : mr->meshes)
            {
                if (!mesh) continue;
                const auto& positions = mesh->GetPositions();
                for (const auto& p : positions)
                {
                    allPoints.push_back({
                        p.x * tf->scale.x,
                        p.y * tf->scale.y,
                        p.z * tf->scale.z
                    });
                }
            }

            // 頂点数を最大256に間引き（Jolt Convex Hull の上限）
            constexpr size_t kMaxPoints = 256;
            if (allPoints.size() > kMaxPoints)
            {
                size_t step = allPoints.size() / kMaxPoints;
                std::vector<XMFLOAT3> sampled;
                sampled.reserve(kMaxPoints);
                for (size_t i = 0; i < allPoints.size() && sampled.size() < kMaxPoints; i += step)
                    sampled.push_back(allPoints[i]);
                allPoints = std::move(sampled);
            }

            if (allPoints.empty()) return;

            // 頂点はモデル原点からの相対座標のまま（オフセットなし）
            // → ボディ位置 = Transform.position そのまま
            ConvexHullCollider col;
            col.points = std::move(allPoints);
            col.offset = { 0.0f, 0.0f, 0.0f };
            reg.emplace_or_replace<ConvexHullCollider>(e.GetHandle(), col);

            // BoxCollider があれば消す（ConvexHull が優先）
            reg.remove<BoxCollider>(e.GetHandle());
            reg.remove<SphereCollider>(e.GetHandle());
            reg.remove<CapsuleCollider>(e.GetHandle());
        },

        "addBoxCollider", [this](PhysicsSystem& /*ps*/, Entity& e,
                                 float hx, float hy, float hz) {
            auto& reg = m_scene->GetRegistry();
            if (reg.any_of<BoxCollider, ConvexHullCollider>(e.GetHandle())) return;
            BoxCollider col;
            col.halfExtents = { hx, hy, hz };
            reg.emplace_or_replace<BoxCollider>(e.GetHandle(), col);
        },
        "addSphereCollider", [this](PhysicsSystem& /*ps*/, Entity& e, float radius) {
            auto& reg = m_scene->GetRegistry();
            if (reg.any_of<SphereCollider, ConvexHullCollider>(e.GetHandle())) return;
            SphereCollider col;
            col.radius = radius;
            reg.emplace_or_replace<SphereCollider>(e.GetHandle(), col);
        },
        "addCapsuleCollider", [this](PhysicsSystem& /*ps*/, Entity& e,
                                     float radius, float halfHeight) {
            auto& reg = m_scene->GetRegistry();
            if (reg.any_of<CapsuleCollider, ConvexHullCollider>(e.GetHandle())) return;
            CapsuleCollider col;
            col.radius = radius;
            col.halfHeight = halfHeight;
            reg.emplace_or_replace<CapsuleCollider>(e.GetHandle(), col);
        },
        "addRigidBody", [this](PhysicsSystem& /*ps*/, Entity& e,
                               int motionTypeInt, float mass) {
            auto& reg = m_scene->GetRegistry();
            // 既に RigidBody があればスキップ（保存データ/エディタの設定を優先）
            if (reg.all_of<RigidBody>(e.GetHandle())) return;
            RigidBody rb;
            rb.motionType = static_cast<MotionType>(motionTypeInt);
            rb.mass = mass;
            reg.emplace_or_replace<RigidBody>(e.GetHandle(), rb);
        },
        "removeRigidBody", [this](PhysicsSystem& ps, Entity& e) {
            auto& reg = m_scene->GetRegistry();
            ps.UnregisterBody(reg, e.GetHandle());
            reg.remove<RigidBody>(e.GetHandle());
        },
        "applyForce", [](PhysicsSystem& ps, Entity& e, XMFLOAT3 force) {
            if (!e.HasComponent<RigidBody>()) return;
            ps.ApplyForce(e.GetComponent<RigidBody>().bodyId, force);
        },
        "applyImpulse", [](PhysicsSystem& ps, Entity& e, XMFLOAT3 impulse) {
            if (!e.HasComponent<RigidBody>()) return;
            ps.ApplyImpulse(e.GetComponent<RigidBody>().bodyId, impulse);
        },
        "setVelocity", [](PhysicsSystem& ps, Entity& e, XMFLOAT3 vel) {
            if (!e.HasComponent<RigidBody>()) return;
            ps.SetLinearVelocity(e.GetComponent<RigidBody>().bodyId, vel);
        },
        "getVelocity", [](PhysicsSystem& ps, Entity& e) -> XMFLOAT3 {
            if (!e.HasComponent<RigidBody>()) return {};
            return ps.GetLinearVelocity(e.GetComponent<RigidBody>().bodyId);
        },
        // setPosition(entity, pos) — RigidBody は body を、CharacterController は
        // CharacterVirtual をテレポートする。CC は Transform を書いても
        // SyncCharactersToTransforms に上書きされて戻るため、こちらを通す必要がある
        // （リスポーン/チェックポイント復帰で必須）。
        "setPosition", [](PhysicsSystem& ps, Entity& e, XMFLOAT3 pos) {
            if (e.HasComponent<RigidBody>())
            {
                ps.SetPosition(e.GetComponent<RigidBody>().bodyId, pos);
                return;
            }
            if (e.HasComponent<CharacterController>())
            {
                // テレポートなので落下速度と移動入力も捨てる（復帰直後に落下継続しない）
                auto& cc = e.GetComponent<CharacterController>();
                cc._verticalVel = 0.0f;
                cc._desiredVel  = {0.0f, 0.0f, 0.0f};
                ps.SetCharacterPosition(e.GetHandle(), pos);
            }
        },
        "raycast", [](PhysicsSystem& ps, XMFLOAT3 origin, XMFLOAT3 dir,
                       float maxDist) -> RaycastHit {
            // normal は本物の面法線（旧実装は (0,1,0) 固定のフェイクだった）。
            return ps.Raycast(origin, dir, maxDist);
        },

        // overlapBox(center, halfExtents, maxResults=32) -> { Entity, ... }
        "overlapBox", [this](PhysicsSystem& ps, XMFLOAT3 center, XMFLOAT3 half,
                             sol::optional<int> maxN) -> sol::table {
            // 負値/0 を size_t にキャストすると SIZE_MAX になり vector 確保でクラッシュするのでクランプ。
            const int rawCap = maxN.value_or(32);
            const size_t cap = rawCap > 0 ? static_cast<size_t>(rawCap) : 32;
            std::vector<entt::entity> buf(cap);
            size_t n = ps.OverlapBox(center, half, buf.data(), cap);
            sol::table t = m_lua->create_table();
            auto& reg = m_scene->GetRegistry();
            for (size_t i = 0; i < n; ++i)
                t[static_cast<int>(i) + 1] = Entity(buf[i], &reg);
            return t;
        },
        // overlapSphere(center, radius, maxResults=32) -> { Entity, ... }
        "overlapSphere", [this](PhysicsSystem& ps, XMFLOAT3 center, float radius,
                                sol::optional<int> maxN) -> sol::table {
            // 負値/0 を size_t にキャストすると SIZE_MAX になり vector 確保でクラッシュするのでクランプ。
            const int rawCap = maxN.value_or(32);
            const size_t cap = rawCap > 0 ? static_cast<size_t>(rawCap) : 32;
            std::vector<entt::entity> buf(cap);
            size_t n = ps.OverlapSphere(center, radius, buf.data(), cap);
            sol::table t = m_lua->create_table();
            auto& reg = m_scene->GetRegistry();
            for (size_t i = 0; i < n; ++i)
                t[static_cast<int>(i) + 1] = Entity(buf[i], &reg);
            return t;
        },
        // setPaused(bool) — 物理タイムステップを止める/再開する
        "setPaused", [](PhysicsSystem& ps, bool p) { ps.SetPaused(p); },
        // step(dt) — 手動で 1 ステップ進める（pause 中の駒送り用）
        "step", [](PhysicsSystem& ps, float dt) { ps.Step(dt); },
        // setGravity(vec3)
        "setGravity", [](PhysicsSystem& ps, XMFLOAT3 g) { ps.SetGravity(g); },

        // --- CharacterController（Jolt CharacterVirtual）---
        // CharacterController を付与（既にあればスキップ）。data 駆動と同じく既存優先。
        "addCharacterController", [this](PhysicsSystem& /*ps*/, Entity& e,
                                         float radius, float halfHeight) {
            auto& reg = m_scene->GetRegistry();
            if (reg.all_of<CharacterController>(e.GetHandle())) return;
            if (reg.all_of<RigidBody>(e.GetHandle())) return; // RigidBody と排他
            CharacterController cc;
            cc.radius = radius; cc.halfHeight = halfHeight;
            reg.emplace_or_replace<CharacterController>(e.GetHandle(), cc);
        },
        // 水平移動入力（world XZ の目標速度）。毎フレーム呼ぶ。呼ばないフレームは停止。
        "move", [](PhysicsSystem& /*ps*/, Entity& e, float vx, float vz) {
            if (!e.HasComponent<CharacterController>()) return;
            auto& cc = e.GetComponent<CharacterController>();
            cc._desiredVel.x = vx; cc._desiredVel.z = vz;
        },
        // ジャンプ要求（接地中のみ有効。amount<=0 なら既定 jumpSpeed）
        "jump", [](PhysicsSystem& /*ps*/, Entity& e, sol::optional<float> amount) {
            if (!e.HasComponent<CharacterController>()) return;
            auto& cc = e.GetComponent<CharacterController>();
            // ★以前は cc.jumpSpeed を直接書き換えていたので、チャージジャンプ等で
            //   一度 physics:jump(e, 3) を通すと、以降の引数なし physics:jump(e) が
            //   既定値ではなく 3 になった（コンポーネントの設定値が勝手に変わる）。
            //   今回ぶんだけの上書きにする。
            cc._jumpOverride = (amount && *amount > 0.0f) ? *amount : -1.0f;
            cc._jumpQueued = true;
        },
        "isGrounded", [](PhysicsSystem& /*ps*/, Entity& e) -> bool {
            if (!e.HasComponent<CharacterController>()) return false;
            return e.GetComponent<CharacterController>()._grounded;
        }
    );

    lua["physics"] = m_physics;
}

// events グローバルを C++ EventBus への薄いバインドとして登録する。
//   events:on(name, fn)     → EventBus::On で購読。fn は EngineEvent を Lua テーブルへ
//                              変換した data を 1 引数で受け取る（旧 events:emit(name, data) 互換）。
//                              戻り値は購読ID。events:off(id) で個別解除に使う。
//   events:off(id)          → EventBus::Off。on が返したIDの購読だけを解除（clear と違い全消去しない）。
//   events:emit(name, data) → EventBus::Emit で即時発火。data は { key=val,... }（num/str/bool）。
//   events:clear()          → EventBus::Clear。
// m_eventBus は実行時に参照する（バインド登録時は null でも安全。WireScriptCallbacks 後に有効化）。
// ラムダは this をキャプチャ。ScriptEngine は lua_State より長命なので lifetime 安全。
void ScriptEngine::RegisterEventsBinding()
{
    auto& lua = *m_lua;

    sol::table ev = lua.create_named_table("events");

    // EngineEvent → Lua table 変換（source/other と data の各キー値を詰める）。
    auto evToTable = [](sol::state_view L, const EngineEvent& e) -> sol::table {
        sol::table t = L.create_table();
        for (const auto& kv : e.data)
            std::visit([&](const auto& val) { t.set(kv.key, val); }, kv.val);
        if (e.source != entt::null)
            t["source"] = static_cast<std::uint32_t>(entt::to_integral(e.source));
        if (e.other != entt::null)
            t["other"] = static_cast<std::uint32_t>(entt::to_integral(e.other));
        return t;
    };

    // on(name, fn) → 購読IDを返す。events:off(id) で個別解除できる（0 は無効ID）。
    ev.set_function("on", [this, evToTable](sol::object /*self*/,
                                            const std::string& name,
                                            sol::function fn) -> std::uint32_t {
        if (!m_eventBus)
        {
            // EventBus は Play 中のみ有効（WireScriptCallbacks 後に注入される）。
            // エディタ停止中に events:on を呼んでも購読は登録されない。
            // OnStart() 内で登録すること。この警告は実行中に1回だけ出る。
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                Logger::Warn("events:on を呼びましたが EventBus が利用できません"
                             "（events:on/emit は Playing 中のみ有効。OnStart() 内で登録してください）");
            }
            return 0;
        }
        sol::main_function mfn = fn;   // GC 寿命を確実に保持
        return m_eventBus->On(name, [this, mfn, evToTable](const EngineEvent& e) mutable {
            sol::protected_function pf = mfn;
            if (!pf.valid()) return;
            auto r = pf(evToTable(sol::state_view(*m_lua), e));
            if (!r.valid())
            {
                sol::error err = r;
                Logger::Warn("イベントハンドラでエラー（{}）: {}", e.name, err.what());
            }
        });
    });

    // off(id) → on が返した購読IDを個別解除（clear と違い全消去しない）。
    ev.set_function("off", [this](sol::object /*self*/, std::uint32_t id) {
        if (m_eventBus) m_eventBus->Off(id);
    });

    ev.set_function("emit", [this](sol::object /*self*/, const std::string& name,
                                   sol::optional<sol::table> data) {
        if (!m_eventBus) return;
        EngineEvent e;
        e.name = name;
        if (data.has_value())
        {
            for (const auto& kv : *data)
            {
                if (!kv.first.is<std::string>()) continue;
                std::string key = kv.first.as<std::string>();
                const sol::object& v = kv.second;
                if      (v.is<bool>())        e.set(key, v.as<bool>());
                else if (v.is<double>())      e.set(key, v.as<double>());
                else if (v.is<std::string>()) e.set(key, v.as<std::string>());
            }
        }
        m_eventBus->Emit(e);
    });

    ev.set_function("clear", [this](sol::object /*self*/) {
        if (m_eventBus) m_eventBus->Clear();
    });
}

void ScriptEngine::RegisterNetworkBindings()
{
    auto& lua = *m_lua;
    sol::table net = lua.create_named_table("net");

    // port/ip 省略時は NetworkConfig の既定値を使う。net が未注入(エディタ等)ならエラー文字列を返す。
    net.set_function("host", [this](sol::object /*self*/, sol::optional<int> port) -> std::string {
        if (!m_network) return "network system unavailable";
        u16 p = port.has_value() ? static_cast<u16>(*port) : m_network->Config().defaultPort;
        std::string err;
        if (!m_network->Host(p, m_network->Config().maxPlayers, err)) return err;
        return "";
    });

    net.set_function("join", [this](sol::object /*self*/, const std::string& ip,
                                     sol::optional<int> port) -> std::string {
        if (!m_network) return "network system unavailable";
        u16 p = port.has_value() ? static_cast<u16>(*port) : m_network->Config().defaultPort;
        std::string err;
        if (!m_network->Join(ip, p, err)) return err;
        return "";
    });

    net.set_function("disconnect", [this](sol::object /*self*/) {
        if (m_network) m_network->Disconnect();
    });

    net.set_function("isServer",    [this](sol::object /*self*/) { return m_network && m_network->IsServer(); });
    net.set_function("isClient",    [this](sol::object /*self*/) { return m_network && m_network->IsClient(); });
    net.set_function("isConnected", [this](sol::object /*self*/) { return m_network && m_network->IsConnected(); });
    net.set_function("localClientId", [this](sol::object /*self*/) -> int {
        return m_network ? static_cast<int>(m_network->LocalClientId()) : 0;
    });

    net.set_function("players", [this](sol::object /*self*/) -> sol::table {
        sol::table out = m_lua->create_table();
        if (!m_network) return out;
        int i = 1;
        for (const auto& p : m_network->Players())
        {
            sol::table row = m_lua->create_table();
            row["id"]  = static_cast<int>(p.id);
            row["rtt"] = static_cast<int>(p.rttMs);
            row["bytesSent"] = static_cast<double>(p.bytesSent);
            row["bytesReceived"] = static_cast<double>(p.bytesReceived);
            out[i++] = row;
        }
        return out;
    });

    // クライアント専用。毎フレーム呼ぶ想定(呼ばなかったフレームは前回値が送られ続ける)。
    // t = { moveX=, moveZ=, aimYaw=, aimPitch=, buttons=, jump= }(全省略可、既定0/false)。
    net.set_function("setInput", [this](sol::object /*self*/, sol::table t) {
        if (!m_network) return;
        NetworkSystem::InputCommand cmd;
        cmd.moveVelX  = t.get_or("moveX", 0.0f);
        cmd.moveVelZ  = t.get_or("moveZ", 0.0f);
        cmd.aimYaw    = t.get_or("aimYaw", 0.0f);
        cmd.aimPitch  = t.get_or("aimPitch", 0.0f);
        cmd.buttons   = static_cast<u32>(t.get_or("buttons", 0));
        cmd.jump      = t.get_or("jump", false);
        m_network->SetLocalInput(cmd);
    });

    // サーバー専用。entity の NetworkIdentity._owner の最新入力を返す
    // (owner未接続/入力未受信なら全部ゼロのテーブル)。
    net.set_function("getInput", [this](sol::object /*self*/, Entity& e) -> sol::table {
        sol::table out = m_lua->create_table();
        out["moveX"] = 0.0f; out["moveZ"] = 0.0f;
        out["aimYaw"] = 0.0f; out["aimPitch"] = 0.0f;
        out["buttons"] = 0; out["jump"] = false;
        if (!m_network || !e.IsValid() || !e.HasComponent<NetworkIdentity>()) return out;

        const auto cmd = m_network->GetLatestInput(e.GetComponent<NetworkIdentity>()._owner);
        out["moveX"] = cmd.moveVelX;   out["moveZ"] = cmd.moveVelZ;
        out["aimYaw"] = cmd.aimYaw;    out["aimPitch"] = cmd.aimPitch;
        out["buttons"] = static_cast<int>(cmd.buttons);
        out["jump"] = cmd.jump;
        return out;
    });

    // サーバー専用。実際の生成はフレーム境界(InstantiatePrefabがcmdListを要るため)。
    // 戻り値: netId(int, 失敗時0), err(string)。生成完了は net.spawned イベントで分かる。
    net.set_function("spawn", [this](sol::object /*self*/, const std::string& prefabPath,
                                       f32 x, f32 y, f32 z, sol::optional<int> owner) -> std::tuple<int, std::string> {
        if (!m_network) return { 0, "network system unavailable" };
        const ClientId ownerId = owner.has_value() ? static_cast<ClientId>(*owner) : kServerClientId;
        std::string err;
        NetId id = m_network->RequestSpawn(prefabPath, x, y, z, ownerId, err);
        return { static_cast<int>(id), err };
    });

    // サーバー専用。破棄は即時(cmdList不要)。
    net.set_function("despawn", [this](sol::object /*self*/, Entity& e) -> std::string {
        if (!m_network) return "network system unavailable";
        if (!e.IsValid() || !e.HasComponent<NetworkIdentity>())
            return "entity has no NetworkIdentity";
        const NetId id = e.GetComponent<NetworkIdentity>()._netId;
        std::string err;
        m_network->RequestDespawn(id, m_scene->GetRegistry(), err);
        return err;
    });

    // netId からエンティティを引く(スポーン完了後のnet.spawnedハンドラ等から使う想定)。
    // 見つからなければ IsValid()==false の Entity を返す。
    net.set_function("findByNetId", [this](sol::object /*self*/, int netId) -> Entity {
        entt::entity e = m_network ? m_network->FindEntityByNetId(static_cast<NetId>(netId)) : entt::null;
        return Entity(e, &m_scene->GetRegistry());
    });

    // RPC: 引数は number/string/boolean/Vec3 のみ対応(テーブルや関数は不可)。
    auto toRpcArgs = [](sol::variadic_args va) -> RpcArgs {
        RpcArgs args;
        for (auto v : va)
        {
            sol::object o = v;
            if      (!o.valid() || o.is<sol::nil_t>()) args.push_back(RpcValue{});
            else if (o.is<bool>())                     args.push_back(RpcValue::MakeBool(o.as<bool>()));
            else if (o.is<double>())                   args.push_back(RpcValue::MakeNumber(o.as<double>()));
            else if (o.is<DirectX::XMFLOAT3>())         args.push_back(RpcValue::MakeVec3(o.as<DirectX::XMFLOAT3>()));
            else if (o.is<std::string>())               args.push_back(RpcValue::MakeString(o.as<std::string>()));
            else args.push_back(RpcValue{});   // 未対応型はnil扱い(警告なし、シンプルさ優先)
        }
        return args;
    };
    auto fromRpcArgs = [](sol::state& lua, const RpcArgs& args) -> std::vector<sol::object> {
        std::vector<sol::object> out;
        out.reserve(args.size());
        for (const auto& a : args)
        {
            switch (a.type)
            {
            case RpcValue::Type::Bool:   out.push_back(sol::make_object(lua, a.b)); break;
            case RpcValue::Type::Number: out.push_back(sol::make_object(lua, a.num)); break;
            case RpcValue::Type::String: out.push_back(sol::make_object(lua, a.str)); break;
            case RpcValue::Type::Vec3:   out.push_back(sol::make_object(lua, a.vec)); break;
            case RpcValue::Type::Nil:
            default:                     out.push_back(sol::make_object(lua, sol::nil)); break;
            }
        }
        return out;
    };

    net.set_function("rpc", [this, toRpcArgs](sol::object /*self*/, const std::string& name,
                                                sol::variadic_args va) -> std::string {
        if (!m_network) return "network system unavailable";
        std::string err;
        m_network->SendRpcToServer(name, toRpcArgs(va), err);
        return err;
    });

    net.set_function("rpcAll", [this, toRpcArgs](sol::object /*self*/, const std::string& name,
                                                   sol::variadic_args va) -> std::string {
        if (!m_network) return "network system unavailable";
        std::string err;
        m_network->SendRpcToAll(name, toRpcArgs(va), err);
        return err;
    });

    net.set_function("rpcClient", [this, toRpcArgs](sol::object /*self*/, int clientId, const std::string& name,
                                                      sol::variadic_args va) -> std::string {
        if (!m_network) return "network system unavailable";
        std::string err;
        m_network->SendRpcToClient(static_cast<ClientId>(clientId), name, toRpcArgs(va), err);
        return err;
    });

    net.set_function("onRpc", [this, fromRpcArgs](sol::object /*self*/, const std::string& name, sol::function fn) {
        if (!m_network) return;
        sol::main_function mfn = fn;   // GC 寿命を確実に保持(events:on と同じ流儀)
        m_network->SetRpcHandler(name, [this, mfn, fromRpcArgs, name](ClientId sender, const RpcArgs& args) mutable {
            sol::protected_function pf = mfn;
            if (!pf.valid()) return;
            std::vector<sol::object> luaArgs;
            luaArgs.push_back(sol::make_object(*m_lua, static_cast<int>(sender)));
            for (auto& a : fromRpcArgs(*m_lua, args)) luaArgs.push_back(a);
            auto r = pf(sol::as_args(luaArgs));
            if (!r.valid())
            {
                sol::error err = r;
                Logger::Warn("RPCハンドラでエラー（{}）: {}", name, err.what());
            }
        });
    });
}

void ScriptEngine::LoadPrelude()
{
    // 高レベルゲームスクリプトAPI（プランナー/AI 向け）。
    // ここで定義した関数/テーブルはグローバルなので、各アタッチスクリプトの
    // 環境フォールバック(lua.globals())経由で参照できる。
    // 詳細リファレンス: docs/SCRIPTING.md
    static const char* kPrelude = R"LUA(
-- ============================================================
--  DX12 Engine - High-level scripting helpers (auto-loaded)
--  低レベルAPI(scene/input/transform...)を包んだ簡単API。
-- ============================================================

-- キー名 -> エンジンのキー定数
local KEYS = {
  W=KEY_W, A=KEY_A, S=KEY_S, D=KEY_D, E=KEY_E, Q=KEY_Q,
  UP=KEY_UP, DOWN=KEY_DOWN, LEFT=KEY_LEFT, RIGHT=KEY_RIGHT,
  SPACE=KEY_SPACE, SHIFT=KEY_SHIFT, TAB=KEY_TAB,
  ENTER=KEY_ENTER, ESC=KEY_ESCAPE, ESCAPE=KEY_ESCAPE,
}
function keyDown(name)    local k = KEYS[name]; return k ~= nil and input:isKeyDown(k)    end
function keyPressed(name) local k = KEYS[name]; return k ~= nil and input:isKeyPressed(k) end

-- ===== gamepad: Xbox コントローラー簡易API（pad 省略時は 0 = 1台目）=====
-- padDown("A")/padPressed("RB")/padReleased("LB") はボタン名文字列で判定。
-- padStick("left"|"right", pad?) はスティックの (x, y) を2値で返す（デッドゾーン適用済み）。
-- padTrigger("left"|"right", pad?) は 0..1 のアナログ値。
-- padVibrate(low, high, seconds?, pad?) は振動（low=強モーター/high=弱モーター、共に0..1）。
--   seconds を渡すとその秒数だけ鳴って自動停止、省略時は手動で padVibrate(0,0) するまで鳴り続ける。
local PAD_BUTTONS = {
  A=PAD_A, B=PAD_B, X=PAD_X, Y=PAD_Y,
  LB=PAD_LB, RB=PAD_RB, BACK=PAD_BACK, START=PAD_START,
  LSTICK=PAD_LSTICK, RSTICK=PAD_RSTICK,
  DPAD_UP=PAD_DPAD_UP, DPAD_DOWN=PAD_DPAD_DOWN, DPAD_LEFT=PAD_DPAD_LEFT, DPAD_RIGHT=PAD_DPAD_RIGHT,
}
function padConnected(pad) return input:isPadConnected(pad or 0) end
function padDown(name, pad)     local b = PAD_BUTTONS[name]; return b ~= nil and input:isPadButtonDown(pad or 0, b) end
function padPressed(name, pad)  local b = PAD_BUTTONS[name]; return b ~= nil and input:isPadButtonPressed(pad or 0, b) end
function padReleased(name, pad) local b = PAD_BUTTONS[name]; return b ~= nil and input:isPadButtonReleased(pad or 0, b) end
function padStick(side, pad)
  pad = pad or 0
  if side == "right" then return input:getPadRightStickX(pad), input:getPadRightStickY(pad) end
  return input:getPadLeftStickX(pad), input:getPadLeftStickY(pad)
end
function padTrigger(side, pad)
  pad = pad or 0
  if side == "right" then return input:getPadRightTrigger(pad) end
  return input:getPadLeftTrigger(pad)
end
function padVibrate(low, high, seconds, pad)
  pad = pad or 0
  if seconds then input:setPadVibrationTimed(pad, low, high, seconds)
  else input:setPadVibration(pad, low, high) end
end
)LUA" R"LUA(
-- ===== events: 疎結合のイベントバス =====
-- どのコンポーネントからでも events:on("name", fn) で購読、events:emit("name", data) で発火。
-- events:on は購読IDを返す。個別解除は events:off(id)、全消去は events:clear()。
-- Trigger の EmitEvent アクション（C++ 側）もこの emit を呼ぶ。Play 開始時に clear される。
-- 実体は C++ EventBus への薄いバインド（RegisterEventsBinding で登録済み）。
-- 旧・純 Lua 実装（events = { _h = {} } ...）はここから削除した。

-- ===== time: タイマー（C++ 側 time.now/dt/setScale 等に追加する純 Lua 部分）=====
-- time.after(sec, fn) -> id   sec 秒後に fn を1回呼ぶ
-- time.every(sec, fn) -> id   sec 秒ごとに fn を繰り返し呼ぶ
-- time.cancel(id)             どちらも解除
-- タイマーはスケール済み時間で進む(setScale(0) 中は止まる)。Play 開始でクリア。
time._timers, time._nextId = {}, 1
function time.after(sec, fn)
  local id = time._nextId; time._nextId = id + 1
  time._timers[id] = { left = sec, fn = fn }
  return id
end
function time.every(sec, fn)
  local id = time._nextId; time._nextId = id + 1
  time._timers[id] = { left = sec, interval = sec, fn = fn }
  return id
end
function time.cancel(id) time._timers[id] = nil end

-- エンティティキー解決: self テーブル(.name) / 名前文字列 / 数値id を受け付ける。
-- キーは名前を優先(イベントで target 名を渡す既存の流儀と噛み合う)。同名エンティティは同一時計になる点に注意。
local function _timeKey(e)
  if type(e) == "table" then return e.name or tostring(e.entity) end
  return e
end

-- ===== time.video: 共有ビデオ時計 =====
-- ステージ全体に1本流れる"動画時間"。ギミックは video.localTime(self) を t にして動きを t の
-- 純関数で書く(決定論タイムライン)。矢が刺さったら video.skip(target, ±sec) でその対象の
-- オフセットだけを動かす = 先送り/巻き戻し。skip は残り時間も自動で消費する(skipCost 倍率、
-- 「先送りすると制限時間が減る」ルールの実装。0 でコスト無し)。
time.video = { _active = false, _t = 0, _dur = 0, _consumed = 0, _skipCost = 1.0, _offsets = {} }
function time.video.start(duration, opts)
  local v = time.video
  v._active, v._t, v._dur, v._consumed = true, 0, duration or 0, 0
  v._skipCost = (opts and opts.skipCost) or 1.0
  v._offsets = {}
end
function time.video.stop() time.video._active = false end
function time.video.active() return time.video._active end
function time.video.now() return time.video._t end
function time.video.duration() return time.video._dur end
function time.video.remaining()
  local v = time.video
  if not v._active then return math.huge end
  local r = v._dur - v._t - v._consumed
  return r > 0 and r or 0
end
function time.video.finished() return time.video._active and time.video.remaining() <= 0 end
function time.video.setOffset(e, off) time.video._offsets[_timeKey(e)] = off end
function time.video.getOffset(e) return time.video._offsets[_timeKey(e)] or 0 end
function time.video.skip(e, amount)
  local v = time.video
  local k = _timeKey(e)
  v._offsets[k] = (v._offsets[k] or 0) + amount
  v._consumed = v._consumed + math.abs(amount) * v._skipCost
  return v._offsets[k]
end
function time.video.localTime(e) return time.video._t + time.video.getOffset(e) end

-- ===== 個別時計: エンティティ単位の独立クロック =====
-- 共有ビデオ時計と無関係に、オブジェクトごとに進む・止まる・スキップできる時計。
-- 初アクセスで t=0 から開始。scaleEntity(e, 0)=そのオブジェクトだけ停止、負値=逆再生。
time._ent = {}
local function _entClock(e)
  local k = _timeKey(e)
  local c = time._ent[k]
  if not c then c = { t = 0, scale = 1 }; time._ent[k] = c end
  return c
end
function time.localTime(e) return _entClock(e).t end
function time.skipEntity(e, amount) local c = _entClock(e); c.t = c.t + amount; return c.t end
function time.scaleEntity(e, s) _entClock(e).scale = s end
function time.getEntityScale(e) return _entClock(e).scale end
function time.resetEntity(e) time._ent[_timeKey(e)] = nil end

-- ===== charge: 押しっぱなしチャージ計測（弓を引く等）=====
-- local c = charge.new("E", { max = 2.0, rate = 1.0 })
-- OnUpdate で c:update() を呼ぶ。c:charging()/c:ratio() でゲージ表示、
-- 離した瞬間 c:released() がチャージ量を返す(それ以外は nil)。
charge = {}
charge.__index = charge
function charge.new(key, opts)
  opts = opts or {}
  return setmetatable({
    key = key, max = opts.max or 2.0, rate = opts.rate or 1.0,
    realtime = opts.realtime or false,   -- true: ポーズ中も実時間で溜まる
    v = 0, _charging = false, _released = false,
  }, charge)
end
function charge:update()
  local dt = self.realtime and time.realDt() or time.dt()
  self._released = false
  if keyDown(self.key) then
    self._charging = true
    self.v = math.min(self.v + dt * self.rate, self.max)
  elseif self._charging then
    self._charging = false
    self._released = true
  end
end
function charge:charging() return self._charging end
function charge:value() return self.v end
function charge:ratio() return self.max > 0 and (self.v / self.max) or 0 end
function charge:released()
  if not self._released then return nil end
  self._released = false
  local v = self.v
  self.v = 0
  return v
end

-- ===== task: コルーチンで「待てる」処理を書く =====
-- 使い方:
--   task.spawn(function()
--     door:open(); wait(1.5)
--     say("誰かいる…"); waitUntil(function() return player.inRoom end)
--     lightsOut()
--   end)
-- time.after のコールバック地獄を置き換えるためのもの。Play 開始でリセットされる。
task = {}
task._list, task._nextId = {}, 1

-- 1 タスクを 1 回だけ進める。戻り値: まだ生きていれば true。
function task._step(id, t)
  local ok, res
  if not t.started then
    t.started = true
    ok, res = coroutine.resume(t.co, table.unpack(t.args, 1, t.args.n))
  else
    ok, res = coroutine.resume(t.co)
  end
  if not ok then
    print("task error: " .. tostring(res))
    task._list[id] = nil
    return false
  end
  if coroutine.status(t.co) == "dead" then
    task._list[id] = nil
    return false
  end
  -- wait/waitFrames/waitUntil が yield したもの。それ以外の yield は「次フレームまで待つ」
  if type(res) == "table" then t.wt, t.wf, t.wp = res.t, res.f, res.p
  else t.wt, t.wf, t.wp = nil, nil, nil end
  return true
end

-- fn をコルーチンとして開始する。戻り値は cancel 用の id。
-- ★最初の wait までは「その場で」走る（Unity の StartCoroutine と同じ）。
--   次フレームまで待たせると task.spawn(function() door:open() ... end) が
--   1 フレーム遅れて開く、という直感に反する挙動になるため。
function task.spawn(fn, ...)
  local id = task._nextId
  task._nextId = id + 1
  local t = { co = coroutine.create(fn), args = table.pack(...), started = false }
  task._list[id] = t
  task._step(id, t)
  return id
end
function task.cancel(id) task._list[id] = nil end
function task.alive(id) return task._list[id] ~= nil end
function task.count() local n = 0; for _ in pairs(task._list) do n = n + 1 end; return n end
function task.cancelAll() task._list = {} end

-- ↓ この3つは task.spawn の中（＝コルーチン内）からだけ呼べる。
function wait(sec)        coroutine.yield({ t = sec or 0 }) end
function waitFrames(n)    coroutine.yield({ f = n or 1 }) end
function waitUntil(pred)  coroutine.yield({ p = pred }) end

function task._tick(dt)
  -- スナップショットしてから回す（コルーチン内で spawn/cancel されても安全）
  local ids = {}
  for id in pairs(task._list) do ids[#ids + 1] = id end
  for _, id in ipairs(ids) do
    local t = task._list[id]
    if t then
      local ready
      if t.wt then t.wt = t.wt - dt; ready = (t.wt <= 0)
      elseif t.wf then t.wf = t.wf - 1; ready = (t.wf <= 0)
      elseif t.wp then local ok, v = pcall(t.wp); ready = (ok and v) and true or false
      else ready = true end

      if ready then
        t.wt, t.wf, t.wp = nil, nil, nil
        task._step(id, t)
      end
    end
  end
end

function __time_reset()
  task._list, task._nextId = {}, 1
  time._timers, time._nextId = {}, 1
  -- time.video はメソッドも入っているテーブルなので丸ごと差し替えず状態フィールドだけ戻す
  local v = time.video
  v._active, v._t, v._dur, v._consumed, v._skipCost = false, 0, 0, 0, 1.0
  v._offsets = {}
  time._ent = {}
  if __anim_reset then __anim_reset() end   -- Tween / Flicker も Play 開始でクリア
end
function __time_tick(dt)
  -- Tween / Flicker（演出レイヤ）を進める。既存の毎フレームフックに1行ぶら下げるだけ。
  if __anim_tick then __anim_tick(dt) end

  -- コルーチンのタスクを進める。wait(0) / waitFrames(1) はどちらも「次のフレームで再開」。
  task._tick(dt)

  -- ビデオ時計・個別時計を進める
  if time.video._active then time.video._t = time.video._t + dt end
  for _, c in pairs(time._ent) do c.t = c.t + dt * c.scale end

  -- タイマー: スナップショットしてから回す(fn 内で after/cancel されても安全)
  local ids = {}
  for id in pairs(time._timers) do ids[#ids + 1] = id end
  for _, id in ipairs(ids) do
    local t = time._timers[id]
    if t then
      t.left = t.left - dt
      while t and t.left <= 0 do
        local ok, err = pcall(t.fn)
        if not ok then print("time timer error: " .. tostring(err)) end
        if t.interval and t.interval > 0 then t.left = t.left + t.interval
        else time._timers[id] = nil; t = nil end
      end
    end
  end
end

local function d2xz(ax, az, bx, bz) local dx, dz = ax-bx, az-bz; return dx*dx + dz*dz end

-- ===== Actor: 名前付きエンティティの薄いラッパー =====
Actor = {}
Actor.__index = Actor

-- actor(name, { speed=, solid="Wall1" or {"Wall1","Wall2"}, half=0.5 })
function actor(name, opts)
  opts = opts or {}
  local a = setmetatable({}, Actor)
  a.name   = name
  a.speed  = opts.speed or 5
  a.half   = opts.half  or 0.5
  a.solids = opts.solid or {}
  if type(a.solids) == "string" then a.solids = { a.solids } end
  local p = a:pos()
  a.x, a.y, a.z = p.x, p.y, p.z
  return a
end

function Actor:entity()
  local e = scene:findEntity(self.name)
  if e and e:isValid() then return e end
  return nil
end
function Actor:valid() return self:entity() ~= nil end

function Actor:pos()
  local e = self:entity()
  if e then return e.transform.position end
  return Vec3.new(0, 0, 0)
end

function Actor:setPos(x, y, z)
  self.x, self.y, self.z = x, y, z
  local e = self:entity()
  if e then e.transform.position = Vec3.new(x, y, z) end
end

-- (nx,nz) が solid 相手の箱と重なるか（AABB, XZ平面。box=半径0.5×scale）
function Actor:_blocked(nx, nz)
  for _, sname in ipairs(self.solids) do
    local s = scene:findEntity(sname)
    if s and s:isValid() then
      local sp, ss = s.transform.position, s.transform.scale
      if math.abs(nx - sp.x) < (self.half + 0.5*ss.x)
         and math.abs(nz - sp.z) < (self.half + 0.5*ss.z) then
        return true
      end
    end
  end
  return false
end

-- WASD(既定) / "Arrows" で見下ろし移動。solid に当たったら軸ごと停止(=壁沿いスライド)
function Actor:moveTopDown(dt, scheme)
  scheme = scheme or "WASD"
  local mv = self.speed * dt
  local dx, dz = 0, 0
  if scheme == "Arrows" then
    if keyDown("UP")   then dz = dz + mv end
    if keyDown("DOWN") then dz = dz - mv end
    if keyDown("LEFT") then dx = dx - mv end
    if keyDown("RIGHT")then dx = dx + mv end
  else
    if keyDown("W") then dz = dz + mv end
    if keyDown("S") then dz = dz - mv end
    if keyDown("A") then dx = dx - mv end
    if keyDown("D") then dx = dx + mv end
  end
  if dx ~= 0 and not self:_blocked(self.x + dx, self.z) then self.x = self.x + dx end
  if dz ~= 0 and not self:_blocked(self.x, self.z + dz) then self.z = self.z + dz end
  self:setPos(self.x, self.y, self.z)
end

-- 相手(Actor)に届いたか（XZ距離 < radius）
function Actor:reached(other, radius)
  radius = radius or 1.0
  local a, b = self:pos(), other:pos()
  return d2xz(a.x, a.z, b.x, b.z) < radius * radius
end

-- ===== カメラ追従（見下ろし）=====
-- cameraFollow(target, { name="GameCamera", height=13, back=8, pitch=55 })
function cameraFollow(target, opts)
  opts = opts or {}
  local cam = scene:findEntity(opts.name or "GameCamera")
  if not (cam and cam:isValid()) then return end
  local p = target:pos()
  cam.transform.position = Vec3.new(p.x, opts.height or 13, p.z - (opts.back or 8))
  cam.transform.rotation = Vec3.new(opts.pitch or 55, 0, 0)
end

-- ===== シーン遷移（フェード付きの分かりやすい別名）=====
function goToScene(path, dur) fadeToScene(path, dur or 0.6) end
function win(dur)  nextScene() end

-- ===== 汎用ユーティリティ =====
function clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi else return v end end
function lerp(a, b, t)    return a + (b - a) * t end
-- 角度の最短差（度）。-180..180 を返す。カメラ/向きの補間に使う。
function angleDelta(from, to)
  local d = (to - from) % 360
  if d > 180 then d = d - 360 end
  return d
end

-- ===== 三人称トレイルカメラ（キーボードTPS向け・マウス不要）=====
-- cameraTPS(target, { name="MainCamera", dist=10, height=6, pitch=26, yaw=<度>, follow=0.1 })
--   target: Actor（:pos() を持つ）または {x=,y=,z=} のテーブル。
--   yaw   : カメラを置きたい方位（度）。プレイヤーの向きを毎フレーム渡すと背後に回り込む。
--   follow: yaw 追従の補間率 0..1（小さいほどゆっくり背後に回る＝トレイル感）。1=即時。
--   カメラ方位の状態は target._camYaw に保持する（target はテーブルである必要あり）。
function cameraTPS(target, opts)
  opts = opts or {}
  local cam = scene:findEntity(opts.name or "MainCamera")
  if not (cam and cam:isValid()) then return end
  local p = (type(target) == "table" and target.pos) and target:pos() or target
  local px, py, pz = p.x, (p.y or 0), p.z
  local dist   = opts.dist   or 10
  local height = opts.height or 6
  local pitch  = opts.pitch  or 26
  local goal   = opts.yaw    or 0
  local follow = opts.follow or 1.0
  local cy = target._camYaw or goal
  -- 大きな方向転換ほど速く背後へ回り込み、小さな調整はゆっくり（滑らか＋素早い狙い直し）
  local delta = angleDelta(cy, goal)
  local t = clamp(follow + (math.abs(delta) / 180) * 0.35, follow, 1)
  cy = cy + delta * t
  if type(target) == "table" then target._camYaw = cy end
  local ry = math.rad(cy)
  local fx, fz = math.sin(ry), math.cos(ry)
  -- プレイヤーの背後 dist、上 height に置き、向き(yaw)＋見下ろし(pitch)でプレイヤーを捉える。
  -- エンジンは rotation.y を yaw、-rotation.x を pitch として描画カメラへ同期する。
  cam.transform.position = Vec3.new(px - fx * dist, py + height, pz - fz * dist)
  cam.transform.rotation = Vec3.new(pitch, cy, 0)
end

-- ===== ロックオン三人称カメラ（ボス戦/デュエル向け・マウス不要）=====
-- カメラを「プレイヤー → ターゲット」軸に固定し、プレイヤーの背後からターゲットを捉える。
-- これにより WASD をこのカメラ基準にすれば「向きが変わっても操作が狂わない」。
-- cameraLockOn(playerPos, targetPos, { name="MainCamera", dist=9, height=5, pitch=18, smooth })
--   playerPos / targetPos: {x=,y=,z=}（Vec3 でも可）。
--   smooth: 角度補間率 0..1（省略=即時）。近接時のジッタ抑制に 0.2〜0.5 程度。
--   state : smooth を使うなら yaw 状態を持つテーブルを渡す（state._lockYaw に保持）。
-- 戻り値: カメラ方位 yaw（度）。WASD のワールド方向はこの yaw から作る。
function cameraLockOn(playerPos, targetPos, opts, state)
  opts = opts or {}
  local dx = targetPos.x - playerPos.x
  local dz = targetPos.z - playerPos.z
  local yaw
  if math.abs(dx) < 1e-4 and math.abs(dz) < 1e-4 then
    yaw = (state and state._lockYaw) or 0   -- 重なった時は前回の向きを維持
  else
    yaw = math.deg(math.atan(dx, dz))        -- player→target 方位（forward=+Z で yaw0）
  end
  if state then
    local prev = state._lockYaw or yaw
    local s = opts.smooth or 1.0
    local step = angleDelta(prev, yaw) * clamp(s, 0, 1)
    local maxStep = opts.maxStep or 9      -- 度/フレーム上限（近接時の急回転を抑える保険）
    if step >  maxStep then step =  maxStep end
    if step < -maxStep then step = -maxStep end
    yaw = prev + step
    state._lockYaw = yaw
  end
  local cam = scene:findEntity(opts.name or "MainCamera")
  if cam and cam:isValid() then
    local dist   = opts.dist   or 9
    local height = opts.height or 5
    local pitch  = opts.pitch  or 18
    local ry = math.rad(yaw)
    local fx, fz = math.sin(ry), math.cos(ry)
    cam.transform.position = Vec3.new(playerPos.x - fx * dist,
                                      (playerPos.y or 0) + height,
                                      playerPos.z - fz * dist)
    cam.transform.rotation = Vec3.new(pitch, yaw, 0)
  end
  return yaw
end
)LUA"
R"LUA(

-- ============================================================
--  FX: ド派手パーティクルプリセット（fx:burst / fx:ring を包む）
--  どのゲームスクリプトからも FX.explosion(...) 等で呼べる。
--  色は 0..1、intensity>1 で HDR 白熱 → ブルームで光る。
-- ============================================================
FX = {}

-- 爆発（撃破など）: 白熱フラッシュ + fbm火球 + 光筋火花 + 拡大リング + 遮蔽煙
function FX.explosion(x, y, z, scale, r, g, b)
  scale = scale or 1.0
  r = r or 1.0; g = g or 0.45; b = b or 0.12
  -- 白熱フラッシュ（スター閃光・一瞬）
  fx:burst{ x=x, y=y, z=z, count=1, kind="star", size=1.6*scale, sizeEnd=0.0, life=0.12,
            r=1, g=0.95, b=0.85, intensity=16 }
  -- 火球（fbm炎・温度ランプ・カール乱流・明滅）
  fx:burst{ x=x, y=y, z=z, count=math.floor(16*scale), spread=1, speed=6*scale, speedVar=0.5,
            kind="fire", size=0.7*scale, sizeEnd=0.1, life=0.55, lifeVar=0.35,
            r=r*1.3, g=g*1.2, b=b, rEnd=r*0.6, gEnd=g*0.3, bEnd=b*0.15,
            intensity=5, gravity=-4, drag=2.2, up=0.6, turbStrength=2.5, turbFreq=1.2, flicker=0.5 }
  -- 飛び散る火花（速度ストレッチ＝光の筋）
  fx:burst{ x=x, y=y, z=z, count=math.floor(16*scale), spread=1, speed=16*scale, speedVar=0.5,
            kind="spark", size=0.12*scale, sizeEnd=0.0, life=0.4, lifeVar=0.4,
            r=1, g=0.95, b=0.7, intensity=10, drag=1.5, stretch=5, gravity=-7 }
  -- 衝撃波リング（単一粒子で拡大）
  fx:burst{ x=x, y=y, z=z, count=1, kind="ring", size=3.2*scale, sizeEnd=3.2*scale, life=0.45,
            r=1, g=0.8, b=0.5, intensity=5 }
  -- 乱流煙（α前乗算＝遮蔽でボリューム感。カールでうねる）
  fx:burst{ x=x, y=y, z=z, count=math.floor(10*scale), spread=0.7, speed=2.2*scale, speedVar=0.4,
            kind="smoke", size=0.6*scale, sizeEnd=1.8*scale, life=1.1, lifeVar=0.4,
            r=0.16, g=0.15, b=0.17, intensity=1, drag=1.2, turbStrength=2.5, turbFreq=0.7, up=0.3 }
end

-- 衝撃波リング（ノヴァ等）: 単一粒子の拡大リング + 放射状の光筋
function FX.shockwave(x, y, z, count, speed, r, g, b)
  r = r or 0.6; g = g or 1.0; b = b or 1.0
  local rad = (speed or 16) * 0.3
  fx:burst{ x=x, y=y, z=z, count=1, kind="ring", size=rad, sizeEnd=rad, life=0.5,
            r=r, g=g, b=b, intensity=7 }
  fx:ring{ x=x, y=y, z=z, count=count or 24, speed=speed or 16, speedVar=0.0,
           kind="spark", size=0.3, sizeEnd=0.0, life=0.4, lifeVar=0.1,
           r=r, g=g, b=b, intensity=6, drag=1.2, stretch=3 }
end

-- 着弾火花（小さく速い・速度ストレッチで筋に）
function FX.spark(x, y, z, count, r, g, b)
  fx:burst{ x=x, y=y, z=z, count=count or 6, spread=1, speed=11, speedVar=0.5,
            kind="spark", size=0.13, sizeEnd=0.0, life=0.3, lifeVar=0.4,
            r=r or 0.6, g=g or 0.95, b=b or 1.0, intensity=9, drag=2, stretch=4 }
end

-- 立ち上る軌跡/オーラ点（1粒ずつ毎フレーム呼ぶ用）
function FX.trail(x, y, z, r, g, b)
  fx:burst{ x=x, y=y, z=z, count=1, spread=0.4, dy=1, speed=1.5, speedVar=0.5,
            kind="spark", size=0.2, sizeEnd=0.0, life=0.45, lifeVar=0.3,
            r=r or 1, g=g or 0.9, b=b or 0.4, intensity=5, gravity=2, drag=1, stretch=1.5 }
end

-- レベルアップ超新星: 白熱フラッシュ + 金リング + 火球 + 金火花 + 画面パルス
function FX.supernova(x, y, z, scale)
  scale = scale or 1.0
  fx:burst{ x=x, y=y, z=z, count=1, kind="star", size=2.4*scale, sizeEnd=0.0, life=0.16,
            r=1, g=0.95, b=0.8, intensity=20 }
  fx:burst{ x=x, y=y, z=z, count=1, kind="ring", size=5.0*scale, sizeEnd=5.0*scale, life=0.7,
            r=1, g=0.85, b=0.35, intensity=8 }
  fx:burst{ x=x, y=y, z=z, count=math.floor(40*scale), spread=1, speed=9*scale, speedVar=0.5,
            kind="fire", size=0.55*scale, sizeEnd=0.08, life=0.8, lifeVar=0.4,
            r=1, g=0.9, b=0.5, rEnd=1, gEnd=0.45, bEnd=0.12,
            intensity=7, gravity=-3, drag=1.5, up=0.5, turbStrength=2.0, flicker=0.4 }
  -- 伸びる金の火花
  fx:burst{ x=x, y=y, z=z, count=math.floor(30*scale), spread=1, speed=18*scale, speedVar=0.5,
            kind="spark", size=0.12, sizeEnd=0.0, life=0.6, lifeVar=0.4,
            r=1, g=0.9, b=0.4, intensity=9, drag=1.2, stretch=5, gravity=-6 }
  fx:pulse(0.8)
end

-- ヒット時の画面パルス（クロマ + 放射ブラー）
function FX.hit(amount) fx:pulse(amount or 0.5) end

-- 連続ビーム（レーザー/エネルギー線）。毎フレーム呼ぶ用（点線にならない一本線）
function FX.beam(x0, y0, z0, x1, y1, z1, r, g, b, width, kind, intensity)
  fx:beam{ x0=x0, y0=y0, z0=z0, x1=x1, y1=y1, z1=z1,
           width=width or 0.4, kind=kind or "energy",
           r=r or 0.4, g=g or 0.9, b=b or 1.0, intensity=intensity or 7, life=0.06 }
end

-- 稲妻ビーム（始点→終点をギザギザの放電で繋ぐ）
function FX.lightning(x0, y0, z0, x1, y1, z1, r, g, b, width)
  fx:beam{ x0=x0, y0=y0, z0=z0, x1=x1, y1=y1, z1=z1,
           width=width or 0.6, kind="electric",
           r=r or 0.6, g=g or 0.8, b=b or 1.0, intensity=8, life=0.06 }
end

-- 動的火柱（噴き上がり→うねり→崩れをシェーダがアニメ。火の粉/閃光/地面リング/煙つき）
function FX.pillar(x, y, z, height, radius, r, g, b)
  height = height or 6.0; radius = radius or 1.2
  r = r or 1.0; g = g or 0.5; b = b or 0.15
  fx:beam{ x0=x, y0=y, z0=z, x1=x, y1=y+height, z1=z, width=radius, kind="fire",
           r=r, g=g, b=b, intensity=7, life=0.7 }
  fx:burst{ x=x, y=y+0.3, z=z, count=24, spread=0.45, dy=1, speed=height*1.1, speedVar=0.5,
            kind="spark", size=0.16, sizeEnd=0.0, life=0.8, lifeVar=0.4,
            r=1, g=0.85, b=0.45, intensity=9, gravity=-2.5, drag=0.5, turbStrength=3.0, stretch=2.5 }
  fx:burst{ x=x, y=y+0.4, z=z, count=1, kind="star", size=radius*1.7, sizeEnd=0.0, life=0.12,
            r=1, g=0.9, b=0.7, intensity=14 }
  fx:burst{ x=x, y=0.1, z=z, count=1, kind="ring", size=radius*3.2, sizeEnd=radius*3.2, life=0.45,
            r=r, g=g*0.7, b=b*0.4, intensity=5 }
  fx:burst{ x=x, y=0.1, z=z, count=1, kind="glow", size=radius*2.6, sizeEnd=radius*2.6, life=0.3,
            r=r, g=g*0.7, b=b*0.5, intensity=4 }
  fx:burst{ x=x, y=y+height*0.6, z=z, count=6, spread=0.4, speed=2, kind="smoke",
            size=radius*0.9, sizeEnd=radius*2.4, life=1.2, r=0.2, g=0.18, b=0.18,
            intensity=1, turbStrength=2.5, up=0.5 }
end

-- ============================================================
--  統一 VFX API（コード自前 と Effekseer を両立させる窓口）
--  ゲームは vfx.play("name", x,y,z[,scale]) を呼ぶだけ。
--  既定は下のコードプリセット（エディタ不要＝Claude/コードのみで完結）。
--  将来 Effekseer 実体（.efkefc）が登録されれば、同じ名前でそちらを再生する。
--  エンジン(C++)側は vfx._efk[name] に再生関数を差し込むだけで上書きできる。
-- ============================================================
vfx = vfx or {}
vfx._code = {}   -- name -> function(x,y,z,scale)  （コード自前プリセット）
vfx._efk  = {}   -- name -> function(x,y,z,scale)  （Effekseer 実体。engine が hook）
function vfx.register(name, fn) vfx._code[name] = fn end
function vfx.play(name, x, y, z, scale)
  scale = scale or 1.0
  local e = vfx._efk[name]          -- Effekseer 実体があれば最優先
  if e then return e(x, y, z, scale) end
  local c = vfx._code[name]         -- 無ければコードプリセット
  if c then return c(x, y, z, scale) end
end
-- 既定コードプリセット（FX.* を名前で引けるように）
vfx.register("explosion", function(x,y,z,s) FX.explosion(x,y,z,s) end)
vfx.register("supernova", function(x,y,z,s) FX.supernova(x,y,z,s) end)
vfx.register("spark",     function(x,y,z,s) FX.spark(x,y,z, math.floor(8*(s or 1))) end)
vfx.register("hit",       function(x,y,z,s) FX.hit(0.6) end)

-- ============================================================
--  uifx: ゲーム内UI(UIRect持ちエンティティ)の定番演出ワンライナー集
--  対象 e は Entity でも エンティティID(ボタンイベントの e.source)でも可。
--  実体は scene:tweenUi の組み合わせ（エンジン改修なしの純 Lua）。
-- ============================================================
uifx = {}

-- ボタンを押した感（一瞬膨らんで戻る）。s=膨らみ倍率
function uifx.punch(e, s, dur)
  s = s or 1.15; dur = dur or 0.22
  scene:tweenUi(e, { scale=s,   duration=dur*0.35, easing="out" })
  scene:tweenUi(e, { scale=1.0, duration=dur*0.65, delay=dur*0.35, easing="back" })
end

-- 色フラッシュ（既定=白く光る。1超え=輝き。ダメージ赤は uifx.flash(e, 3, 0.3, 0.3)）
function uifx.flash(e, r, g, b, dur)
  r = r or 2.5; g = g or 2.5; b = b or 2.5; dur = dur or 0.3
  scene:tweenUi(e, { color={r,g,b}, duration=dur*0.25, easing="out" })
  scene:tweenUi(e, { color={1,1,1}, duration=dur*0.75, delay=dur*0.25, easing="out" })
end

-- 振動（ダメージ/エラー通知）。amp=px
function uifx.shake(e, amp, dur)
  scene:tweenUi(e, { shake=amp or 10, duration=dur or 0.4 })
end

-- 赤フラッシュ + 振動（被ダメの定番セット）
function uifx.hit(e, amp)
  uifx.flash(e, 3, 0.35, 0.35, 0.35)
  uifx.shake(e, amp or 8, 0.35)
end

-- ぽよんと登場（0 からバウンドで等倍へ）
function uifx.bounceIn(e, dur)
  dur = dur or 0.5
  scene:tweenUi(e, { scale=0.01, alpha=0, duration=0.01 })
  scene:tweenUi(e, { scale=1.0,  alpha=1, duration=dur, delay=0.02, easing="bounce" })
end

-- ぺしゃんこ→開く（フリップ風の注目演出。結果表示やカード公開に）
function uifx.flipIn(e, dur)
  dur = dur or 0.4
  scene:tweenUi(e, { scaleY=0.01, alpha=0, duration=0.01 })
  scene:tweenUi(e, { scaleY=1.0,  alpha=1, duration=dur, delay=0.02, easing="back" })
end

-- 縮んで消える（ポップアップを閉じる）
function uifx.popOut(e, dur)
  scene:tweenUi(e, { scale=0.01, alpha=0, duration=dur or 0.25, easing="in" })
end

-- フェードイン / アウト
function uifx.fadeIn(e, dur)  scene:tweenUi(e, { alpha=1, duration=dur or 0.3 }) end
function uifx.fadeOut(e, dur) scene:tweenUi(e, { alpha=0, duration=dur or 0.3 }) end

-- ステージャー（リスト項目の順次入場。間隔の相場は 0.05〜0.10 秒）。
-- list = Entity/ID の配列、fn は関数(e, delay) か uifx の関数名文字列。
-- 例: uifx.stagger({item1, item2, item3}, 0.07, uifx.slideInLeft)
--     uifx.stagger(items, 0.06, function(e, d) scene:tweenUi(e, {alpha=1, duration=0.3, delay=d}) end)
function uifx.stagger(list, step, fn, ...)
  step = step or 0.07
  for i, e in ipairs(list) do fn(e, (i - 1) * step, ...) end
end

-- 方向スライド入場（delay 対応 = stagger と組み合わせる）。dist=px
function uifx.slideInLeft(e, delay, dist, dur)
  dist = dist or 80; dur = dur or 0.35; delay = delay or 0
  scene:tweenUi(e, { alpha=0, duration=0.01, delay=delay })
  scene:tweenUi(e, { dx=-dist, duration=0.01, delay=delay })
  scene:tweenUi(e, { dx=dist, alpha=1, duration=dur, delay=delay + 0.02, easing="expo" })
end
function uifx.slideInRight(e, delay, dist, dur)
  dist = dist or 80; dur = dur or 0.35; delay = delay or 0
  scene:tweenUi(e, { alpha=0, duration=0.01, delay=delay })
  scene:tweenUi(e, { dx=dist, duration=0.01, delay=delay })
  scene:tweenUi(e, { dx=-dist, alpha=1, duration=dur, delay=delay + 0.02, easing="expo" })
end
function uifx.slideInUp(e, delay, dist, dur)   -- 下から上がってくる
  dist = dist or 60; dur = dur or 0.35; delay = delay or 0
  scene:tweenUi(e, { alpha=0, duration=0.01, delay=delay })
  scene:tweenUi(e, { dy=dist, duration=0.01, delay=delay })
  scene:tweenUi(e, { dy=-dist, alpha=1, duration=dur, delay=delay + 0.02, easing="expo" })
end
function uifx.popIn(e, delay, dur)             -- ぽんと出る（stagger 対応版 bounceIn）
  dur = dur or 0.35; delay = delay or 0
  scene:tweenUi(e, { scale=0.01, alpha=0, duration=0.01, delay=delay })
  scene:tweenUi(e, { scale=1.0,  alpha=1, duration=dur, delay=delay + 0.02, easing="back" })
end

-- 数字ロール（スコア/所持金のカウントアップ）。fmt 例: "%d" "%05d" "%.1f"
function uifx.countTo(e, to, dur, fmt)
  scene:tweenUi(e, { countTo=to, countFmt=fmt or "%d", duration=dur or 0.6, easing="expo" })
end

-- ゲージをなめらかに増減（fill は絶対目標値 0..1）
function uifx.fillTo(e, v, dur, easing)
  scene:tweenUi(e, { fill=v, duration=dur or 0.35, easing=easing or "out" })
end

-- ゴーストバー付きダメージ（front=本体バー ghost=背後の白/黄バー。格ゲー/アクションの定番。
-- 本体は即落ち、ゴーストが遅れて追従して「削られた量」を見せる）
function uifx.damageBar(front, ghost, v, ghostDelay)
  scene:tweenUi(front, { fill=v, duration=0.08, easing="out" })
  scene:tweenUi(ghost, { fill=v, duration=0.45, delay=ghostDelay or 0.35, easing="out" })
end

-- 注目のゆらぎ（通知バッジ等を左右にクイックに振る）
function uifx.wiggle(e, deg, dur)
  deg = deg or 8; dur = dur or 0.4
  scene:tweenUi(e, { rotate=-deg,  duration=dur*0.2, easing="out" })
  scene:tweenUi(e, { rotate=deg,   duration=dur*0.3, delay=dur*0.2, easing="inOut" })
  scene:tweenUi(e, { rotate=0,     duration=dur*0.5, delay=dur*0.5, easing="elastic" })
end

-- ハートビート（2 連パルス。クールダウン完了/低 HP 警告のワンショット）
function uifx.heartbeat(e, s, dur)
  s = s or 1.12; dur = dur or 0.5
  scene:tweenUi(e, { scale=s,   duration=dur*0.15, easing="out" })
  scene:tweenUi(e, { scale=1.0, duration=dur*0.15, delay=dur*0.15, easing="in" })
  scene:tweenUi(e, { scale=s,   duration=dur*0.2,  delay=dur*0.35, easing="out" })
  scene:tweenUi(e, { scale=1.0, duration=dur*0.45, delay=dur*0.55, easing="out" })
end
)LUA"
R"LUA(

-- ============================================================
--  Tween: 汎用プロパティ補間（Godot の tween_property 相当）
--  target[prop] を to まで duration 秒かけて動かすだけ。target は Lua テーブルでも
--  usertype（Light / Transform / self）でも可。数値と 3 要素（色/ベクトル）の両方を補間する。
--  「演出ごとの専用 API」を増やさずに、時間変化はこの 1 本で賄うのが方針。
--    Tween(target, prop, to, duration, { ease=, delay=, loop=, pingpong=, onComplete= }) -> id
--    stopTween(id) / Anim.clear()
--  時間はスケール済み dt で進む（time.setScale(0) で止まる）。Play 開始で全部クリア。
--  注: self.transform を直接 tween する場合は、対象エンティティが補間中に消えないこと。
-- ============================================================
Ease = {
  linear    = function(t) return t end,
  inQuad    = function(t) return t * t end,
  outQuad   = function(t) return 1 - (1 - t) * (1 - t) end,
  inOutQuad = function(t) if t < 0.5 then return 2*t*t else return 1 - ((-2*t + 2)^2) / 2 end end,
  inCubic   = function(t) return t * t * t end,
  outCubic  = function(t) return 1 - (1 - t)^3 end,
  inOutSine = function(t) return -(math.cos(math.pi * t) - 1) / 2 end,
  outBack   = function(t)
    local c1 = 1.70158
    return 1 + (c1 + 1) * (t - 1)^3 + c1 * (t - 1)^2
  end,
  outBounce = function(t)
    local n, d = 7.5625, 2.75
    if t < 1/d then return n*t*t
    elseif t < 2/d then t = t - 1.5/d;   return n*t*t + 0.75
    elseif t < 2.5/d then t = t - 2.25/d;  return n*t*t + 0.9375
    else t = t - 2.625/d; return n*t*t + 0.984375 end
  end,
}

Anim = { _list = {}, _next = 1 }

-- 値を {x,y,z} の3要素へ正規化する。数値/非対応なら nil（= スカラー補間へ）。
local function _triple(v)
  local tv = type(v)
  if tv == "table" then
    local x, y, z = v.x or v[1], v.y or v[2], v.z or v[3]
    if type(x) == "number" and type(y) == "number" and type(z) == "number" then
      return { x, y, z }
    end
  elseif tv == "userdata" then
    local ok, x = pcall(function() return v.x end)
    if ok and type(x) == "number" then return { x, v.y, v.z } end
  end
  return nil
end

local function _applyTween(tw, k)
  local f, t = tw.from, tw.to
  if tw.n == 1 then
    tw.target[tw.prop] = f[1] + (t[1] - f[1]) * k
  else
    local x = f[1] + (t[1] - f[1]) * k
    local y = f[2] + (t[2] - f[2]) * k
    local z = f[3] + (t[3] - f[3]) * k
    -- 元が usertype(Vec3) なら Vec3、テーブルなら配列/名前つき両対応のテーブルで返す
    if tw.mk then tw.target[tw.prop] = Vec3.new(x, y, z)
    else tw.target[tw.prop] = { x, y, z, x = x, y = y, z = z } end
  end
end

function Tween(target, prop, to, duration, opts)
  opts = opts or {}
  if target == nil or prop == nil then return nil end
  local cur = target[prop]
  local from, dest, n, mk
  local ct = _triple(cur)
  if ct then
    local tt = _triple(to)
    if not tt then local v = tonumber(to) or 0; tt = { v, v, v } end
    from, dest, n, mk = ct, tt, 3, (type(cur) == "userdata")
  else
    from, dest, n, mk = { tonumber(cur) or 0 }, { tonumber(to) or 0 }, 1, false
  end

  -- loop: true=無限 / 数値=総再生回数 / 省略=1回
  local loops = 0
  if opts.loop == true then loops = -1
  elseif type(opts.loop) == "number" then loops = math.max(0, math.floor(opts.loop) - 1) end

  local id = Anim._next
  Anim._next = id + 1
  Anim._list[id] = {
    target = target, prop = prop, from = from, to = dest, n = n, mk = mk,
    dur = math.max(duration or 0.3, 0.0001),
    t = -(opts.delay or 0),
    ease = Ease[opts.ease or "outQuad"] or Ease.outQuad,
    loops = loops, pingpong = (opts.pingpong == true),
    onComplete = opts.onComplete, _back = false,
  }
  return id
end

function stopTween(id) if id ~= nil then Anim._list[id] = nil end end
function Anim.clear() Anim._list, Anim._next = {}, 1 end

-- ============================================================
--  Flicker: Quake 由来の lightstyle 文字列で明滅させる
--  1 文字 = 1/10 秒（hz で変更可）。'a'=消灯 / 'm'=通常(1.0) / 'z'≒2.08 倍。
--  ロウソク・蛍光灯・故障灯・雷が、この文字列 1 本で全部書ける（実装コスト最小・表現力最大）。
--    Flicker(light, "candle")            プリセット
--    Flicker(light, "mmnmmommonqnmmo")   生の lightstyle
--    stopFlicker(light)                  元の明るさへ戻す
-- ============================================================
LIGHT_STYLES = {
  normal      = "m",
  candle      = "mmmaaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
  fluorescent = "mmamammmmammamamaaamammma",
  broken      = "mmnmmommommnonmmonqnmmo",
  pulse       = "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
  storm       = "maaaaaaaaaaaaaaazzaaaaaaaaaammaaaaaaaaaaazzzaaaaaa",
  strobe      = "mamamamamama",
  slowStrobe  = "aaaaaaaazzzzzzzz",
  gentle      = "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
}

Flick = { _list = {} }

function Flicker(light, style, hz)
  if not (light and light.isValid and light:isValid()) then return nil end
  local s = LIGHT_STYLES[style or "candle"] or style
  if type(s) ~= "string" or #s == 0 then s = LIGHT_STYLES.candle end
  local key = light.id
  local prev = Flick._list[key]
  Flick._list[key] = {
    light = light, style = s, hz = hz or 10,
    base = prev and prev.base or light.intensity, t = 0,
  }
  return light
end

function stopFlicker(light)
  if not light then return end
  local fl = Flick._list[light.id]
  if not fl then return end
  Flick._list[light.id] = nil
  fl.light.intensity = fl.base
end

-- __time_tick / __time_reset からぶら下がる駆動部（C++ 側の更新フックはそのまま）
function __anim_reset()
  Anim._list, Anim._next = {}, 1
  Flick._list = {}
  Lighting._hour = nil
end

function __anim_tick(dt)
  local dead
  for id, tw in pairs(Anim._list) do
    tw.t = tw.t + dt
    if tw.t >= 0 then
      local k, fin = tw.t / tw.dur, false
      if k >= 1 then k, fin = 1, true end
      local ok, err = pcall(_applyTween, tw, tw.ease(k))
      if not ok then
        logWarn("Tween エラー (" .. tostring(tw.prop) .. "): " .. tostring(err))
        dead = dead or {}; dead[#dead + 1] = id
      elseif fin then
        if tw.pingpong and not tw._back then
          tw._back = true
          tw.from, tw.to = tw.to, tw.from
          tw.t = 0
        elseif tw.loops ~= 0 then
          tw.loops = tw.loops - 1        -- -1 は減っても 0 にならない = 無限ループ
          if tw._back then tw._back = false; tw.from, tw.to = tw.to, tw.from end
          tw.t = 0
        else
          dead = dead or {}; dead[#dead + 1] = id
          if tw.onComplete then
            local ok2, err2 = pcall(tw.onComplete)
            if not ok2 then logWarn("Tween onComplete エラー: " .. tostring(err2)) end
          end
        end
      end
    end
  end
  if dead then for _, id in ipairs(dead) do Anim._list[id] = nil end end

  for _, fl in pairs(Flick._list) do
    fl.t = fl.t + dt
    local i = (math.floor(fl.t * fl.hz) % #fl.style) + 1
    local c = string.byte(fl.style, i) - 97      -- 'a' = 0
    if c < 0 then c = 0 end
    fl.light.intensity = fl.base * (c / 12.0)    -- 'm'(=12) で等倍
  end
end
)LUA"
R"LUA(

-- ============================================================
--  Lighting: 時間帯と定番のライティング演出
--  新しいシーン設定は増やさず、既存の DirectionalLight(太陽) と post を叩くだけ。
-- ============================================================
Lighting = {}
Lighting.dayColor       = { 1.00, 0.97, 0.92 }
Lighting.duskColor      = { 1.00, 0.46, 0.18 }
Lighting.nightColor     = { 0.40, 0.52, 0.85 }
Lighting.dayIntensity   = 3.0
Lighting.nightIntensity = 0.35
Lighting.dayAmbient     = 0.30
Lighting.nightAmbient   = 0.05
Lighting.duskAmbient    = 0.12   -- 地平線での環境光。昼側/夜側どちらから来てもこの値＝継ぎ目が出ない

-- hour(0..24) → 太陽の向き / 色 / 強度 / 環境光。上の定数を書き換えれば作風ごと変わる。
-- 日の出/日の入り（6時/18時）をまたぐ瞬間に絵が飛ばないよう、地平線で強度 0・環境光
-- duskAmbient に揃えてある（昼夜の切替はここで連続になる）。
-- 戻り: dx, dy, dz, r, g, b, intensity, ambient
function Lighting.sample(hour)
  local h = (hour or 12) % 24
  local a = (h - 6) / 12 * math.pi          -- 6時=東の地平線 / 12時=天頂 / 18時=西の地平線
  local sx, sy, sz = -math.cos(a), math.sin(a), 0.35
  local night = (sy <= 0)
  if night then sx, sy, sz = -sx, -sy, -sz end   -- 夜は月（太陽の反対側）を光源にする
  local len = math.sqrt(sx*sx + sy*sy + sz*sz)
  local t = clamp((sy / len) / 0.35, 0, 1)       -- 地平線=0 → 20度ほど昇れば 1
  t = t * t * (3 - 2 * t)                        -- smoothstep（薄明の立ち上がりを滑らかに）
  local lo = night and Lighting.nightColor or Lighting.duskColor
  local hi = night and Lighting.nightColor or Lighting.dayColor
  local imax = night and Lighting.nightIntensity or Lighting.dayIntensity
  local amb1 = night and Lighting.nightAmbient or Lighting.dayAmbient
  return -sx/len, -sy/len, -sz/len,
         lerp(lo[1], hi[1], t), lerp(lo[2], hi[2], t), lerp(lo[3], hi[3], t),
         imax * t,
         lerp(Lighting.duskAmbient, amb1, t)
end

function Lighting.sun() return scene:sun() end
function Lighting.timeOfDay() return Lighting._hour or 12 end

-- 時刻を即反映（太陽が無いシーンでは時刻だけ覚えて何もしない）
function Lighting.setTimeOfDay(hour)
  Lighting._hour = (hour or 12) % 24
  local sun = scene:sun()
  if not sun then return nil end
  local dx, dy, dz, r, g, b, i, amb = Lighting.sample(Lighting._hour)
  sun:setDirection(dx, dy, dz)
  sun:setColor(r, g, b)
  sun.intensity = i
  sun.ambient   = amb
  return sun
end

-- 「時刻」を Tween の対象にするためのプロキシ（__newindex で setTimeOfDay を呼ぶ）。
-- これで時間帯の変化も汎用 Tween 1 本で済む＝専用の補間器を足さない。
Lighting._tod = setmetatable({}, {
  __index    = function(_, k) if k == "hour" then return Lighting.timeOfDay() end end,
  __newindex = function(_, k, v) if k == "hour" then Lighting.setTimeOfDay(v) end end,
})

-- hour まで duration 秒かけて時間を進める（既定は最短方向。opts.forward=true で必ず前進）
function Lighting.tweenTimeOfDay(hour, duration, opts)
  opts = opts or {}
  local from = Lighting.timeOfDay()
  local d = ((hour or 12) - from) % 24
  if d > 12 and not opts.forward then d = d - 24 end
  return Tween(Lighting._tod, "hour", from + d, duration or 3.0,
               { ease = opts.ease or "inOutQuad", delay = opts.delay,
                 onComplete = opts.onComplete })
end

-- 雷の閃光: 太陽を一瞬だけ白く強くして戻す（既定は 2 連フラッシュ）
-- opts: { power=6, color={r,g,b}, times=2, gap=0.09, dur=0.06 }
function Lighting.lightningFlash(opts)
  opts = opts or {}
  local sun = scene:sun()
  if not sun then return end
  local bc = sun.color
  local bi, br, bg, bb, ba = sun.intensity, bc.x, bc.y, bc.z, sun.ambient
  local col   = opts.color or { 0.85, 0.90, 1.00 }
  local power = opts.power or 6
  local times = opts.times or 2
  local gap   = opts.gap or 0.09
  local dur   = opts.dur or 0.06
  local function restore()
    local s = scene:sun()
    if not s then return end
    s.intensity = bi; s:setColor(br, bg, bb); s.ambient = ba
  end
  for k = 0, times - 1 do
    local at = k * (dur + gap)
    time.after(at, function()
      local s = scene:sun()
      if not s then return end
      s.intensity = bi * power
      s:setColor(col[1], col[2], col[3])
      s.ambient = ba + 0.25
    end)
    time.after(at + dur, restore)
  end
end

-- 画面の暗転 / 復帰（露出を落とすだけ。ライトを全部触らないので確実＆安い）
function Lighting.fadeToBlack(sec, onDone)
  post.set("exposureOn", true)
  return Tween(Post, "exposure", 0.0, sec or 1.0, { ease = "inQuad", onComplete = onDone })
end
function Lighting.fadeFromBlack(sec, onDone)
  post.set("exposureOn", true)
  post.set("exposure", 0.0)
  return Tween(Post, "exposure", 1.0, sec or 1.0, { ease = "outQuad", onComplete = onDone })
end

-- ライトを min..max で往復させる（呼吸・鼓動・魔法陣）。hz = 往復の回数/秒
function Lighting.pulse(light, hz, min, max)
  if not (light and light.isValid and light:isValid()) then return nil end
  min = min or 0.4
  max = max or ((light.intensity > 0) and light.intensity or 2.0)
  light.intensity = min
  return Tween(light, "intensity", max, 0.5 / (hz or 1.0),
               { ease = "inOutSine", loop = true, pingpong = true })
end

-- 色 / 明るさをなめらかに変える（状態表現のワンライナー）
function Lighting.tweenColor(light, r, g, b, dur, opts)
  if not light then return nil end
  opts = opts or {}
  return Tween(light, "color", { r, g, b }, dur or 0.5,
               { ease = opts.ease or "outQuad", onComplete = opts.onComplete })
end
function Lighting.tweenIntensity(light, v, dur, opts)
  if not light then return nil end
  opts = opts or {}
  return Tween(light, "intensity", v, dur or 0.5,
               { ease = opts.ease or "outQuad", onComplete = opts.onComplete })
end

-- post / ssao の糖衣。Tween の対象にできる（Post.bloom = 0.8 のようにも書ける）
Post = setmetatable({}, {
  __index    = function(_, k) return post.get(k) end,
  __newindex = function(_, k, v) post.set(k, v) end,
})
Ssao = setmetatable({}, {
  __index    = function(_, k) return ssao.get(k) end,
  __newindex = function(_, k, v) ssao.set(k, v) end,
})

-- 名前 / Entity / self から Light プロキシを引く近道。無ければ nil。
function findLight(x)
  if x == nil then return nil end
  local e = x
  if type(x) == "string" then e = scene:findEntity(x)
  elseif type(x) == "table" then e = scene:findEntity(x.name or "") end
  if not (e and e.isValid and e:isValid()) then return nil end
  return e:light()
end
)LUA";

    auto r = m_lua->safe_script(kPrelude, sol::script_pass_on_error);
    if (!r.valid())
    {
        sol::error err = r;
        Logger::Error("ScriptEngine の初期化スクリプト（prelude）でエラー: {}", err.what());
    }
}

void ScriptEngine::SetScreenSize(int w, int h)
{
    if (!m_lua) return;
    (*m_lua)["SCREEN_W"] = w;
    (*m_lua)["SCREEN_H"] = h;
}

void ScriptEngine::LoadScript(const std::string& filePath)
{
    // VFS 経由で読む（ゲームモード: pak 復号。エディタ/disk: ルーズファイル）。
    auto b = vfs::ReadAssetAbs(filePath);
    auto result = b.empty()
        ? m_lua->safe_script_file(filePath, sol::script_pass_on_error)
        : m_lua->safe_script(std::string(b.begin(), b.end()), sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        m_lastError = err.what();
        Logger::Error("Luaエラー（スクリプト読み込み）: {}", m_lastError);
    }
    else
    {
        m_lastError.clear();
        Logger::Info("Lua script loaded: {}", filePath);
    }
}

void ScriptEngine::LoadScriptFromString(const std::string& code, const std::string& /*chunkName*/)
{
    auto result = m_lua->safe_script(code, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        m_lastError = err.what();
        Logger::Error("Luaエラー（文字列スクリプト読み込み）: {}", m_lastError);
    }
    else
    {
        m_lastError.clear();
    }
}

void ScriptEngine::CallOnStart()
{
    sol::protected_function fn = (*m_lua)["OnStart"];
    if (fn.valid())
    {
        auto result = fn();
        if (!result.valid())
        {
            sol::error err = result;
            m_lastError = err.what();
            Logger::Error("Luaエラー（OnStart）: {}", m_lastError);
        }
        else
        {
            m_lastError.clear();
        }
    }
}

void ScriptEngine::CallOnUpdate(f32 dt)
{
    // time API を進める(Play ループで毎フレーム1回、UpdateAttachedScripts より先に呼ばれる)。
    // 以降スクリプトへ渡る dt は全てスケール済み(m_timeDt)になる。
    m_timeUnscaledDt = dt;
    m_timeDt         = dt * m_timeScale;
    m_timeUnscaled  += dt;
    m_timeElapsed   += m_timeDt;
    ++m_timeFrame;
    {
        sol::protected_function tick = (*m_lua)["__time_tick"];
        if (tick.valid())
        {
            auto r = tick(m_timeDt);
            if (!r.valid())
            {
                sol::error err = r;
                Logger::Error("Luaエラー（time timer）: {}", err.what());
            }
        }
    }

    sol::protected_function fn = (*m_lua)["OnUpdate"];
    if (fn.valid())
    {
        auto result = fn(m_timeDt);
        if (!result.valid())
        {
            sol::error err = result;
            m_lastError = err.what();
            Logger::Error("Luaエラー（OnUpdate）: {}", m_lastError);
        }
    }
}

void ScriptEngine::AttachScriptToEntity(entt::entity e, const std::string& scriptPath)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e)) return;

    LuaScript* existing = reg.try_get<LuaScript>(e);
    if (existing)
    {
        existing->scriptPath = scriptPath;
        existing->enabled    = true;
        existing->env.reset();
        existing->self.reset();
        existing->started    = false;
        existing->loadError  = false;
        existing->errorMessage.clear();
    }
    else
    {
        LuaScript ls;
        ls.scriptPath = scriptPath;
        reg.emplace<LuaScript>(e, std::move(ls));
    }
    Logger::Info("LuaScript attached: entity={} path={}",
                 static_cast<u32>(e), scriptPath);
}

void ScriptEngine::DetachScriptFromEntity(entt::entity e)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return;
    reg.remove<LuaScript>(e);
    Logger::Info("LuaScript detached: entity={}", static_cast<u32>(e));
}

const std::vector<ScriptPropDef>& ScriptEngine::GetPropertySchema(const std::string& scriptPath)
{
    auto it = m_propSchemaCache.find(scriptPath);
    if (it != m_propSchemaCache.end()) return it->second;

    std::vector<ScriptPropDef> defs;
    ParsePropertySchema(scriptPath, defs);
    auto res = m_propSchemaCache.emplace(scriptPath, std::move(defs));
    return res.first->second;
}

void ScriptEngine::InvalidatePropertySchema(const std::string& scriptPath)
{
    m_propSchemaCache.erase(scriptPath);
}

void ScriptEngine::ParsePropertySchema(const std::string& scriptPath,
                                       std::vector<ScriptPropDef>& out)
{
    out.clear();
    if (!m_lua || scriptPath.empty()) return;

    namespace fs = std::filesystem;
    fs::path abs = fs::path(m_assetsDir) / scriptPath;

    // VFS 経由でスクリプトを読む。空ならディスクにフォールバック。
    auto vfsCode = vfs::ReadAsset(scriptPath);
    std::string code;
    if (!vfsCode.empty())
    {
        code.assign(vfsCode.begin(), vfsCode.end());
    }
    else
    {
        std::ifstream ifs(abs.string(), std::ios::binary);
        if (!ifs) return;
        std::stringstream ss; ss << ifs.rdbuf();
        code = ss.str();
    }

    // "properties" を含まないスクリプト（旧来のコントローラ等）は実行しない＝副作用ゼロ。
    if (code.find("properties") == std::string::npos) return;

    // 独立した環境で実行し properties テーブルだけ読む。エンジン API(scene/fx/net 等)は
    // 意図的に見せない: この解析はエディタ中(Play 外)にも走るため、トップレベルで
    // エンジン API を呼ぶスクリプトを本物のグローバルへフォールバックさせると、
    // 未初期化サブシステム経由のネイティブクラッシュを踏み得る。純関数ライブラリと
    // スカラー定数(KEY_* 等)だけを写した砂箱で実行し、それ以外の呼び出しは
    // Lua エラー → 下の警告ログに落とす。
    sol::environment env(*m_lua, sol::create);
    {
        static const char* kSafeLibs[] = { "math", "string", "table", "pairs", "ipairs",
                                           "tostring", "tonumber", "type", "select", "next",
                                           "unpack", "rawget", "rawset", "setmetatable",
                                           "getmetatable", "error", "pcall" };
        sol::table g = m_lua->globals();
        for (const char* k : kSafeLibs)
        {
            sol::object o = g[k];
            if (o.valid()) env[k] = o;
        }
        // 数値/文字列/bool のグローバル定数(KEY_A 等)はそのまま見せる
        for (auto& [key, value] : g)
        {
            const sol::type t = value.get_type();
            if (t == sol::type::number || t == sol::type::string || t == sol::type::boolean)
                env[key] = value;
        }
        env["print"] = [](sol::variadic_args va) {
            std::string line;
            for (auto v : va)
            {
                if (!line.empty()) line += "\t";
                line += v.get<sol::object>().is<std::string>()
                        ? v.get<std::string>() : std::string("(non-string)");
            }
            Logger::Info("[lua properties] {}", line);
        };
    }
    auto r = m_lua->safe_script(code, env, sol::script_pass_on_error);
    if (!r.valid())
    {
        sol::error err = r;
        Logger::Warn("スクリプトの properties 解析に失敗（{}）: {}", scriptPath, err.what());
        return;
    }

    sol::object propsObj = env["properties"];
    if (!propsObj.is<sol::table>()) return;
    sol::table props = propsObj.as<sol::table>();

    auto getStr = [](sol::table& t, const char* key, const std::string& dflt) -> std::string {
        sol::object o = t[key];
        return o.is<std::string>() ? o.as<std::string>() : dflt;
    };
    auto typeFromStr = [](const std::string& s) -> ScriptPropType {
        if (s == "int")    return ScriptPropType::Int;
        if (s == "bool")   return ScriptPropType::Bool;
        if (s == "string") return ScriptPropType::String;
        if (s == "vec3")   return ScriptPropType::Vec3;
        if (s == "color")  return ScriptPropType::Color;
        if (s == "entity") return ScriptPropType::Entity;
        return ScriptPropType::Float;
    };
    auto readVec = [](const sol::object& o, DirectX::XMFLOAT3 dflt) -> DirectX::XMFLOAT3 {
        if (o.is<sol::table>())
        {
            sol::table a = o.as<sol::table>();
            return { a.get_or(1, dflt.x), a.get_or(2, dflt.y), a.get_or(3, dflt.z) };
        }
        return dflt;
    };

    for (std::size_t i = 1; ; ++i)
    {
        sol::object item = props[i];
        if (!item.valid()) break;
        if (!item.is<sol::table>()) continue;
        sol::table t = item.as<sol::table>();

        ScriptPropDef d;
        d.name = getStr(t, "name", std::string{});
        if (d.name.empty()) continue;
        d.type  = typeFromStr(getStr(t, "type", "float"));
        d.label = getStr(t, "label", d.name);
        d.def.name = d.name;
        d.def.type = d.type;

        sol::object dv = t["default"];
        switch (d.type)
        {
        case ScriptPropType::Float:
        case ScriptPropType::Int:
            d.def.num = dv.is<double>() ? dv.as<double>() : 0.0; break;
        case ScriptPropType::Bool:
            d.def.b = dv.is<bool>() ? dv.as<bool>() : false; break;
        case ScriptPropType::String:
            d.def.str = dv.is<std::string>() ? dv.as<std::string>() : std::string{}; break;
        case ScriptPropType::Vec3:
            d.def.vec = readVec(dv, {0.0f, 0.0f, 0.0f}); break;
        case ScriptPropType::Color:
            d.def.vec = readVec(dv, {1.0f, 1.0f, 1.0f}); break;
        case ScriptPropType::Entity:
            d.def.str = dv.is<std::string>() ? dv.as<std::string>() : std::string{}; break;
        }

        sol::object mn = t["min"], mx = t["max"];
        if (mn.is<double>() && mx.is<double>())
        {
            d.hasRange = true;
            d.minVal = static_cast<float>(mn.as<double>());
            d.maxVal = static_cast<float>(mx.as<double>());
        }

        out.push_back(std::move(d));
    }
}

void ScriptEngine::ReloadScript(entt::entity e)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return;
    auto& ls = reg.get<LuaScript>(e);
    ls.env.reset();
    ls.self.reset();
    ls.started   = false;
    ls.loadError = false;
    ls.errorMessage.clear();
    InvalidatePropertySchema(ls.scriptPath);   // ファイルが書き換わった可能性 → 再解析
    Logger::Info("LuaScript reload queued: entity={}", static_cast<u32>(e));
    // 実際の再構築は UpdateAttachedScripts のループで行う
}

int ScriptEngine::ReloadChangedScripts()
{
    if (!m_scene) return 0;
    namespace fs = std::filesystem;
    auto& reg  = m_scene->GetRegistry();
    auto  view = reg.view<LuaScript>();

    // 1) 使用中の scriptPath ごとに 1 回だけ mtime を見る（同じ .lua を複数エンティティが使う）。
    std::unordered_map<std::string, bool> changed;   // scriptPath → 書き換わったか
    for (auto e : view)
    {
        const auto& ls = view.get<LuaScript>(e);
        if (ls.scriptPath.empty() || changed.count(ls.scriptPath)) continue;

        std::error_code ec;
        const auto t = fs::last_write_time(fs::path(m_assetsDir) / ls.scriptPath, ec);
        // pak 運用（封印ランタイム）や編集中の一時的な消失では stat が失敗する。黙って見送る。
        if (ec) { changed.emplace(ls.scriptPath, false); continue; }

        const int64_t stamp = t.time_since_epoch().count();
        auto [it, inserted] = m_scriptMtimes.try_emplace(ls.scriptPath, stamp);
        const bool diff = !inserted && it->second != stamp;   // 初見は基準を作るだけでリロードしない
        it->second = stamp;
        changed.emplace(ls.scriptPath, diff);
    }

    // 2) 書き換わった .lua を使うエンティティを作り直す。
    //    ReloadScript が loadError を落とすので、エラーで死んでいたスクリプトも保存だけで復活する。
    int n = 0;
    for (auto e : view)
    {
        const auto& ls = view.get<LuaScript>(e);
        auto it = changed.find(ls.scriptPath);
        if (it == changed.end() || !it->second) continue;
        ReloadScript(e);   // 実際の再構築は次の UpdateAttachedScripts
        ++n;
    }
    if (n > 0) Logger::Info("Lua ホットリロード: {} 個のコンポーネントを作り直します", n);
    return n;
}

int ScriptEngine::ReloadAllScripts(const std::string& onlyPath)
{
    if (!m_scene) return 0;
    auto& reg = m_scene->GetRegistry();

    // ★mtime の基準も捨てる。ReloadChangedScripts は「初見は基準を作るだけ」なので、
    //   ここで消しておかないと、リロード後に同じファイルをもう一度編集したときの
    //   1 回目の変更を取りこぼす。
    if (onlyPath.empty()) m_scriptMtimes.clear();
    else                  m_scriptMtimes.erase(onlyPath);

    int n = 0;
    for (auto [e, ls] : reg.view<LuaScript>().each())
    {
        if (ls.scriptPath.empty()) continue;
        if (!onlyPath.empty() && ls.scriptPath != onlyPath) continue;
        ReloadScript(e);   // 実際の再構築は次の UpdateAttachedScripts
        ++n;
    }
    if (n > 0) Logger::Info("Lua 強制リロード: {} 個のコンポーネントを作り直します", n);
    return n;
}

std::vector<ScriptEngine::ScriptError> ScriptEngine::CollectScriptErrors()
{
    std::vector<ScriptError> out;
    if (!m_scene) return out;
    auto& reg  = m_scene->GetRegistry();
    auto  view = reg.view<LuaScript>();
    for (auto e : view)
    {
        const auto& ls = view.get<LuaScript>(e);
        if (!ls.loadError) continue;
        const auto* tag = reg.try_get<NameTag>(e);
        out.push_back({ static_cast<u32>(e), tag ? tag->name : std::string{},
                        ls.scriptPath, ls.errorMessage });
    }
    return out;
}

bool ScriptEngine::CheckLuaSyntax(const std::string& code, std::string& err)
{
    if (!m_lua) { err = "lua state not initialized"; return false; }
    // load はコンパイルするだけで実行しない(副作用なし)。構文エラーだけ拾える。
    sol::load_result lr = m_lua->load(code);
    if (lr.valid()) return true;
    sol::error e = lr;
    err = e.what();
    return false;
}

bool ScriptEngine::EvalLua(const std::string& code, std::string& resultStr, std::string& err)
{
    resultStr.clear();
    err.clear();
    if (!m_lua) { err = "lua state not initialized"; return false; }
    // 関数で包んで実行する: code が文でも式(return 込み)でも受け付けたいのと、
    // 戻り値を C++ 側では protected_function_result の多値インデックスに頼らず、
    // Lua 自身の tostring() で文字列化してから受け取ることで sol2 の型変換を単純にする。
    const std::string wrapped =
        "local __eval_fn = function()\n" + code + "\nend\n"
        "local __r = { __eval_fn() }\n"
        "if #__r > 0 then return tostring(__r[1]) end\n"
        "return nil\n";
    sol::protected_function_result result = m_lua->safe_script(wrapped, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error e = result;
        err = e.what();
        return false;
    }
    sol::optional<std::string> s = result;
    if (s) resultStr = *s;
    return true;
}

std::vector<std::string> ScriptEngine::GetCompletions(const std::string& line)
{
    std::vector<std::string> out;
    if (!m_lua) return out;

    // 行末の補完対象トークンを切り出す(識別子と . : で構成される末尾部分)。
    // 例: "log(time.vi" → token="time.vi" → base="time", partial="vi"
    size_t start = line.size();
    while (start > 0)
    {
        const char c = line[start - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':') --start;
        else break;
    }
    const std::string token = line.substr(start);
    const size_t sepPos = token.find_last_of(".:");
    const std::string basePath = (sepPos == std::string::npos) ? "" : token.substr(0, sepPos);
    const std::string partial  = (sepPos == std::string::npos) ? token : token.substr(sepPos + 1);
    if (basePath.empty() && partial.empty()) return out;

    auto collect = [&](sol::table t) {
        for (auto& kv : t)
        {
            if (!kv.first.is<std::string>()) continue;
            std::string key = kv.first.as<std::string>();
            if (key.rfind("__", 0) == 0) continue;                       // メタ/内部キー
            if (basePath.empty() && key.rfind('_', 0) == 0) continue;    // 内部グローバル(_timers 等)
            if (!partial.empty() && key.rfind(partial, 0) != 0) continue;
            out.push_back(std::move(key));
        }
    };

    if (basePath.empty())
    {
        collect(m_lua->globals());
    }
    else
    {
        // basePath を . : で分割して globals からテーブルを辿る(userdata は末端のみ対応)
        sol::object cur = m_lua->globals();
        size_t p = 0;
        while (p <= basePath.size())
        {
            const size_t q = basePath.find_first_of(".:", p);
            const std::string part = basePath.substr(p, (q == std::string::npos ? basePath.size() : q) - p);
            if (part.empty()) return out;
            if (cur.get_type() != sol::type::table) return out;   // 途中に userdata が挟まる形は非対応
            cur = cur.as<sol::table>()[part];
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (cur.get_type() == sol::type::table)
        {
            collect(cur.as<sol::table>());
        }
        else if (cur.get_type() == sol::type::userdata)
        {
            // usertype(scene/input/physics 等)はメソッドがメタテーブルに入っている
            sol::object mt = cur.as<sol::userdata>()[sol::metatable_key];
            if (mt.get_type() == sol::type::table) collect(mt.as<sol::table>());
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void ScriptEngine::ClearBlackboard()
{
    m_blackboard.clear();
}

void ScriptEngine::OnPlayStart()
{
    auto& reg = m_scene->GetRegistry();

    // time API リセット（経過時間/フレーム/スケール/Lua タイマー）
    m_timeElapsed = 0.0; m_timeUnscaled = 0.0;
    m_timeScale = 1.0f; m_timeDt = 0.0f; m_timeUnscaledDt = 0.0f; m_timeFrame = 0;
    if (m_lua)
    {
        sol::protected_function reset = (*m_lua)["__time_reset"];
        if (reset.valid()) reset();
    }

    // イベントバスの購読をクリア（前回 Play のリスナーが残らないように）。
    // 通常は Application が OnPlayStart 直前に m_eventBus.Clear() を呼ぶが、念のため。
    if (m_eventBus) m_eventBus->Clear();

    // パーティクル放出器のランタイム状態を初期化（playOnStart で放出 ON/OFF を決める）
    {
        auto peView = reg.view<ParticleEmitter>();
        for (auto e : peView)
        {
            auto& pe = peView.get<ParticleEmitter>(e);
            pe._active    = pe.playOnStart;
            pe._age       = 0.0f;
            pe._emitAccum = 0.0f;
        }
    }
    // 連番アニメ(フリップブック)の再生位置を初期化。
    // ★これが無いと、エディタで放置していた分だけ _animT が進んだまま Play に入る。
    //   Application::Update はエディタ中もプレビュー再生で _animT を足し続けているので、
    //   単発(animMode=1)の爆発スプライトは「Play した瞬間もう終わっている」＝最終フレームで
    //   静止し、isSpriteAnimDone も初回フレームから true になる。
    //   Stop 側はシーンを JSON から作り直すので直る＝Stop→Play を素早く繰り返すと直り、
    //   放置してから Play すると壊れる、という一番たちの悪い出方をしていた。
    for (auto [e, sp] : reg.view<Sprite2D>().each())  sp._animT  = 0.0f;
    for (auto [e, img] : reg.view<UIImage>().each())  img._animT = 0.0f;
    for (auto [e, mr] : reg.view<MeshRenderer>().each()) mr._animT = 0.0f;

    // Trigger のランタイム状態を初期化
    {
        auto trView = reg.view<Trigger>();
        for (auto e : trView)
        {
            auto& tr = trView.get<Trigger>(e);
            tr._wasInside = false;
            tr._firedOnce = false;
        }
    }

    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        ls.env.reset();
        ls.self.reset();
        ls.started   = false;
        ls.loadError = false;
        // ★errorMessage も消す。残すと、直したあとの Play でも Inspector と
        //   get_script_errors に前回のエラー文が出続けて「まだ壊れている」と誤読させる。
        ls.errorMessage.clear();
        if (ls.scriptPath.empty()) continue;
        const auto& schema = GetPropertySchema(ls.scriptPath);
        InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError, &schema);
    }
    Logger::Info("ScriptEngine: OnPlayStart done");
}

void ScriptEngine::OnPlayStop()
{
    auto& reg = m_scene->GetRegistry();
    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        ls.env.reset();
        ls.self.reset();
        ls.started   = false;
        // loadError は残して Inspector に見せる
    }
    // Play 中に登録された Lua ハンドラ（sol::function を保持）を EventBus から除去する。
    // Lua state がここで無効化されるため、残留ハンドラが後続 Flush/Emit で呼ばれると UAF になる。
    // Application::EnterEditorMode でも Clear を呼ぶが、OnPlayStop 経路を一本化して確実に除去する。
    if (m_eventBus) m_eventBus->Clear();
    Logger::Info("ScriptEngine: OnPlayStop done");
}

void ScriptEngine::UpdateAttachedScripts(f32 dt)
{
    auto& reg = m_scene->GetRegistry();

    // ★エンティティ一覧を先に写してから回す。
    //   OnUpdate の中で scene:remove(other) を呼ぶ（弾が当たった敵を消す等）と、
    //   反復中の storage が swap_and_pop で詰め替わる。entt の単一型ビューは
    //   末尾→先頭へ走るので、「まだ今フレーム走っていない手前の要素」を消すと
    //   **もう走り終えた末尾の要素がその位置へ降りてきて再訪される**＝
    //   無関係な別キャラの OnUpdate が同フレームに 2 回走る（移動が 2 倍進む、
    //   弾が 2 発出る、ダメージが二重に入る）。
    //   同ファイルの UpdateTriggers は既に「列挙中の再入を避けるため」溜めてから
    //   消す流儀にしてあり、こちらだけ揃っていなかった。
    std::vector<entt::entity> targets;
    {
        auto view = reg.view<LuaScript>();
        targets.reserve(view.size());
        for (auto e : view) targets.push_back(e);
    }

    for (auto e : targets)
    {
        // 写した後に消えている可能性があるので、毎回引き直す（get より先に確認する）。
        if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) continue;
        auto& ls = reg.get<LuaScript>(e);
        if (!ls.enabled) continue;
        if (ls.loadError) continue;
        if (ls.scriptPath.empty()) continue;

        // env 未構築（Play 中に Attach された or Reload された） → 初期化
        if (!ls.env || !ls.started)
        {
            const auto& schema = GetPropertySchema(ls.scriptPath);
            if (!InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError, &schema))
                continue;
        }

        auto* env  = static_cast<sol::environment*>(ls.env.get());
        auto* self = static_cast<sol::table*>(ls.self.get());
        if (!env || !self) continue;

        // self.transform のポインタを最新化（コンポーネントが再配置される場合に備える）
        if (auto* tf = reg.try_get<Transform>(e))
            (*self)["transform"] = tf;
        (*self)["enabled"] = ls.enabled;

        // raw_get = フォールバック無し。globals の game.lua 製 OnUpdate を誤って拾わない
        sol::object fnObj = env->raw_get<sol::object>("OnUpdate");
        if (fnObj.get_type() != sol::type::function) continue;
        sol::protected_function fn = fnObj;
        auto result = fn(*self, dt * m_timeScale);   // time.setScale がコンポーネントにも効く
        if (!result.valid())
        {
            sol::error err = result;
            m_lastError = err.what();
            Logger::Error("Luaエラー（OnUpdate, entity={}）: {}",
                          static_cast<u32>(e), m_lastError);
            ls.loadError = true; ls.errorMessage = m_lastError;
        }
    }
}

void ScriptEngine::UpdateTriggers(f32 /*dt*/)
{
    if (!m_scene || !m_lua) return;
    auto& reg = m_scene->GetRegistry();

    std::vector<entt::entity> toDestroy;

    // 1 アクションを実行する
    auto exec = [&](entt::entity self, entt::entity defaultTarget, const TriggerAction& a)
    {
        // guid が正、名前はフォールバック（旧データ / guid の指す先が消えている場合）。
        entt::entity at = (a.targetGuid == 0 && a.target.empty())
                            ? defaultTarget
                            : ResolveEntityRef(reg, a.targetGuid, a.target);

        switch (static_cast<TriggerActionType>(a.type))
        {
        case TriggerActionType::Enable:
            if (at != entt::null) if (auto* ls = reg.try_get<LuaScript>(at)) ls->enabled = true;
            break;
        case TriggerActionType::Disable:
            if (at != entt::null) if (auto* ls = reg.try_get<LuaScript>(at)) ls->enabled = false;
            break;
        case TriggerActionType::Destroy:
            if (at != entt::null) toDestroy.push_back(at);
            break;
        case TriggerActionType::Move:
            if (at != entt::null) if (auto* tf = reg.try_get<Transform>(at))
            { tf->position.x += a.vec.x; tf->position.y += a.vec.y; tf->position.z += a.vec.z; }
            break;
        case TriggerActionType::PlayEffect:
            if (at != entt::null) if (auto* pe = reg.try_get<ParticleEmitter>(at))
            { pe->_active = true; pe->_age = 0.0f; pe->_emitAccum = 0.0f; }
            break;
        case TriggerActionType::StopEffect:
            if (at != entt::null) if (auto* pe = reg.try_get<ParticleEmitter>(at)) pe->_active = false;
            break;
        case TriggerActionType::PlaySound:
            if (at != entt::null) if (auto* as = reg.try_get<AudioSource>(at))
            { if (m_audio && !as->clipPath.empty()) m_audio->PlaySFX(as->clipPath, as->loop, as->volume); }
            break;
        case TriggerActionType::LoadScene:
            if (!a.str.empty() && m_loadSceneCb) m_loadSceneCb(a.str);
            break;
        case TriggerActionType::FadeToScene:
            if (!a.str.empty() && m_transitionCb)
                m_transitionCb(a.str, 0, a.num > 0.0 ? static_cast<float>(a.num) : 0.6f);
            break;
        case TriggerActionType::SetProperty:
            if (at != entt::null && !a.str.empty()) if (auto* ls = reg.try_get<LuaScript>(at))
            {
                if (ls->self)
                {
                    auto* tbl = static_cast<sol::table*>(ls->self.get());
                    (*tbl)[a.str] = a.num;
                }
            }
            break;
        case TriggerActionType::EmitEvent:
            if (!a.str.empty() && m_eventBus)
            {
                // フレーム末の Flush で配信（列挙中の再入を避けるため Post）。
                EngineEvent ev;
                ev.name   = a.str;
                ev.source = self;            // Trigger を持つエンティティ
                ev.other  = defaultTarget;   // 反応した対象エンティティ
                ev.set("value", a.num);
                if (!a.target.empty()) ev.set("target", a.target);
                m_eventBus->Post(std::move(ev));
            }
            break;
        }
    };

    auto view = reg.view<Trigger, Transform>();
    for (auto e : view)
    {
        auto& tr = view.get<Trigger>(e);
        if (tr._firedOnce) continue;

        const auto& tf = view.get<Transform>(e);
        DirectX::XMMATRIX w = ComputeWorldMatrix(reg, e);
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center, w.r[3]);
        center.x += tr.offset.x; center.y += tr.offset.y; center.z += tr.offset.z;

        // 反応対象。guid が正、名前はフォールバック、どちらも空なら "Player" の暗黙指定。
        const std::string targetName = tr.filter.empty() ? std::string("Player") : tr.filter;
        entt::entity te = ResolveEntityRef(reg, tr.filterGuid, targetName);

        bool inside = false;
        if (te != entt::null && reg.all_of<Transform>(te))
        {
            DirectX::XMMATRIX tw = ComputeWorldMatrix(reg, te);
            DirectX::XMFLOAT3 tp;
            DirectX::XMStoreFloat3(&tp, tw.r[3]);
            if (tr.shape == static_cast<int>(TriggerShape::Sphere))
            {
                float sc = tf.scale.x;
                if (tf.scale.y > sc) sc = tf.scale.y;
                if (tf.scale.z > sc) sc = tf.scale.z;
                float r  = tr.radius * sc;
                float dx = tp.x - center.x, dy = tp.y - center.y, dz = tp.z - center.z;
                inside = (dx*dx + dy*dy + dz*dz) <= r*r;
            }
            else
            {
                float hx = tr.halfExtents.x * tf.scale.x;
                float hy = tr.halfExtents.y * tf.scale.y;
                float hz = tr.halfExtents.z * tf.scale.z;
                inside = std::fabs(tp.x - center.x) <= hx
                      && std::fabs(tp.y - center.y) <= hy
                      && std::fabs(tp.z - center.z) <= hz;
            }
        }

        const bool enter = inside && !tr._wasInside;
        const bool exit  = !inside && tr._wasInside;
        tr._wasInside = inside;

        for (const auto& a : tr.actions)
        {
            const bool fire =
                (a.when == static_cast<int>(TriggerWhen::Enter) && enter) ||
                (a.when == static_cast<int>(TriggerWhen::Exit)  && exit)  ||
                (a.when == static_cast<int>(TriggerWhen::Stay)  && inside);
            if (fire) exec(e, te, a);
        }

        if (tr.once && enter) tr._firedOnce = true;
    }

    for (auto d : toDestroy)
        if (reg.valid(d)) m_scene->Remove(Entity(d, &reg));
}

void ScriptEngine::Shutdown()
{
    // Lua state リセット前に EventBus を Clear して、Lua ラムダ（sol::function を
    // キャプチャした購読ハンドラ）の dangling 参照を防ぐ。
    if (m_eventBus) m_eventBus->Clear();

    m_propSchemaCache.clear();
    m_scriptMtimes.clear();   // 次の Play で mtime の基準を取り直す
    if (m_lua)
    {
        m_lua.reset();
        Logger::Info("ScriptEngine shutdown");
    }
}

} // namespace dx12e
