#include "physics/PhysicsSystem.h"
#include "physics/ColliderShape.h"   // 当たり判定の実効サイズの唯一の規約
#include "physics/PhysicsLogic.h"    // 補間・キャラと動的剛体の押し合いの規約（純粋関数。ctest で固定）
#include "ecs/Components.h"
#include "ecs/InstanceGroup.h"   // インスタンス群のコライダー
#include "ecs/EditorFlags.h"   // EntityDisabled（無効なエンティティは剛体 / キャラを作らない）
#include "scene/MissingModel.h"   // モデルが読めなかったエンティティ（ヘッダのみ。判定は作らない）
#include "core/Logger.h"
#include "terrain/HeightField.h"   // 地形コライダー（Terrain::_hf の高さ配列を Jolt へ渡す）
#include "terrain/SculptMesh.h"    // スカルプトコライダー（SculptMesh::_data の三角形を Jolt へ渡す）
#include "renderer/Mesh.h"      // meshCollider（MeshRenderer のメッシュを Jolt へ渡す）

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>

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
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>   // 複合形状の回転精度（w≈0 の回避）   // インスタンス群（≤128 個ごとに 1 ボディ）  // meshCollider（形状はモデル単位で共有し、拡縮だけ被せる）
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>   // AnyHitCollisionCollector（キャラとのオーバーラップ / 始点内判定）
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/EPhysicsUpdateError.h>   // Update の戻り値（上限超過）
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>       // Raycast: 真の面法線を取るための BodyLockRead
#include <Jolt/Physics/Body/MotionProperties.h> // 動的剛体の質量（逆質量）を読む
#include <Jolt/Physics/Body/BodyFilter.h>     // Raycast: セルフヒット除外
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


// ---- CharacterVirtual ⇔ 動的剛体の押し合いの規約 ----
//
// 既定のままだと、質量 70kg のキャラが既定質量 1kg の箱に載った瞬間、Jolt が体重ぶんの
// インパルスを箱へ与え（反発係数 0.4 で跳ね返って）箱が十数 m/s で飛ぶ。さらに箱の速度を
// 接地面速度としてキャラが拾うので、キャラまで吹き飛ぶ（実測: 箱 17m/s・キャラ数百 m/s）。
// 数値は PhysicsLogic.h（tests/physics_logic_test.cpp が固定）。
//
//   ・足元（接触法線がキャラの下向き）の動的剛体: Jolt はキャラの mMass×重力 のインパルスを毎ステップ
//     その剛体へ与える。この mMass を、剛体の質量に見合う実効質量へ頭打ちにする（StandingMass。
//     軽い箱は 70kg → 約 1kg 相当、約 65kg 以上の剛体は体重そのまま）。StepOneCharacter が毎ステップ頭で元へ戻す。
//   ・横の接触（歩いて押す）: 従来どおりキャラが剛体を押せる（強さは mMaxStrength）。
//   ・キャラの半分未満の軽い剛体は、キャラを押し返せない(mCanPushCharacter=false)。
//     足元の軽い箱が揺れても、キャラは静止した面に立っているものとして扱われる。
//   ・接地面速度の継承も質量で絞る（StepOneCharacter / GroundVelocityScale）。
// 静的/キネマティックには何もしない（動く床・エレベーターは従来どおり運ぶ）。
class EngineCharacterListener final : public JPH::CharacterContactListener
{
public:
    explicit EngineCharacterListener(JPH::PhysicsSystem* sys) : m_sys(sys) {}

    void OnContactAdded(const JPH::CharacterVirtual* ch, const JPH::BodyID& body, const JPH::SubShapeID&,
                        JPH::RVec3Arg, JPH::Vec3Arg normal, JPH::CharacterContactSettings& io) override
    { Classify(ch, body, normal, io); }

    void OnContactPersisted(const JPH::CharacterVirtual* ch, const JPH::BodyID& body, const JPH::SubShapeID&,
                            JPH::RVec3Arg, JPH::Vec3Arg normal, JPH::CharacterContactSettings& io) override
    { Classify(ch, body, normal, io); }

    // 動的剛体の質量(kg)。動的でなければ負（= 静的/キネマティックは触らない）。
    // メインスレッド(物理ステップの外)からだけ呼ぶので NoLock で読む。
    static f32 DynamicBodyMass(const JPH::PhysicsSystem* sys, const JPH::BodyID& id)
    {
        if (id.IsInvalid()) return -1.0f;
        const JPH::BodyLockRead lock(sys->GetBodyLockInterfaceNoLock(), id);
        if (!lock.Succeeded()) return -1.0f;
        const JPH::Body& b = lock.GetBody();
        if (!b.IsDynamic()) return -1.0f;
        const f32 inv = b.GetMotionProperties()->GetInverseMass();
        return inv > 1.0e-9f ? 1.0f / inv : 1.0e9f;
    }

private:
    void Classify(const JPH::CharacterVirtual* ch, const JPH::BodyID& body, JPH::Vec3Arg normal,
                  JPH::CharacterContactSettings& io) const
    {
        const f32 bodyMass = DynamicBodyMass(m_sys, body);
        if (bodyMass < 0.0f) return;   // 静的/キネマティック: 既定のまま
        const f32 charMass = ch->GetMass();
        if (!physlogic::CanBodyPushCharacter(bodyMass, charMass))
            io.mCanPushCharacter = false;
        // 足元（法線がキャラの下方向と 45 度以内）の動的剛体へ掛かる体重を、剛体の質量に見合う実効質量へ
        // 頭打ちにする（Jolt はこの mMass で毎ステップ体重インパルスを与える）。
        // StepOneCharacter が毎ステップ頭で mMass を元へ戻すので、ここでは min で下げるだけ。
        // ★contact normal は「キャラから接触面へ」向く（足元の面は下向き＝ -up。実測 ny=-1）。
        // ★mCanReceiveImpulses=false で体重を止める案は【不可】: 実測で箱が 27m/s で飛んだ
        //   （足元以外の接触点にだけ押し出しが残るため）。質量を下げる方が安定する。
        if (normal.Dot(ch->GetUp()) < -0.7f)
        {
            const f32 cap = physlogic::StandingMass(charMass, m_sys->GetGravity().Length(), bodyMass);
            if (cap < ch->GetMass())
                const_cast<JPH::CharacterVirtual*>(ch)->SetMass(cap);
        }
    }

    JPH::PhysicsSystem* m_sys;
};

} // anonymous namespace

// ========== JoltImpl ==========

struct PhysicsSystem::JoltImpl
{
    // 一時メモリ。固定領域（kTempAllocatorBytes）が足りなくなったら malloc に逃がす
    // （素の TempAllocatorImpl は足りないと std::abort() する。接触が増える大きなシーンで落ちるのを避ける）。
    std::unique_ptr<JPH::TempAllocator>        tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool>  jobSystem;
    BPLayerInterface                           bpLayerInterface;
    ObjVsBPLayerFilter                         objVsBpFilter;
    ObjLayerPairFilter                         objLayerPairFilter;
    std::unique_ptr<JPH::PhysicsSystem>        physicsSystem;
    std::unique_ptr<JPH::ContactListener>      contactListener; // EngineContactListener
    // 全キャラ共通。characters より前に宣言＝キャラより後に破棄される（キャラが生きている間は有効）。
    std::unique_ptr<EngineCharacterListener>   charListener;
    // キャラ同士の衝突（CharacterVirtual はボディを持たないので、互いに素通りしていた）。
    // characters より前に宣言＝キャラより後に破棄される（キャラは破棄前に Remove する）。
    JPH::CharacterVsCharacterCollisionSimple   charVsChar;

    // entity → CharacterVirtual（Play 中のみ）。Ref で寿命管理。
    // メンバ宣言順では characters が physicsSystem の後ろ＝デストラクト時に先に破棄される
    // ので、CharacterVirtual が生きている PhysicsSystem を参照する破棄順は安全。
    std::unordered_map<entt::entity, JPH::Ref<JPH::CharacterVirtual>> characters;
    // キャラの「1 固定ステップ前」の位置。描画は current との間を accumulator の
    // 端数で補間する（→ SyncCharactersToTransforms）。ここに置いてあるのは
    // CharacterController のレイアウトを変えずに済ませるため。
    std::unordered_map<entt::entity, JPH::RVec3> prevCharPos;

    // 動的剛体の「1 固定ステップ前」の姿勢（描画補間の起点。キャラの prevCharPos の剛体版）。
    // stamp = 控えたときのステップ番号。最後のステップで控えていないもの（そのステップ中に
    // 起きた/寝ていた）は古い姿勢なので補間に使わず、現在の姿勢をそのまま書く。
    struct BodyPrev
    {
        JPH::RVec3 pos = JPH::RVec3::sZero();
        JPH::Quat  rot = JPH::Quat::sIdentity();
        u32        stamp = 0;
        bool       interpolated = false;   // 補間値(alpha<1)を Transform に書いたまま。寝る時に最終姿勢で上書きする
    };
    std::unordered_map<uint32_t, BodyPrev> prevBody;
    u32 stepStamp = 0;   // 実行した固定ステップの通し番号

    // MeshCollider の三角形形状キャッシュ。キー = modelPath + "|" + scale。
    // 同じモデルを何千個置く「レベル丸ごと取り込み」で BVH を毎回組むと数十秒かかるので、
    // 形状は共有する（Jolt の Shape は refcount 管理＝複数ボディから安全に指せる）。
    std::unordered_map<std::string, JPH::RefConst<JPH::Shape>> meshShapeCache;

    // 静的 / キネマティックの「いまの Jolt 側の状態」の控え（Play 中の Transform・スケール・コライダー部品の
    // 変更を拾うため）。chain = 親を含む Transform 生値のハッシュ、comp = コライダー部品 + RigidBody の値のハッシュ、
    // shape = comp + ワールドスケール（これが変われば形状から作り直す）、pos/rot = 置いた姿勢。
    struct BodySig
    {
        uint64_t   chain = 0;
        uint64_t   comp  = 0;
        uint64_t   shape = 0;
        JPH::RVec3 pos   = JPH::RVec3::sZero();
        JPH::Quat  rot   = JPH::Quat::sIdentity();
    };
    std::unordered_map<uint32_t, BodySig> bodySig;
    // FollowStaticChanges が巡回する静的 / キネマティックのエンティティ（登録時に足し、消えたものは巡回中に外す）。
    // ★registry の view を毎フレーム全部なめると、13,522 体（Dead Mall）で 0.7ms かかった（キャッシュミス）ので、
    //   この配列の一部（kStaticScanPerFrame 体）だけを毎フレーム見る。
    std::vector<entt::entity>            trackList;
    std::unordered_set<entt::entity>     trackSet;
    size_t                               scanCursor = 0;

    // 接触イベントの整理（enter の重複・「寝た」による偽の exit の抑止）。キーは 2 つのボディ ID の対。
    //   contactCount: 触れている間のサブシェイプ対の数（0 になった時だけ exit を出す）
    //   contactSuspended: 寝て接触が外れた対（起きて再び触れても enter を出さない / 本当に離れた時だけ exit）
    std::unordered_map<uint64_t, int> contactCount;
    std::unordered_set<uint64_t>      contactSuspended;
};

// 一時メモリの固定領域。接触拘束バッファ（maxContactConstraints × 約 250 バイト）を毎ステップ確保するので、
// 上限を上げるならここも同時に上げる。足りない分は TempAllocatorImplWithMallocFallback が malloc で補う。
static constexpr JPH::uint kTempAllocatorBytes = 64u * 1024u * 1024u;

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

// Jolt の上限（Initialize で固定される）。診断（PhysicsStats）にも出す。
static constexpr JPH::uint kMaxBodies             = 131072;
static constexpr JPH::uint kMaxBodyPairs          = 131072;
static constexpr JPH::uint kMaxContactConstraints = 65536;

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

    // 一時メモリ（固定 64MB + 足りなければ malloc。素の TempAllocatorImpl は足りないと abort する）, 4 job threads
    m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(kTempAllocatorBytes);
    m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 4);

    // ★body 上限。4096 だとレベルを丸ごと取り込む使い方（実測: Dreamcore の Dead_Mall は
    //   静的コライダー 13,522 個）で【黙って作られない】。Jolt は上限に達すると
    //   CreateAndAddBody が無効な BodyID を返すだけでログも出さないので、
    //   「一部の床だけすり抜ける」「Play した瞬間に落ち続ける」という形でしか気付けない。
    //   静的 body 1 個あたりのコストは数百バイト程度なので、上限は素直に大きく取る。
    // ★2026-10 に引き上げ: 密に積んだ動的の箱は 2,560 個で接触拘束（8192）が尽き、床を抜けて落ち続けた
    //   （戻り値もログも無し）。静的の箱 70,000 個では 65,536 個で打ち止めになり 4,464 個が判定なしだった。
    //   上限を超えても Jolt は黙って接触 / ボディを捨てるだけなので、Update の戻り値を StepJolt が見て通知する。
    //   接触拘束バッファは一時メモリから毎ステップ確保されるので、kTempAllocatorBytes と一緒に動かすこと。
    constexpr JPH::uint maxBodies             = kMaxBodies;
    constexpr JPH::uint numBodyMutexes        = 0; // default
    constexpr JPH::uint maxBodyPairs           = kMaxBodyPairs;
    constexpr JPH::uint maxContactConstraints  = kMaxContactConstraints;

    m_impl->physicsSystem = std::make_unique<JPH::PhysicsSystem>();
    m_impl->physicsSystem->Init(
        maxBodies, numBodyMutexes, maxBodyPairs, maxContactConstraints,
        m_impl->bpLayerInterface, m_impl->objVsBpFilter, m_impl->objLayerPairFilter);

    // ★スリープの速度しきい値を Jolt 既定の 0.03 から 0.05 m/s へ（2026-10）。
    //   1kg の箱を 10 段積んだ塔は、静止中も 0.04〜0.06 m/s の微小な揺れが残り（既定のしきい値 0.03 を超えて寝られず）、
    //   揺れが数秒かけて育って 15 秒経っても落ち着かない / 倒れる（実測: 12 本中 5 本が倒壊・4 本が揺れ続け）。
    //   速度反復を 30・位置反復を 8 まで上げても、Baumgarte・スロップ・投機接触距離を変えても直らない
    //   （= ソルバの精度ではなく「寝られない」ことが原因）。0.05 にすると 12 本すべてが 1〜2 秒で寝て静止する。
    //   見た目への影響: 秒速 5cm 未満で 0.5 秒動き続けた物は止まる（転がる球の最後の減速など。目では分からない）。
    {
        JPH::PhysicsSettings ps = m_impl->physicsSystem->GetPhysicsSettings();
        ps.mPointVelocitySleepThreshold = 0.05f;
        m_impl->physicsSystem->SetPhysicsSettings(ps);
    }
    // Gravity: Y-up, ゲーム向けにやや強め（リアル=9.81、ゲーム=14.0）
    m_impl->physicsSystem->SetGravity(JPH::Vec3(0.0f, -14.0f, 0.0f));

    // ContactListener 登録（接触イベントを pending バッファへ積む）
    auto listener = std::make_unique<EngineContactListener>();
    m_impl->physicsSystem->SetContactListener(listener.get());
    m_impl->contactListener = std::move(listener);

    m_accumulator = 0.0f;
    m_updateErrorFlags = 0; m_updateErrorSteps = 0; m_updateErrorLogged = 0;
    m_failedBodies = 0; m_failedBodiesLogged = 0; m_droppedSteps = 0; m_lastFrameSteps = 0;
    m_frame = 0; m_regBackoff.clear(); m_regWarned.clear();
    m_initialized = true;
    Logger::Info("PhysicsSystem initialized (Jolt Physics, bodies<={} pairs<={} contacts<={})",
                 maxBodies, maxBodyPairs, maxContactConstraints);
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

    // ★Play 中に足された RigidBody / CharacterController をここで拾って登録する。
    //   以前は Play 開始とランタイムのシーン切替の一括スイープでしか登録しておらず、
    //   OnUpdate や time.after の中で spawnBox → addBoxCollider → addRigidBody しても
    //   **Jolt のボディが作られないまま**だった（箱は空中に固まり、当たりもしない。
    //   ログもエラーも出ない）。addCharacterController も同じで、move/jump/isGrounded が
    //   全部黙って無効になっていた。「OnStart では動いたのに time.after に移したら動かない」
    //   という形で出る。地形の _colliderDirty 消化と同じ場所・同じ流儀で毎フレーム拾う。
    ++m_frame;
    RegisterPendingPhysicsBodies(registry);
    // ★逆に、消えたエンティティのボディを Jolt から外す。
    //   Scene::Remove は registry.destroy を呼ぶだけで UnregisterBody を呼ばず、
    //   registry に on_destroy フックも無かったので、`scene:remove(e)` で消した
    //   敵・箱・弾の**当たり判定だけが最後の位置に残り続けていた**（見えない壁になる）。
    //   overlapSphere / overlapBox も破棄済みエンティティを返し続け、
    //   Jolt の body 上限（maxBodies）にも効くので、弾のような使い捨てで枯渇する。
    ReleaseOrphanedPhysicsBodies(registry);

    // 地形を彫った直後なら、ステップ前にコライダーを作り直しておく
    //（描画メッシュと当たり判定が 1 フレームでもズレないようにする）。
    RefreshTerrainColliders(registry);
    // 彫ったスカルプトメッシュも同じ理屈でコライダーを作り直す（ストローク終了時にだけ立つ）。
    RefreshSculptColliders(registry);
    // インスタンス群のコライダー（InstanceSet の差し替え / 群の移動で作り直す）。
    RefreshInstanceGroupColliders(registry);
    // Play 中に動かした静的 / キネマティックの壁・拡縮・コライダー部品の変更を Jolt へ反映する。
    FollowStaticChanges(registry);

    // ★時間の扱い（2026-10 見直し）。
    //   以前は「dt が 4 ステップ(66.7ms)を超えたら 1 ステップに潰す」で、15fps の前後で物理の速さが
    //   4 倍違う崖があった（dt=0.1s のフレームを続けるとゲーム時間 1.28s で物理は 0.2s ぶんしか進まない）。
    //   今は dt を kMaxFrameDt(0.25s) へ丸め、1 フレームに進める固定ステップを kMaxStepsPerFrame(8) までにする。
    //   超えた分の時間は捨てる（遅れを次フレームへ持ち越すと、重いシーンで余計に重くなる死のスパイラル）。
    //   → fps が下がるほど物理は連続的に遅くなるだけで、崖は無い（7.5fps 以上は実時間どおり）。
    //   1 秒を超える dt はロード直後・モード切替の停止とみなし、1 ステップだけ進める（従来と同じ）。
    // ★SyncTransformsToPhysics より前に決める。キネマティック体を MoveKinematic で
    //   動かすので、そこで「実際にシミュレートされる時間」を使う（クランプ前の巨大な dt を渡さない）。
    if (!(dt > 0.0f)) dt = 0.0f;
    if (dt > 1.0f)             dt = kFixedTimeStep;
    else if (dt > kMaxFrameDt) dt = kMaxFrameDt;

    // ★MoveKinematic へ渡すのは「このフレームで実際にシミュレートされる時間」。
    //   フレームの dt をそのまま渡すと、
    //     - dt が極端に小さいフレーム（0 サブステップ）で velocity = 差分/dt が発散する
    //     - dt と実際に進む時間（fixed の整数倍）がズレて毎フレーム行き過ぎ／戻りする
    //   実際、フレーム dt を渡した版は動く床を 2 秒動かしただけで
    //   Jolt の中で FLT_OVERFLOW（Body::UpdateSleepStateInternal）を起こして落ちた。
    //   このフレームで 0 ステップなら 0 を渡し、キネマティックの更新自体を見送る
    //   （差分は次のフレームへ持ち越され、そこでまとめて適用される）。
    const int plannedSteps = (std::min)(
        static_cast<int>(std::floor((m_accumulator + dt) / kFixedTimeStep)), kMaxStepsPerFrame);
    const f32 simDt = static_cast<f32>(plannedSteps) * kFixedTimeStep;
    SyncTransformsToPhysics(registry, simDt);

    m_accumulator += dt;
    int steppedCount = 0;
    while (m_accumulator >= kFixedTimeStep && steppedCount < kMaxStepsPerFrame)
    {
        CapturePrevBodyPoses();   // 描画補間の起点（動的剛体の「このステップを踏む前」の姿勢）
        StepJolt(kFixedTimeStep);
        StepCharacters(kFixedTimeStep, registry);   // 剛体ステップ後にキャラを進める
        ++steppedCount;
        m_accumulator -= kFixedTimeStep;
    }
    if (m_accumulator >= kFixedTimeStep)
    {
        // 上限に達して残った時間は捨てる（端数だけ持ち越す）。
        m_droppedSteps += static_cast<uint64_t>(std::floor(m_accumulator / kFixedTimeStep));
        m_accumulator = std::fmod(m_accumulator, kFixedTimeStep);
    }
    m_lastFrameSteps = static_cast<uint32_t>(steppedCount);

    // ★水平入力のゼロ化は「1 フレームに 1 回」。以前は StepCharacters の中、つまり
    //   サブステップごとにゼロ化していたので、1 フレームで 2 ステップ回る状況
    //   （60fps を下回ると起きる。dt=33ms なら 2 ステップ）では 2 本目以降が
    //   _desiredVel=0 で進み、**水平移動速度がフレームレートに比例して落ちていた**。
    //   鉛直（重力・ジャンプ初速）は _verticalVel が持続するので落ちず、
    //   「60fps では届くジャンプが 30fps では届かない」という形で出る。
    //   0 ステップのフレームでは消さない（次のステップまで入力を取りこぼさないため。
    //   これが元コメントの意図で、そこは維持する）。
    if (steppedCount > 0)
    {
        for (auto [e, cc] : registry.view<CharacterController>().each())
            cc._desiredVel = { 0.0f, 0.0f, 0.0f };
    }

    SyncPhysicsToTransforms(registry);
    SyncCharactersToTransforms(registry);

    // ContactListener の pending をメインスレッドで EventBus へ流す。
    FlushPendingContacts();
}

// Jolt を 1 ステップ進め、戻り値（EPhysicsUpdateError）を記録する。
// ★戻り値を見ないと、接触拘束 / ボディ対 / マニフォールドの上限を超えても気付けない
//   （超えた分は黙って捨てられ、箱が床を抜けて落ち続けるという形でしか出ない）。
//   ERROR は種類ごとにセッション 1 回、以降は 600 ステップごとの要約 + セッション終了時の要約
//   （LogSessionSummary）。毎ステップ出し続けない。
void PhysicsSystem::StepJolt(f32 dt)
{
    const JPH::EPhysicsUpdateError err = m_impl->physicsSystem->Update(
        dt, kCollisionSteps, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
    const uint32_t bits = static_cast<uint32_t>(err);
    if (bits == 0) return;

    m_updateErrorFlags |= bits;
    ++m_updateErrorSteps;
    const uint32_t fresh = bits & ~m_updateErrorLogged;
    if (fresh != 0)
    {
        m_updateErrorLogged |= fresh;
        if (fresh & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ContactConstraintsFull))
            Logger::Error("物理: 接触拘束の上限（{}）を超えました。超えた分の接触は無視され、物が床や壁を抜けることがあります。"
                          "動く剛体の数・密な積み重ねを減らしてください（診断: get_physics_state）", kMaxContactConstraints);
        if (fresh & static_cast<uint32_t>(JPH::EPhysicsUpdateError::ManifoldCacheFull))
            Logger::Error("物理: 接触のキャッシュの上限を超えました。一部の接触が無視されます（接触拘束の上限 {}）", kMaxContactConstraints);
        if (fresh & static_cast<uint32_t>(JPH::EPhysicsUpdateError::BodyPairCacheFull))
            Logger::Error("物理: ボディ対のキャッシュの上限（{}）を超えました。一部の接触が無視されます", kMaxBodyPairs);
    }
    else if (m_updateErrorSteps % 600 == 0)
    {
        Logger::Warn("物理: 上限超過が続いています（{} ステップ・種類 0x{:x}）", m_updateErrorSteps, m_updateErrorFlags);
    }
}

void PhysicsSystem::NoteBodyCreateFailed()
{
    ++m_failedBodies;
    if (m_failedBodiesLogged == 0)
    {
        m_failedBodiesLogged = 1;
        // ★最初の 1 件だけ即座に出す（以降は要約）。Jolt は上限に達しても無効な ID を返すだけで何も言わない。
        Logger::Error("物理: Jolt のボディを作れませんでした（ボディ数の上限 {}）。これ以降のボディは当たり判定が作られません。"
                      "個数は終了時 / get_physics_state に出します", kMaxBodies);
    }
    else if (m_failedBodies >= m_failedBodiesLogged * 4)
    {
        m_failedBodiesLogged = m_failedBodies;
        Logger::Warn("物理: ここまでに {} 個のボディを作れていません（上限 {}）", m_failedBodies, kMaxBodies);
    }
}

// セッション（Play）の終わりに、上限超過の要約を 1 回だけ出して数え直す。
void PhysicsSystem::LogSessionSummary()
{
    if (m_updateErrorSteps > 0)
        Logger::Warn("物理: このセッションで上限超過が {} ステップありました（種類 0x{:x}: 1=キャッシュ 2=ボディ対 4=接触拘束）",
                     m_updateErrorSteps, m_updateErrorFlags);
    if (m_failedBodies > 0)
        Logger::Warn("物理: このセッションで {} 個のボディを作れませんでした（当たり判定なし）", m_failedBodies);
    if (m_droppedSteps > 0)
        Logger::Info("物理: このセッションで、重いフレームのために {} ステップぶんの時間を捨てました（1 フレーム最大 {} ステップ）",
                     m_droppedSteps, kMaxStepsPerFrame);
    m_updateErrorFlags = 0; m_updateErrorSteps = 0; m_updateErrorLogged = 0;
    m_failedBodies = 0; m_failedBodiesLogged = 0; m_droppedSteps = 0;
}

PhysicsStats PhysicsSystem::GetStats() const
{
    PhysicsStats st;
    st.initialized = m_initialized;
    if (!m_initialized || !m_impl) return st;
    st.maxBodies = kMaxBodies; st.maxBodyPairs = kMaxBodyPairs; st.maxContactConstraints = kMaxContactConstraints;
    st.bodies = m_impl->physicsSystem->GetNumBodies();
    st.activeBodies = m_impl->physicsSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    st.tempAllocatorBytes = kTempAllocatorBytes;
    st.updateErrorFlags = m_updateErrorFlags;
    st.updateErrorSteps = m_updateErrorSteps;
    st.failedBodies = m_failedBodies;
    st.droppedSteps = m_droppedSteps;
    st.lastFrameSteps = m_lastFrameSteps;
    return st;
}

void PhysicsSystem::FlushPendingContacts()
{
    if (!m_eventBus || !m_impl || !m_impl->contactListener) return;

    auto* listener = static_cast<EngineContactListener*>(m_impl->contactListener.get());
    auto contacts = listener->Drain();
    const auto& bodyIf = m_impl->physicsSystem->GetBodyInterfaceNoLock();
    for (const auto& c : contacts)
    {
        auto it1 = m_bodyToEntity.find(c.bodyId1);
        auto it2 = m_bodyToEntity.find(c.bodyId2);
        if (it1 == m_bodyToEntity.end() || it2 == m_bodyToEntity.end()) continue;

        // ★接触の整理。Jolt は「寝た」ボディの接触を接触キャッシュから外すので、箱が床に乗ったまま
        //   0.5 秒後（sleep 時間）に OnContactRemoved が呼ばれ、「乗っている間だけ」の仕掛け（感圧板など）が
        //   勝手に外れていた。起きれば enter がもう一度出る問題もあった。
        //   ・同じボディ対のサブシェイプ接触は数えて、最初の enter だけ・最後の exit だけを出す。
        //   ・両方のボディが非アクティブ（= 寝た・静止）のときの exit は「離れた」ではないので出さず、
        //     「寝て外れた対」として覚える。起きて再び触れても enter は出さない。
        //     本当に離れる時は少なくとも片方が動いている（アクティブ）ので、そのとき exit を出す。
        {
            uint32_t lo = c.bodyId1, hi = c.bodyId2;
            if (lo > hi) std::swap(lo, hi);
            const uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
            if (c.isEnter)
            {
                int& cnt = m_impl->contactCount[key];
                const bool resumed = m_impl->contactSuspended.erase(key) > 0;
                ++cnt;
                if (resumed || cnt > 1) continue;
            }
            else
            {
                auto cit = m_impl->contactCount.find(key);
                if (cit != m_impl->contactCount.end())
                {
                    if (--cit->second > 0) continue;   // 別のサブシェイプ対がまだ触れている
                    m_impl->contactCount.erase(cit);
                    const bool asleep = !bodyIf.IsActive(JPH::BodyID(c.bodyId1))
                                     && !bodyIf.IsActive(JPH::BodyID(c.bodyId2));
                    if (asleep) { m_impl->contactSuspended.insert(key); continue; }
                }
            }
        }

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

namespace {

// 親子階層を解決した「ワールドの」位置 / 回転 / スケール。
//
// ★物理は長らく Transform.parent を無視して**ローカル値をそのまま**使っていた。
//   親（グループ化した空エンティティ等）の下に置いた剛体は、当たり判定がローカル座標に
//   作られ、描画は親の分だけずれる＝**見えている箱と当たる箱が別の場所にある**。
//   実測: 親を (10,0,0)、子をローカル (0,6,0) に置いて落とすと、箱は world x=10 に描かれるのに
//   overlapSphere が拾うのは x=0（親なしの対照は一致する）。
//   スケールも同じで、親を 2 倍にしても当たり判定は等倍のままだった。
// MeshRenderer のメッシュから Jolt の三角形メッシュ形状（スケール抜き・静的専用）を作る。
// 作れなければ nullptr。MeshCollider（1 エンティティ 1 ボディ）とインスタンス群が共有する。
JPH::RefConst<JPH::Shape> BuildStaticMeshShape(const MeshRenderer& mr)
{
    JPH::RefConst<JPH::Shape> base;
    JPH::VertexList          verts;
    JPH::IndexedTriangleList tris;
    uint32_t                 vbase = 0;
    for (size_t mi = 0; mi < mr.meshes.size(); ++mi)
    {
        const Mesh* mesh = mr.meshes[mi];
        if (mesh == nullptr) continue;
        const auto& positions = mesh->GetPositions();
        const auto& indices   = mesh->GetIndices();
        if (positions.empty() || indices.size() < 3) continue;

        // 静的モデルはノード変換を頂点へ焼き込み済み（＝単位行列）だが、
        // 焼いていない経路のために持っていれば掛ける。
        DirectX::XMMATRIX node = DirectX::XMMatrixIdentity();
        if (mi < mr.meshNodeTransforms.size())
            node = DirectX::XMLoadFloat4x4(&mr.meshNodeTransforms[mi]);

        verts.reserve(verts.size() + positions.size());
        for (const auto& lp : positions)
        {
            DirectX::XMFLOAT3 wp{};
            DirectX::XMStoreFloat3(&wp,
                DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&lp), node));
            verts.push_back(JPH::Float3(wp.x, wp.y, wp.z));
        }

        tris.reserve(tris.size() + indices.size() / 3);
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
            tris.push_back(JPH::IndexedTriangle(vbase + indices[t],
                                                vbase + indices[t + 1],
                                                vbase + indices[t + 2], 0));
        vbase += static_cast<uint32_t>(positions.size());
    }

    // ★一方向メッシュ対策（両面化）。Jolt の MeshShape は裏面を無視するので、厚さ 0 の一枚板（プリミティブの平面・
    //   クワッド）は下から来た動的剛体が素通りした（レイは裏面にも当たるので「レイでは床があるのに体は抜ける」不一致）。
    //   レイ側を裏面無視に揃える案は、閉じていない / 内側から見るメッシュ（Dead Mall の 3,000 本のレイ一致など）の
    //   既存の当たり方を変えるので採らず、物理側を両面にする: AABB の最小辺が最大辺の 0.1%（下限 0.1mm）以下の
    //   「平面メッシュ」だけ、逆巻きの三角形を足す。厚みのあるメッシュ（閉じた箱・壁）には何もしない。
    //   キャラ（CharacterVirtual）は既定で裏面にも当たる。
    if (!verts.empty() && !tris.empty())
    {
        JPH::Float3 mn = verts[0], mx = verts[0];
        for (const auto& v : verts)
        {
            mn.x = (std::min)(mn.x, v.x); mn.y = (std::min)(mn.y, v.y); mn.z = (std::min)(mn.z, v.z);
            mx.x = (std::max)(mx.x, v.x); mx.y = (std::max)(mx.y, v.y); mx.z = (std::max)(mx.z, v.z);
        }
        const f32 ext[3] = { mx.x - mn.x, mx.y - mn.y, mx.z - mn.z };
        const f32 maxExt = (std::max)({ ext[0], ext[1], ext[2] });
        const f32 minExt = (std::min)({ ext[0], ext[1], ext[2] });
        if (maxExt > 0.0f && minExt <= (std::max)(1.0e-4f, maxExt * 1.0e-3f))
        {
            const size_t n = tris.size();
            tris.reserve(n * 2);
            for (size_t i = 0; i < n; ++i)
                tris.push_back(JPH::IndexedTriangle(tris[i].mIdx[0], tris[i].mIdx[2], tris[i].mIdx[1], 0));
        }
    }

    if (!verts.empty() && !tris.empty())
    {
        JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
        auto result = settings.Create();
        if (result.IsValid()) base = result.Get();
        else Logger::Error("meshCollider の三角形形状を作れませんでした（{}）: {}",
                           mr.modelPath, result.GetError().c_str());
    }
    return base;
}

struct WorldTRS
{
    DirectX::XMFLOAT3 pos{0.0f, 0.0f, 0.0f};
    JPH::Quat         rot = JPH::Quat::sIdentity();
    DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
};

// 単位クォータニオンから外れていたら正規化する（Jolt は正規化済みの回転を前提にする）。
// 外れていなければ 1 ビットも変えない（親なしの大多数は従来と同一）。
JPH::Quat NormalizedIfNeeded(const JPH::Quat& q)
{
    const f32 l2 = q.LengthSq();
    if (std::fabs(l2 - 1.0f) <= 1.0e-4f) return q;
    return l2 > 1.0e-12f ? q.Normalized() : JPH::Quat::sIdentity();
}

// 親なし（大多数）は従来と 1 ビットも変えない。親付きだけワールド行列から取り出す。
// ★分解は collider::DecomposeWorld。せん断（非一様スケールの親 + 回転した子）や 0 スケールで
//   XMMatrixDecompose が失敗しても、近似（せん断を捨てる）で必ず値を返す
//   （以前は失敗すると既定値 = 世界の原点を返し、判定が原点に出ていた）。
WorldTRS ResolveWorldTRS(const entt::registry& reg, entt::entity e, const Transform& t,
                         bool* approximated = nullptr)
{
    WorldTRS out;
    const bool parented = (t.parent != entt::null) && reg.valid(t.parent);

    if (!parented)
    {
        out.pos   = t.position;
        out.scale = t.scale;
        if (t.useQuaternion)
            out.rot = NormalizedIfNeeded(JPH::Quat(t.quaternion.x, t.quaternion.y, t.quaternion.z, t.quaternion.w));
        else
        {
            DirectX::XMFLOAT4 qf;
            DirectX::XMStoreFloat4(&qf, DirectX::XMQuaternionRotationRollPitchYaw(
                DirectX::XMConvertToRadians(t.rotation.x),
                DirectX::XMConvertToRadians(t.rotation.y),
                DirectX::XMConvertToRadians(t.rotation.z)));
            out.rot = JPH::Quat(qf.x, qf.y, qf.z, qf.w);
        }
        return out;
    }

    const collider::WorldDecomposed d = collider::DecomposeWorld(ComputeWorldMatrix(reg, e));
    out.pos   = d.pos;
    out.scale = d.scale;
    out.rot   = NormalizedIfNeeded(JPH::Quat(d.rot.x, d.rot.y, d.rot.z, d.rot.w));
    if (approximated) *approximated = d.approximated;
    return out;
}

// 親のローカル空間の位置へ変換する（親なしならそのまま）。逆行列が取れなければワールド値をそのまま返す。
DirectX::XMFLOAT3 WorldPosToLocalPos(const entt::registry& reg, const Transform& t, const DirectX::XMFLOAT3& worldPos)
{
    const bool parented = (t.parent != entt::null) && reg.valid(t.parent);
    if (!parented) return worldPos;
    DirectX::XMVECTOR det;
    const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(&det, ComputeWorldMatrix(reg, t.parent));
    if (DirectX::XMVectorGetX(DirectX::XMVectorAbs(det)) < 1e-12f) return worldPos;
    DirectX::XMFLOAT3 out;
    DirectX::XMStoreFloat3(&out, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&worldPos), inv));
    return out;
}

// quaternion を書いたら Euler（rotation）も揃える。
// ★物理は quaternion + useQuaternion=true だけを書いていたので、Lua / MCP / Inspector が読む
//   transform.rotation が（落ちて転がった後も）初期値のまま古かった。
inline void SyncEulerFromQuaternion(Transform& t)
{
    t.rotation = QuaternionToEulerDegrees(t.quaternion);
}

// 物理が返したワールドの位置 / 回転を、親のローカル空間へ戻して Transform へ書く。
// 親なしならそのまま代入（従来の挙動）。
void WriteBackWorldToLocal(const entt::registry& reg, Transform& t,
                           const DirectX::XMFLOAT3& worldPos, const JPH::Quat& worldRot)
{
    const bool parented = (t.parent != entt::null) && reg.valid(t.parent);
    if (!parented)
    {
        t.position   = worldPos;
        t.quaternion = { worldRot.GetX(), worldRot.GetY(), worldRot.GetZ(), worldRot.GetW() };
        t.useQuaternion = true;
        SyncEulerFromQuaternion(t);
        return;
    }

    // 親のワールド行列の逆行列を通す。ここを通さないと**ワールド値をローカルへ代入**する形になり、
    // 親のオフセットぶん毎フレーム描画がずれる（親の下では物理が二重に効いて見える）。
    DirectX::XMVECTOR det;
    const DirectX::XMMATRIX parentW   = ComputeWorldMatrix(reg, t.parent);
    const DirectX::XMMATRIX parentInv = DirectX::XMMatrixInverse(&det, parentW);
    if (DirectX::XMVectorGetX(DirectX::XMVectorAbs(det)) < 1e-12f)
    {
        // 親のスケールが 0 等で逆行列が取れない。ワールド値をそのまま入れる（従来と同じ壊れ方）
        t.position   = worldPos;
        t.quaternion = { worldRot.GetX(), worldRot.GetY(), worldRot.GetZ(), worldRot.GetW() };
        t.useQuaternion = true;
        SyncEulerFromQuaternion(t);
        return;
    }

    const DirectX::XMMATRIX world =
        DirectX::XMMatrixRotationQuaternion(
            DirectX::XMVectorSet(worldRot.GetX(), worldRot.GetY(), worldRot.GetZ(), worldRot.GetW()))
        * DirectX::XMMatrixTranslation(worldPos.x, worldPos.y, worldPos.z);

    // 親が非一様スケール + 回転だと world * parentInv にせん断が入り XMMatrixDecompose が失敗する。
    // 近似分解（せん断を捨てる）で必ず値を返す（失敗で黙って書き戻しをやめると、その剛体は止まって見える）。
    const collider::WorldDecomposed d = collider::DecomposeWorld(world * parentInv);
    t.position   = d.pos;
    t.quaternion = d.rot;
    t.useQuaternion = true;
    SyncEulerFromQuaternion(t);
}

// ---- 静的 / キネマティックの変更検出用のハッシュ（Play 中に動かした壁・拡縮・コライダー変更を拾う）----
inline void HashMix(uint64_t& h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}
inline void HashF(uint64_t& h, f32 f)
{
    uint32_t b; std::memcpy(&b, &f, sizeof b);
    HashMix(h, b);
}
inline void HashF3(uint64_t& h, const DirectX::XMFLOAT3& v) { HashF(h, v.x); HashF(h, v.y); HashF(h, v.z); }

// ストレージ（コンポーネントの表）を 1 回だけ引いて持つ。FollowStaticChanges は毎フレーム数千体ぶんの変更検出を回すので、
// エンティティごとに registry.try_get<T>()（型 → 表の探索）を繰り返すと Dead Mall（静的 13,522 体）で 0.5〜0.8ms かかった。
struct PoolSet
{
    using R = const entt::registry;
    R* reg;
    decltype(std::declval<R&>().storage<Transform>())          tf;
    decltype(std::declval<R&>().storage<RigidBody>())          rb;
    decltype(std::declval<R&>().storage<BoxCollider>())        box;
    decltype(std::declval<R&>().storage<SphereCollider>())     sphere;
    decltype(std::declval<R&>().storage<CapsuleCollider>())    capsule;
    decltype(std::declval<R&>().storage<ConvexHullCollider>()) convex;
    decltype(std::declval<R&>().storage<MeshCollider>())       meshCol;
    decltype(std::declval<R&>().storage<MeshRenderer>())       meshRend;
    explicit PoolSet(R& r)
        : reg(&r), tf(r.storage<Transform>()), rb(r.storage<RigidBody>()), box(r.storage<BoxCollider>()), sphere(r.storage<SphereCollider>()),
          capsule(r.storage<CapsuleCollider>()), convex(r.storage<ConvexHullCollider>()),
          meshCol(r.storage<MeshCollider>()), meshRend(r.storage<MeshRenderer>()) {}
    template <class S>
    static auto Get(S* s, entt::entity e) -> decltype(&s->get(e)) { return (s != nullptr && s->contains(e)) ? &s->get(e) : nullptr; }
};

// 親を含む Transform の生値（位置・Euler・スケール・クォータニオン・親）。安価なので毎回の前判定に使う。
uint64_t HashTransformChain(const PoolSet& p, entt::entity e)
{
    uint64_t h = 1469598103934665603ULL;
    for (int depth = 0; depth < 64 && p.reg->valid(e); ++depth)
    {
        const auto* t = PoolSet::Get(p.tf, e);
        if (!t) break;
        HashF3(h, t->position); HashF3(h, t->rotation); HashF3(h, t->scale);
        HashF(h, t->quaternion.x); HashF(h, t->quaternion.y); HashF(h, t->quaternion.z); HashF(h, t->quaternion.w);
        HashMix(h, t->useQuaternion ? 1u : 0u);
        e = t->parent;
    }
    return h;
}
uint64_t HashTransformChain(const entt::registry& reg, entt::entity e) { return HashTransformChain(PoolSet(reg), e); }

// コライダー部品と RigidBody の値（形状を作り直すべきか / 見るもの）。スケールは含めない（親に依存するので別に足す）。
uint64_t HashColliderComps(const PoolSet& p, entt::entity e, const RigidBody& rb)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    HashMix(h, static_cast<uint64_t>(rb.motionType));
    HashF(h, rb.mass); HashF(h, rb.friction); HashF(h, rb.restitution);
    HashF(h, rb.linearDamping); HashF(h, rb.angularDamping);
    HashMix(h, (rb.useGravity ? 1u : 0u) | (rb.continuousCollision ? 2u : 0u));
    if (const auto* c = PoolSet::Get(p.box, e))     { HashMix(h, 1); HashF3(h, c->halfExtents); HashF3(h, c->offset); }
    if (const auto* c = PoolSet::Get(p.sphere, e))  { HashMix(h, 2); HashF(h, c->radius); HashF3(h, c->offset); }
    if (const auto* c = PoolSet::Get(p.capsule, e)) { HashMix(h, 3); HashF(h, c->radius); HashF(h, c->halfHeight); HashF3(h, c->offset); }
    if (const auto* c = PoolSet::Get(p.convex, e))
    {
        HashMix(h, 4); HashF3(h, c->offset); HashMix(h, c->points.size());
        if (!c->points.empty()) { HashF3(h, c->points.front()); HashF3(h, c->points.back()); }
    }
    if (const auto* c = PoolSet::Get(p.meshCol, e))
    {
        HashMix(h, 5); HashF3(h, c->offset);
        if (const auto* mr = PoolSet::Get(p.meshRend, e))
        {
            HashMix(h, mr->meshes.size());
            if (!mr->meshes.empty()) HashMix(h, reinterpret_cast<uintptr_t>(mr->meshes.front()));
        }
    }
    return h;
}
uint64_t HashColliderComps(const entt::registry& reg, entt::entity e, const RigidBody& rb)
{
    return HashColliderComps(PoolSet(reg), e, rb);
}

uint64_t HashShapeKey(uint64_t comp, const DirectX::XMFLOAT3& worldScale)
{
    uint64_t h = comp;
    HashF3(h, worldScale);
    return h;
}

// MeshRenderer のメッシュ形状の署名（頂点数・インデックス数・サンプル頂点）。
// ★形状キャッシュのキーに modelPath だけを使うと、プリミティブ（"__primitive_sphere__" などは大きさが
//   メッシュに焼き込まれているのに modelPath が同じ）の大きさ違いが 1 つ目の形状に取り違えられた。
//   サンプルは全頂点を舐めずに済むよう最大 16 個（先頭・末尾を含む等間隔）。
uint64_t MeshShapeSignature(const MeshRenderer& mr)
{
    uint64_t h = 0x84222325cbf29ce4ULL;
    HashMix(h, mr.meshes.size());
    for (const Mesh* mesh : mr.meshes)
    {
        if (mesh == nullptr) { HashMix(h, 0xdead); continue; }
        const auto& pos = mesh->GetPositions();
        HashMix(h, pos.size());
        HashMix(h, mesh->GetIndices().size());
        if (pos.empty()) continue;
        const size_t n = pos.size();
        const size_t samples = (std::min<size_t>)(16, n);
        for (size_t i = 0; i < samples; ++i)
            HashF3(h, pos[samples > 1 ? i * (n - 1) / (samples - 1) : 0]);
    }
    return h;
}

// 接触イベントの整理テーブルから、消えたボディを含む対を捨てる。
void PurgeContactState(std::unordered_map<uint64_t, int>& counts, std::unordered_set<uint64_t>& suspended, uint32_t bodyId)
{
    auto has = [bodyId](uint64_t key) {
        return static_cast<uint32_t>(key >> 32) == bodyId || static_cast<uint32_t>(key & 0xFFFFFFFFu) == bodyId;
    };
    for (auto it = counts.begin(); it != counts.end(); )
        it = has(it->first) ? counts.erase(it) : std::next(it);
    for (auto it = suspended.begin(); it != suspended.end(); )
        it = has(*it) ? suspended.erase(it) : std::next(it);
}

} // namespace

void PhysicsSystem::SyncTransformsToPhysics(entt::registry& registry, f32 dt)
{
    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterfaceNoLock();

    auto view = registry.view<Transform, RigidBody>();
    for (auto [entity, transform, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;
        if (rb.motionType != MotionType::Kinematic) continue;

        JPH::BodyID joltId(rb.bodyId);
        // ★親子を解決したワールド値で送る（親なしなら従来と同一）
        const WorldTRS w = ResolveWorldTRS(registry, entity, transform);
        JPH::RVec3 pos(w.pos.x, w.pos.y, w.pos.z);
        JPH::Quat  rot = w.rot;

        // ★以前は SetPositionAndRotation で毎フレーム**テレポート**していた。
        //   それだと Jolt 側の MotionProperties に速度が入らないので
        //   CharacterVirtual::GetGroundVelocity() が常に 0 になり、
        //   StepCharacters の接地面速度の加算（下の `+ ground.GetX()`）が死にコードだった。
        //   ＝**動く床・エレベーター・回転床に乗ってもキャラが運ばれない**。
        //   MoveKinematic は目標位置から速度を逆算するので、接地面速度と押し出しの
        //   両方が同時に効くようになる（毎フレーム現在位置から引き直すので誤差は自己補正される）。
        // dt=0（このフレームは 1 ステップも進まない）なら何もしない。差分は次フレームへ。
        if (dt <= 0.0f) continue;

        // 大きく飛ぶとき（シーン読み込み・リスポーン・physics:setPosition 相当）は
        // 速度に直すと極端な値になるのでテレポートにする。動く床の速度は高が知れているので、
        // 1 ステップ 10m を超えたら「移動」ではなく「瞬間移動」とみなす。
        const JPH::RVec3 cur = bodyInterface.GetPosition(joltId);
        const float      dist = (pos - cur).Length();
        if (dist > 10.0f)
            bodyInterface.SetPositionAndRotation(joltId, pos, rot, JPH::EActivation::DontActivate);
        else
            bodyInterface.MoveKinematic(joltId, pos, rot, dt);
    }
}

// 動的剛体の「このステップを踏む前」の姿勢を控える。物理ステップの直前に毎回呼ぶ。
// 起きている剛体だけが対象（寝ている物は動かないので控える必要が無い）。
void PhysicsSystem::CapturePrevBodyPoses()
{
    auto& physics = *m_impl->physicsSystem;
    auto& bodyInterface = physics.GetBodyInterfaceNoLock();
    ++m_impl->stepStamp;

    const JPH::BodyID* active = physics.GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
    const JPH::uint    count  = physics.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    for (JPH::uint i = 0; i < count; ++i)
    {
        const JPH::BodyID id = active[i];
        if (bodyInterface.GetMotionType(id) != JPH::EMotionType::Dynamic) continue;
        auto& prev = m_impl->prevBody[id.GetIndexAndSequenceNumber()];
        prev.pos   = bodyInterface.GetPosition(id);
        prev.rot   = bodyInterface.GetRotation(id);
        prev.stamp = m_impl->stepStamp;
    }
}

void PhysicsSystem::SyncPhysicsToTransforms(entt::registry& registry)
{
    SyncPhysicsToTransformsAlpha(registry, -1.0f);
}

void PhysicsSystem::SyncPhysicsToTransformsAlpha(entt::registry& registry, f32 alphaOverride)
{
    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterfaceNoLock();

    // ★描画用の姿勢は「1 固定ステップ前」と現在の間を accumulator の端数で補間して書く
    //   （キャラの SyncCharactersToTransforms と同じ流儀）。
    //   物理は 60Hz 固定、描画は 144fps 等なので、素の姿勢を書くと「同じ姿勢のフレーム」と
    //   「2 ステップぶん飛ぶフレーム」が混ざって、転がる・落ちる剛体ががくがく動いて見える
    //   （実測: 144fps で落下中の箱の 58% のフレームが移動 0）。しかもキャラ（カメラ）だけが
    //   補間されていたので、相対的にもガタついて見えていた。
    //
    //   【影響】Transform は最大 1 固定ステップ(16.7ms)遅れた姿勢になる。ゲームロジックが
    //   Transform から読む剛体の位置は、その分だけ過去（60Hz 換算で数 cm）。
    //   物理クエリ（raycast / overlap / ground チェック）は Jolt 上の素の姿勢を使うので、
    //   見た目だけが遅れ、当たり判定は遅れない。
    //   キネマティックは対象外（ゲームが Transform を動かす側で、ここは書き戻さない）。
    //   寝ている剛体は従来どおり書かない（寝る直前に補間値が残っていたら最終姿勢で 1 回だけ上書きする）。
    //   テレポート（physics:setPosition/warp・1 ステップで 5m を超える移動・寝ていた物が起きた直後）は
    //   補間せず現在の姿勢をそのまま出す。
    // alphaOverride >= 0 は手動ステップ（physics:step）後の 1.0 = 補間なしで最終姿勢を書く。
    const f32 alpha = alphaOverride >= 0.0f ? alphaOverride
                                            : physlogic::InterpAlpha(m_accumulator, kFixedTimeStep);

    auto view = registry.view<Transform, RigidBody>();
    for (auto [entity, transform, rb] : view.each())
    {
        if (rb.bodyId == kInvalidBodyId) continue;
        if (rb.motionType != MotionType::Dynamic) continue;

        JPH::BodyID joltId(rb.bodyId);

        auto prevIt = m_impl->prevBody.find(rb.bodyId);
        const bool active = bodyInterface.IsActive(joltId);
        if (!active)
        {
            // 寝た。補間値のまま止まっていたら、最終姿勢（Jolt 上の素の姿勢）で 1 回だけ確定させる。
            if (prevIt == m_impl->prevBody.end() || !prevIt->second.interpolated) continue;
            prevIt->second.interpolated = false;
        }

        JPH::RVec3 pos = bodyInterface.GetPosition(joltId);
        JPH::Quat  rot = bodyInterface.GetRotation(joltId);

        if (active && prevIt != m_impl->prevBody.end() && prevIt->second.stamp == m_impl->stepStamp)
        {
            auto& prev = prevIt->second;
            const JPH::RVec3 d = pos - prev.pos;
            if (!physlogic::IsTeleport(static_cast<f32>(d.GetX()), static_cast<f32>(d.GetY()),
                                       static_cast<f32>(d.GetZ())))
            {
                const f32 a3[3] = { static_cast<f32>(prev.pos.GetX()), static_cast<f32>(prev.pos.GetY()),
                                    static_cast<f32>(prev.pos.GetZ()) };
                const f32 b3[3] = { static_cast<f32>(pos.GetX()), static_cast<f32>(pos.GetY()),
                                    static_cast<f32>(pos.GetZ()) };
                f32 p3[3];
                physlogic::LerpVec3(a3, b3, alpha, p3);
                const f32 a4[4] = { prev.rot.GetX(), prev.rot.GetY(), prev.rot.GetZ(), prev.rot.GetW() };
                const f32 b4[4] = { rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW() };
                f32 q4[4];
                physlogic::NlerpQuat(a4, b4, alpha, q4);
                pos = JPH::RVec3(p3[0], p3[1], p3[2]);
                rot = JPH::Quat(q4[0], q4[1], q4[2], q4[3]);
                prev.interpolated = alpha < 1.0f;
            }
            else
            {
                prev.interpolated = false;
            }
        }
        else if (prevIt != m_impl->prevBody.end())
        {
            prevIt->second.interpolated = false;
        }

        // ★ボディの原点 = エンティティの原点（コライダーの offset は形状の内側 = RotatedTranslatedShape に入れてある）
        //   ので、offset を足し引きせずそのまま書き戻す。以前は offset をワールド軸で足していたので、回転すると
        //   見た目の軸と判定の軸がずれ、physics:setPosition は offset ぶんずれた Transform を作っていた。
        // ★親のローカル空間へ戻してから書く。以前は**ワールド値をローカルへ代入**していたので、
        //   親の下に置いた剛体は毎フレーム親のオフセットぶんずれた場所に描かれていた。
        const DirectX::XMFLOAT3 worldPos{ static_cast<f32>(pos.GetX()),
                                          static_cast<f32>(pos.GetY()),
                                          static_cast<f32>(pos.GetZ()) };
        WriteBackWorldToLocal(registry, transform, worldPos, rot);
    }
}

// エンティティが消えたのに残っている Jolt のボディ / キャラを解放する。
// UnregisterBody は RigidBody コンポーネントを見るので、破棄済みエンティティには使えない
// （component ごと消えている）。ここでは bodyId から直接外す。
void PhysicsSystem::ReleaseOrphanedPhysicsBodies(entt::registry& registry)
{
    if (!m_impl) return;

    {
        auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
        for (auto it = m_bodyToEntity.begin(); it != m_bodyToEntity.end(); )
        {
            const bool alive = registry.valid(it->second)
                            && registry.all_of<RigidBody>(it->second);
            if (alive) { ++it; continue; }
            JPH::BodyID joltId(it->first);
            bodyInterface.RemoveBody(joltId);
            bodyInterface.DestroyBody(joltId);
            m_impl->prevBody.erase(it->first);
            m_impl->bodySig.erase(it->first);
            PurgeContactState(m_impl->contactCount, m_impl->contactSuspended, it->first);
            it = m_bodyToEntity.erase(it);
        }
    }

    for (auto it = m_impl->characters.begin(); it != m_impl->characters.end(); )
    {
        const bool alive = registry.valid(it->first)
                        && registry.all_of<CharacterController>(it->first);
        if (alive) { ++it; continue; }
        m_impl->prevCharPos.erase(it->first);
        m_impl->charVsChar.Remove(it->second.GetPtr());
        it = m_impl->characters.erase(it);   // Ref 解放で CharacterVirtual も落ちる
    }
}

// 未登録の RigidBody / CharacterController を拾って Jolt へ登録する。
// Play 開始時の一括スイープと同じことを毎フレームの取りこぼしぶんだけ行う。
void PhysicsSystem::RegisterPendingPhysicsBodies(entt::registry& registry)
{
    // 反復中に registry を触らないよう、対象を集めてから登録する。
    std::vector<entt::entity> pendingBodies;
    for (auto [e, rb] : registry.view<RigidBody>().each())
    {
        if (rb.bodyId != kInvalidBodyId || m_groupBodies.count(e)) continue;   // 群は登録済み（空でも）なら再試行しない
        // ★作れなかったもの（メッシュが空・ボディ上限）は 120 フレーム（約 2 秒）おきにしか再試行しない。
        //   以前は毎フレーム再試行して、そのたびに警告を出していた（1 エンティティで 120 フレームに 133 行）。
        if (auto bo = m_regBackoff.find(e); bo != m_regBackoff.end() && m_frame < bo->second) continue;
        pendingBodies.push_back(e);
    }
    for (auto e : pendingBodies)
        RegisterBody(registry, e);
    if (m_regBackoff.size() > 256 || m_regWarned.size() > 256)
    {
        // 破棄済みエンティティの記録を掃除する
        for (auto it = m_regBackoff.begin(); it != m_regBackoff.end(); )
            it = registry.valid(it->first) ? std::next(it) : m_regBackoff.erase(it);
        for (auto it = m_regWarned.begin(); it != m_regWarned.end(); )
            it = registry.valid(it->first) ? std::next(it) : m_regWarned.erase(it);
    }

    std::vector<entt::entity> pendingChars;
    for (auto [e, cc] : registry.view<CharacterController>().each())
        if (!cc._registered) pendingChars.push_back(e);
    for (auto e : pendingChars)
        RegisterCharacter(registry, e);
}

// ========== 静的 / キネマティックの変更の追従（Play 中）==========
//
// 以前は静的剛体の Transform / スケール / コライダー部品を Play 中に変えても Jolt のボディが古いままだった
// （インスペクタ・Lua・MCP で動かした壁の当たり判定が元の場所に残る。スケールを 2→6 にしても判定は 2 のまま）。
// キネマティックは MoveKinematic で姿勢だけ追うが、形状（スケール・コライダー）は同じく古かった。
//   ・姿勢だけ変わった静的 → SetPositionAndRotation（ボディ ID・接触はそのまま）。
//   ・形状（スケール・コライダー部品・RigidBody の値）が変わった静的 / キネマティック → ボディを作り直す。
//   ・動かした静的ボディの旧位置・新位置の近くで寝ている動的剛体を起こす（寝たまま空中に残らないように）。
// 毎フレーム全部を調べると Dead Mall（静的 13,522 体）で数 ms かかるので、静的は 1 フレーム最大 kStaticScanPerFrame 個
// ずつ巡回する（小さなシーンは毎フレーム全部。大きなシーンの反映は最大数フレーム遅れる）。キネマティックは毎フレーム。
// 地形・スカルプトの専用経路・インスタンス群（sig が無い）は対象外。
void PhysicsSystem::FollowStaticChanges(entt::registry& registry)
{
    if (!m_impl || m_impl->trackList.empty()) return;
    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();

    // 1 フレームに見る数: 全体を約 32 フレーム（0.5 秒）で一巡する。256 体以下は毎フレーム全部。上限 2048。
    //   実測（Dead Mall = 静的 13,522 体）: registry の view を毎フレーム全部なめて 4,096 体を検査すると 0.70ms、
    //   巡回リスト + 1 フレーム 422 体にして 0.085ms。大きなシーンは反映が最大約 0.5 秒遅れるのと引き換え。
    const size_t kStaticScanPerFrame = (std::min<size_t>)(2048, (std::max<size_t>)(256, (m_impl->trackList.size() + 31) / 32));
    const PoolSet pools(registry);

    auto bounds = [&](JPH::BodyID id) {
        return bodyInterface.GetTransformedShape(id).GetWorldSpaceBounds();
    };
    auto wake = [&](JPH::AABox box) {
        if (!box.IsValid()) return;
        box.ExpandBy(JPH::Vec3::sReplicate(0.1f));
        bodyInterface.ActivateBodiesInAABox(
            box, JPH::DefaultBroadPhaseLayerFilter(m_impl->objVsBpFilter, Layers::NON_MOVING),
            JPH::DefaultObjectLayerFilter(m_impl->objLayerPairFilter, Layers::NON_MOVING));
    };

    auto& list = m_impl->trackList;
    std::vector<entt::entity> rebuild;
    size_t budget = (std::min)(list.size(), kStaticScanPerFrame);
    while (budget > 0 && !list.empty())
    {
        if (m_impl->scanCursor >= list.size()) m_impl->scanCursor = 0;
        const entt::entity e = list[m_impl->scanCursor];
        --budget;

        const Transform* tf = registry.valid(e) ? PoolSet::Get(pools.tf, e) : nullptr;
        const RigidBody* rbp = tf ? PoolSet::Get(pools.rb, e) : nullptr;
        auto sit = (rbp && rbp->bodyId != kInvalidBodyId) ? m_impl->bodySig.find(rbp->bodyId) : m_impl->bodySig.end();
        if (rbp == nullptr || rbp->motionType == MotionType::Dynamic || sit == m_impl->bodySig.end())
        {
            // 消えた / 動的に変わった / 登録が外れた: 巡回から外す（再登録時に RegisterBody が足し直す）
            m_impl->trackSet.erase(e);
            list[m_impl->scanCursor] = list.back();
            list.pop_back();
            continue;
        }
        ++m_impl->scanCursor;

        const RigidBody& rb = *rbp;
        const bool kinematic = rb.motionType == MotionType::Kinematic;
        JoltImpl::BodySig& sig = sit->second;

        const uint64_t chain = HashTransformChain(pools, e);
        const uint64_t comp  = HashColliderComps(pools, e, rb);
        if (chain == sig.chain && comp == sig.comp) continue;

        const WorldTRS w = ResolveWorldTRS(registry, e, *tf);
        if (HashShapeKey(comp, w.scale) != sig.shape) { rebuild.push_back(e); continue; }

        sig.chain = chain; sig.comp = comp;
        if (kinematic) continue;   // 姿勢は SyncTransformsToPhysics（MoveKinematic）が毎フレーム送る

        const JPH::RVec3 np(w.pos.x, w.pos.y, w.pos.z);
        if ((np - sig.pos).LengthSq() > 1.0e-10f || std::fabs(sig.rot.Dot(w.rot)) < 1.0f - 1.0e-8f)
        {
            const JPH::BodyID id(rb.bodyId);
            const JPH::AABox oldB = bounds(id);
            bodyInterface.SetPositionAndRotation(id, np, w.rot, JPH::EActivation::DontActivate);
            wake(oldB);
            wake(bounds(id));
            sig.pos = np; sig.rot = w.rot;
        }
    }

    for (entt::entity e : rebuild)
    {
        auto* rb = registry.try_get<RigidBody>(e);
        if (!rb || rb->bodyId == kInvalidBodyId) continue;
        const JPH::BodyID oldId(rb->bodyId);
        const JPH::AABox oldB = bounds(oldId);
        UnregisterBody(registry, e);
        RegisterBody(registry, e);
        wake(oldB);
        if (rb->bodyId != kInvalidBodyId) wake(bounds(JPH::BodyID(rb->bodyId)));
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

// ========== インスタンス群（InstanceGroup）==========

size_t PhysicsSystem::InstanceGroupBodyCount(entt::entity e) const
{
    auto it = m_groupBodies.find(e);
    return it == m_groupBodies.end() ? 0 : it->second.bodyIds.size();
}

void PhysicsSystem::UnregisterGroupBodies(entt::registry& registry, entt::entity entity, bool removeFromJolt)
{
    auto it = m_groupBodies.find(entity);
    if (it == m_groupBodies.end()) return;
    if (removeFromJolt && m_initialized)
    {
        auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
        for (uint32_t id : it->second.bodyIds)
        {
            JPH::BodyID joltId(id);
            bodyInterface.RemoveBody(joltId);
            bodyInterface.DestroyBody(joltId);
            m_impl->prevBody.erase(id);
            PurgeContactState(m_impl->contactCount, m_impl->contactSuspended, id);
        }
    }
    for (uint32_t id : it->second.bodyIds) m_bodyToEntity.erase(id);
    m_groupBodies.erase(it);
    if (registry.valid(entity))
        if (auto* rb = registry.try_get<RigidBody>(entity)) rb->bodyId = kInvalidBodyId;
}

void PhysicsSystem::RegisterGroupBodies(entt::registry& registry, entt::entity entity)
{
    auto* rb  = registry.try_get<RigidBody>(entity);
    auto* grp = registry.try_get<InstanceGroup>(entity);
    auto* mr  = registry.try_get<MeshRenderer>(entity);
    if (!rb || !grp) return;

    GroupBodies gb;
    const std::string nm = registry.all_of<NameTag>(entity) ? registry.get<NameTag>(entity).name : std::string("InstanceGroup");
    const auto* meshCol = registry.try_get<MeshCollider>(entity);
    if (!grp->_set || grp->_set->items.empty()) { m_groupBodies[entity] = gb; return; }
    if (!meshCol)
    {
        Logger::Warn("インスタンス群 '{}' に剛体がありますが MeshCollider がありません（群は MeshCollider の静的コライダーだけ対応）", nm);
        m_groupBodies[entity] = gb;
        return;
    }
    if (rb->motionType != MotionType::Static)
    {
        Logger::Warn("インスタンス群 '{}' の剛体は Static のみ対応です（動く剛体にするには「展開」してください）", nm);
        m_groupBodies[entity] = gb;
        return;
    }
    if (!mr || mr->meshes.empty())
    {
        Logger::Warn("インスタンス群 '{}' の MeshRenderer のメッシュが空です", nm);
        m_groupBodies[entity] = gb;
        return;
    }

    const std::string key = mr->modelPath + "|tri";
    JPH::RefConst<JPH::Shape> base;
    if (auto it = m_impl->meshShapeCache.find(key); it != m_impl->meshShapeCache.end()) base = it->second;
    else { base = BuildStaticMeshShape(*mr); m_impl->meshShapeCache[key] = base; }
    if (base.GetPtr() == nullptr) { m_groupBodies[entity] = gb; return; }   // 形が作れなかったら箱で代用しない

    const instgroup::InstanceSet& set = *grp->_set;
    const auto& worlds = instgroup::WorldMatrices(registry, entity, set);
    gb.setId    = set.id;
    gb.worldKey = set.worldKey;
    const u32 n = set.Count();

    // 空間的にまとまったチャンクにする（Morton 順）。順番はインスタンス番号とは無関係（userData が番号）。
    DirectX::XMFLOAT3 mn{FLT_MAX, FLT_MAX, FLT_MAX}, mx{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (u32 i = 0; i < n; ++i)
    {
        mn = {(std::min)(mn.x, worlds[i]._41), (std::min)(mn.y, worlds[i]._42), (std::min)(mn.z, worlds[i]._43)};
        mx = {(std::max)(mx.x, worlds[i]._41), (std::max)(mx.y, worlds[i]._42), (std::max)(mx.z, worlds[i]._43)};
    }
    auto spread = [](uint32_t v) {   // 10 bit → 30 bit（2 bit おき）
        v &= 0x3FFu;
        v = (v | (v << 16)) & 0x030000FFu;
        v = (v | (v << 8))  & 0x0300F00Fu;
        v = (v | (v << 4))  & 0x030C30C3u;
        v = (v | (v << 2))  & 0x09249249u;
        return v;
    };
    auto q = [](f32 v, f32 lo, f32 hi) -> uint32_t {
        const f32 span = hi - lo;
        return span > 1e-6f ? static_cast<uint32_t>((std::min)(1023.0f, (std::max)(0.0f, (v - lo) / span * 1023.0f))) : 0u;
    };
    std::vector<std::pair<uint32_t, u32>> order(n);
    for (u32 i = 0; i < n; ++i)
        order[i] = {spread(q(worlds[i]._41, mn.x, mx.x)) | (spread(q(worlds[i]._42, mn.y, mx.y)) << 1) | (spread(q(worlds[i]._43, mn.z, mx.z)) << 2), i};
    std::sort(order.begin(), order.end());

    const JPH::Vec3 off(meshCol->offset.x, meshCol->offset.y, meshCol->offset.z);
    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    constexpr u32 kChunk = 128;
    for (u32 c0 = 0; c0 < n; c0 += kChunk)
    {
        JPH::StaticCompoundShapeSettings cs;
        u32 added = 0;
        for (u32 k = c0; k < (std::min)(n, c0 + kChunk); ++k)
        {
            const u32 idx = order[k].second;
            DirectX::XMVECTOR s, qv, p;
            if (!DirectX::XMMatrixDecompose(&s, &qv, &p, DirectX::XMLoadFloat4x4(&worlds[idx]))) { ++gb.skipped; continue; }
            DirectX::XMFLOAT3 sf, pf; DirectX::XMFLOAT4 qf;
            DirectX::XMStoreFloat3(&sf, s); DirectX::XMStoreFloat3(&pf, p); DirectX::XMStoreFloat4(&qf, qv);
            const JPH::Vec3 scl(sf.x, sf.y, sf.z);
            JPH::RefConst<JPH::Shape> sub;
            if (scl != JPH::Vec3::sOne())
            {
                if (!base->IsValidScale(scl)) { ++gb.skipped; continue; }
                sub = new JPH::ScaledShape(base, scl);
            }
            else sub = base;
            // ★Jolt の複合形状はサブシェイプの回転を xyz の 3 float で持ち、w を sqrt(1 - |xyz|^2) で復元する。
            //   w が 0 に近い（≈180° 回転）と精度が sqrt(eps) ≈ 3e-4 まで落ちて、遠くのレイが数 cm ずれる（実測）。
            //   そこで「軸まわり 180°」（xyz が厳密に表せる）を形状側へ移す: q = p * R。p は w が大きく（≒0°）なるよう R を選ぶ。
            JPH::Quat rot(qf.x, qf.y, qf.z, qf.w);
            rot = rot.Normalized();
            const JPH::Quat qOrig = rot;   // offset の回転用（下の 180° 退避で rot を差し替える前の値）
            if (std::abs(rot.GetW()) < 0.5f)
            {
                const f32 ax = std::abs(rot.GetX()), ay = std::abs(rot.GetY()), az = std::abs(rot.GetZ());
                const JPH::Quat R = (ax >= ay && ax >= az) ? JPH::Quat(1, 0, 0, 0) : (ay >= az ? JPH::Quat(0, 1, 0, 0) : JPH::Quat(0, 0, 1, 0));
                JPH::RotatedTranslatedShapeSettings rts(JPH::Vec3::sZero(), R, sub);
                auto rr = rts.Create();
                if (rr.IsValid())
                {
                    sub = rr.Get();
                    rot = (rot * R.Conjugated()).Normalized();
                }
            }
            // offset はインスタンスのローカル（回転・スケールを通す）。単体の MeshCollider と同じ規約。
            cs.AddShape(JPH::Vec3(pf.x, pf.y, pf.z) + qOrig * (off * scl), rot, sub, idx);
            ++added;
        }
        if (added == 0) continue;
        auto res = cs.Create();
        if (!res.IsValid())
        {
            Logger::Error("インスタンス群 '{}' の複合形状を作れませんでした: {}", nm, res.GetError().c_str());
            continue;
        }
        JPH::BodyCreationSettings bs(res.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::NON_MOVING);
        bs.mRestitution = collider::SafeRestitution(rb->restitution);
        bs.mFriction    = collider::SafeFriction(rb->friction);
        const JPH::BodyID id = bodyInterface.CreateAndAddBody(bs, JPH::EActivation::DontActivate);
        if (id.IsInvalid())
        {
            NoteBodyCreateFailed();   // 最初の 1 件だけ即 ERROR、以降は要約（Play ごとに数え直す）
            break;
        }
        gb.bodyIds.push_back(id.GetIndexAndSequenceNumber());
        m_bodyToEntity[id.GetIndexAndSequenceNumber()] = entity;
    }
    if (gb.skipped > 0)
        Logger::Warn("インスタンス群 '{}': {} 個のインスタンスは形状を作れず当たり判定がありません（スケール 0 など）", nm, gb.skipped);
    if (!gb.bodyIds.empty()) rb->bodyId = gb.bodyIds.front();
    m_groupBodies[entity] = std::move(gb);
}

void PhysicsSystem::RefreshInstanceGroupColliders(entt::registry& registry)
{
    if (!m_initialized || m_groupBodies.empty()) return;
    std::vector<entt::entity> redo, drop, gone;
    for (auto& kv : m_groupBodies)
    {
        const entt::entity e = kv.first;
        if (!registry.valid(e)) { gone.push_back(e); continue; }
        const auto* g = registry.try_get<InstanceGroup>(e);
        if (!g || !registry.all_of<RigidBody>(e)) { drop.push_back(e); continue; }
        if (!g->_set) { if (kv.second.setId != 0) redo.push_back(e); continue; }
        instgroup::WorldMatrices(registry, e, *g->_set);   // worldKey を最新にする
        if (g->_set->id != kv.second.setId || g->_set->worldKey != kv.second.worldKey) redo.push_back(e);
    }
    for (entt::entity e : gone) m_groupBodies.erase(e);   // ボディは ReleaseOrphanedPhysicsBodies が外し済み
    for (entt::entity e : drop) UnregisterGroupBodies(registry, e, true);
    for (entt::entity e : redo)
    {
        UnregisterGroupBodies(registry, e, true);
        RegisterBody(registry, e);
    }
}

// ========== Body Registration ==========

void PhysicsSystem::RegisterBody(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;

    auto* rb = registry.try_get<RigidBody>(entity);
    if (!rb) return;
    if (rb->bodyId != kInvalidBodyId) return; // already registered
    if (eflags::IsDisabled(registry, entity)) return;   // 無効（インスペクタの「有効」OFF）: 剛体を作らない

    // インスタンス群: 群のエンティティ自身の剛体は作らず、インスタンスのコライダーをチャンクごとの複合形状で張る。
    if (registry.all_of<InstanceGroup>(entity))
    {
        RegisterGroupBodies(registry, entity);
        return;
    }

    auto* transform = registry.try_get<Transform>(entity);
    if (!transform) return;

    // ★モデルが読めなかったエンティティ（MissingModel: MeshRenderer 無しで残される）は判定を作らない。
    //   コライダー部品の無い剛体は Transform のスケールから箱を作るので、そのまま作ると見えない壁になる。
    //   警告は 1 エンティティ 1 回・再試行は間引く（モデルが戻れば、次に開いたとき普通に登録される）。
    if (registry.all_of<MissingModel>(entity))
    {
        if (!m_regWarned[entity])
        {
            m_regWarned[entity] = true;
            Logger::Warn("物理: '{}' はモデルが読み込めなかったので当たり判定を作りません（モデル: {}）",
                         registry.all_of<NameTag>(entity) ? registry.get<NameTag>(entity).name : std::string("entity"),
                         registry.get<MissingModel>(entity).modelPath);
        }
        m_regBackoff[entity] = m_frame + 1200;
        return;
    }

    // ★親子を解決したワールドの位置 / 回転 / スケール。物理は長らくローカル値で
    //   ボディを作っていたので、グループ化した親の下の剛体は
    //   **見えている場所と当たる場所が別**、親を拡大しても当たり判定は等倍のままだった。
    //   親なしのときは従来と 1 ビットも変わらない（ResolveWorldTRS の早期パス）。
    bool approximated = false;
    const WorldTRS wtrs = ResolveWorldTRS(registry, entity, *transform, &approximated);
    if (approximated && !m_regWarned[entity])
    {
        m_regWarned[entity] = true;
        Logger::Warn("物理: '{}' のワールド行列にせん断（非一様スケールの親 + 回転した子）があります。"
                     "せん断を捨てた近似で判定を作ります（見た目と判定が少しずれることがあります）",
                     registry.all_of<NameTag>(entity) ? registry.get<NameTag>(entity).name : std::string("entity"));
    }
    if (!std::isfinite(wtrs.pos.x + wtrs.pos.y + wtrs.pos.z))
    {
        if (!m_regWarned[entity]) { m_regWarned[entity] = true; Logger::Warn("物理: 位置が NaN / 無限大のエンティティはボディを作りません"); }
        m_regBackoff[entity] = m_frame + 120;
        return;
    }

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
            JPH::RVec3(wtrs.pos.x, wtrs.pos.y, wtrs.pos.z),
            JPH::Quat::sIdentity(),          // 地形は回転させない（ハイトフィールドの前提）
            JPH::EMotionType::Static,
            Layers::NON_MOVING);
        terrainSettings.mFriction    = collider::SafeFriction(rb->friction);
        terrainSettings.mRestitution = collider::SafeRestitution(rb->restitution);

        JPH::BodyID terrainId = bodyInterface.CreateAndAddBody(terrainSettings,
                                                               JPH::EActivation::DontActivate);
        if (terrainId.IsInvalid()) { NoteBodyCreateFailed(); terrain->_colliderDirty = false; return; }
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
        const f32 sclX = wtrs.scale.x;
        const f32 sclY = wtrs.scale.y;
        const f32 sclZ = wtrs.scale.z;

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
                         "フォールバックしました（三角形メッシュ形状は静的な剛体にしか付けられません）。"
                         "彫った凹み/穴は当たり判定では埋まるで。",
                         registry.all_of<NameTag>(entity) ? registry.get<NameTag>(entity).name
                                                          : std::string("Sculpt"));
        }

        // 形状を作れなかったときも箱で代用はしない（原因が見えない当たり判定を残さない）。
        if (shape.GetPtr() == nullptr) return;
    }

    // ---- MeshCollider（描いてる形そのままの三角形コライダー）------------------
    // 部屋の殻・廊下・階段のように「中が空洞」の形は凸包では歩けない（中身が詰まる）。
    // MeshRenderer のメッシュから Jolt の MeshShape を作る。
    // ★形状は **スケール抜き** で（modelPath + メッシュの署名）単位にキャッシュし、拡縮は ScaledShape を
    //   被せて表現する。レベルを丸ごと取り込むと同じモデルが数千個・スケールは
    //   1 個ずつ微妙に違う（Dead_Mall は 9096 配置で 3387 通り）ので、スケールを
    //   形状に焼くとキャッシュが効かず BVH を数千回組むことになる。
    // ★キーに modelPath だけを使うと、プリミティブ（"__primitive_sphere__" 等。大きさがメッシュに焼き込まれて
    //   いるのに modelPath は同じ）の大きさ違いが 1 つ目の形状に取り違えられた（半径 0.5 と 2 の球が両方 2、
    //   10m と 50m の平面が両方 10m）。メッシュの署名（頂点数・インデックス数・サンプル頂点）を足した。
    // 選ばれたコライダーのローカル offset。最後に形状の内側へ入れる（ボディの原点 = エンティティの原点）。
    DirectX::XMFLOAT3 colliderOffset{ 0.0f, 0.0f, 0.0f };
    const auto warnOnce = [this, &registry, entity]() {
        if (m_regWarned[entity]) return false;
        m_regWarned[entity] = true;
        (void)registry;
        return true;
    };
    if (shape.GetPtr() == nullptr && registry.try_get<MeshCollider>(entity) != nullptr)
    {
        const auto* mr = registry.try_get<MeshRenderer>(entity);
        if (mr == nullptr || mr->meshes.empty())
        {
            // メッシュがまだ読めていない＝形が無い。箱で代用すると「見えない壁」になるので作らない。
            // 警告は 1 エンティティ 1 回・再試行は間引く（以前は毎フレーム警告していた）。
            if (warnOnce())
                Logger::Warn("meshCollider が付いているのに MeshRenderer のメッシュが空です（entity {}）",
                             static_cast<uint32_t>(entt::to_integral(entity)));
            m_regBackoff[entity] = m_frame + 120;
            return;
        }
        colliderOffset = registry.get<MeshCollider>(entity).offset;

        const bool isStatic = (rb->motionType == MotionType::Static);
        const std::string key = mr->modelPath + "|" + std::to_string(MeshShapeSignature(*mr)) + (isStatic ? "|tri" : "|hull");

        JPH::RefConst<JPH::Shape> base;
        if (auto it = m_impl->meshShapeCache.find(key); it != m_impl->meshShapeCache.end())
        {
            base = it->second;
        }
        else if (isStatic)
        {
            base = BuildStaticMeshShape(*mr);
            if (base.GetPtr() != nullptr) m_impl->meshShapeCache[key] = base;   // 失敗（nullptr）はキャッシュしない
        }
        else
        {
            // 動く剛体は MeshShape を持てない（Jolt の制約）ので凸包で妥協する＝凹みは埋まる。
            std::vector<JPH::Vec3> hull;
            for (const Mesh* mesh : mr->meshes)
            {
                if (mesh == nullptr) continue;
                for (const auto& lp : mesh->GetPositions())
                    hull.push_back(JPH::Vec3(lp.x, lp.y, lp.z));
            }
            if (!hull.empty())
            {
                JPH::ConvexHullShapeSettings hs(hull.data(), static_cast<int>(hull.size()), 0.01f);
                hs.mMaxConvexRadius = 0.05f;
                auto result = hs.Create();
                if (result.IsValid()) base = result.Get();
            }
            if (base.GetPtr() != nullptr) m_impl->meshShapeCache[key] = base;
            Logger::Warn("meshCollider '{}' は Static ではないので凸包にフォールバックしました"
                         "（三角形メッシュ形状は静的な剛体にしか付けられません）", mr->modelPath);
        }

        if (base.GetPtr() == nullptr)   // 形が作れなかったら箱で代用しない
        {
            m_regBackoff[entity] = m_frame + 120;
            return;
        }

        // ★スケールの 0 / NaN は 1mm へ（0 のまま IsValidScale が偽 → 原寸の形が出る「見えない壁」になっていた）。
        //   符号（鏡像）は残す。
        const DirectX::XMFLOAT3 ms = collider::SafeMeshScale(wtrs.scale);
        const JPH::Vec3 scl(ms.x, ms.y, ms.z);
        if (scl != JPH::Vec3::sOne())
        {
            if (!base->IsValidScale(scl))
            {
                if (warnOnce())
                    Logger::Warn("meshCollider '{}' のスケール ({}, {}, {}) は形状に使えません。判定を作りません",
                                 mr->modelPath, ms.x, ms.y, ms.z);
                return;
            }
            shape = new JPH::ScaledShape(base, scl);
        }
        else
            shape = base;
    }

    // Determine shape
    auto* convex  = registry.try_get<ConvexHullCollider>(entity);
    auto* box     = registry.try_get<BoxCollider>(entity);
    auto* sphere  = registry.try_get<SphereCollider>(entity);
    auto* capsule = registry.try_get<CapsuleCollider>(entity);

    // ★形状は collider::Effective*（符号を捨て 0 / NaN は 1mm へ）。負 / 0 スケールで球・カプセルが負の半径になり
    //   床を抜け続けた（実測 y=-26.8）・箱が浮いた問題の入口。offset は選んだ形状のもの（以前は後勝ちの別の部品のものが
    //   混ざり得た）。
    if (shape.GetPtr() != nullptr)
    {
        // 上のスカルプトメッシュ / MeshCollider 経路で形状が決まっている（コライダー部品より優先）
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
        colliderOffset = convex->offset;
    }
    else if (box)
    {
        // halfExtents は Transform.scale 乗算が前提（Trigger/EditorIconRenderer と同じ規約）。
        // これが無いと SpawnBox→scale で見た目だけ拡大したボックス（Ground/Wall等）が
        // 実際にはデフォルトの 0.5 半径でしか衝突しなくなる。
        const DirectX::XMFLOAT3 he = collider::EffectiveBoxHalfExtents(box->halfExtents, wtrs.scale);
        shape = new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z), collider::BoxConvexRadius(he));
        colliderOffset = box->offset;
    }
    else if (sphere)
    {
        shape = new JPH::SphereShape(collider::EffectiveSphereRadius(sphere->radius, wtrs.scale));
        colliderOffset = sphere->offset;
    }
    else if (capsule)
    {
        shape = new JPH::CapsuleShape(collider::EffectiveCapsuleHalfHeight(capsule->halfHeight, wtrs.scale),
                                       collider::EffectiveCapsuleRadius(capsule->radius, wtrs.scale));
        colliderOffset = capsule->offset;
    }
    else
    {
        // Fallback: box from scale
        const DirectX::XMFLOAT3 he = collider::EffectiveFallbackHalfExtents(wtrs.scale);
        shape = new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z), collider::BoxConvexRadius(he));
    }

    // ★コライダーの offset は「エンティティのローカル」（回転とスケールを通す）。ボディの原点はエンティティの原点に
    //   固定し、offset は形状の内側（RotatedTranslatedShape）へ入れる。これで初期配置・キネマティックの追従・動的の
    //   書き戻し・physics:setPosition が全部「Transform = ボディ位置」で揃う（以前は offset をワールド軸で足し引きして
    //   いたので、回転 / スケールで判定とデバッグ表示がずれ、キネマティックは最初のステップで offset が消えた）。
    {
        const DirectX::XMFLOAT3 so = collider::ScaledOffset(colliderOffset, wtrs.scale);
        if (std::isfinite(so.x + so.y + so.z) && (so.x != 0.0f || so.y != 0.0f || so.z != 0.0f))
        {
            JPH::RotatedTranslatedShapeSettings rts(JPH::Vec3(so.x, so.y, so.z), JPH::Quat::sIdentity(), shape.GetPtr());
            auto rr = rts.Create();
            if (rr.IsValid()) shape = rr.Get();
            else Logger::Warn("コライダーの offset を形状へ入れられませんでした: {}", rr.GetError().c_str());
        }
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

    // ★親子を解決したワールド値でボディを置く（親なしなら従来と同一）。
    //   ローカル値で作ると、グループ化した親の下の剛体は
    //   **見えている場所と当たる場所が別**になる。
    JPH::RVec3 pos(wtrs.pos.x, wtrs.pos.y, wtrs.pos.z);
    JPH::Quat  rot = wtrs.rot;

    JPH::BodyCreationSettings bodySettings(shape, pos, rot, joltMotion, layer);

    if (rb->motionType == MotionType::Dynamic)
    {
        // ★質量 0 / 負 / NaN は Jolt の質量計算が FLT_INVALID_OPERATION でプロセスごと落ちる（実測）。
        //   静的で作った剛体（v2 の表は mass:0 の静的）を Inspector で動的に切り替えるだけで成立していた。
        //   入口で既定質量へ読み替える（コンポーネントの値は書き換えない）。
        const f32 safeMass = collider::SafeMass(rb->mass);
        if (safeMass != rb->mass && warnOnce())
            Logger::Warn("物理: '{}' の質量 {} は不正（0 以下 / NaN）なので {} として扱います",
                         registry.all_of<NameTag>(entity) ? registry.get<NameTag>(entity).name : std::string("entity"),
                         rb->mass, safeMass);
        bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = safeMass;
    }

    // 負の摩擦 / 反発 / 減衰も Jolt を壊す（摩擦 -1 で FLT_INVALID_OPERATION）ので入口でクランプする。
    bodySettings.mRestitution    = collider::SafeRestitution(rb->restitution);
    bodySettings.mFriction       = collider::SafeFriction(rb->friction);
    bodySettings.mLinearDamping  = collider::SafeDamping(rb->linearDamping);
    bodySettings.mAngularDamping = collider::SafeDamping(rb->angularDamping);
    bodySettings.mGravityFactor  = rb->useGravity ? 1.0f : 0.0f;
    // CCD。既定の Discrete は移動後の位置でしか当たりを見ないので、1 ステップで
    // 自分の厚みより長く動く物は薄い壁をすり抜ける。LinearCast は移動線分で判定する。
    // 静的/キネマティックには意味が無いので Dynamic のときだけ立てる。
    if (rb->continuousCollision && rb->motionType == MotionType::Dynamic)
        bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;

    // ゲーム向け: すぐ sleep しない（不安定な積み方でも少し動き続ける）
    bodySettings.mAllowSleeping  = true;

    JPH::BodyID id = bodyInterface.CreateAndAddBody(bodySettings, JPH::EActivation::Activate);
    if (id.IsInvalid())
    {
        // ★Jolt は body 上限に達しても例外も警告も出さず無効な ID を返すだけ。
        //   ここで言わないと「床だけすり抜ける」形でしか表面化しない。
        //   最初の 1 件だけ即 ERROR、以降は件数つきの要約（NoteBodyCreateFailed）。再試行は間引く。
        NoteBodyCreateFailed();
        m_regBackoff[entity] = m_frame + 120;
        return;
    }
    rb->bodyId = id.GetIndexAndSequenceNumber();
    m_bodyToEntity[rb->bodyId] = entity;   // bodyId→entity 逆引きを登録
    m_regBackoff.erase(entity);

    // 静的 / キネマティックは「いまの Jolt 側の状態」を控える（Play 中の変更の追従 = FollowStaticChanges）。
    if (rb->motionType != MotionType::Dynamic)
    {
        JoltImpl::BodySig sig;
        sig.chain = HashTransformChain(registry, entity);
        sig.comp  = HashColliderComps(registry, entity, *rb);
        sig.shape = HashShapeKey(sig.comp, wtrs.scale);
        sig.pos   = pos;
        sig.rot   = rot;
        m_impl->bodySig[rb->bodyId] = sig;
        if (m_impl->trackSet.insert(entity).second) m_impl->trackList.push_back(entity);
    }
}

void PhysicsSystem::UnregisterBody(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;

    auto* rb = registry.try_get<RigidBody>(entity);
    if (m_groupBodies.count(entity)) { UnregisterGroupBodies(registry, entity, true); return; }
    if (!rb || rb->bodyId == kInvalidBodyId) return;

    auto& bodyInterface = m_impl->physicsSystem->GetBodyInterface();
    JPH::BodyID joltId(rb->bodyId);
    bodyInterface.RemoveBody(joltId);
    bodyInterface.DestroyBody(joltId);
    m_bodyToEntity.erase(rb->bodyId);
    m_impl->prevBody.erase(rb->bodyId);
    m_impl->bodySig.erase(rb->bodyId);
    PurgeContactState(m_impl->contactCount, m_impl->contactSuspended, rb->bodyId);
    rb->bodyId = kInvalidBodyId;
}

void PhysicsSystem::UnregisterAllBodies(entt::registry& registry)
{
    if (!m_initialized) return;

    {
        // インスタンス群のボディ（先に外す。RigidBody::bodyId に入っている先頭のボディの二重解放を避ける）
        std::vector<entt::entity> groups;
        for (const auto& kv : m_groupBodies) groups.push_back(kv.first);
        for (entt::entity ge : groups) UnregisterGroupBodies(registry, ge, true);
    }
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
    m_impl->prevBody.clear();
    m_impl->bodySig.clear();
    m_impl->trackList.clear();
    m_impl->trackSet.clear();
    m_impl->scanCursor = 0;
    m_impl->contactCount.clear();
    m_impl->contactSuspended.clear();
    m_impl->meshShapeCache.clear();   // メッシュ形状キャッシュも捨てる（次のシーンの Mesh* とは無関係）
    m_regBackoff.clear();
    m_regWarned.clear();
    LogSessionSummary();              // セッションの終わり: 上限超過・作れなかったボディの要約を 1 回だけ出して数え直す
}

// ========== Character Controller（CharacterVirtual）==========

void PhysicsSystem::RegisterCharacter(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;
    auto* cc = registry.try_get<CharacterController>(entity);
    if (!cc) return;
    if (cc->_registered) return;
    if (eflags::IsDisabled(registry, entity)) return;   // 無効（インスペクタの「有効」OFF）: キャラを作らない
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
    // キャラが剛体を押せる最大の力。Jolt 既定の 100N は質量 70kg のキャラには弱く、軽い物には
    // 過剰なので、質量に比例させる（70kg → 350N）。軽い箱は相対速度ぶんしか押されないので飛ばない。
    settings->mMaxStrength   = (std::max)(100.0f, cc->mass * 5.0f);
    settings->mMaxSlopeAngle = DirectX::XMConvertToRadians(cc->maxSlopeDeg); // 度→ラジアン必須
    settings->mShapeOffset   = JPH::Vec3(cc->offset.x, cc->offset.y, cc->offset.z);
    settings->mUp            = JPH::Vec3(0, 1, 0);    // base
    // 足元の支持面: カプセル下端を少し上にした平面（接地安定化）
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3(0, 1, 0), -cc->radius);

    // 初期位置は Transform 原点そのまま。形状ローカルオフセットは mShapeOffset に一本化する
    // （mShapeOffset は mPosition に加算され衝突カプセル中心 = transform.position + offset となる）。
    // ここで + offset するとオフセットが二重適用されるので加算しない。
    // キャラは Y 軸回転を物理に渡さない（接地判定簡素化）。
    // ★親子を解決したワールド位置（RegisterBody と同じ）。以前はローカル値をそのままワールド座標として作っていたので、
    //   親の下に置いたキャラは「見た目は親の位置・物理は原点の床」にいた。
    const WorldTRS wtrs = ResolveWorldTRS(registry, entity, *transform);
    JPH::RVec3 pos(wtrs.pos.x, wtrs.pos.y, wtrs.pos.z);
    JPH::Quat rot = JPH::Quat::sIdentity();

    JPH::Ref<JPH::CharacterVirtual> ch = new JPH::CharacterVirtual(
        settings, pos, rot, static_cast<JPH::uint64>(entt::to_integral(entity)),
        m_impl->physicsSystem.get());

    // 動的剛体との押し合いの規約（足元の軽い箱を跳ね飛ばさない・軽い箱にキャラを押させない）。
    if (!m_impl->charListener)
        m_impl->charListener = std::make_unique<EngineCharacterListener>(m_impl->physicsSystem.get());
    ch->SetListener(m_impl->charListener.get());
    // キャラ同士の衝突（CharacterVirtual はブロードフェーズにボディを持たないので、以前は互いに素通りしていた）。
    m_impl->charVsChar.Add(ch.GetPtr());
    ch->SetCharacterVsCharacterCollision(&m_impl->charVsChar);

    m_impl->characters[entity] = ch;
    cc->_registered  = true;
    cc->_verticalVel = 0.0f;
    cc->_grounded    = false;
    // Play→Stop→Play でコンポーネントは破棄されないため、前回終了時のランタイム入力状態
    // （_desiredVel / _jumpQueued）が残ると最初の接地ステップで余分なジャンプや移動を誘発する。
    // 形状生成と同時に明示リセットする。
    cc->_desiredVel  = { 0.0f, 0.0f, 0.0f };
    cc->_jumpQueued  = false;
    cc->_jumpOverride = -1.0f;
}

void PhysicsSystem::UnregisterCharacter(entt::registry& registry, entt::entity entity)
{
    if (!m_initialized) return;
    if (auto it = m_impl->characters.find(entity); it != m_impl->characters.end())
        m_impl->charVsChar.Remove(it->second.GetPtr());
    m_impl->characters.erase(entity);   // Ref 解放
    m_impl->prevCharPos.erase(entity);
    if (auto* cc = registry.try_get<CharacterController>(entity)) cc->_registered = false;
}

void PhysicsSystem::UnregisterAllCharacters(entt::registry& registry)
{
    if (!m_initialized) return;
    m_impl->charVsChar.mCharacters.clear();
    m_impl->characters.clear();
    m_impl->prevCharPos.clear();
    for (auto [e, cc] : registry.view<CharacterController>().each())
        cc._registered = false;
}

void PhysicsSystem::StepCharacters(f32 fixedDt, entt::registry& registry)
{
    if (!m_initialized || m_impl->characters.empty()) return;

    // ★ここではゼロ化しない。1 フレームに複数ステップ回ると 2 本目以降が
    //   入力ゼロで進んでしまうため、Update() のループを抜けた所で 1 回だけ消す。
    for (auto& [entity, ch] : m_impl->characters)
        StepOneCharacter(entity, fixedDt, registry, /*recordPrev=*/true);
}

void PhysicsSystem::StepOneCharacter(entt::entity entity, f32 fixedDt, entt::registry& registry, bool recordPrev)
{
    auto it = m_impl->characters.find(entity);
    if (it == m_impl->characters.end()) return;
    auto* cc = registry.try_get<CharacterController>(entity);
    if (!cc) return;
    auto& ch = it->second;

    const JPH::PhysicsSystem* sys = m_impl->physicsSystem.get();
    const JPH::Vec3 baseGravity = m_impl->physicsSystem->GetGravity(); // (0,-14,0)

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings; // 既定 + stepHeight 反映
    // フィルタ: MOVING レイヤとして全衝突（既存 ObjLayerPairFilter と整合）
    JPH::DefaultBroadPhaseLayerFilter bpFilter(m_impl->objVsBpFilter, Layers::MOVING);
    JPH::DefaultObjectLayerFilter     objFilter(m_impl->objLayerPairFilter, Layers::MOVING);

    // 描画補間用に、このステップを踏む前の位置を控える
    if (recordPrev) m_impl->prevCharPos[entity] = ch->GetPosition();

    // 体重の頭打ち（contact listener が足元の動的剛体ごとに掛ける）は毎ステップ掛け直すので、まず元の質量へ戻す。
    ch->SetMass(cc->mass);

    // 接地状態更新（前ステップ結果）
    const bool grounded = ch->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;

    // 鉛直速度: 接地中はリセット、空中は重力積分
    if (grounded && cc->_verticalVel < 0.0f) cc->_verticalVel = 0.0f;
    cc->_verticalVel += baseGravity.GetY() * cc->gravityScale * fixedDt;

    // ジャンプ要求（接地中のみ受け付け）
    if (cc->_jumpQueued && grounded)
        cc->_verticalVel = (cc->_jumpOverride > 0.0f) ? cc->_jumpOverride : cc->jumpSpeed;
    cc->_jumpQueued   = false;
    cc->_jumpOverride = -1.0f;   // 今回ぶんだけ

    // 目標速度合成: 水平=move()入力 + 接地面速度、鉛直=積分結果
    // ★接地面速度の継承は、静的/キネマティック（動く床・エレベーター）は従来どおり全部、
    //   動的剛体は質量で絞る（PhysicsLogic.h GroundVelocityScale）。
    //   以前は箱の速度をそのまま拾っていたので、キャラが箱を押す→箱が揺れる→その速度を
    //   キャラが拾う、のフィードバックで、軽い箱に載ると両方が吹き飛んだ。
    JPH::Vec3 ground = JPH::Vec3::sZero();
    if (grounded)
    {
        ground = ch->GetGroundVelocity();
        const f32 bodyMass = EngineCharacterListener::DynamicBodyMass(sys, ch->GetGroundBodyID());
        if (bodyMass >= 0.0f)
            ground *= physlogic::GroundVelocityScale(bodyMass, ch->GetMass());
    }
    JPH::Vec3 vel(cc->_desiredVel.x + ground.GetX(),
                  cc->_verticalVel,
                  cc->_desiredVel.z + ground.GetZ());
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

void PhysicsSystem::StepSingleCharacter(entt::entity entity, f32 fixedDt, entt::registry& registry)
{
    // マルチプレイ予測リコンシリエーションのリプレイ専用。StepCharacters と同一ロジック
    // (StepOneCharacter 共通)で指定した1体のみを進める(他キャラ/剛体のワールド状態は現在のまま=一般的な近似)。
    // リプレイは補間の起点(prevCharPos)を触らない。
    if (!m_initialized || m_impl->characters.find(entity) == m_impl->characters.end()) return;
    StepOneCharacter(entity, fixedDt, registry, /*recordPrev=*/false);
    if (auto* cc = registry.try_get<CharacterController>(entity))
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
    SyncCharactersToTransformsAlpha(registry, -1.0f);
}

void PhysicsSystem::SyncCharactersToTransformsAlpha(entt::registry& registry, f32 alphaOverride)
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
            const f32 a = alphaOverride >= 0.0f ? alphaOverride
                                                : std::clamp(m_accumulator / kFixedTimeStep, 0.0f, 1.0f);
            p = prev->second + (p - prev->second) * a;
        }
        // ★物理の位置はワールド。親の下のキャラは親のローカル空間へ戻して書く
        //   （以前はワールド値をローカルへ代入していたので、見た目と物理が親のオフセットぶんずれた）。
        tf->position = WorldPosToLocalPos(registry, *tf,
                                          { static_cast<f32>(p.GetX()),
                                            static_cast<f32>(p.GetY()),
                                            static_cast<f32>(p.GetZ()) });
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
    // テレポートなので描画補間の起点も捨てる（さもないと旧位置から新位置を 1 ステップかけて舐める）。
    // 次の固定ステップで控え直すまでは現在の姿勢をそのまま出す。
    m_impl->prevBody.erase(bodyId);
}

// ========== Raycast ==========

namespace
{
// レイの除外: 指定ボディ・指定エンティティのボディ（剛体 / 群のチャンク）。
struct RayIgnoreFilter final : public JPH::BodyFilter
{
    uint32_t ignoreBody = 0xFFFFFFFFu;
    entt::entity ignoreEntity = entt::null;
    const std::unordered_map<uint32_t, entt::entity>* map = nullptr;

    bool ShouldCollide(const JPH::BodyID& id) const override
    {
        const uint32_t v = id.GetIndexAndSequenceNumber();
        if (v == ignoreBody) return false;
        if (ignoreEntity != entt::null && map != nullptr)
        {
            const auto it = map->find(v);
            if (it != map->end() && it->second == ignoreEntity) return false;
        }
        return true;
    }
    bool ShouldCollideLocked(const JPH::Body&) const override { return true; }
};
} // anonymous namespace

RaycastHit PhysicsSystem::Raycast(XMFLOAT3 origin, XMFLOAT3 direction,
                                    f32 maxDistance, uint32_t ignoreBody,
                                    entt::entity ignoreEntity, bool includeCharacters) const
{
    RaycastHit result;
    if (!m_initialized) return result;
    if (!(maxDistance > 0.0f)) return result;

    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    if (XMVectorGetX(XMVector3LengthSq(dir)) < 0.5f) return result;   // ゼロ方向
    XMFLOAT3 normDir;
    XMStoreFloat3(&normDir, dir);

    const JPH::RRayCast ray(
        JPH::RVec3(origin.x, origin.y, origin.z),
        JPH::Vec3(normDir.x * maxDistance, normDir.y * maxDistance, normDir.z * maxDistance));

    // セルフヒット除外（ボディ ID / エンティティ）。
    RayIgnoreFilter bodyFilter;
    bodyFilter.ignoreBody   = ignoreBody;
    bodyFilter.ignoreEntity = ignoreEntity;
    bodyFilter.map          = &m_bodyToEntity;

    JPH::RayCastResult hit;
    const bool bodyHit = m_impl->physicsSystem->GetNarrowPhaseQuery().CastRay(
            ray, hit, {}, {}, bodyFilter);

    // ★キャラ（CharacterVirtual）はブロードフェーズにボディを持たないので、上のレイには当たらない。
    //   カプセルとして別に判定する（当たったら RaycastHit::entity がキャラ）。始点がキャラのカプセルの中にある
    //   ときはそのキャラを無視する（カメラ・銃口が自分のカプセルの中にある使い方で、自分に距離 0 で当たらない）。
    //   ボディを作る案（mInnerBodyShape）は、動的剛体との押し合い（前回の調整済み）を変えるので採らない。
    entt::entity charEntity = entt::null;
    JPH::RayCastResult charBest;
    JPH::TransformedShape charShape;
    if (bodyHit) charBest.mFraction = hit.mFraction;
    for (const auto& [ent, ch] : m_impl->characters)
    {
        if (!includeCharacters) break;
        if (ent == ignoreEntity) continue;
        const JPH::TransformedShape ts = ch->GetTransformedShape();
        JPH::AnyHitCollisionCollector<JPH::CollidePointCollector> inside;
        ts.CollidePoint(ray.mOrigin, inside);
        if (inside.HadHit()) continue;
        if (ts.CastRay(ray, charBest)) { charEntity = ent; charShape = ts; }
    }

    if (charEntity != entt::null)
    {
        const JPH::RVec3 hp = ray.GetPointOnRay(charBest.mFraction);
        result.hit      = true;
        result.distance = charBest.mFraction * maxDistance;
        result.entity   = charEntity;
        result.point    = { static_cast<f32>(hp.GetX()), static_cast<f32>(hp.GetY()), static_cast<f32>(hp.GetZ()) };
        const JPH::Vec3 n = charShape.GetWorldSpaceSurfaceNormal(charBest.mSubShapeID2, hp);
        result.normal = n.LengthSq() > 1.0e-6f ? XMFLOAT3{ n.GetX(), n.GetY(), n.GetZ() } : XMFLOAT3{ 0.0f, 1.0f, 0.0f };
        return result;
    }
    if (!bodyHit) return result;

    result.hit      = true;
    result.distance = hit.mFraction * maxDistance;
    result.bodyId   = hit.mBodyID.GetIndexAndSequenceNumber();
    if (auto eit = m_bodyToEntity.find(result.bodyId); eit != m_bodyToEntity.end())
        result.entity = eit->second;

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
            // インスタンス群のボディ（複合形状）なら、当たったサブシェイプの userData = インスタンス番号
            const JPH::Shape* shp = lock.GetBody().GetShape();
            if (shp && shp->GetSubType() == JPH::EShapeSubType::StaticCompound)
            {
                const auto* cs = static_cast<const JPH::CompoundShape*>(shp);
                JPH::SubShapeID rem;
                const JPH::uint idx = cs->GetSubShapeIndexFromID(hit.mSubShapeID2, rem);
                if (idx < cs->GetNumSubShapes())
                    result.instanceIndex = cs->GetSubShape(idx).mUserData;
            }
        }
    }

    return result;
}

// ========== Overlap Queries（形状で判定）==========
//
// ★以前はブロードフェーズの AABB だけで判定していたので、回した薄い壁（Y 回転 45°）の AABB の四隅（壁から 3.9m 離れた点）や、
//   メッシュコライダーの巨大な静的ボディ（AABB 内のどこでも）が返った。今は球 / 箱をナローフェーズで
//   相手の実形状と判定する（CollideShape）。キャラ（CharacterVirtual）もカプセルとして含む。
namespace
{
// ヒットしたボディを bodyId→entity で解決して out へ詰める。同じエンティティは 1 回だけ。cap に達したら打ち切る。
struct OverlapEntityCollector final : public JPH::CollideShapeCollector
{
    entt::entity* out = nullptr;
    size_t cap = 0;
    size_t count = 0;
    entt::entity ignore = entt::null;
    const std::unordered_map<uint32_t, entt::entity>* map = nullptr;

    void AddHit(const JPH::CollideShapeResult& r) override
    {
        const auto it = map->find(r.mBodyID2.GetIndexAndSequenceNumber());
        if (it == map->end()) return;
        const entt::entity e = it->second;
        if (e == ignore) return;
        for (size_t i = 0; i < count; ++i)
            if (out[i] == e) return;
        if (count < cap) out[count++] = e;
        if (count >= cap) ForceEarlyOut();
    }
};

size_t OverlapShapeQuery(const JPH::PhysicsSystem& sys,
                         const std::unordered_map<uint32_t, entt::entity>& bodyMap,
                         const std::unordered_map<entt::entity, JPH::Ref<JPH::CharacterVirtual>>& chars,
                         const JPH::Shape* shape, JPH::RVec3Arg center,
                         entt::entity* out, size_t cap, entt::entity ignore)
{
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode     = JPH::EBackFaceMode::CollideWithBackFaces;   // 一枚板のメッシュも裏から重なる
    settings.mActiveEdgeMode   = JPH::EActiveEdgeMode::CollideWithAll;
    settings.mMaxSeparationDistance = 0.0f;

    OverlapEntityCollector col;
    col.out = out; col.cap = cap; col.ignore = ignore; col.map = &bodyMap;

    const JPH::RMat44 com = JPH::RMat44::sTranslation(center);
    sys.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sOne(), com, settings, JPH::RVec3::sZero(), col);
    size_t count = col.count;

    for (const auto& [ent, ch] : chars)
    {
        if (count >= cap) break;
        if (ent == ignore) continue;
        bool dup = false;
        for (size_t i = 0; i < count; ++i) if (out[i] == ent) { dup = true; break; }
        if (dup) continue;
        JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> cc;
        ch->GetTransformedShape().CollideShape(shape, JPH::Vec3::sOne(), com, settings, JPH::RVec3::sZero(), cc);
        if (cc.HadHit()) out[count++] = ent;
    }
    return count;
}
} // anonymous namespace

size_t PhysicsSystem::OverlapBox(const XMFLOAT3& center,
                                 const XMFLOAT3& half,
                                 entt::entity* out, size_t cap,
                                 entt::entity ignoreEntity) const
{
    if (!m_initialized || !m_impl || cap == 0 || !out) return 0;
    if (m_bodyToEntity.empty() && m_impl->characters.empty()) return 0;

    const XMFLOAT3 he = collider::EffectiveBoxHalfExtents(half, { 1.0f, 1.0f, 1.0f });
    JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z), collider::BoxConvexRadius(he));
    return OverlapShapeQuery(*m_impl->physicsSystem, m_bodyToEntity, m_impl->characters, shape.GetPtr(),
                             JPH::RVec3(center.x, center.y, center.z), out, cap, ignoreEntity);
}

size_t PhysicsSystem::OverlapSphere(const XMFLOAT3& center,
                                    float radius,
                                    entt::entity* out, size_t cap,
                                    entt::entity ignoreEntity) const
{
    if (!m_initialized || !m_impl || cap == 0 || !out) return 0;
    if (m_bodyToEntity.empty() && m_impl->characters.empty()) return 0;

    JPH::RefConst<JPH::Shape> shape = new JPH::SphereShape(collider::SafeSize(radius));
    return OverlapShapeQuery(*m_impl->physicsSystem, m_bodyToEntity, m_impl->characters, shape.GetPtr(),
                             JPH::RVec3(center.x, center.y, center.z), out, cap, ignoreEntity);
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
    StepJolt(dt);

    // 手動ステップ（physics:step）で生じた接触も同フレームで配信する。
    // これを呼ばないと pause 中の駒送り時に接触イベントが1フレーム遅延、または
    // pause が続くと永久に届かない。
    FlushPendingContacts();
}

// 手動ステップ（physics:setPaused(true) + physics:step）。以前の Step(dt) は Jolt だけを進めて Transform へ
// 書き戻さなかった（Jolt 上の箱は 2.5m 進んだのに Transform.x は 700.101 のまま）。
// ここではキネマティックへの送り → Jolt → キャラ 1 ステップ → Transform へ書き戻し（補間なし）まで行う。
void PhysicsSystem::Step(float dt, entt::registry& registry)
{
    if (!m_initialized || !m_impl) return;
    if (!(dt > 0.0f)) dt = kFixedTimeStep;
    if (dt > kMaxFrameDt) dt = kMaxFrameDt;

    RegisterPendingPhysicsBodies(registry);
    FollowStaticChanges(registry);
    SyncTransformsToPhysics(registry, dt);
    CapturePrevBodyPoses();
    StepJolt(dt);
    StepCharacters(dt, registry);
    for (auto [e, cc] : registry.view<CharacterController>().each())
        cc._desiredVel = { 0.0f, 0.0f, 0.0f };
    SyncPhysicsToTransformsAlpha(registry, 1.0f);
    SyncCharactersToTransformsAlpha(registry, 1.0f);
    FlushPendingContacts();
}

void PhysicsSystem::SetGravity(XMFLOAT3 g)
{
    if (!m_initialized || !m_impl) return;
    m_impl->physicsSystem->SetGravity(JPH::Vec3(g.x, g.y, g.z));
}

} // namespace dx12e
