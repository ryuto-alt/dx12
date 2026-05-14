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
    // self.this は physics:applyImpulse(self.this, ...) のように Entity 型 API に渡せる
    (*self)["this"]    = Entity(e, &reg);
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
            if (type == "RigidBody")          return e.HasComponent<RigidBody>();
            if (type == "BoxCollider")        return e.HasComponent<BoxCollider>();
            if (type == "SphereCollider")     return e.HasComponent<SphereCollider>();
            if (type == "CapsuleCollider")    return e.HasComponent<CapsuleCollider>();
            if (type == "ConvexHullCollider") return e.HasComponent<ConvexHullCollider>();
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
    // log() は 3 系統に出力: spdlog / OutputDebugString (Visual Studio 出力ウィンドウ) / Toolbar 表示
    lua["log"] = [this](const std::string& msg) {
        Logger::Info("[Lua] {}", msg);
        std::string line = "[Lua] " + msg + "\n";
        OutputDebugStringA(line.c_str());
        m_lastLuaMessage = msg;
    };

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
            char buf[160];
            if (!e.HasComponent<RigidBody>()) {
                OutputDebugStringA("[setVelocity] FAIL: no RigidBody component\n");
                return;
            }
            auto& rb = e.GetComponent<RigidBody>();
            if (rb.bodyId == kInvalidBodyId) {
                snprintf(buf, sizeof(buf),
                    "[setVelocity] FAIL: invalid bodyId (motionType=%d, mass=%.2f) "
                    "-> RigidBody がまだ Jolt に登録されていない. Collider が必要かも\n",
                    (int)rb.motionType, rb.mass);
                OutputDebugStringA(buf);
                return;
            }
            if (rb.motionType == MotionType::Static) {
                OutputDebugStringA("[setVelocity] FAIL: motionType=Static -> Dynamic に変更してや\n");
                return;
            }
            ps.SetLinearVelocity(rb.bodyId, vel);
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
    int total = 0, ok = 0;
    for (auto e : view)
    {
        auto& ls = view.get<LuaScript>(e);
        ls.env.reset();
        ls.self.reset();
        ls.started   = false;
        ls.loadError = false;
        if (ls.scriptPath.empty()) continue;
        ++total;
        const auto* tag = reg.try_get<NameTag>(e);
        std::string entityName = tag ? tag->name : std::string("(no name)");
        Logger::Info("ScriptEngine: initializing entity='{}' script='{}'",
                     entityName, ls.scriptPath);
        if (InitializeLuaScriptInstance(*m_lua, reg, e, ls, m_assetsDir, m_lastError))
            ++ok;
    }
    Logger::Info("ScriptEngine: OnPlayStart done ({} / {} scripts initialized)", ok, total);
    // OutputDebugString にも出して Visual Studio で確認できるように
    char buf[128];
    snprintf(buf, sizeof(buf),
             "[ScriptEngine] OnPlayStart: %d/%d scripts initialized\n", ok, total);
    OutputDebugStringA(buf);
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

        // self.transform / self.this を毎フレーム更新（コンポーネント再配置や registry 変更に備える）
        if (auto* tf = reg.try_get<Transform>(e))
            (*self)["transform"] = tf;
        (*self)["this"]    = Entity(e, &reg);
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
