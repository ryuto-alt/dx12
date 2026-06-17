#include "scripting/ScriptEngine.h"
#include "core/Logger.h"

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
#include "input/InputSystem.h"
#include "renderer/Camera.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsSystem.h"
#include "animation/Skeleton.h"
#include "animation/Animator.h"
#include "animation/AnimationClip.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/NodeGraph.h"

#include <DirectXMath.h>
#include <cfloat>
#include <cmath>
#include <filesystem>

namespace dx12e
{

namespace {

// LuaScript コンポーネントに env / self を構築し、OnStart を呼ぶ。
// 失敗時は loadError を true に、Logger::Error を出す。
// 戻り値: 成功 true
bool InitializeLuaScriptInstance(sol::state& lua,
                                  entt::registry& reg,
                                  entt::entity e,
                                  LuaScript& ls,
                                  const std::string& assetsDir,
                                  std::string& lastError)
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

    (*env)["self"] = *self;

    auto result = lua.safe_script_file(
        abs.string(), *env, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        lastError = err.what();
        Logger::Error("Lua load error (entity={} path={}): {}",
                      static_cast<u32>(e), ls.scriptPath, lastError);
        ls.loadError = true;
        return false;
    }

    ls.env       = env;
    ls.self      = self;
    ls.loadError = false;

    // OnStart(self) を呼ぶ
    sol::protected_function fn = (*env)["OnStart"];
    if (fn.valid())
    {
        auto r = fn(*self);
        if (!r.valid())
        {
            sol::error err = r;
            lastError = err.what();
            Logger::Error("Lua OnStart error (entity={}): {}",
                          static_cast<u32>(e), lastError);
            ls.loadError = true;
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

        // Component query
        "hasComponent", [](const Entity& e, const std::string& type) -> bool {
            if (type == "Transform")          return e.HasComponent<Transform>();
            if (type == "MeshRenderer")       return e.HasComponent<MeshRenderer>();
            if (type == "SkeletalAnimation")  return e.HasComponent<SkeletalAnimation>();
            if (type == "NodeAnimation")      return e.HasComponent<NodeAnimationComp>();
            if (type == "GridPlane")          return e.HasComponent<GridPlane>();
            if (type == "PointLight")         return e.HasComponent<PointLight>();
            if (type == "DirectionalLight")   return e.HasComponent<DirectionalLight>();
            if (type == "SpotLight")          return e.HasComponent<SpotLight>();
            if (type == "Camera")             return e.HasComponent<CameraComponent>();
            if (type == "AudioSource")        return e.HasComponent<AudioSource>();
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
            auto* device = s.GetDevice();
            if (!device) return;
            for (auto* mesh : mr.meshes)
            {
                if (mesh) mesh->SetVertexColor(*device, r, g, b, 1.0f);
            }
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
        "getMouseDeltaY",  &InputSystem::GetMouseDeltaY
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
        "setMouseSensitivity", &Camera::SetMouseSensitivity
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

    // --- ユーティリティ ---
    lua["log"] = [](const std::string& msg) { Logger::Info("[Lua] {}", msg); };

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
            return p;
        };

        auto fx = lua.create_named_table("fx");
        fx.set_function("burst", [this, buildParams](sol::object, sol::table t) {
            if (!m_particleSystem) return;
            auto p = buildParams(t); p.ring = false;
            m_particleSystem->Emit(p);
        });
        fx.set_function("ring", [this, buildParams](sol::object, sol::table t) {
            if (!m_particleSystem) return;
            auto p = buildParams(t); p.ring = true;
            m_particleSystem->Emit(p);
        });
        fx.set_function("pulse", [this](sol::object, sol::optional<float> amt) {
            if (m_particleSystem) m_particleSystem->AddPulse(amt.value_or(0.5f));
        });
        fx.set_function("clear", [this](sol::object) {
            if (m_particleSystem) m_particleSystem->Clear();
        });
    }

    RegisterPhysicsBindings();

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
        }
    );

    lua["physics"] = m_physics;
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

-- ============================================================
--  FX: ド派手パーティクルプリセット（fx:burst / fx:ring を包む）
--  どのゲームスクリプトからも FX.explosion(...) 等で呼べる。
--  色は 0..1、intensity>1 で HDR 白熱 → ブルームで光る。
-- ============================================================
FX = {}

-- 爆発（撃破など）: 火球コア + 白い飛散火花
function FX.explosion(x, y, z, scale, r, g, b)
  scale = scale or 1.0
  r = r or 1.0; g = g or 0.45; b = b or 0.12
  fx:burst{ x=x, y=y, z=z, count=math.floor(20*scale), spread=1, speed=7*scale, speedVar=0.5,
            size=0.55*scale, sizeEnd=0.02, life=0.5, lifeVar=0.35,
            r=r, g=g, b=b, rEnd=r*0.6, gEnd=g*0.4, bEnd=b*0.2,
            intensity=5, gravity=-5, drag=2.5, up=0.6 }
  fx:burst{ x=x, y=y, z=z, count=math.floor(10*scale), spread=1, speed=13*scale, speedVar=0.4,
            size=0.18*scale, sizeEnd=0.0, life=0.4, lifeVar=0.3,
            r=1, g=0.95, b=0.7, intensity=8, drag=1.5 }
end

-- 衝撃波リング（ノヴァ等）: XZ平面に等間隔で外へ
function FX.shockwave(x, y, z, count, speed, r, g, b)
  fx:ring{ x=x, y=y, z=z, count=count or 28, speed=speed or 16, speedVar=0.0,
           size=0.6, sizeEnd=0.05, life=0.55, lifeVar=0.0,
           r=r or 0.6, g=g or 1.0, b=b or 1.0, intensity=6, drag=1.2 }
end

-- 着弾火花（小さく速い）
function FX.spark(x, y, z, count, r, g, b)
  fx:burst{ x=x, y=y, z=z, count=count or 6, spread=1, speed=9, speedVar=0.5,
            size=0.16, sizeEnd=0.0, life=0.3, lifeVar=0.3,
            r=r or 0.6, g=g or 0.95, b=b or 1.0, intensity=7, drag=2 }
end

-- 立ち上る軌跡/オーラ点（1粒ずつ毎フレーム呼ぶ用）
function FX.trail(x, y, z, r, g, b)
  fx:burst{ x=x, y=y, z=z, count=1, spread=0.4, dy=1, speed=1.5, speedVar=0.5,
            size=0.22, sizeEnd=0.0, life=0.45, lifeVar=0.3,
            r=r or 1, g=g or 0.9, b=b or 0.4, intensity=4, gravity=2, drag=1 }
end

-- レベルアップ超新星: 金リング + 大量火花 + 画面パルス
function FX.supernova(x, y, z, scale)
  scale = scale or 1.0
  fx:ring{ x=x, y=y, z=z, count=40, speed=20*scale, size=0.7, sizeEnd=0.05, life=0.7,
           r=1, g=0.85, b=0.3, intensity=8, drag=1 }
  fx:burst{ x=x, y=y, z=z, count=60, spread=1, speed=10*scale, speedVar=0.5,
            size=0.5, sizeEnd=0.0, life=0.8, lifeVar=0.4,
            r=1, g=0.95, b=0.6, rEnd=1, gEnd=0.5, bEnd=0.1,
            intensity=7, gravity=-4, drag=1.5, up=0.5 }
  fx:pulse(0.8)
end

-- ヒット時の画面パルス（クロマ + 放射ブラー）
function FX.hit(amount) fx:pulse(amount or 0.5) end
)LUA";

    auto r = m_lua->safe_script(kPrelude, sol::script_pass_on_error);
    if (!r.valid())
    {
        sol::error err = r;
        Logger::Error("ScriptEngine prelude error: {}", err.what());
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
    auto result = m_lua->safe_script_file(filePath, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        m_lastError = err.what();
        Logger::Error("Lua load error: {}", m_lastError);
    }
    else
    {
        m_lastError.clear();
        Logger::Info("Lua script loaded: {}", filePath);
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
            Logger::Error("Lua OnStart error: {}", m_lastError);
        }
        else
        {
            m_lastError.clear();
        }
    }
}

void ScriptEngine::CallOnUpdate(f32 dt)
{
    sol::protected_function fn = (*m_lua)["OnUpdate"];
    if (fn.valid())
    {
        auto result = fn(dt);
        if (!result.valid())
        {
            sol::error err = result;
            m_lastError = err.what();
            Logger::Error("Lua OnUpdate error: {}", m_lastError);
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

void ScriptEngine::ReloadScript(entt::entity e)
{
    auto& reg = m_scene->GetRegistry();
    if (!reg.valid(e) || !reg.all_of<LuaScript>(e)) return;
    auto& ls = reg.get<LuaScript>(e);
    ls.env.reset();
    ls.self.reset();
    ls.started   = false;
    ls.loadError = false;
    Logger::Info("LuaScript reload queued: entity={}", static_cast<u32>(e));
    // 実際の再構築は UpdateAttachedScripts のループで行う
}

void ScriptEngine::OnPlayStart()
{
    auto& reg = m_scene->GetRegistry();
    auto view = reg.view<LuaScript>();
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        ls.env.reset();
        ls.self.reset();
        ls.started   = false;
        ls.loadError = false;
        if (ls.scriptPath.empty()) continue;
        InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError);
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
            if (!InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError))
                continue;
        }

        auto* env  = static_cast<sol::environment*>(ls.env.get());
        auto* self = static_cast<sol::table*>(ls.self.get());
        if (!env || !self) continue;

        // self.transform のポインタを最新化（コンポーネントが再配置される場合に備える）
        if (auto* tf = reg.try_get<Transform>(e))
            (*self)["transform"] = tf;
        (*self)["enabled"] = ls.enabled;

        sol::protected_function fn = (*env)["OnUpdate"];
        if (!fn.valid()) continue;
        auto result = fn(*self, dt);
        if (!result.valid())
        {
            sol::error err = result;
            m_lastError = err.what();
            Logger::Error("Lua OnUpdate error (entity={}): {}",
                          static_cast<u32>(e), m_lastError);
            ls.loadError = true;
        }
    }
}

void ScriptEngine::Shutdown()
{
    if (m_lua)
    {
        m_lua.reset();
        Logger::Info("ScriptEngine shutdown");
    }
}

} // namespace dx12e
