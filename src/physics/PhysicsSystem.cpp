#include "physics/PhysicsSystem.h"
#include "ecs/Components.h"
#include "core/Logger.h"

#include <cstdarg>
#include <mutex>
#include <vector>
#include <unordered_map>

// Jolt includes — warnings suppressed via /external:anglebrackets /external:W0
#pragma warning(push)
#pragma warning(disable: 4100 4127 4244 4265 4324 4365 4800)
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#pragma warning(pop)

#include <entt/entt.hpp>

using namespace DirectX;

namespace dx12e
{

// ========== Jolt Layer Definitions ==========

namespace Layers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
}

// BroadPhaseLayer mapping
class BPLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterface()
    {
        m_objectToBroadPhase[Layers::NON_MOVING] = JPH::BroadPhaseLayer(0);
        m_objectToBroadPhase[Layers::MOVING]     = JPH::BroadPhaseLayer(1);
    }

    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return m_objectToBroadPhase[layer];
    }

private:
    JPH::BroadPhaseLayer m_objectToBroadPhase[Layers::NUM_LAYERS];
};

// Object vs BroadPhase filter
class ObjVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override
    {
        switch (obj)
        {
        case Layers::NON_MOVING:
            return bp == JPH::BroadPhaseLayer(1); // Static only collides with moving
        case Layers::MOVING:
            return true; // Moving collides with everything
        default:
            return false;
        }
    }
};

// Object layer pair filter
class ObjLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        switch (a)
        {
        case Layers::NON_MOVING:
            return b == Layers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

// ========== ContactListener ==========
//
// Jolt の物理ステップスレッドから OnContactAdded/Removed が呼ばれる（全 Body ロック中）。
// この時点で BodyInterface や m_bodyToEntity に触るのは禁止（デッドロック/データレース）。
// そこで mutex 保護の pending バッファへ最小限の値だけ積み、メインスレッド（Update）で
// Drain → EventBus::Post する。これで /W4 /WX 下でも警告なし。
//
namespace
{

class EngineContactListener final : public JPH::ContactListener
{
public:
    struct PendingContact
    {
        uint32_t bodyId1 = 0;
        uint32_t bodyId2 = 0;
        DirectX::XMFLOAT3 point{};
        bool isEnter = true; // true=OnContactAdded, false=OnContactRemoved
    };

    JPH::ValidateResult OnContactValidate(const JPH::Body& /*b1*/, const JPH::Body& /*b2*/,
                                          JPH::RVec3Arg /*baseOffset*/,
                                          const JPH::CollideShapeResult& /*collisionResult*/) override
    {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*settings*/) override
    {
        DirectX::XMFLOAT3 pt{ 0.0f, 0.0f, 0.0f };
        if (!manifold.mRelativeContactPointsOn1.empty())
        {
            JPH::RVec3 cp = manifold.GetWorldSpaceContactPointOn1(0);
            pt = { static_cast<float>(cp.GetX()),
                   static_cast<float>(cp.GetY()),
                   static_cast<float>(cp.GetZ()) };
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending.push_back({
            b1.GetID().GetIndexAndSequenceNumber(),
            b2.GetID().GetIndexAndSequenceNumber(),
            pt,
            true });
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending.push_back({
            pair.GetBody1ID().GetIndexAndSequenceNumber(),
            pair.GetBody2ID().GetIndexAndSequenceNumber(),
            { 0.0f, 0.0f, 0.0f },
            false });
    }

    // メインスレッドから呼ぶ。pending を swap して返す。
    std::vector<PendingContact> Drain()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::vector<PendingContact> out;
        out.swap(m_pending);
        return out;
    }

private:
    std::mutex m_mtx;
    std::vector<PendingContact> m_pending;
};

} // anonymous namespace

// ========== JoltImpl ==========

struct PhysicsSystem::JoltImpl
{
    std::unique_ptr<JPH::TempAllocatorImpl>    tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>  jobSystem;
    BPLayerInterface                           bpLayerInterface;
    ObjVsBPLayerFilter                         objVsBpFilter;
    ObjLayerPairFilter                         objLayerPairFilter;
    std::unique_ptr<JPH::PhysicsSystem>        physicsSystem;
    std::unique_ptr<JPH::ContactListener>      contactListener; // EngineContactListener

    // entity → CharacterVirtual（Play 中のみ）。Ref で寿命管理。
    // メンバ宣言順では characters が physicsSystem の後ろ＝デストラクト時に先に破棄される
    // ので、CharacterVirtual が生きている PhysicsSystem を参照する破棄順は安全。
    std::unordered_map<entt::entity, JPH::Ref<JPH::CharacterVirtual>> characters;
};

// ========== Trace/Assert callbacks ==========

static void JoltTraceImpl(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Logger::Debug("[Jolt] {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertImpl(const char* expr, const char* msg,
                           const char* file, JPH::uint line)
{
    Logger::Error("[Jolt Assert] {} : {} ({}:{})", expr, msg ? msg : "", file, line);
    return true; // breakpoint
}
#endif

// ========== PhysicsSystem Implementation ==========

PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() { Shutdown(); }

void PhysicsSystem::Initialize()
{
    if (m_initialized) return;

    // Jolt global init (process-wide, idempotent guard)
    static bool sJoltRegistered = false;
    if (!sJoltRegistered)
    {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = JoltAssertImpl;
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        sJoltRegistered = true;
    }

    m_impl = std::make_unique<JoltImpl>();

    // 10MB temp allocator, 4 job threads
    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
    m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 4);

    constexpr JPH::uint maxBodies             = 4096;
    constexpr JPH::uint numBodyMutexes        = 0; // default
    constexpr JPH::uint maxBodyPairs           = 4096;
    constexpr JPH::uint maxContactConstraints  = 2048;

    m_impl->physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_impl->physicsSystem->Init(
        maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints,
        m_impl->bpLayerInterface, m_impl->objVsBpFilter, m_impl->objLayerPairFilter);

    // Gravity: Y-up, ゲーム向けにやや強め（リアル=9.81、ゲーム=14.0）
    m_impl->physicsSystem->SetGravity(JPH::Vec3(0.0f, -14.0f, 0.0f));

    // ContactListener 登録（接触イベントを pending バッファへ積む）
    auto listener = std::make_unique<EngineContactListener>();
    m_impl->physicsSystem->SetContactListener(listener.get());
    m_impl->contactListener = std::move(listener);

    m_accumulator = 0.0f;
    m_initialized = true;
    Logger::Info("PhysicsSystem initialized (Jolt Physics)");
}

void PhysicsSystem::Shutdown()
{
    if (!m_initialized) return;

    m_bodyToEntity.clear();
    m_paused = false;
    // m_eventBus は外部所有（Application の安定メンバ）なので null 化しない。
    // 物理を Shutdown→Initialize で再構築しても購読先は同じバスを使い続ける。

    m_impl.reset();   // contactListener も physicsSystem も破棄
    m_initialized = false;
    m_accumulator = 0.0f;
    Logger::Info("PhysicsSystem shutdown");
}

void PhysicsSystem::Update(f32 dt, entt::registry& registry)
{
    if (!m_initialized) return;

    // 一時停止中はタイムステップを進めない。
    // 直前フレームで生じた接触の pending だけはフラッシュして届ける。
    if (m_paused)
    {
        FlushPendingContacts();
        return;
    }

    SyncTransformsToPhysics(registry);

    // dt をクランプ（モード切替時の大きな dt で一気に何ステップも走るのを防ぐ）
    if (dt > kFixedTimeStep * 4.0f)
        dt = kFixedTimeStep;

    m_accumulator += dt;
    while (m_accumulator >= kFixedTimeStep)
    {
        m_impl->physicsSystem->Update(
            kFixedTimeStep, kCollisionSteps,
            m_impl->tempAllocator.get(), m_impl->jobSystem.get());
        StepCharacters(kFixedTimeStep, registry);   // 剛体ステップ後にキャラを進める
        m_accumulator -= kFixedTimeStep;
    }

    SyncPhysicsToTransforms(registry);
    SyncCharactersToTransforms(registry);

    // ContactListener の pending をメインスレッドで EventBus へ流す。
    FlushPendingContacts();
}

void PhysicsSystem::FlushPendingContacts()
{
    if (!m_eventBus || !m_impl || !m_impl->contactListener) return;

    auto* listener = static_cast<EngineContactListener*>(m_impl->contactListener.get());
    auto contacts = listener->Drain();
    for (const auto& c : contacts)
    {
        auto it1 = m_bodyToEntity.find(c.bodyId1);
        auto it2 = m_bodyToEntity.find(c.bodyId2);
        if (it1 == m_bodyToEntity.end() || it2 == m_bodyToEntity.end()) continue;

        EngineEvent ev;
        ev.name   = c.isEnter ? "engine.contact.enter" : "engine.contact.exit";
        ev.source = it1->second;
        ev.other  = it2->second;
        ev.set("px", static_cast<double>(c.point.x));
        ev.set("py", static_cast<double>(c.point.y));
        ev.set("pz", static_cast<double>(c.point.z));
        m_eventBus->Post(std::move(ev));
    }
}

void PhysicsSystem::SyncTransformsToPhysics(entt::registry& registry)
{
    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterfaceNoLock();

    auto view = registry.view<Transform, RigidBody>();
    for (auto [entity, transform, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;
        if (rb.motionType != MotionType::Kinematic) continue;

        JPH::BodyID joltId(rb.bodyId);
        JPH::RVec3 pos(transform.position.x, transform.position.y, transform.position.z);

        JPH::Quat rot;
        if (transform.useQuaternion)
        {
            rot = JPH::Quat(transform.quaternion.x, transform.quaternion.y,
                            transform.quaternion.z, transform.quaternion.w);
        }
        else
        {
            XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(transform.rotation.x),
                XMConvertToRadians(transform.rotation.y),
                XMConvertToRadians(transform.rotation.z));
            XMFLOAT4 qf;
            XMStoreFloat4(&qf, q);
            rot = JPH::Quat(qf.x, qf.y, qf.z, qf.w);
        }

        bodyInterface.SetPositionAndRotation(joltId, pos, rot, JPH::EActivation::DontActivate);
    }
}

void PhysicsSystem::SyncPhysicsToTransforms(entt::registry& registry)
{
    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterfaceNoLock();

    auto view = registry.view<Transform, RigidBody>();
    for (auto [entity, transform, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;
        if (rb.motionType != MotionType::Dynamic) continue;

        JPH::BodyID joltId(rb.bodyId);

        if (!bodyInterface.IsActive(joltId)) continue;

        JPH::RVec3 pos = bodyInterface.GetPosition(joltId);
        JPH::Quat  rot = bodyInterface.GetRotation(joltId);

        // コライダーのオフセットを引いてTransform位置に戻す
        f32 offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
        auto* convex  = registry.try_get<ConvexHullCollider>(entity);
        auto* box     = registry.try_get<BoxCollider>(entity);
        auto* sphere  = registry.try_get<SphereCollider>(entity);
        auto* capsule = registry.try_get<CapsuleCollider>(entity);
        if (convex)  { offsetX = convex->offset.x;  offsetY = convex->offset.y;  offsetZ = convex->offset.z; }
        if (box)     { offsetX = box->offset.x;     offsetY = box->offset.y;     offsetZ = box->offset.z; }
        if (sphere)  { offsetX = sphere->offset.x;  offsetY = sphere->offset.y;  offsetZ = sphere->offset.z; }
        if (capsule) { offsetX = capsule->offset.x; offsetY = capsule->offset.y; offsetZ = capsule->offset.z; }

        transform.position = { static_cast<f32>(pos.GetX()) - offsetX,
                               static_cast<f32>(pos.GetY()) - offsetY,
                               static_cast<f32>(pos.GetZ()) - offsetZ };

        // Quaternion で保持（Gimbal Lock 回避）
        transform.quaternion = { rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW() };
        transform.useQuaternion = true;
    }
}

// ========== Body Registration ==========

void PhysicsSystem::RegisterBody(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;

    auto* rb = registry.try_get<RigidBody>(entity);
    if (!rb) return;
    if (rb->bodyId != kInvalidBodyId) return; // already registered

    auto* transform = registry.try_get<Transform>(entity);
    if (!transform) return;

    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();

    // Determine shape
    JPH::ShapeRefC shape;
    auto* convex  = registry.try_get<ConvexHullCollider>(entity);
    auto* box     = registry.try_get<BoxCollider>(entity);
    auto* sphere  = registry.try_get<SphereCollider>(entity);
    auto* capsule = registry.try_get<CapsuleCollider>(entity);

    if (convex && !convex->points.empty())
    {
        // Convex Hull: 頂点データから凸包を生成
        std::vector<JPH::Vec3> joltPoints;
        joltPoints.reserve(convex->points.size());
        for (const auto& p : convex->points)
            joltPoints.push_back(JPH::Vec3(p.x, p.y, p.z));

        JPH::ConvexHullShapeSettings settings(joltPoints.data(),
            static_cast<int>(joltPoints.size()), 0.01f);  // 0.01 convex radius
        settings.mMaxConvexRadius = 0.05f;
        auto result = settings.Create();
        if (result.IsValid())
            shape = result.Get();
        else
            shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f)); // fallback
    }
    else if (box)
    {
        shape = new JPH::BoxShape(JPH::Vec3(box->halfExtents.x, box->halfExtents.y, box->halfExtents.z));
    }
    else if (sphere)
    {
        shape = new JPH::SphereShape(sphere->radius);
    }
    else if (capsule)
    {
        shape = new JPH::CapsuleShape(capsule->halfHeight, capsule->radius);
    }
    else
    {
        // Fallback: box from scale
        shape = new JPH::BoxShape(JPH::Vec3(
            transform->scale.x * 0.5f,
            transform->scale.y * 0.5f,
            transform->scale.z * 0.5f));
    }

    // Motion type
    JPH::EMotionType joltMotion;
    JPH::ObjectLayer layer;
    switch (rb->motionType)
    {
    case MotionType::Static:
        joltMotion = JPH::EMotionType::Static;
        layer = Layers::NON_MOVING;
        break;
    case MotionType::Kinematic:
        joltMotion = JPH::EMotionType::Kinematic;
        layer = Layers::MOVING;
        break;
    case MotionType::Dynamic:
    default:
        joltMotion = JPH::EMotionType::Dynamic;
        layer = Layers::MOVING;
        break;
    }

    // Position & Rotation（コライダーのオフセットを加算）
    f32 offsetX = 0.0f, offsetY = 0.0f, offsetZ = 0.0f;
    if (convex)  { offsetX = convex->offset.x;  offsetY = convex->offset.y;  offsetZ = convex->offset.z; }
    if (box)     { offsetX = box->offset.x;     offsetY = box->offset.y;     offsetZ = box->offset.z; }
    if (sphere)  { offsetX = sphere->offset.x;  offsetY = sphere->offset.y;  offsetZ = sphere->offset.z; }
    if (capsule) { offsetX = capsule->offset.x; offsetY = capsule->offset.y; offsetZ = capsule->offset.z; }

    JPH::RVec3 pos(transform->position.x + offsetX,
                   transform->position.y + offsetY,
                   transform->position.z + offsetZ);
    JPH::Quat  rot = JPH::Quat::sIdentity();

    if (transform->useQuaternion)
    {
        rot = JPH::Quat(transform->quaternion.x, transform->quaternion.y,
                        transform->quaternion.z, transform->quaternion.w);
    }
    else
    {
        XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(transform->rotation.x),
            XMConvertToRadians(transform->rotation.y),
            XMConvertToRadians(transform->rotation.z));
        XMFLOAT4 qf;
        XMStoreFloat4(&qf, q);
        rot = JPH::Quat(qf.x, qf.y, qf.z, qf.w);
    }

    JPH::BodyCreationSettings bodySettings(shape, pos, rot, joltMotion, layer);

    if (rb->motionType == MotionType::Dynamic)
    {
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = rb->mass;
    }

    bodySettings.mRestitution    = rb->restitution;
    bodySettings.mFriction       = rb->friction;
    bodySettings.mLinearDamping  = rb->linearDamping;
    bodySettings.mAngularDamping = rb->angularDamping;
    bodySettings.mGravityFactor  = rb->useGravity ? 1.0f : 0.0f;

    // ゲーム向け: すぐ sleep しない（不安定な積み方でも少し動き続ける）
    bodySettings.mAllowSleeping  = true;

    JPH::BodyID id = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);
    rb->bodyId = id.GetIndexAndSequenceNumber();
    m_bodyToEntity[rb->bodyId] = entity;   // bodyId→entity 逆引きを登録
}

void PhysicsSystem::UnregisterBody(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;

    auto* rb = registry.try_get<RigidBody>(entity);
    if (!rb || rb->bodyId == kInvalidBodyId) return;

    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    JPH::BodyID joltId(rb->bodyId);
    bodyInterface.RemoveBody(joltId);
    bodyInterface.DestroyBody(joltId);
    m_bodyToEntity.erase(rb->bodyId);
    rb->bodyId = kInvalidBodyId;
}

void PhysicsSystem::UnregisterAllBodies(entt::registry& registry)
{
    if (!m_initialized) return;

    auto view = registry.view<RigidBody>();
    for (auto [entity, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;

        auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
        JPH::BodyID joltId(rb.bodyId);
        bodyInterface.RemoveBody(joltId);
        bodyInterface.DestroyBody(joltId);
        m_bodyToEntity.erase(rb.bodyId);
        rb.bodyId = kInvalidBodyId;
    }
    m_bodyToEntity.clear();   // 念のため全消去
}

// ========== Character Controller（CharacterVirtual）==========

void PhysicsSystem::RegisterCharacter(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;
    auto* cc = registry.try_get<CharacterController>(entity);
    if (!cc) return;
    if (cc->_registered) return;
    auto* transform = registry.try_get<Transform>(entity);
    if (!transform) return;

    // カプセル形状（RegisterBody と同じ引数順: halfHeight, radius）
    JPH::CapsuleShapeSettings shapeSettings(cc->halfHeight, cc->radius);
    auto shapeResult = shapeSettings.Create();
    if (!shapeResult.IsValid()) return;
    JPH::ShapeRefC shape = shapeResult.Get();

    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape         = shape;                 // base CharacterBaseSettings::mShape
    settings->mMass          = cc->mass;
    settings->mMaxSlopeAngle = DirectX::XMConvertToRadians(cc->maxSlopeDeg); // 度→ラジアン必須
    settings->mShapeOffset   = JPH::Vec3(cc->offset.x, cc->offset.y, cc->offset.z);
    settings->mUp            = JPH::Vec3(0, 1, 0);    // base
    // 足元の支持面: カプセル下端を少し上にした平面（接地安定化）
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3(0, 1, 0), -cc->radius);

    // 初期位置（Transform + offset）。キャラは Y 軸回転を物理に渡さない（接地判定簡素化）
    JPH::RVec3 pos(transform->position.x + cc->offset.x,
                   transform->position.y + cc->offset.y,
                   transform->position.z + cc->offset.z);
    JPH::Quat rot = JPH::Quat::sIdentity();

    JPH::Ref<JPH::CharacterVirtual> ch = new JPH::CharacterVirtual(
        settings, pos, rot, static_cast<JPH::uint64>(entt::to_integral(entity)),
        m_impl->physicsSystem.get());

    m_impl->characters[entity] = ch;
    cc->_registered  = true;
    cc->_verticalVel = 0.0f;
    cc->_grounded    = false;
}

void PhysicsSystem::UnregisterCharacter(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;
    m_impl->characters.erase(entity);   // Ref 解放
    if (auto* cc = registry.try_get<CharacterController>(entity)) cc->_registered = false;
}

void PhysicsSystem::UnregisterAllCharacters(entt::registry& registry)
{
    if (!m_initialized) return;
    m_impl->characters.clear();
    for (auto [e, cc] : registry.view<CharacterController>().each())
        cc._registered = false;
}

void PhysicsSystem::StepCharacters(f32 fixedDt, entt::registry& registry)
{
    if (!m_initialized || m_impl->characters.empty()) return;

    const JPH::Vec3 baseGravity = m_impl->physicsSystem->GetGravity(); // (0,-14,0)

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings; // 既定 + stepHeight 反映
    // フィルタ: MOVING レイヤとして全衝突（既存 ObjLayerPairFilter と整合）
    JPH::DefaultBroadPhaseLayerFilter bpFilter(m_impl->objVsBpFilter, Layers::MOVING);
    JPH::DefaultObjectLayerFilter     objFilter(m_impl->objLayerPairFilter, Layers::MOVING);

    for (auto& [entity, ch] : m_impl->characters)
    {
        auto* cc = registry.try_get<CharacterController>(entity);
        if (!cc) continue;

        // 接地状態更新（前ステップ結果）
        bool grounded = ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;

        // 鉛直速度: 接地中はリセット、空中は重力積分
        if (grounded && cc->_verticalVel < 0.0f) cc->_verticalVel = 0.0f;
        cc->_verticalVel += baseGravity.GetY() * cc->gravityScale * fixedDt;

        // ジャンプ要求（接地中のみ受け付け）
        if (cc->_jumpQueued && grounded) { cc->_verticalVel = cc->jumpSpeed; }
        cc->_jumpQueued = false;

        // 目標速度合成: 水平=move()入力 + 接地面速度、鉛直=積分結果
        JPH::Vec3 ground = ch->GetGroundVelocity();
        JPH::Vec3 vel(cc->_desiredVel.x + (grounded ? ground.GetX() : 0.0f),
                      cc->_verticalVel,
                      cc->_desiredVel.z + (grounded ? ground.GetZ() : 0.0f));
        ch->SetLinearVelocity(vel);

        // step height（段差登り）
        updateSettings.mWalkStairsStepUp = JPH::Vec3(0, cc->stepHeight, 0);

        ch->ExtendedUpdate(
            fixedDt,
            baseGravity * cc->gravityScale,
            updateSettings,
            bpFilter, objFilter,
            JPH::BodyFilter{}, JPH::ShapeFilter{},
            *m_impl->tempAllocator);

        cc->_grounded = ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    }
}

void PhysicsSystem::SyncCharactersToTransforms(entt::registry& registry)
{
    if (!m_initialized || m_impl->characters.empty()) return;
    for (auto& [entity, ch] : m_impl->characters)
    {
        auto* tf = registry.try_get<Transform>(entity);
        auto* cc = registry.try_get<CharacterController>(entity);
        if (!tf || !cc) continue;
        JPH::RVec3 p = ch->GetPosition();
        tf->position = { static_cast<f32>(p.GetX()) - cc->offset.x,
                         static_cast<f32>(p.GetY()) - cc->offset.y,
                         static_cast<f32>(p.GetZ()) - cc->offset.z };
        // 回転は CC からは書き戻さない（Yaw は Lua/ゲーム側が Transform.rotation で管理）

        // move しなければ止まる: 次フレーム Lua が再設定するまで水平入力を消す。
        cc->_desiredVel = { 0.0f, 0.0f, 0.0f };
    }
}

// ========== Physics Operations ==========

void PhysicsSystem::ApplyForce(uint32_t bodyId, XMFLOAT3 force)
{
    if (!m_initialized || bodyId == kInvalidBodyId) return;
    auto& bi = m_impl->physicsSystem->GetBodyInterface();
    bi.AddForce(JPH::BodyID(bodyId), JPH::Vec3(force.x, force.y, force.z));
}

void PhysicsSystem::ApplyImpulse(uint32_t bodyId, XMFLOAT3 impulse)
{
    if (!m_initialized || bodyId == kInvalidBodyId) return;
    auto& bi = m_impl->physicsSystem->GetBodyInterface();
    bi.AddImpulse(JPH::BodyID(bodyId), JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void PhysicsSystem::SetLinearVelocity(uint32_t bodyId, XMFLOAT3 vel)
{
    if (!m_initialized || bodyId == kInvalidBodyId) return;
    auto& bi = m_impl->physicsSystem->GetBodyInterface();
    bi.SetLinearVelocity(JPH::BodyID(bodyId), JPH::Vec3(vel.x, vel.y, vel.z));
}

XMFLOAT3 PhysicsSystem::GetLinearVelocity(uint32_t bodyId) const
{
    if (!m_initialized || bodyId == kInvalidBodyId) return {};
    auto& bi = m_impl->physicsSystem->GetBodyInterfaceNoLock();
    JPH::Vec3 v = bi.GetLinearVelocity(JPH::BodyID(bodyId));
    return { v.GetX(), v.GetY(), v.GetZ() };
}

void PhysicsSystem::SetPosition(uint32_t bodyId, XMFLOAT3 pos)
{
    if (!m_initialized || bodyId == kInvalidBodyId) return;
    auto& bi = m_impl->physicsSystem->GetBodyInterface();
    bi.SetPosition(JPH::BodyID(bodyId),
                   JPH::RVec3(pos.x, pos.y, pos.z),
                   JPH::EActivation::Activate);
}

// ========== Raycast ==========

RaycastHit PhysicsSystem::Raycast(XMFLOAT3 origin, XMFLOAT3 direction, f32 maxDistance) const
{
    RaycastHit result;
    if (!m_initialized) return result;

    // Normalize direction
    XMVECTOR dir = XMLoadFloat3(&direction);
    dir = XMVector3Normalize(dir);
    XMFLOAT3 normDir;
    XMStoreFloat3(&normDir, dir);

    JPH::RRayCast ray(
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(normDir.x * maxDistance, normDir.y * maxDistance, normDir.z * maxDistance));

    JPH::RayCastResult hit;
    if (m_impl->physicsSystem->GetNarrowPhaseQuery().CastRay(ray, hit))
    {
        result.hit      = true;
        result.distance = hit.mFraction * maxDistance;
        result.bodyId   = hit.mBodyID.GetIndexAndSequenceNumber();

        JPH::RVec3 hitPoint = ray.GetPointOnRay(hit.mFraction);
        result.point = { static_cast<f32>(hitPoint.GetX()),
                         static_cast<f32>(hitPoint.GetY()),
                         static_cast<f32>(hitPoint.GetZ()) };

        // Normal: approximate with up vector (full surface normal requires body lock)
        result.normal = { 0.0f, 1.0f, 0.0f };
    }

    return result;
}

// ========== Overlap Queries（Broadphase）==========

namespace
{
// Broadphase の AABB/Sphere ヒットを bodyId→entity マップで解決して out へ詰める Collector。
// cap に達したら以降は無視（早期終了せず単に切り捨て、Broadphase の安全な反復を維持）。
struct OverlapCollector final : public JPH::CollideShapeBodyCollector
{
    entt::entity* out = nullptr;
    size_t cap = 0;
    size_t count = 0;
    const std::unordered_map<uint32_t, entt::entity>* map = nullptr;

    void AddHit(const JPH::BodyID& id) override
    {
        if (count >= cap) return;
        auto it = map->find(id.GetIndexAndSequenceNumber());
        if (it != map->end()) out[count++] = it->second;
    }
};
} // anonymous namespace

size_t PhysicsSystem::OverlapBox(const XMFLOAT3& center,
                                 const XMFLOAT3& half,
                                 entt::entity* out, size_t cap) const
{
    if (!m_initialized || !m_impl || cap == 0 || !out || m_bodyToEntity.empty()) return 0;

    JPH::AABox box(
        JPH::Vec3(center.x - half.x, center.y - half.y, center.z - half.z),
        JPH::Vec3(center.x + half.x, center.y + half.y, center.z + half.z));

    OverlapCollector col;
    col.out = out; col.cap = cap; col.map = &m_bodyToEntity;

    m_impl->physicsSystem->GetBroadPhaseQuery().CollideAABox(box, col);
    return col.count;
}

size_t PhysicsSystem::OverlapSphere(const XMFLOAT3& center,
                                    float radius,
                                    entt::entity* out, size_t cap) const
{
    if (!m_initialized || !m_impl || cap == 0 || !out || m_bodyToEntity.empty()) return 0;

    OverlapCollector col;
    col.out = out; col.cap = cap; col.map = &m_bodyToEntity;

    m_impl->physicsSystem->GetBroadPhaseQuery().CollideSphere(
        JPH::Vec3(center.x, center.y, center.z), radius, col);
    return col.count;
}

// ========== Time Model（pause / manual step / gravity）==========

void PhysicsSystem::Step(float dt)
{
    if (!m_initialized || !m_impl) return;
    m_impl->physicsSystem->Update(
        dt, kCollisionSteps,
        m_impl->tempAllocator.get(), m_impl->jobSystem.get());

    // 手動ステップ（physics:step）で生じた接触も同フレームで配信する。
    // これを呼ばないと pause 中の駒送り時に接触イベントが1フレーム遅延、または
    // pause が続くと永久に届かない。
    FlushPendingContacts();
}

void PhysicsSystem::SetGravity(XMFLOAT3 g)
{
    if (!m_initialized || !m_impl) return;
    m_impl->physicsSystem->SetGravity(JPH::Vec3(g.x, g.y, g.z));
}

} // namespace dx12e
