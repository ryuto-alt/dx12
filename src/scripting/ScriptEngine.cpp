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
#include <filesystem>

namespace dx12e
{

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
            if (type == "Camera")             return e.HasComponent<CameraComponent>();
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
        "findEntity", &Scene::FindEntity
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

    // --- ユーティリティ ---
    lua["log"] = [](const std::string& msg) { Logger::Info("[Lua] {}", msg); };

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
        "addBoxCollider", [this](PhysicsSystem& /*ps*/, Entity& e,
                                 float hx, float hy, float hz) {
            auto& reg = m_scene->GetRegistry();
            BoxCollider col;
            col.halfExtents = { hx, hy, hz };
            reg.emplace_or_replace<BoxCollider>(e.GetHandle(), col);
        },
        "addSphereCollider", [this](PhysicsSystem& /*ps*/, Entity& e, float radius) {
            auto& reg = m_scene->GetRegistry();
            SphereCollider col;
            col.radius = radius;
            reg.emplace_or_replace<SphereCollider>(e.GetHandle(), col);
        },
        "addCapsuleCollider", [this](PhysicsSystem& /*ps*/, Entity& e,
                                     float radius, float halfHeight) {
            auto& reg = m_scene->GetRegistry();
            CapsuleCollider col;
            col.radius = radius;
            col.halfHeight = halfHeight;
            reg.emplace_or_replace<CapsuleCollider>(e.GetHandle(), col);
        },
        "addRigidBody", [this](PhysicsSystem& /*ps*/, Entity& e,
                               int motionTypeInt, float mass) {
            auto& reg = m_scene->GetRegistry();
            // 既存のボディがあれば何もしない（二重登録防止）
            auto* existing = reg.try_get<RigidBody>(e.GetHandle());
            if (existing && existing->bodyId != kInvalidBodyId) return;
            RigidBody rb;
            rb.motionType = static_cast<MotionType>(motionTypeInt);
            rb.mass = mass;
            reg.emplace_or_replace<RigidBody>(e.GetHandle(), rb);
            // ボディ登録は EnterPlayMode で一括実行（ここでは登録しない）
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

void ScriptEngine::Shutdown()
{
    if (m_lua)
    {
        m_lua.reset();
        Logger::Info("ScriptEngine shutdown");
    }
}

} // namespace dx12e
