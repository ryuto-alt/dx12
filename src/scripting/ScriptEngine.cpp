#include "scripting/ScriptEngine.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4244 4267 4996)
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#pragma warning(pop)

#include "scene/Scene.h"
#include "scene/Entity.h"
#include "scene/Transform.h"
#include "renderer/Mesh.h"
#include "input/InputSystem.h"
#include "renderer/Camera.h"
#include "animation/Animator.h"
#include "animation/AnimationClip.h"

#include <DirectXMath.h>
#include <filesystem>

namespace dx12e
{

ScriptEngine::ScriptEngine() = default;
ScriptEngine::~ScriptEngine() { Shutdown(); }

void ScriptEngine::Initialize(Scene* scene, InputSystem* input, Camera* camera,
                               const std::string& assetsDir)
{
    m_scene     = scene;
    m_input     = input;
    m_camera    = camera;
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
        "name",          &Entity::name,
        "transform",     &Entity::transform,
        "hasSkeleton",   sol::readonly(&Entity::hasSkeleton),
        "useGridShader", sol::readonly(&Entity::useGridShader),
        "playAnim", [](Entity& e, int clipIndex, float blendDuration) {
            if (e.animator && clipIndex >= 0 &&
                clipIndex < static_cast<int>(e.animClips.size()))
            {
                e.animator->CrossFadeTo(e.animClips[clipIndex].get(), blendDuration);
            }
        },
        "playAnimByName", [](Entity& e, const std::string& name, float blendDuration) {
            if (!e.animator) return;
            for (const auto& clip : e.animClips)
            {
                if (clip->GetName() == name)
                {
                    e.animator->CrossFadeTo(clip.get(), blendDuration);
                    return;
                }
            }
        },
        "setLooping", [](Entity& e, bool loop) {
            if (e.animator) e.animator->SetLooping(loop);
        },
        "getAnimCount", [](const Entity& e) -> int {
            return static_cast<int>(e.animClips.size());
        },
        "getAnimName", [](const Entity& e, int index) -> std::string {
            if (index >= 0 && index < static_cast<int>(e.animClips.size()))
                return e.animClips[index]->GetName();
            return "";
        }
    );

    // --- Scene ---
    lua.new_usertype<Scene>("Scene",
        "spawn", [](Scene& s, const std::string& name, const std::string& modelPath,
                     DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 rot, DirectX::XMFLOAT3 scale) -> Entity* {
            return s.Spawn(name, modelPath, pos, rot, scale);
        },
        "spawnPlane", [](Scene& s, const std::string& name, DirectX::XMFLOAT3 pos,
                         float size, bool grid) -> Entity* {
            return s.SpawnPlane(name, pos, size, grid);
        },
        "spawnBox", [](Scene& s, const std::string& name, DirectX::XMFLOAT3 pos,
                       DirectX::XMFLOAT3 rot, DirectX::XMFLOAT3 scale) -> Entity* {
            return s.SpawnBox(name, pos, rot, scale);
        },
        "spawnSphere", [](Scene& s, const std::string& name, DirectX::XMFLOAT3 pos,
                          float radius) -> Entity* {
            return s.SpawnSphere(name, pos, radius);
        },
        "remove", &Scene::Remove,
        "getEntityCount", &Scene::GetEntityCount,
        "findEntity", [](Scene& s, const std::string& name) -> Entity* {
            for (const auto& e : s.GetEntities())
            {
                if (e->name == name) return e.get();
            }
            return nullptr;
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
        "getMoveSpeed",  &Camera::GetMoveSpeed,
        "setMoveSpeed",  &Camera::SetMoveSpeed,
        "getMouseSensitivity", &Camera::GetMouseSensitivity,
        "setMouseSensitivity", &Camera::SetMouseSensitivity
    );

    // --- グローバル変数 ---
    lua["scene"]  = m_scene;
    lua["input"]  = m_input;
    lua["camera"] = m_camera;
    lua["ASSETS"] = m_assetsDir;

    // --- キーコード定数 ---
    lua["KEY_W"]     = 'W';
    lua["KEY_A"]     = 'A';
    lua["KEY_S"]     = 'S';
    lua["KEY_D"]     = 'D';
    lua["KEY_SPACE"] = VK_SPACE;
    lua["KEY_SHIFT"] = VK_SHIFT;
    lua["KEY_TAB"]   = VK_TAB;
    lua["KEY_ESCAPE"] = VK_ESCAPE;
    lua["KEY_F1"]    = VK_F1;
    lua["KEY_F2"]    = VK_F2;
    lua["KEY_F3"]    = VK_F3;
    lua["KEY_RBUTTON"] = VK_RBUTTON;

    // --- ユーティリティ ---
    lua["log"] = [](const std::string& msg) { Logger::Info("[Lua] {}", msg); };

    Logger::Info("Lua bindings registered");
}

void ScriptEngine::LoadScript(const std::string& filePath)
{
    auto result = m_lua->safe_script_file(filePath, sol::script_pass_on_error);
    if (!result.valid())
    {
        sol::error err = result;
        Logger::Error("Lua load error: {}", err.what());
    }
    else
    {
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
            Logger::Error("Lua OnStart error: {}", err.what());
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
            Logger::Error("Lua OnUpdate error: {}", err.what());
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
