#include "physics/PhysicsSystem.h"
#include "ecs/Components.h"
#include "core/Logger.h"

#include <cstdarg>

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
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
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

// ========== JoltImpl ==========

struct PhysicsSystem::JoltImpl
{
    std::unique_ptr<JPH::TempAllocatorImpl>    tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>  jobSystem;
    BPLayerInterface                           bpLayerInterface;
    ObjVsBPLayerFilter                         objVsBpFilter;
    ObjLayerPairFilter                         objLayerPairFilter;
    std::unique_ptr<JPH::PhysicsSystem>        physicsSystem;
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

    // Gravity: Y-up, -9.81
    m_impl->physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    m_accumulator = 0.0f;
    m_initialized = true;
    Logger::Info("PhysicsSystem initialized (Jolt Physics)");
}

void PhysicsSystem::Shutdown()
{
    if (!m_initialized) return;

    m_impl.reset();
    m_initialized = false;
    m_accumulator = 0.0f;
    Logger::Info("PhysicsSystem shutdown");
}

void PhysicsSystem::Update(f32 dt, entt::registry& registry)
{
    if (!m_initialized) return;

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
        m_accumulator -= kFixedTimeStep;
    }

    SyncPhysicsToTransforms(registry);
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

        transform.position = { static_cast<f32>(pos.GetX()),
                               static_cast<f32>(pos.GetY()),
                               static_cast<f32>(pos.GetZ()) };

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
    auto* box     = registry.try_get<BoxCollider>(entity);
    auto* sphere  = registry.try_get<SphereCollider>(entity);
    auto* capsule = registry.try_get<CapsuleCollider>(entity);

    if (box)
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

    // Position & Rotation
    JPH::RVec3 pos(transform->position.x, transform->position.y, transform->position.z);
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

    JPH::BodyID id = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);
    rb->bodyId = id.GetIndexAndSequenceNumber();
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
        rb.bodyId = kInvalidBodyId;
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

} // namespace dx12e
