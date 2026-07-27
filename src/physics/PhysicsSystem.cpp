#include "physics/PhysicsSystem.h"
#include "ecs/Components.h"
#include "core/Logger.h"
#include "terrain/HeightField.h"   // 地形コライダー（Terrain::_hf の高さ配列を Jolt へ渡す）
#include "terrain/SculptMesh.h"    // スカルプトコライダー（SculptMesh::_data の三角形を Jolt へ渡す）

#include <algorithm>
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
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>   // スカルプトメッシュ（彫った異形）のコライダー
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
#include <Jolt/Physics/Body/BodyLock.h>       // RaycastEx: 真の面法線を取るための BodyLockRead
#include <Jolt/Physics/Body/BodyFilter.h>     // RaycastEx: セルフヒット除外
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
    // 接触点は plain float×3 で保持する（XMFLOAT3 にしない）。
    // OnContactAdded は Jolt の物理スレッドから（DirectX ヘッダを意識せず）呼ばれるので、
    // ここで Jolt 値 → plain float に落とし、XMFLOAT3 への変換は drain 側（メインスレッド）に
    // 任せて Jolt/DirectX ヘッダの結合を切る。
    struct PendingContact
    {
        uint32_t bodyId1 = 0;
        uint32_t bodyId2 = 0;
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
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
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        if (!manifold.mRelativeContactPointsOn1.empty())
        {
            JPH::RVec3 cp = manifold.GetWorldSpaceContactPointOn1(0);
            px = static_cast<float>(cp.GetX());
            py = static_cast<float>(cp.GetY());
            pz = static_cast<float>(cp.GetZ());
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending.push_back({
            b1.GetID().GetIndexAndSequenceNumber(),
            b2.GetID().GetIndexAndSequenceNumber(),
            px, py, pz,
            true });
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending.push_back({
            pair.GetBody1ID().GetIndexAndSequenceNumber(),
            pair.GetBody2ID().GetIndexAndSequenceNumber(),
            0.0f, 0.0f, 0.0f,
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
    // キャラの「1 固定ステップ前」の位置。描画は current との間を accumulator の
    // 端数で補間する（→ SyncCharactersToTransforms）。ここに置いてあるのは
    // CharacterController のレイアウトを変えずに済ませるため。
    std::unordered_map<entt::entity, JPH::RVec3> prevCharPos;
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
    // m_eventBus は外部所有（Application の安定メンバ）。EventBus 破棄より前に
    // 呼び出し側が SetEventBus(nullptr) して参照を切る責務を負う
    // （Application::Shutdown / EnterEditorMode がその流儀）。
    // ここで m_eventBus を保持したままにするのは Shutdown→Initialize の再利用のため
    // （物理を作り直しても購読先は同じバスを使い続けられる）。null 化はしない。

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

    // 地形を彫った直後なら、ステップ前にコライダーを作り直しておく
    //（描画メッシュと当たり判定が 1 フレームでもズレないようにする）。
    RefreshTerrainColliders(registry);
    // 彫ったスカルプトメッシュも同じ理屈でコライダーを作り直す（ストローク終了時にだけ立つ）。
    RefreshSculptColliders(registry);

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

        // drain 側（メインスレッド）で plain float×3 → XMFLOAT3 へ変換し、
        // DirectX ヘッダに依存する側へ橋渡しする（Jolt コールバック側とは結合を切る）。
        const DirectX::XMFLOAT3 point{ c.px, c.py, c.pz };

        EngineEvent ev;
        ev.name   = c.isEnter ? "engine.contact.enter" : "engine.contact.exit";
        ev.source = it1->second;
        ev.other  = it2->second;
        ev.set("px", static_cast<double>(point.x));
        ev.set("py", static_cast<double>(point.y));
        ev.set("pz", static_cast<double>(point.z));
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

void PhysicsSystem::RefreshTerrainColliders(entt::registry& registry)
{
    if (!m_initialized) return;

    // 反復中に RegisterBody/UnregisterBody がコンポーネントへ書き込むので、
    // 対象を先に集めてから処理する（view の反復と書き込みを混ぜない）。
    std::vector<entt::entity> dirty;
    for (auto [e, terrain, rb] : registry.view<Terrain, RigidBody>().each())
    {
        (void)rb;
        if (terrain._colliderDirty) dirty.push_back(e);
    }
    if (dirty.empty()) return;

    for (entt::entity e : dirty)
    {
        auto* rb = registry.try_get<RigidBody>(e);
        if (!rb) continue;
        if (rb->bodyId != kInvalidBodyId) UnregisterBody(registry, e);
        RegisterBody(registry, e);   // 成功/失敗どちらでも _colliderDirty は落とす
        if (auto* t = registry.try_get<Terrain>(e)) t->_colliderDirty = false;
    }
}

void PhysicsSystem::RefreshSculptColliders(entt::registry& registry)
{
    if (!m_initialized) return;

    // RefreshTerrainColliders と同じ流儀: 反復中に RegisterBody/UnregisterBody が
    // コンポーネントへ書き込むので、対象を先に集めてから処理する。
    std::vector<entt::entity> dirty;
    for (auto [e, sculpt, rb] : registry.view<SculptMesh, RigidBody>().each())
    {
        (void)rb;
        if (sculpt._colliderDirty) dirty.push_back(e);
    }
    if (dirty.empty()) return;

    for (entt::entity e : dirty)
    {
        auto* rb = registry.try_get<RigidBody>(e);
        if (!rb) continue;
        if (rb->bodyId != kInvalidBodyId) UnregisterBody(registry, e);
        RegisterBody(registry, e);   // 成功/失敗どちらでも _colliderDirty は落とす
        if (auto* sc = registry.try_get<SculptMesh>(e)) sc->_colliderDirty = false;
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

    // ---- 地形（ハイトフィールド）----
    // 高さを解析的に引ける形状なので、メッシュコライダーを作らず Jolt の HeightFieldShape に
    // そのまま渡す。これだけで「重力で落ちてきた剛体が乗る/めり込まない/斜面で滑る」も
    // CharacterVirtual の斜面登坂も Raycast も全部そのまま効く（Terrain 専用の解決コードは不要）。
    // HeightFieldShape は静的専用なので motionType の指定に関わらず Static で作る。
    if (auto* terrain = registry.try_get<Terrain>(entity); terrain && terrain->_hf && terrain->_hf->IsValid())
    {
        const HeightField& hf = *terrain->_hf;
        const f32 cell = hf.CellSize();
        const f32 half = hf.HalfSize();

        // 座標規約は TerrainMeshBuilder と同一:
        //   pos = offset + scale * (ix, heights[iz*n+ix], iz)、offset = (-half, 0, -half)
        // ＝描画メッシュと 1 サンプルもズレない。
        JPH::HeightFieldShapeSettings hfSettings(
            hf.Heights().data(),
            JPH::Vec3(-half, 0.0f, -half),
            JPH::Vec3(cell, 1.0f, cell),
            static_cast<JPH::uint32>(hf.Resolution()));
        hfSettings.mBlockSize     = 2;   // サンプル数はこの倍数である必要がある（Normalize 済み）
        hfSettings.mBitsPerSample = 8;   // ブロックごとの min/max 基準なので 8bit で十分な精度

        auto hfResult = hfSettings.Create();
        if (!hfResult.IsValid())
        {
            Logger::Error("地形コライダーの生成に失敗しました: {}", hfResult.GetError().c_str());
            return;
        }
        JPH::ShapeRefC terrainShape = hfResult.Get();

        JPH::BodyCreationSettings terrainSettings(
            terrainShape,
            JPH::RVec3(transform->position.x, transform->position.y, transform->position.z),
            JPH::Quat::sIdentity(),          // 地形は回転させない（ハイトフィールドの前提）
            JPH::EMotionType::Static,
            Layers::NON_MOVING);
        terrainSettings.mFriction    = rb->friction;
        terrainSettings.mRestitution = rb->restitution;

        JPH::BodyID terrainId = bodyInterface.CreateAndAddBody(terrainSettings,
                                                               JPH::EActivation::DontActivate);
        rb->bodyId = terrainId.GetIndexAndSequenceNumber();
        m_bodyToEntity[rb->bodyId] = entity;
        terrain->_colliderDirty = false;
        return;
    }

    // ---- スカルプトメッシュ（彫った異形）----
    // 頂点を直接動かして作る形なので、凸包では洞窟もアーチも表現できない。三角形メッシュ形状
    //（Jolt の MeshShape）をそのまま張る＝彫った通りに当たる。Transform のスケールは形状へ
    // 焼き込む（BoxCollider が halfExtents に scale を掛けるのと同じ規約）。
    // MeshShape は静的専用（Shape::MustBeStatic）なので、動かす剛体に付いている場合だけ
    // 凸包へフォールバックし、その旨を警告に出す。
    JPH::ShapeRefC shape;
    if (auto* sculpt = registry.try_get<SculptMesh>(entity);
        sculpt && sculpt->_data && sculpt->_data->IsValid())
    {
        sculpt->_colliderDirty = false;   // 成功/失敗どちらでも消化する（無限リトライを避ける）
        // 当たり判定オフ＝物理ボディそのものを作らない。ここで return しないと下の
        // 「コライダー部品が無ければ scale から箱」フォールバックに落ちて、
        // 見えない 1x1x1 の箱が湧く（OFF にしたのにぶつかる、という最悪の挙動）。
        if (!sculpt->collision) return;

        const SculptMeshData& data      = *sculpt->_data;
        const auto&           positions = data.Positions();
        const auto&           indices   = data.Indices();
        const f32 sclX = transform->scale.x;
        const f32 sclY = transform->scale.y;
        const f32 sclZ = transform->scale.z;

        if (rb->motionType == MotionType::Static)
        {
            JPH::VertexList verts;
            verts.reserve(positions.size());
            for (const auto& p : positions)
                verts.push_back(JPH::Float3(p.x * sclX, p.y * sclY, p.z * sclZ));

            JPH::IndexedTriangleList tris;
            tris.reserve(indices.size() / 3);
            for (size_t t = 0; t + 2 < indices.size(); t += 3)
                tris.push_back(JPH::IndexedTriangle(indices[t], indices[t + 1], indices[t + 2], 0));

            // 巻き順は SculptMesh 側で「cross(v1-v0,v2-v0) が外向き」＝ Jolt が要求する CCW に
            // 揃えてある。退化三角形は MeshShapeSettings のコンストラクタが落とす。
            JPH::MeshShapeSettings meshSettings(std::move(verts), std::move(tris));
            auto meshResult = meshSettings.Create();
            if (meshResult.IsValid())
            {
                shape = meshResult.Get();
            }
            else
            {
                Logger::Error("スカルプトメッシュのコライダー生成に失敗しました: {}",
                              meshResult.GetError().c_str());
            }
        }
        else
        {
            // 動く剛体は MeshShape を持てない（Jolt の制約）。凸包で妥協する＝穴は塞がる。
            std::vector<JPH::Vec3> hullPoints;
            hullPoints.reserve(positions.size());
            for (const auto& p : positions)
                hullPoints.push_back(JPH::Vec3(p.x * sclX, p.y * sclY, p.z * sclZ));

            JPH::ConvexHullShapeSettings hullSettings(hullPoints.data(),
                static_cast<int>(hullPoints.size()), 0.01f);
            hullSettings.mMaxConvexRadius = 0.05f;
            auto hullResult = hullSettings.Create();
            if (hullResult.IsValid()) shape = hullResult.Get();

            Logger::Warn("スカルプトメッシュ '{}' は Static ではないので凸包コライダーに"
                         "フォールバックしました（三角形メッシュ形状は静的な剛体にしか付けられへん）。"
                         "彫った凹み/穴は当たり判定では埋まるで。",
                         registry.all_of<NameTag>(entity) ? registry.get<NameTag>(entity).name
                                                          : std::string("Sculpt"));
        }

        // 形状を作れなかったときも箱で代用はしない（原因が見えない当たり判定を残さない）。
        if (shape.GetPtr() == nullptr) return;
    }

    // Determine shape
    auto* convex  = registry.try_get<ConvexHullCollider>(entity);
    auto* box     = registry.try_get<BoxCollider>(entity);
    auto* sphere  = registry.try_get<SphereCollider>(entity);
    auto* capsule = registry.try_get<CapsuleCollider>(entity);

    if (shape.GetPtr() != nullptr)
    {
        // 上のスカルプトメッシュ経路で形状が決まっている（コライダー部品より優先）
    }
    else if (convex && !convex->points.empty())
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
        // halfExtents は Transform.scale 乗算が前提（Trigger/EditorIconRenderer と同じ規約）。
        // これが無いと SpawnBox→scale で見た目だけ拡大したボックス（Ground/Wall等）が
        // 実際にはデフォルトの 0.5 半径でしか衝突しなくなる。
        shape = new JPH::BoxShape(JPH::Vec3(box->halfExtents.x * transform->scale.x,
                                             box->halfExtents.y * transform->scale.y,
                                             box->halfExtents.z * transform->scale.z));
    }
    else if (sphere)
    {
        f32 s = std::max({transform->scale.x, transform->scale.y, transform->scale.z});
        shape = new JPH::SphereShape(sphere->radius * s);
    }
    else if (capsule)
    {
        f32 radiusScale = std::max(transform->scale.x, transform->scale.z);
        shape = new JPH::CapsuleShape(capsule->halfHeight * transform->scale.y,
                                       capsule->radius * radiusScale);
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

    // 初期位置は Transform 原点そのまま。形状ローカルオフセットは mShapeOffset に一本化する
    // （mShapeOffset は mPosition に加算され衝突カプセル中心 = transform.position + offset となる）。
    // ここで + offset するとオフセットが二重適用されるので加算しない。
    // キャラは Y 軸回転を物理に渡さない（接地判定簡素化）。
    JPH::RVec3 pos(transform->position.x,
                   transform->position.y,
                   transform->position.z);
    JPH::Quat rot = JPH::Quat::sIdentity();

    JPH::Ref<JPH::CharacterVirtual> ch = new JPH::CharacterVirtual(
        settings, pos, rot, static_cast<JPH::uint64>(entt::to_integral(entity)),
        m_impl->physicsSystem.get());

    m_impl->characters[entity] = ch;
    cc->_registered  = true;
    cc->_verticalVel = 0.0f;
    cc->_grounded    = false;
    // Play→Stop→Play でコンポーネントは破棄されないため、前回終了時のランタイム入力状態
    // （_desiredVel / _jumpQueued）が残ると最初の接地ステップで余分なジャンプや移動を誘発する。
    // 形状生成と同時に明示リセットする。
    cc->_desiredVel  = { 0.0f, 0.0f, 0.0f };
    cc->_jumpQueued  = false;
}

void PhysicsSystem::UnregisterCharacter(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;
    m_impl->characters.erase(entity);   // Ref 解放
    m_impl->prevCharPos.erase(entity);
    if (auto* cc = registry.try_get<CharacterController>(entity)) cc->_registered = false;
}

void PhysicsSystem::UnregisterAllCharacters(entt::registry& registry)
{
    if (!m_initialized) return;
    m_impl->characters.clear();
    m_impl->prevCharPos.clear();
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

        // 描画補間用に、このステップを踏む前の位置を控える
        m_impl->prevCharPos[entity] = ch->GetPosition();

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

        // move しなければ止まる: 水平入力はこのステップで消費したのでゼロ化する。
        // 実際に消費したステップ末でゼロ化することで、0 サブステップのフレームでも
        // 直前の move() 入力を次ステップまで取りこぼさない。
        cc->_desiredVel = { 0.0f, 0.0f, 0.0f };
    }
}

void PhysicsSystem::StepSingleCharacter(entt::entity entity, f32 fixedDt, entt::registry& registry)
{
    // マルチプレイ予測リコンシリエーションのリプレイ専用。StepCharacters と同一ロジックだが
    // 指定した1体のみを進める(他キャラ/剛体のワールド状態は現在のまま=一般的な近似)。
    if (!m_initialized) return;
    auto it = m_impl->characters.find(entity);
    if (it == m_impl->characters.end()) return;
    auto* cc = registry.try_get<CharacterController>(entity);
    if (!cc) return;

    auto& ch = it->second;
    const JPH::Vec3 baseGravity = m_impl->physicsSystem->GetGravity();

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    JPH::DefaultBroadPhaseLayerFilter bpFilter(m_impl->objVsBpFilter, Layers::MOVING);
    JPH::DefaultObjectLayerFilter     objFilter(m_impl->objLayerPairFilter, Layers::MOVING);

    bool grounded = ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    if (grounded && cc->_verticalVel < 0.0f) cc->_verticalVel = 0.0f;
    cc->_verticalVel += baseGravity.GetY() * cc->gravityScale * fixedDt;
    if (cc->_jumpQueued && grounded) { cc->_verticalVel = cc->jumpSpeed; }
    cc->_jumpQueued = false;

    JPH::Vec3 ground = ch->GetGroundVelocity();
    JPH::Vec3 vel(cc->_desiredVel.x + (grounded ? ground.GetX() : 0.0f),
                  cc->_verticalVel,
                  cc->_desiredVel.z + (grounded ? ground.GetZ() : 0.0f));
    ch->SetLinearVelocity(vel);

    updateSettings.mWalkStairsStepUp = JPH::Vec3(0, cc->stepHeight, 0);

    ch->ExtendedUpdate(
        fixedDt,
        baseGravity * cc->gravityScale,
        updateSettings,
        bpFilter, objFilter,
        JPH::BodyFilter{}, JPH::ShapeFilter{},
        *m_impl->tempAllocator);

    cc->_grounded = ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
    cc->_desiredVel = { 0.0f, 0.0f, 0.0f };
}

void PhysicsSystem::SetCharacterPosition(entt::entity entity, DirectX::XMFLOAT3 pos)
{
    if (!m_initialized) return;
    auto it = m_impl->characters.find(entity);
    if (it == m_impl->characters.end()) return;
    it->second->SetPosition(JPH::RVec3(pos.x, pos.y, pos.z));
    // テレポートなので補間の起点も飛ばす（さもないと描画が旧位置から新位置へ 1 ステップ舐める）
    m_impl->prevCharPos[entity] = JPH::RVec3(pos.x, pos.y, pos.z);
}

void PhysicsSystem::SyncCharactersToTransforms(entt::registry& registry)
{
    if (!m_initialized || m_impl->characters.empty()) return;
    for (auto& [entity, ch] : m_impl->characters)
    {
        auto* tf = registry.try_get<Transform>(entity);
        auto* cc = registry.try_get<CharacterController>(entity);
        if (!tf || !cc) continue;
        // GetPosition() = mPosition（mShapeOffset は含まない）。オフセットは mShapeOffset に
        // 一本化したので、Transform へはそのまま書き戻す（- offset しない）。
        //
        // ★描画は「1 固定ステップ前」と現在の間を accumulator の端数で補間する。
        //   物理は 60Hz 固定だが描画はそうではないので、素の位置を書くと
        //   「同じ座標のフレーム」と「2 ステップ分飛ぶフレーム」が混ざり、
        //   一人称視点では歩行が細かくガタつく（＝これが無かった頃の症状）。
        //   最大 1 ステップぶん遅れるだけで、当たり判定は素の位置のままなので
        //   ゲームロジックへの影響は 2cm 未満。
        JPH::RVec3 p = ch->GetPosition();
        auto prev = m_impl->prevCharPos.find(entity);
        if (prev != m_impl->prevCharPos.end())
        {
            const f32 a = std::clamp(m_accumulator / kFixedTimeStep, 0.0f, 1.0f);
            p = prev->second + (p - prev->second) * a;
        }
        tf->position = { static_cast<f32>(p.GetX()),
                         static_cast<f32>(p.GetY()),
                         static_cast<f32>(p.GetZ()) };
        // 回転は CC からは書き戻さない（Yaw は Lua/ゲーム側が Transform.rotation で管理）
        // _desiredVel のゼロ化はここでは行わない。固定dtループで 0 サブステップのフレームでも
        // 無条件にゼロ化すると、その直前の move() 入力が ExtendedUpdate に渡らず取りこぼす。
        // 実際に入力を消費した StepCharacters の末尾でゼロ化する。
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

RaycastHit PhysicsSystem::RaycastEx(XMFLOAT3 origin, XMFLOAT3 direction,
                                    f32 maxDistance, uint32_t ignoreBody) const
{
    RaycastHit result;
    if (!m_initialized) return result;
    if (maxDistance <= 0.0f) return result;

    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    if (XMVectorGetX(XMVector3LengthSq(dir)) < 0.5f) return result;   // ゼロ方向
    XMFLOAT3 normDir;
    XMStoreFloat3(&normDir, dir);

    const JPH::RRayCast ray(
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(normDir.x * maxDistance, normDir.y * maxDistance, normDir.z * maxDistance));

    // セルフヒット除外。CharacterVirtual はブロードフェーズに body を持たないので
    // キャラコン使用時は不要だが、RigidBody キャラや足元の自前コライダー用に用意する。
    JPH::IgnoreSingleBodyFilter selfFilter{JPH::BodyID(ignoreBody)};
    const JPH::BodyFilter defaultFilter;
    const JPH::BodyFilter& bodyFilter =
        (ignoreBody != 0xFFFFFFFFu) ? static_cast<const JPH::BodyFilter&>(selfFilter)
                                    : defaultFilter;

    JPH::RayCastResult hit;
    if (!m_impl->physicsSystem->GetNarrowPhaseQuery().CastRay(
            ray, hit, {}, {}, bodyFilter))
    {
        return result;
    }

    result.hit      = true;
    result.distance = hit.mFraction * maxDistance;
    result.bodyId   = hit.mBodyID.GetIndexAndSequenceNumber();

    const JPH::RVec3 hitPoint = ray.GetPointOnRay(hit.mFraction);
    result.point = { static_cast<f32>(hitPoint.GetX()),
                     static_cast<f32>(hitPoint.GetY()),
                     static_cast<f32>(hitPoint.GetZ()) };

    // ★本物の面法線★ サブシェイプ ID とヒット点から取る。
    // Body へ触るのでロックが要る（HeightField / Mesh でも三角形の法線が返る）。
    result.normal = { 0.0f, 1.0f, 0.0f };   // ロックに失敗したときの保険
    {
        const JPH::BodyLockRead lock(m_impl->physicsSystem->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPoint);
            if (n.LengthSq() > 1.0e-6f)
                result.normal = { n.GetX(), n.GetY(), n.GetZ() };
        }
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

entt::entity PhysicsSystem::EntityForBody(uint32_t bodyId) const
{
    auto it = m_bodyToEntity.find(bodyId);
    return it != m_bodyToEntity.end() ? it->second : entt::null;
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
