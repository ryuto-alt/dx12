#include "scripting/ScriptEngine.h"
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
#include "renderer/Mesh.h"
#include "renderer/ParticleSystem.h"
#include "renderer/GpuParticleSystem.h"
#include "input/InputSystem.h"
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

// 名前からエンティティを引く（NameTag 一致。先頭一致を返す）。見つからなければ entt::null。
entt::entity FindEntityByName(entt::registry& reg, const std::string& name)
{
    if (name.empty()) return entt::null;
    auto view = reg.view<NameTag>();
    for (auto e : view)
        if (view.get<NameTag>(e).name == name) return e;
    return entt::null;
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
            // 参照先エンティティ名を解決して Entity を注入（self.<name>:isValid() で確認できる）。
            const std::string& refName = ov ? ov->str : def.def.str;
            entt::entity re = FindEntityByName(reg, refName);
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
    m_lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::io);

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
            if (type == "Trigger")            return e.HasComponent<Trigger>();
            if (type == "UICanvas")           return e.HasComponent<UICanvas>();
            if (type == "UIRect")             return e.HasComponent<UIRect>();
            if (type == "UIImage")            return e.HasComponent<UIImage>();
            if (type == "UIText")             return e.HasComponent<UIText>();
            if (type == "UIButton")           return e.HasComponent<UIButton>();
            if (type == "UIAnimator")         return e.HasComponent<UIAnimator>();
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
        // --- ゲーム内UI（retained-mode）: スコア表示・HPバー等をスクリプトから書き換える ---
        // UIText::text を書き換える(スコア・残機・メッセージ)。UIText が無ければ何もしない。
        "setUiText", [](Scene& s, Entity& e, const std::string& text) {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIText>(e.GetHandle())) return;
            reg.get<UIText>(e.GetHandle()).text = text;
        },
        // UIText::text を読む。UIText が無ければ空文字列。
        "getUiText", [](Scene& s, Entity& e) -> std::string {
            auto& reg = s.GetRegistry();
            if (!reg.all_of<UIText>(e.GetHandle())) return std::string();
            return reg.get<UIText>(e.GetHandle()).text;
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
        // params: { dx=, dy=(相対移動px), scale=(視覚拡縮), alpha=(視覚透明度0..1),
        //           duration=0.3, delay=0, easing="out" }
        // easing: "linear"/"in"/"out"/"inOut"/"back"(勢い)/"bounce"/"elastic"(または 0..6 の数値)
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
                }
                else if (eo.is<int>())
                {
                    t.easing = std::clamp(eo.as<int>(), 0, 6);
                }
            }
            const float dx = params.get_or("dx", 0.0f);
            const float dy = params.get_or("dy", 0.0f);
            if (dx != 0.0f || dy != 0.0f) { t.hasMove = true; t.moveDelta = {dx, dy}; }
            if (sol::object v = params["scale"]; v.is<float>())
            { t.hasScale = true; t.scaleTo = (std::max)(0.0f, v.as<float>()); }
            if (sol::object v = params["alpha"]; v.is<float>())
            { t.hasAlpha = true; t.alphaTo = std::clamp(v.as<float>(), 0.0f, 1.0f); }
            if (!t.hasMove && !t.hasScale && !t.hasAlpha) return;
            reg.get_or_emplace<UITweenState>(h).tweens.push_back(t);
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
        "setPadVibrationTimed", &InputSystem::SetPadVibrationTimed
    );

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
        "playBGM",         &AudioSystem::PlayBGM,
        "stopBGM",         &AudioSystem::StopBGM,
        "pauseBGM",        &AudioSystem::PauseBGM,
        "resumeBGM",       &AudioSystem::ResumeBGM,
        "playSFX",         &AudioSystem::PlaySFX,
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

    // --- ゲーム制御（Application が注入したコールバック経由）---
    lua["loadScene"] = [this](const std::string& rel) { if (m_loadSceneCb) m_loadSceneCb(rel); };
    lua["nextScene"] = [this]() { if (m_nextSceneCb) m_nextSceneCb(); };
    lua["quit"]      = [this]() { if (m_quitCb) m_quitCb(); };
    // フェード等のトランジション付きシーン切替（type: 0=Fade,1=Wipe,2=Circle,3=縦Wipe）
    lua["fadeToScene"] = [this](const std::string& rel, sol::optional<float> dur) {
        if (m_transitionCb) m_transitionCb(rel, 0, dur.value_or(0.6f));
    };
    lua["transitionToScene"] = [this](const std::string& rel, int type, sol::optional<float> dur) {
        if (m_transitionCb) m_transitionCb(rel, type, dur.value_or(0.6f));
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
        "setPosition", [](PhysicsSystem& ps, Entity& e, XMFLOAT3 pos) {
            if (!e.HasComponent<RigidBody>()) return;
            ps.SetPosition(e.GetComponent<RigidBody>().bodyId, pos);
        },
        "raycast", [](PhysicsSystem& ps, XMFLOAT3 origin, XMFLOAT3 dir,
                       float maxDist) -> RaycastHit {
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
            if (amount && *amount > 0.0f) cc.jumpSpeed = *amount;
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

function __time_reset()
  time._timers, time._nextId = {}, 1
  -- time.video はメソッドも入っているテーブルなので丸ごと差し替えず状態フィールドだけ戻す
  local v = time.video
  v._active, v._t, v._dur, v._consumed, v._skipCost = false, 0, 0, 0, 1.0
  v._offsets = {}
  time._ent = {}
end
function __time_tick(dt)
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

    // 独立した環境で実行し properties テーブルだけ読む（グローバルへフォールバックするが書込は env 内）。
    sol::environment env(*m_lua, sol::create, m_lua->globals());
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
    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        if (!reg.valid(e)) continue;
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
        entt::entity at = a.target.empty() ? defaultTarget : FindEntityByName(reg, a.target);

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
            { if (m_audio && !as->clipPath.empty()) m_audio->PlaySFX(as->clipPath, as->loop); }
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

        // 反応対象（filter 空なら "Player"）
        const std::string targetName = tr.filter.empty() ? std::string("Player") : tr.filter;
        entt::entity te = FindEntityByName(reg, targetName);

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
    if (m_lua)
    {
        m_lua.reset();
        Logger::Info("ScriptEngine shutdown");
    }
}

} // namespace dx12e
