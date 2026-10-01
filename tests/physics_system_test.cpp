// 物理（PhysicsSystem / Jolt）のヘッドレス回帰テスト。GPU 不要（箱・球・カプセルだけ。Mesh は使わない）。
//
// PHYSICS_REVIEW_REPORT.md の不具合ごとに 1 つ以上のチェックを置く。直した後の挙動を固定する:
//   #1  質量 0 / 負・負の摩擦・NaN で落ちない（Jolt が FLT_INVALID_OPERATION で即死していた）
//   #2  キネマティックの offset が最初のステップで消えない
//   #3  offset は回転・スケールを通る（ボディ原点 = エンティティ原点・書き戻し・setPosition が揃う）
//   #4  密な積み重ねで床を抜けない・上限超過を数える（戻り値を見る）・ボディ上限超過を数える
//   #6  せん断（非一様スケールの親 + 回転した子）でも親の位置にボディが出る
//   #7  負 / 0 スケールで球・カプセルが床を抜けない（正のスケールと同じ静止位置）
//   #8  Play 中に静的剛体を動かす・拡縮する・コライダーを変えると判定が追従する
//   #9  CharacterController が親の Transform を通る
//   #10 キャラはレイ / オーバーラップに出る・始点が自分のカプセルの中なら自分は無視・ignoreEntity・キャラ同士は素通りしない
//   #11 overlap は形状判定（回した壁の AABB の四隅は当たらない）
//   #14 寝ただけでは exit が出ない・enter が重複しない・離れたら exit が出る
//   #15 15fps 前後の崖が無い（dt=0.1s のフレームで実時間どおり進む）・ステップ数の上限
//   #16 動的剛体の Euler が更新される
//   #18 physics:setPaused + step で Transform が更新される
//
// 実行: ctest --output-on-failure -R PhysicsSystem

#include "physics/PhysicsSystem.h"
#include "physics/ColliderShape.h"
#include "ecs/Components.h"
#include "engine/core/EventBus.h"

#include <entt/entt.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

using namespace dx12e;
using DirectX::XMFLOAT3;

namespace
{
int g_failures = 0;
int g_checks   = 0;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
    do {                                                                               \
        ++g_checks;                                                                    \
        const double va = static_cast<double>(a), vb = static_cast<double>(b);         \
        if (!(std::fabs(va - vb) <= (tol))) {                                          \
            std::printf("FAIL %s:%d: %s = %.6f, want %.6f (tol %g)\n", __FILE__, __LINE__, #a, va, vb, double(tol)); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

namespace
{

constexpr float kDt = 1.0f / 60.0f;

struct Env
{
    PhysicsSystem  ps;
    entt::registry reg;
    EventBus       bus;
    int enters = 0, exits = 0;

    Env()
    {
        ps.Initialize();
        ps.SetEventBus(&bus);
        bus.On("engine.contact.enter", [this](const EngineEvent&) { ++enters; });
        bus.On("engine.contact.exit",  [this](const EngineEvent&) { ++exits; });
    }
    ~Env() { ps.SetEventBus(nullptr); }

    void Run(int steps, float dt = kDt)
    {
        for (int i = 0; i < steps; ++i) { ps.Update(dt, reg); bus.Flush(); }
    }

    entt::entity Make(XMFLOAT3 pos, MotionType m, XMFLOAT3 scale = {1, 1, 1}, float mass = 1.0f,
                      XMFLOAT3 rotDeg = {0, 0, 0})
    {
        const entt::entity e = reg.create();
        Transform t;
        t.position = pos; t.scale = scale; t.rotation = rotDeg;
        reg.emplace<Transform>(e, t);
        RigidBody rb;
        rb.motionType = m;
        rb.mass = mass;
        rb.friction = 0.6f;
        rb.restitution = 0.0f;
        rb.useGravity = (m == MotionType::Dynamic);
        reg.emplace<RigidBody>(e, rb);
        return e;
    }
    entt::entity MakeBox(XMFLOAT3 pos, MotionType m, XMFLOAT3 scale = {1, 1, 1}, float mass = 1.0f,
                         XMFLOAT3 rotDeg = {0, 0, 0}, XMFLOAT3 offset = {0, 0, 0})
    {
        const entt::entity e = Make(pos, m, scale, mass, rotDeg);
        BoxCollider b; b.offset = offset;
        reg.emplace<BoxCollider>(e, b);
        return e;
    }
    // 床: 上面が y=0（厚さ 1m の静的な箱）
    entt::entity Floor(float x = 0.0f, float z = 0.0f, float size = 40.0f)
    {
        return MakeBox({x, -0.5f, z}, MotionType::Static, {size, 1.0f, size});
    }
    Transform& T(entt::entity e) { return reg.get<Transform>(e); }
    RigidBody& R(entt::entity e) { return reg.get<RigidBody>(e); }

    size_t Overlap(XMFLOAT3 c, float r, entt::entity* out, size_t cap, entt::entity ignore = entt::null) const
    { return ps.OverlapSphere(c, r, out, cap, ignore); }
    bool OverlapHas(XMFLOAT3 c, float r, entt::entity who) const
    {
        entt::entity buf[32];
        const size_t n = ps.OverlapSphere(c, r, buf, 32);
        for (size_t i = 0; i < n; ++i) if (buf[i] == who) return true;
        return false;
    }
};

// ---- #1 質量 0 / 負・負の摩擦・NaN ----
void TestInvalidParams()
{
    Env w;
    w.Floor();
    const float nan = std::nanf("");
    std::vector<entt::entity> es;
    for (int i = 0; i < 6; ++i)
        es.push_back(w.MakeBox({-4.0f + i * 1.6f, 1.0f, 0}, MotionType::Dynamic, {1, 1, 1}, 1.0f));
    w.R(es[0]).mass = 0.0f;
    w.R(es[1]).mass = -5.0f;
    w.R(es[2]).mass = nan;
    w.R(es[3]).friction = -1.0f;
    w.R(es[4]).restitution = -2.0f;
    w.R(es[5]).linearDamping = -3.0f;
    w.Run(180);
    for (auto e : es)
    {
        CHECK(std::isfinite(w.T(e).position.y));
        CHECK_NEAR(w.T(e).position.y, 0.5, 0.05);    // 床の上に静止（質量が補正され、落ちていない）
    }
    // 質量の補正関数
    CHECK(collider::SafeMass(0.0f) == 1.0f);
    CHECK(collider::SafeMass(-1.0f) == 1.0f);
    CHECK(collider::SafeMass(nan) == 1.0f);
    CHECK(collider::SafeMass(2.5f) == 2.5f);
    CHECK(collider::SafeFriction(-1.0f) == 0.0f);
    CHECK(collider::SafeRestitution(-1.0f) == 0.0f);
    CHECK(collider::SafeRestitution(5.0f) == 1.0f);
}

// ---- #7 負 / 0 スケール ----
void TestNegativeScale()
{
    Env w;
    w.Floor();
    auto box = [&](XMFLOAT3 pos, XMFLOAT3 scale) { return w.MakeBox(pos, MotionType::Dynamic, scale); };
    const auto bPos  = box({-6, 1.0f, 0}, {1, 1, 1});
    const auto bNegX = box({-4, 1.0f, 0}, {-1, 1, 1});
    const auto bNeg3 = box({-2, 1.0f, 0}, {-2, -2, -2});
    const auto bPos2 = box({-0, 1.0f, 0}, {2, 2, 2});
    const auto bZero = box({2, 1.0f, 0}, {0, 0, 0});

    const auto sphereMake = [&](XMFLOAT3 pos, XMFLOAT3 scale) {
        const auto e = w.Make(pos, MotionType::Dynamic, scale);
        SphereCollider s; s.radius = 0.5f; w.reg.emplace<SphereCollider>(e, s);
        return e;
    };
    const auto sPos = sphereMake({6, 2.0f, 5}, {1, 1, 1});
    const auto sNeg = sphereMake({8, 2.0f, 5}, {-1, -1, -1});

    const auto capMake = [&](XMFLOAT3 pos, XMFLOAT3 scale) {
        const auto e = w.Make(pos, MotionType::Dynamic, scale);
        CapsuleCollider c; c.radius = 0.5f; c.halfHeight = 1.0f; w.reg.emplace<CapsuleCollider>(e, c);
        return e;
    };
    const auto cPos = capMake({-6, 3.0f, 5}, {1, 1, 1});
    const auto cNeg = capMake({-8, 3.0f, 5}, {1, -1, 1});

    w.Run(240);
    CHECK_NEAR(w.T(bPos).position.y, 0.5, 0.03);
    CHECK_NEAR(w.T(bNegX).position.y, 0.5, 0.03);     // 修正前: 0.638 で浮いた
    CHECK_NEAR(w.T(bNeg3).position.y, 1.0, 0.04);     // 半サイズ 1 の箱（符号を捨てた）
    CHECK_NEAR(w.T(bPos2).position.y, 1.0, 0.04);
    CHECK(w.T(bZero).position.y > -0.05f && w.T(bZero).position.y < 0.1f);   // 1mm の箱になるが落ちない
    CHECK_NEAR(w.T(sPos).position.y, 0.5, 0.03);
    CHECK_NEAR(w.T(sNeg).position.y, 0.5, 0.03);      // 修正前: 負の半径で床を抜けて落ち続けた
    CHECK_NEAR(w.T(cPos).position.y, 1.5, 0.05);
    CHECK_NEAR(w.T(cNeg).position.y, 1.5, 0.05);      // 修正前: 床抜け

    // 純粋関数
    CHECK(collider::EffectiveSphereRadius(0.5f, {-1, -1, -1}) == 0.5f);
    CHECK(collider::EffectiveSphereRadius(0.5f, {0, 0, 0}) == collider::kMinColliderSize);
    const XMFLOAT3 he = collider::EffectiveBoxHalfExtents({0.5f, 0.5f, 0.5f}, {-2, 0, 1});
    CHECK(he.x == 1.0f && he.y == collider::kMinColliderSize && he.z == 0.5f);
}

// ---- #2 / #3 offset ----
XMFLOAT3 RotatedOffset(XMFLOAT3 off, XMFLOAT3 scale, XMFLOAT3 rotDeg)
{
    using namespace DirectX;
    const XMFLOAT3 so = collider::ScaledOffset(off, scale);
    const XMMATRIX r = XMMatrixRotationRollPitchYaw(XMConvertToRadians(rotDeg.x), XMConvertToRadians(rotDeg.y),
                                                    XMConvertToRadians(rotDeg.z));
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3TransformNormal(XMLoadFloat3(&so), r));
    return out;
}

void TestOffsetStaticAndKinematic()
{
    for (MotionType m : { MotionType::Static, MotionType::Kinematic })
    {
        Env w;
        const XMFLOAT3 p{10, 1, 0}, rot{0, 90, 0}, off{2, 0, 0};
        const auto e = w.MakeBox(p, m, {1, 1, 1}, 1.0f, rot, off);
        w.Run(30);   // キネマティックは最初のステップで offset が消えていた（#2）。数ステップ回してから見る
        const XMFLOAT3 d = RotatedOffset(off, {1, 1, 1}, rot);
        const XMFLOAT3 want{p.x + d.x, p.y + d.y, p.z + d.z};
        CHECK(w.OverlapHas(want, 0.1f, e));                            // 回した位置に判定がある
        CHECK(!w.OverlapHas({p.x + 2, p.y, p.z}, 0.1f, e));            // ワールド軸の位置（旧）には無い
        CHECK(!w.OverlapHas(p, 0.1f, e));                              // エンティティの原点でもない（offset が消えていない）
    }
    // スケールを通る: scale 2 + offset (2,0,0) → 中心は x=4（ローカル × スケール）
    {
        Env w;
        const auto e = w.MakeBox({0, 1, 0}, MotionType::Static, {2, 2, 2}, 1.0f, {0, 0, 0}, {2, 0, 0});
        w.Run(2);
        CHECK(w.OverlapHas({4, 1, 0}, 0.1f, e));
        CHECK(!w.OverlapHas({2, 1, 0}, 0.1f, e));
    }
}

void TestOffsetDynamicAndSetPosition()
{
    Env w;
    w.Floor();
    // 動的 + offset (0,1,0): 判定の中心は Transform の 1m 上。床に落ちると Transform は 0.5 - 1 = -0.5 に静止する
    const auto e = w.MakeBox({0, 3, 0}, MotionType::Dynamic, {1, 1, 1}, 1.0f, {0, 0, 0}, {0, 1, 0});
    w.Run(240);
    CHECK_NEAR(w.T(e).position.y, -0.5, 0.05);

    // setPosition: Transform = ボディ位置（offset を足し引きしない）。修正前は x=227 になった
    Env w2;
    const auto f = w2.MakeBox({0, 5, 0}, MotionType::Dynamic, {1, 1, 1}, 1.0f, {0, 0, 0}, {3, 0, 0});
    w2.R(f).useGravity = false;
    w2.Run(2);
    w2.ps.SetPosition(w2.R(f).bodyId, {230, 2, 0});
    w2.Run(2);
    CHECK_NEAR(w2.T(f).position.x, 230.0, 0.05);
    CHECK_NEAR(w2.T(f).position.y, 2.0, 0.05);
}

// ---- #6 せん断（非一様スケールの親 + 回転した子）----
void TestShearParent()
{
    Env w;
    const auto parent = w.reg.create();
    Transform pt; pt.position = {5, 0, 0}; pt.scale = {3, 1, 1};
    w.reg.emplace<Transform>(parent, pt);
    const auto child = w.MakeBox({0, 0, 0}, MotionType::Static, {1, 1, 1}, 1.0f, {0, 30, 0});
    w.T(child).parent = parent;
    w.Run(2);
    // 子のワールド位置は親の位置 (5,0,0)。修正前は XMMatrixDecompose が失敗して世界の原点に出た
    CHECK(w.OverlapHas({5, 0, 0}, 0.2f, child));
    CHECK(!w.OverlapHas({0, 0, 0}, 0.2f, child));

    // 純粋関数: せん断を含む行列でも位置は行 3
    using namespace DirectX;
    const XMMATRIX m = XMMatrixRotationY(XMConvertToRadians(30.0f)) * XMMatrixScaling(3, 1, 1) * XMMatrixTranslation(5, 2, 7);
    const collider::WorldDecomposed d = collider::DecomposeWorld(m);
    CHECK_NEAR(d.pos.x, 5.0, 1e-4); CHECK_NEAR(d.pos.y, 2.0, 1e-4); CHECK_NEAR(d.pos.z, 7.0, 1e-4);
    CHECK(std::isfinite(d.rot.x) && std::isfinite(d.rot.w));
    // 0 スケールでも壊れない
    const collider::WorldDecomposed z = collider::DecomposeWorld(XMMatrixScaling(0, 0, 0) * XMMatrixTranslation(1, 2, 3));
    CHECK_NEAR(z.pos.x, 1.0, 1e-5); CHECK(std::isfinite(z.rot.x));
}

// ---- #8 Play 中の静的剛体の変更の追従 ----
void TestStaticFollowsChanges()
{
    Env w;
    const auto wall = w.MakeBox({0, 1, 10}, MotionType::Static);
    w.Run(2);
    auto ray = [&](float x = 0.0f) { return w.ps.Raycast({x, 1, 0}, {0, 0, 1}, 100.0f); };
    RaycastHit h = ray();
    CHECK(h.hit); CHECK_NEAR(h.distance, 9.5, 0.01);
    CHECK(h.entity == wall);                                  // ヒットしたエンティティ

    w.T(wall).position.z = 20.0f;                             // Transform を動かす（Lua / MCP / Inspector と同じ）
    w.Run(2);
    h = ray();
    CHECK(h.hit); CHECK_NEAR(h.distance, 19.5, 0.01);         // 修正前: 9.5 のまま

    w.T(wall).scale = {1, 1, 6};                              // スケール: 奥行き 6 → z 17..23
    w.Run(2);
    h = ray();
    CHECK(h.hit); CHECK_NEAR(h.distance, 17.0, 0.01);         // 修正前: 古い形のまま

    w.reg.get<BoxCollider>(wall).halfExtents = {5.0f, 0.5f, 0.5f};   // コライダー部品
    w.Run(2);
    h = ray(4.0f);                                            // x=4 は幅 5*1=5 の中
    CHECK(h.hit);
    h = ray(5.5f);
    CHECK(!h.hit);

    // キネマティック: 形状（スケール）の変更も追う
    Env w2;
    const auto k = w2.MakeBox({0, 1, 10}, MotionType::Kinematic);
    w2.Run(2);
    w2.T(k).scale = {1, 1, 5};
    w2.Run(3);
    const RaycastHit kh = w2.ps.Raycast({0, 1, 0}, {0, 0, 1}, 100.0f);
    CHECK(kh.hit); CHECK_NEAR(kh.distance, 7.5, 0.05);
}

// ---- #11 overlap は形状判定 ----
void TestOverlapIsShapeBased()
{
    Env w;
    // 10 x 4 x 0.2 の薄い壁（Y 回転 45°）
    const auto wall = w.MakeBox({0, 0, 0}, MotionType::Static, {10, 4, 0.2f}, 1.0f, {0, 45, 0});
    w.Run(2);
    CHECK(w.OverlapHas({0, 0, 0.3f}, 0.5f, wall));              // 壁の上は当たる
    // AABB の四隅（壁から 4.8m 離れた点）。AABB だけの判定では当たっていた
    CHECK(!w.OverlapHas({3.4f, 0, 3.4f}, 0.5f, wall));
    CHECK(!w.OverlapHas({-3.4f, 0, -3.4f}, 0.5f, wall));
    entt::entity buf[8];
    CHECK(w.ps.OverlapBox({3.4f, 0, 3.4f}, {0.1f, 0.1f, 0.1f}, buf, 8) == 0);
    CHECK(w.ps.OverlapBox({0, 0, 0}, {0.1f, 0.1f, 0.1f}, buf, 8) == 1);
    // 同じエンティティは 1 回だけ・ignoreEntity
    CHECK(w.Overlap({0, 0, 0}, 1.0f, buf, 8) == 1);
    CHECK(w.Overlap({0, 0, 0}, 1.0f, buf, 8, wall) == 0);
}

// ---- #9 / #10 キャラ ----
void TestCharacters()
{
    // レイ / オーバーラップに出る・自分は無視・ignoreEntity
    {
        Env w;
        w.Floor();
        const auto a = w.reg.create();
        Transform t; t.position = {0, 1.0f, 0};
        w.reg.emplace<Transform>(a, t);
        w.reg.emplace<CharacterController>(a);
        w.Run(30);
        RaycastHit h = w.ps.Raycast({-5, 1.0f, 0}, {1, 0, 0}, 50.0f);
        CHECK(h.hit); CHECK(h.entity == a);                      // 修正前: 貫通して hit=false
        CHECK_NEAR(h.distance, 4.6, 0.06);                       // 半径 0.4 のカプセル
        CHECK(h.bodyId == 0xFFFFFFFFu);

        h = w.ps.Raycast({0, 1.0f, 0}, {1, 0, 0}, 50.0f);        // 始点が自分のカプセルの中 → 自分には当たらない
        CHECK(!h.hit);
        h = w.ps.Raycast({-5, 1.0f, 0}, {1, 0, 0}, 50.0f, 0xFFFFFFFFu, a);   // ignoreEntity
        CHECK(!h.hit);
        h = w.ps.Raycast({-5, 1.0f, 0}, {1, 0, 0}, 50.0f, 0xFFFFFFFFu, entt::null, /*includeCharacters=*/false);
        CHECK(!h.hit);                                           // 内部用途（足の IK など）は従来どおりキャラを無視

        CHECK(w.OverlapHas({0.2f, 1.0f, 0}, 0.1f, a));           // 修正前: 0 件
        entt::entity buf[4];
        CHECK(w.Overlap({0.2f, 1.0f, 0}, 0.1f, buf, 4, a) == 0);
        CHECK(!w.OverlapHas({3, 1.0f, 0}, 0.5f, a));
    }
    // キャラ同士は素通りしない
    {
        Env w;
        w.Floor();
        auto mk = [&](float x) {
            const auto e = w.reg.create();
            Transform t; t.position = {x, 1.0f, 0};
            w.reg.emplace<Transform>(e, t);
            w.reg.emplace<CharacterController>(e);
            return e;
        };
        const auto a = mk(0.0f), b = mk(3.0f);
        w.Run(20);
        for (int i = 0; i < 180; ++i)
        {
            w.reg.get<CharacterController>(b)._desiredVel = {-3.0f, 0, 0};
            w.Run(1);
        }
        CHECK(w.T(b).position.x > w.T(a).position.x + 0.7f);     // 修正前: b は a を突き抜けて x < 0
    }
    // 親の Transform を通る（#9）。床は親の位置 (300,0,0) にだけある。修正前は物理が原点（床なし）に作られて落ち続けた
    {
        Env w;
        w.Floor(300.0f, 0.0f, 40.0f);
        const auto parent = w.reg.create();
        Transform pt; pt.position = {300, 0, 0};
        w.reg.emplace<Transform>(parent, pt);
        const auto c = w.reg.create();
        Transform ct; ct.position = {0, 1.5f, 0}; ct.parent = parent;
        w.reg.emplace<Transform>(c, ct);
        w.reg.emplace<CharacterController>(c);
        w.Run(120);
        CHECK(w.reg.get<CharacterController>(c)._grounded);
        CHECK_NEAR(w.T(c).position.y, 1.0, 0.05);                // ローカル（親が y=0 なので同値）
        CHECK_NEAR(w.T(c).position.x, 0.0, 0.05);                // ローカル x は 0 のまま（ワールド値を代入しない）
    }
}

// ---- #14 接触イベント ----
void TestContactEvents()
{
    Env w;
    w.Floor();
    const auto e = w.MakeBox({0, 1.0f, 0}, MotionType::Dynamic);
    w.Run(300);   // 着地 → 0.5 秒後に寝る → さらに待つ
    CHECK_NEAR(w.T(e).position.y, 0.5, 0.03);
    CHECK(w.enters == 1);          // 着地で 1 回
    CHECK(w.exits == 0);           // 修正前: 寝たとき（約 0.5 秒後）に exit が出た
    w.ps.ApplyImpulse(w.R(e).bodyId, {0, 20.0f, 0});   // 本当に離れる（滞空 約 2.9 秒）
    w.Run(300);
    CHECK(w.exits >= 1);           // 離れたら exit
    CHECK(w.enters >= 2);          // 降りてきて再接触
}

// ---- #15 / 時間の扱い ----
void TestFrameTime()
{
    // 重力のみで落ちる箱。dt=0.1s のフレームを 10 回（実時間 1s）。落下 ≈ 0.5*14*1² = 7m
    {
        Env w;
        const auto e = w.MakeBox({0, 100, 0}, MotionType::Dynamic);
        w.Run(10, 0.1f);
        const double fall = 100.0 - w.T(e).position.y;
        std::printf("  (dt=0.1s のフレーム x10: 落下 %.2f m / 理論 約 7.1 m)\n", fall);
        CHECK(fall > 6.3 && fall < 7.8);                        // 修正前: 0.45m（1 ステップに潰れていた）
        CHECK(w.ps.GetStats().lastFrameSteps == 6);
    }
    // 60fps と 10fps で同じ実時間の落下がほぼ同じ（崖が無い）
    {
        Env a, b;
        const auto ea = a.MakeBox({0, 100, 0}, MotionType::Dynamic);
        const auto eb = b.MakeBox({0, 100, 0}, MotionType::Dynamic);
        a.Run(60, 1.0f / 60.0f);
        b.Run(10, 0.1f);
        CHECK_NEAR(100.0 - a.T(ea).position.y, 100.0 - b.T(eb).position.y, 0.4);
    }
    // 1 フレームのステップ数の上限（0.5s のフレーム → 0.25s に丸め → 上限 8 ステップ・残りは捨てる）
    {
        Env w;
        w.MakeBox({0, 100, 0}, MotionType::Dynamic);
        w.Run(1, 0.5f);
        const PhysicsStats st = w.ps.GetStats();
        CHECK(st.lastFrameSteps == 8);
        CHECK(st.droppedSteps >= 6);
        w.Run(1, 5.0f);   // 1 秒を超える停止（ロード直後など）は 1 ステップだけ
        CHECK(w.ps.GetStats().lastFrameSteps == 1);
    }
}

// ---- #16 Euler / #18 手動ステップ ----
void TestEulerAndManualStep()
{
    {
        Env w;
        w.Floor();
        const auto e = w.MakeBox({0, 1.5f, 0}, MotionType::Dynamic, {1, 1, 1}, 1.0f, {20, 0, 30});
        w.Run(300);
        const Transform& t = w.T(e);
        CHECK(t.useQuaternion);
        // Euler が quaternion と一致している（修正前は (20,0,30) のまま古かった）
        const DirectX::XMVECTOR q1 = DirectX::XMQuaternionRotationRollPitchYaw(
            DirectX::XMConvertToRadians(t.rotation.x), DirectX::XMConvertToRadians(t.rotation.y), DirectX::XMConvertToRadians(t.rotation.z));
        const DirectX::XMVECTOR q2 = DirectX::XMLoadFloat4(&t.quaternion);
        CHECK(std::fabs(DirectX::XMVectorGetX(DirectX::XMQuaternionDot(q1, q2))) > 0.9995f);
        CHECK(!(std::fabs(t.rotation.x - 20.0f) < 1e-3f && std::fabs(t.rotation.z - 30.0f) < 1e-3f));
    }
    {
        Env w;
        const auto e = w.MakeBox({0, 100, 0}, MotionType::Dynamic);
        w.Run(2);
        w.ps.SetPaused(true);
        const float y0 = w.T(e).position.y;
        w.Run(10);
        CHECK_NEAR(w.T(e).position.y, y0, 1e-6);                 // 一時停止中は動かない
        for (int i = 0; i < 30; ++i) w.ps.Step(kDt, w.reg);       // 手動ステップ 30 回
        CHECK(y0 - w.T(e).position.y > 1.5f);                    // 修正前: Transform は動かなかった（Jolt だけ進んだ）
    }
}

// ---- #4 上限 ----
void TestDensePileAndLimits()
{
    Env w;
    w.Floor(0, 0, 60);
    constexpr int NX = 20, NZ = 20, NY = 10;
    std::vector<entt::entity> boxes;
    for (int y = 0; y < NY; ++y)
        for (int x = 0; x < NX; ++x)
            for (int z = 0; z < NZ; ++z)
                boxes.push_back(w.MakeBox({x * 1.0f - NX / 2.0f, 0.5f + y * 1.0f, z * 1.0f - NZ / 2.0f},
                                          MotionType::Dynamic));
    const auto tp0 = std::chrono::steady_clock::now();
    w.Run(150);
    const double stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count() / 150.0;
    std::printf("  (積み重ね %d 個: 1 ステップ平均 %.2f ms)\n", static_cast<int>(boxes.size()), stepMs);
    int below = 0;
    for (auto e : boxes) if (w.T(e).position.y < 0.3f) ++below;
    const PhysicsStats st = w.ps.GetStats();
    CHECK(below == 0);                               // 修正前（2,560 個以上）: 床を抜けて落ち続けた
    CHECK(st.updateErrorFlags == 0);
    CHECK(st.maxContactConstraints >= 65536);
    CHECK(st.bodies >= static_cast<uint32_t>(boxes.size()));
}

// 上限を上げたことの固定コスト（接触キャッシュ・ボディ対のハッシュ表の初期化）。ほぼ空のシーンでの 1 ステップの平均。情報のみ。
void BenchIdleStep()
{
    Env w;
    w.Floor();
    w.MakeBox({0, 0.5f, 0}, MotionType::Dynamic);
    w.Run(120);   // 寝かせる
    const auto t0 = std::chrono::steady_clock::now();
    const int n = 600;
    w.Run(n);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("  (ほぼ空のシーン: Update 1 回の平均 %.3f ms)\n", ms);
}

void TestBodyLimitIsCounted()
{
    Env w;
    const uint32_t maxBodies = w.ps.GetStats().maxBodies;
    CHECK(maxBodies >= 100000);                       // 修正前: 65,536
    // ボディ上限を超えて作ろうとしたら、作れなかった数が数えられる（以前は一度だけ ERROR を出して以降は黙っていた）
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t extra = 40;
    for (uint32_t i = 0; i < maxBodies + extra; ++i)
    {
        const entt::entity e = w.reg.create();
        Transform t; t.position = { static_cast<float>(i % 512) * 2.0f, 0.0f, static_cast<float>(i / 512) * 2.0f };
        w.reg.emplace<Transform>(e, t);
        RigidBody rb; rb.motionType = MotionType::Static; rb.mass = 0.0f; rb.useGravity = false;
        w.reg.emplace<RigidBody>(e, rb);
        w.reg.emplace<BoxCollider>(e, BoxCollider{});
    }
    w.Run(1);
    const PhysicsStats st = w.ps.GetStats();
    CHECK(st.bodies == maxBodies);
    CHECK(st.failedBodies >= extra);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  (ボディ上限テスト: %u 個 / %.1f 秒 / 作れなかった %u 個)\n", maxBodies + extra, sec, st.failedBodies);
}

// ---- #13 積み重ね: 10 段の塔が落ち着く（スリープのしきい値 0.03 → 0.05。実測は PHYSICS_FIX2_REPORT.md）----
void TestTowerSettles()
{
    struct Cfg { const char* name; float friction, restitution; bool ccd; };
    const Cfg cfgs[] = { {"旧既定 0.3/0.4", 0.3f, 0.4f, false}, {"0.5/0", 0.5f, 0.0f, false},
                         {"新規の既定 0.6/0.1+CCD", 0.6f, 0.1f, true}, {"0.8/0", 0.8f, 0.0f, true},
                         {"1.0/0", 1.0f, 0.0f, true}, {"0.7/0.1+CCD", 0.7f, 0.1f, true} };
    Env w;
    w.Floor(0, 0, 400);
    std::vector<std::vector<entt::entity>> towers;
    int k = 0;
    for (const Cfg& c : cfgs)
    {
        ++k;
        std::vector<entt::entity> t;
        for (int i = 0; i < 10; ++i)
        {
            const auto e = w.MakeBox({k * 6.0f, 0.5f + i, (k % 3) * 50.0f}, MotionType::Dynamic);
            RigidBody& rb = w.R(e);
            rb.friction = c.friction; rb.restitution = c.restitution; rb.continuousCollision = c.ccd;
            rb.linearDamping = 0.02f; rb.angularDamping = 0.01f;
            t.push_back(e);
        }
        towers.push_back(std::move(t));
    }
    w.Run(900);   // 15 秒
    k = 0;
    for (const Cfg& c : cfgs)
    {
        const auto top = towers[k++].back();
        const XMFLOAT3 v = w.ps.GetLinearVelocity(w.R(top).bodyId);
        const float sp = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (!(w.T(top).position.y > 9.3f && sp < 0.05f))
            std::printf("  (塔 %s: 最上段 y=%.3f 速さ=%.3f)\n", c.name, w.T(top).position.y, sp);
        CHECK(w.T(top).position.y > 9.3f);   // 倒れていない
        CHECK(sp < 0.05f);                   // 落ち着いている（修正前は 0.4〜1.0 m/s で揺れ続けた）
    }
}

// ---- #17 Trigger の内外判定（純粋関数）----
void TestTriggerContains()
{
    using namespace DirectX;
    // 回した長い箱（半幅 3 x 0.5）: Y 回転 45°。長軸方向の点は内、回転前の軸上の点は外
    {
        const XMMATRIX w = XMMatrixRotationY(XMConvertToRadians(45.0f)) * XMMatrixTranslation(400, 0, 0);
        const XMFLOAT3 he{3.0f, 1.0f, 0.5f};
        const XMMATRIX r = XMMatrixRotationY(XMConvertToRadians(45.0f));
        XMFLOAT3 axis;   // ローカル x 軸（長軸）のワールド方向
        XMStoreFloat3(&axis, XMVector3TransformNormal(XMVectorSet(1, 0, 0, 0), r));
        const XMFLOAT3 onLong{400 + axis.x * 2.5f, 0, axis.z * 2.5f};
        CHECK(collider::TriggerBoxContains(he, {0, 0, 0}, w, onLong));           // 回った長軸上（修正前は外）
        CHECK(!collider::TriggerBoxContains(he, {0, 0, 0}, w, {402.4f, 0, 0}));   // 回転前の軸上（修正前は内）
    }
    // 負スケール (-1,1,1): 中心に立てば入る（修正前は fabs(d) <= 負の半幅 で常に外）
    {
        const XMMATRIX w = XMMatrixScaling(-1, 1, 1) * XMMatrixTranslation(10, 0, 0);
        CHECK(collider::TriggerBoxContains({1, 1, 1}, {0, 0, 0}, w, {10, 0, 0}));
        CHECK(collider::TriggerBoxContains({1, 1, 1}, {0.5f, 0, 0}, w, {9.5f, 0, 0}));    // offset も鏡像（ローカル + スケール）
        CHECK(!collider::TriggerBoxContains({1, 1, 1}, {0, 0, 0}, w, {12, 0, 0}));
    }
    // 親のスケール 4（ワールド行列に入っている）: 半幅 1 → 世界半幅 4
    {
        const XMMATRIX w = XMMatrixScaling(4, 4, 4);
        CHECK(collider::TriggerBoxContains({1, 1, 1}, {0, 0, 0}, w, {3.5f, 0, 0}));
        CHECK(!collider::TriggerBoxContains({1, 1, 1}, {0, 0, 0}, w, {4.5f, 0, 0}));
        CHECK(collider::TriggerSphereContains(1.0f, {0, 0, 0}, w, {3.5f, 0, 0}));
        CHECK(!collider::TriggerSphereContains(1.0f, {0, 0, 0}, w, {4.5f, 0, 0}));
    }
    // スケール 0（つぶれた箱）は何も入らない・落ちない
    CHECK(!collider::TriggerBoxContains({1, 1, 1}, {0, 0, 0}, XMMatrixScaling(0, 0, 0), {0, 0, 0}));
}

} // namespace

int main()
{
    TestInvalidParams();
    TestNegativeScale();
    TestOffsetStaticAndKinematic();
    TestOffsetDynamicAndSetPosition();
    TestShearParent();
    TestStaticFollowsChanges();
    TestOverlapIsShapeBased();
    TestCharacters();
    TestContactEvents();
    TestFrameTime();
    TestEulerAndManualStep();
    TestTowerSettles();
    TestDensePileAndLimits();
    BenchIdleStep();
    TestBodyLimitIsCounted();
    TestTriggerContains();

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
