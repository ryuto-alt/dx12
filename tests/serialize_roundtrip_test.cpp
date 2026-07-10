// シリアライズ往復（round-trip）テスト — big-bang リファクタの「安全網」。
//
// なに:
//   SceneSerializer::SerializeEntity → InstantiateEntity を通し、
//   データ系コアコンポーネントの「保存される全フィールド」が保存→復元で一致することを検証する。
//
// なぜ:
//   エンジン/ゲーム境界の再設計では、コンポーネント直列化を if(all_of<T>) 連鎖から
//   レジストリ駆動へ移していく。その過程で「あるフィールドが黙って保存されなくなる」
//   サイレント欠落が最も怖い。これを機械検出する基準線をフェーズ0で先に張る。
//
// 制約:
//   GPU/D3D12 デバイス不要にするため、device 依存（meshRenderer / primitive / gridPlane /
//   convexHullCollider / material / color / uvTiling）は対象外。ライト・カメラ・物理・
//   オーディオ・トリガ・パーティクル・LuaScript などのデータ系のみを扱う。
//
// 注意:
//   Gimmick / Trigger(actions) / ParticleEmitter.kind は Phase 3 で「ゲーム側 Lua / データ」へ
//   移設される予定。その移行時に本テストが赤くなったら、それは「移行で挙動が変わった」シグナル。
//   テストを移行と同時に更新すること（黙って消さない）。
//
// 実行: ctest --output-on-failure  （失敗があれば終了コード 1）

#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
// Scene は `std::vector<std::unique_ptr<Mesh>>` をメンバに持つ。Scene をスタックに置くと
// デストラクタで Mesh の完全型が要る（エンジン内 .cpp は Mesh.h 取り込み済みなので顕在化しない）。
#include "renderer/Mesh.h"

// Phase 1 土台ヘッダの compile/link チェックを兼ねる（中立ヘッダが /WX で通ることを保証）。
#include "engine/ecs/ComponentRegistry.h"

#include <entt/entt.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace dx12e;

namespace
{
int g_failures = 0;
int g_checks   = 0;

bool feq(float a, float b)
{
    return std::fabs(a - b) <= 1e-4f * (1.0f + std::fabs(a) + std::fabs(b));
}
bool deq(double a, double b)
{
    return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(a) + std::fabs(b));
}
} // namespace

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_F(a, b)  CHECK(feq((a), (b)))
#define CHECK_D(a, b)  CHECK(deq((a), (b)))
#define CHECK_V3(v, X, Y, Z)                                                  \
    do { CHECK_F((v).x, (X)); CHECK_F((v).y, (Y)); CHECK_F((v).z, (Z)); } while (0)

// src に NameTag + Transform 付きエンティティを作り、build で部品を盛り、
// JSON 直列化 → 別 Scene(dst) へ復元して dst 側 entity を返す。
static entt::entity RoundTrip(Scene& src, Scene& dst,
                              const std::function<void(entt::registry&, entt::entity)>& build)
{
    auto& reg = src.GetRegistry();
    entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{"E"});
    reg.emplace<Transform>(e);
    build(reg, e);

    const std::string js = SceneSerializer::SerializeEntity(src, e, "");
    if (js.empty())
    {
        std::printf("FAIL: SerializeEntity returned empty\n");
        ++g_failures;
        return entt::null;
    }
    entt::entity e2 = SceneSerializer::InstantiateEntity(dst, js, "");
    if (e2 == entt::null)
    {
        std::printf("FAIL: InstantiateEntity returned null\n");
        ++g_failures;
    }
    return e2;
}

// build を満たす entity を往復させ、dst 側で T を取り verify(component) を呼ぶ。
template <typename T, typename Build, typename Verify>
static void Case(Build&& build, Verify&& verify)
{
    Scene src, dst;
    entt::entity e2 = RoundTrip(src, dst, std::forward<Build>(build));
    auto& d = dst.GetRegistry();
    const bool ok = (e2 != entt::null) && d.all_of<T>(e2);
    CHECK(ok);
    if (ok) verify(d.get<T>(e2));
}

static void Test_Transform()
{
    Scene src, dst;
    entt::entity e2 = RoundTrip(src, dst, [](entt::registry& r, entt::entity e) {
        auto& t = r.get<Transform>(e);
        t.position = {1.5f, -2.0f, 3.25f};
        t.rotation = {10.0f, 20.0f, 30.0f};
        t.scale    = {2.0f, 3.0f, 4.0f};
    });
    auto& d = dst.GetRegistry();
    const bool ok = (e2 != entt::null) && d.all_of<Transform>(e2) && d.all_of<NameTag>(e2);
    CHECK(ok);
    if (ok)
    {
        const auto& t = d.get<Transform>(e2);
        CHECK_V3(t.position, 1.5f, -2.0f, 3.25f);
        CHECK_V3(t.rotation, 10.0f, 20.0f, 30.0f);
        CHECK_V3(t.scale,    2.0f, 3.0f, 4.0f);
    }
}

static void Test_PointLight()
{
    Case<PointLight>(
        [](entt::registry& r, entt::entity e) {
            PointLight pl;
            pl.color = {0.1f, 0.2f, 0.3f};
            pl.intensity = 5.0f;
            pl.range = 12.0f;
            r.emplace<PointLight>(e, pl);
        },
        [](const PointLight& pl) {
            CHECK_V3(pl.color, 0.1f, 0.2f, 0.3f);
            CHECK_F(pl.intensity, 5.0f);
            CHECK_F(pl.range, 12.0f);
        });
}

static void Test_DirectionalLight()
{
    Case<DirectionalLight>(
        [](entt::registry& r, entt::entity e) {
            DirectionalLight dl;
            dl.direction = {0.0f, -0.5f, 0.5f};
            dl.color = {0.4f, 0.5f, 0.6f};
            dl.intensity = 2.0f;
            dl.ambient = 0.7f;
            r.emplace<DirectionalLight>(e, dl);
        },
        [](const DirectionalLight& dl) {
            CHECK_V3(dl.direction, 0.0f, -0.5f, 0.5f);
            CHECK_V3(dl.color, 0.4f, 0.5f, 0.6f);
            CHECK_F(dl.intensity, 2.0f);
            CHECK_F(dl.ambient, 0.7f);
        });
}

static void Test_SpotLight()
{
    Case<SpotLight>(
        [](entt::registry& r, entt::entity e) {
            SpotLight sl;
            sl.color = {0.7f, 0.8f, 0.9f};
            sl.intensity = 4.5f;
            sl.range = 20.0f;
            sl.direction = {0.0f, -1.0f, 0.2f};
            sl.innerConeDeg = 15.0f;
            sl.outerConeDeg = 35.0f;
            r.emplace<SpotLight>(e, sl);
        },
        [](const SpotLight& sl) {
            CHECK_V3(sl.color, 0.7f, 0.8f, 0.9f);
            CHECK_F(sl.intensity, 4.5f);
            CHECK_F(sl.range, 20.0f);
            CHECK_V3(sl.direction, 0.0f, -1.0f, 0.2f);
            CHECK_F(sl.innerConeDeg, 15.0f);
            CHECK_F(sl.outerConeDeg, 35.0f);
        });
}

static void Test_Camera()
{
    // dst は専用の Scene なので、他にアクティブカメラが居らず isActive=true が往復する。
    Case<CameraComponent>(
        [](entt::registry& r, entt::entity e) {
            CameraComponent cam;
            cam.fovDegrees = 70.0f;
            cam.nearClip = 0.2f;
            cam.farClip = 500.0f;
            cam.isActive = true;
            cam.projection = CameraProjection::Orthographic;
            cam.orthoSize = 8.5f;
            r.emplace<CameraComponent>(e, cam);
        },
        [](const CameraComponent& cam) {
            CHECK_F(cam.fovDegrees, 70.0f);
            CHECK_F(cam.nearClip, 0.2f);
            CHECK_F(cam.farClip, 500.0f);
            CHECK(cam.isActive == true);
            CHECK(cam.projection == CameraProjection::Orthographic);
            CHECK_F(cam.orthoSize, 8.5f);
        });
}

static void Test_Gimmick()
{
    Case<Gimmick>(
        [](entt::registry& r, entt::entity e) {
            Gimmick gm;
            gm.kind = 2;
            gm.period = 3.0f;
            gm.phase = 0.25f;
            gm.amplitude = 2.5f;
            gm.threshold = 0.7f;
            gm.solid = true;
            gm.deadly = true;
            r.emplace<Gimmick>(e, gm);
        },
        [](const Gimmick& gm) {
            CHECK(gm.kind == 2);
            CHECK_F(gm.period, 3.0f);
            CHECK_F(gm.phase, 0.25f);
            CHECK_F(gm.amplitude, 2.5f);
            CHECK_F(gm.threshold, 0.7f);
            CHECK(gm.solid == true);
            CHECK(gm.deadly == true);
        });
}

static void Test_AudioSource()
{
    Case<AudioSource>(
        [](entt::registry& r, entt::entity e) {
            AudioSource as;
            as.clipPath = "audio/sfx/foot.wav";
            as.volume = 0.5f;
            as.loop = true;
            as.spatial = false;
            as.playOnStart = false;
            as.minDistance = 2.0f;
            as.maxDistance = 40.0f;
            r.emplace<AudioSource>(e, as);
        },
        [](const AudioSource& as) {
            CHECK(as.clipPath == "audio/sfx/foot.wav");
            CHECK_F(as.volume, 0.5f);
            CHECK(as.loop == true);
            CHECK(as.spatial == false);
            CHECK(as.playOnStart == false);
            CHECK_F(as.minDistance, 2.0f);
            CHECK_F(as.maxDistance, 40.0f);
        });
}

static void Test_ParticleEmitter()
{
    Case<ParticleEmitter>(
        [](entt::registry& r, entt::entity e) {
            ParticleEmitter pe;
            pe.kind = 3;
            pe.blend = 1;
            pe.rate = 50.0f;
            pe.playOnStart = false;
            pe.looping = false;
            pe.duration = 2.0f;
            pe.dir = {0.0f, 0.5f, 1.0f};
            pe.spread = 0.6f;
            pe.speed = 4.0f;
            pe.speedVar = 0.7f;
            pe.size = 0.9f;
            pe.sizeEnd = 0.1f;
            pe.life = 1.2f;
            pe.lifeVar = 0.4f;
            pe.color = {0.9f, 0.3f, 0.1f};
            pe.colorEnd = {0.2f, 0.1f, 0.05f};
            pe.intensity = 2.5f;
            pe.gravity = -0.5f;
            pe.drag = 0.8f;
            pe.up = 0.3f;
            pe.stretch = 0.6f;
            r.emplace<ParticleEmitter>(e, pe);
        },
        [](const ParticleEmitter& pe) {
            CHECK(pe.kind == 3);
            CHECK(pe.blend == 1);
            CHECK_F(pe.rate, 50.0f);
            CHECK(pe.playOnStart == false);
            CHECK(pe.looping == false);
            CHECK_F(pe.duration, 2.0f);
            CHECK_V3(pe.dir, 0.0f, 0.5f, 1.0f);
            CHECK_F(pe.spread, 0.6f);
            CHECK_F(pe.speed, 4.0f);
            CHECK_F(pe.speedVar, 0.7f);
            CHECK_F(pe.size, 0.9f);
            CHECK_F(pe.sizeEnd, 0.1f);
            CHECK_F(pe.life, 1.2f);
            CHECK_F(pe.lifeVar, 0.4f);
            CHECK_V3(pe.color, 0.9f, 0.3f, 0.1f);
            CHECK_V3(pe.colorEnd, 0.2f, 0.1f, 0.05f);
            CHECK_F(pe.intensity, 2.5f);
            CHECK_F(pe.gravity, -0.5f);
            CHECK_F(pe.drag, 0.8f);
            CHECK_F(pe.up, 0.3f);
            CHECK_F(pe.stretch, 0.6f);
        });
}

static void Test_Trigger()
{
    Case<Trigger>(
        [](entt::registry& r, entt::entity e) {
            Trigger tr;
            tr.shape = 1;
            tr.halfExtents = {2.0f, 2.0f, 2.0f};
            tr.radius = 3.0f;
            tr.offset = {0.0f, 1.0f, 0.0f};
            tr.filter = "Enemy";
            tr.once = true;
            TriggerAction a;
            a.when = 1;
            a.type = 5;
            a.target = "Door";
            a.str = "open";
            a.num = 2.5;
            a.vec = {1.0f, 0.0f, -1.0f};
            tr.actions.push_back(a);
            r.emplace<Trigger>(e, tr);
        },
        [](const Trigger& tr) {
            CHECK(tr.shape == 1);
            CHECK_V3(tr.halfExtents, 2.0f, 2.0f, 2.0f);
            CHECK_F(tr.radius, 3.0f);
            CHECK_V3(tr.offset, 0.0f, 1.0f, 0.0f);
            CHECK(tr.filter == "Enemy");
            CHECK(tr.once == true);
            CHECK(tr.actions.size() == 1);
            if (tr.actions.size() == 1)
            {
                const auto& a = tr.actions[0];
                CHECK(a.when == 1);
                CHECK(a.type == 5);
                CHECK(a.target == "Door");
                CHECK(a.str == "open");
                CHECK_D(a.num, 2.5);
                CHECK_V3(a.vec, 1.0f, 0.0f, -1.0f);
            }
        });
}

static void Test_RigidBody()
{
    Case<RigidBody>(
        [](entt::registry& r, entt::entity e) {
            RigidBody rb;
            rb.motionType = MotionType::Kinematic;
            rb.mass = 7.0f;
            rb.restitution = 0.6f;
            rb.friction = 0.5f;
            rb.linearDamping = 0.1f;
            rb.angularDamping = 0.2f;
            rb.useGravity = false;
            r.emplace<RigidBody>(e, rb);
        },
        [](const RigidBody& rb) {
            CHECK(rb.motionType == MotionType::Kinematic);
            CHECK_F(rb.mass, 7.0f);
            CHECK_F(rb.restitution, 0.6f);
            CHECK_F(rb.friction, 0.5f);
            CHECK_F(rb.linearDamping, 0.1f);
            CHECK_F(rb.angularDamping, 0.2f);
            CHECK(rb.useGravity == false);
        });
}

static void Test_NetworkIdentity()
{
    Case<NetworkIdentity>(
        [](entt::registry& r, entt::entity e) {
            NetworkIdentity ni;
            ni.interestRadius = 25.0f;
            ni.serverAuthority = false;
            r.emplace<NetworkIdentity>(e, ni);
        },
        [](const NetworkIdentity& ni) {
            CHECK_F(ni.interestRadius, 25.0f);
            CHECK(ni.serverAuthority == false);
        });
}

static void Test_NetworkTransform()
{
    Case<NetworkTransform>(
        [](entt::registry& r, entt::entity e) {
            NetworkTransform nt;
            nt.syncMode = 1;
            nt.sendRate = 30.0f;
            nt.syncPosition = true;
            nt.syncRotation = false;
            nt.syncScale = true;
            nt.interpDelayMs = 50.0f;
            nt.snapDistance = 2.5f;
            r.emplace<NetworkTransform>(e, nt);
        },
        [](const NetworkTransform& nt) {
            CHECK(nt.syncMode == 1);
            CHECK_F(nt.sendRate, 30.0f);
            CHECK(nt.syncPosition == true);
            CHECK(nt.syncRotation == false);
            CHECK(nt.syncScale == true);
            CHECK_F(nt.interpDelayMs, 50.0f);
            CHECK_F(nt.snapDistance, 2.5f);
        });
}

static void Test_BoxCollider()
{
    Case<BoxCollider>(
        [](entt::registry& r, entt::entity e) {
            BoxCollider col;
            col.halfExtents = {1.0f, 2.0f, 3.0f};
            col.offset = {0.1f, 0.2f, 0.3f};
            r.emplace<BoxCollider>(e, col);
        },
        [](const BoxCollider& col) {
            CHECK_V3(col.halfExtents, 1.0f, 2.0f, 3.0f);
            CHECK_V3(col.offset, 0.1f, 0.2f, 0.3f);
        });
}

static void Test_SphereCollider()
{
    Case<SphereCollider>(
        [](entt::registry& r, entt::entity e) {
            SphereCollider col;
            col.radius = 2.5f;
            col.offset = {1.0f, 1.0f, 1.0f};
            r.emplace<SphereCollider>(e, col);
        },
        [](const SphereCollider& col) {
            CHECK_F(col.radius, 2.5f);
            CHECK_V3(col.offset, 1.0f, 1.0f, 1.0f);
        });
}

static void Test_CapsuleCollider()
{
    Case<CapsuleCollider>(
        [](entt::registry& r, entt::entity e) {
            CapsuleCollider col;
            col.radius = 0.7f;
            col.halfHeight = 1.5f;
            col.offset = {0.0f, 0.5f, 0.0f};
            r.emplace<CapsuleCollider>(e, col);
        },
        [](const CapsuleCollider& col) {
            CHECK_F(col.radius, 0.7f);
            CHECK_F(col.halfHeight, 1.5f);
            CHECK_V3(col.offset, 0.0f, 0.5f, 0.0f);
        });
}

static void Test_CharacterController()
{
    Case<CharacterController>(
        [](entt::registry& r, entt::entity e) {
            CharacterController cc;
            cc.radius       = 0.55f;
            cc.halfHeight   = 0.8f;
            cc.offset       = {0.0f, 0.1f, 0.0f};
            cc.mass         = 65.0f;
            cc.maxSlopeDeg  = 45.0f;
            cc.stepHeight   = 0.4f;
            cc.jumpSpeed    = 7.5f;
            cc.gravityScale = 1.5f;
            r.emplace<CharacterController>(e, cc);
        },
        [](const CharacterController& cc) {
            CHECK_F(cc.radius, 0.55f);
            CHECK_F(cc.halfHeight, 0.8f);
            CHECK_V3(cc.offset, 0.0f, 0.1f, 0.0f);
            CHECK_F(cc.mass, 65.0f);
            CHECK_F(cc.maxSlopeDeg, 45.0f);
            CHECK_F(cc.stepHeight, 0.4f);
            CHECK_F(cc.jumpSpeed, 7.5f);
            CHECK_F(cc.gravityScale, 1.5f);
            // ランタイム専有はシリアライズされず既定値のまま
            CHECK(cc._registered == false);
            CHECK(cc._grounded == false);
        });
}

static void Test_LuaScript()
{
    Case<LuaScript>(
        [](entt::registry& r, entt::entity e) {
            LuaScript ls;
            ls.scriptPath = "scripts/player.lua";
            ls.enabled = false;

            ScriptProp pf; pf.name = "speed";  pf.type = ScriptPropType::Float;  pf.num = 9.5;            ls.props.push_back(pf);
            ScriptProp pi; pi.name = "hp";     pi.type = ScriptPropType::Int;    pi.num = 42.0;           ls.props.push_back(pi);
            ScriptProp pb; pb.name = "solid";  pb.type = ScriptPropType::Bool;   pb.b   = true;           ls.props.push_back(pb);
            ScriptProp ps; ps.name = "tag";    ps.type = ScriptPropType::String; ps.str = "hero";         ls.props.push_back(ps);
            ScriptProp pv; pv.name = "tint";   pv.type = ScriptPropType::Vec3;   pv.vec = {0.2f, 0.4f, 0.6f}; ls.props.push_back(pv);

            r.emplace<LuaScript>(e, ls);
        },
        [](const LuaScript& ls) {
            CHECK(ls.scriptPath == "scripts/player.lua");
            CHECK(ls.enabled == false);
            CHECK(ls.props.size() == 5);
            if (ls.props.size() == 5)
            {
                CHECK(ls.props[0].name == "speed"); CHECK(ls.props[0].type == ScriptPropType::Float);  CHECK_D(ls.props[0].num, 9.5);
                CHECK(ls.props[1].name == "hp");    CHECK(ls.props[1].type == ScriptPropType::Int);    CHECK_D(ls.props[1].num, 42.0);
                CHECK(ls.props[2].name == "solid"); CHECK(ls.props[2].type == ScriptPropType::Bool);   CHECK(ls.props[2].b == true);
                CHECK(ls.props[3].name == "tag");   CHECK(ls.props[3].type == ScriptPropType::String); CHECK(ls.props[3].str == "hero");
                CHECK(ls.props[4].name == "tint");  CHECK(ls.props[4].type == ScriptPropType::Vec3);   CHECK_V3(ls.props[4].vec, 0.2f, 0.4f, 0.6f);
            }
        });
}

// 複数部品を同一エンティティに載せても相互干渉せず往復することを確認する。
static void Test_Combined()
{
    Scene src, dst;
    entt::entity e2 = RoundTrip(src, dst, [](entt::registry& r, entt::entity e) {
        PointLight pl; pl.color = {0.3f, 0.6f, 0.9f}; pl.intensity = 3.0f; pl.range = 8.0f;
        r.emplace<PointLight>(e, pl);
        RigidBody rb; rb.motionType = MotionType::Dynamic; rb.mass = 2.0f; rb.useGravity = true;
        r.emplace<RigidBody>(e, rb);
        BoxCollider bc; bc.halfExtents = {0.5f, 0.5f, 0.5f};
        r.emplace<BoxCollider>(e, bc);
    });
    auto& d = dst.GetRegistry();
    const bool ok = (e2 != entt::null) && d.all_of<PointLight>(e2) && d.all_of<RigidBody>(e2) && d.all_of<BoxCollider>(e2);
    CHECK(ok);
    if (ok)
    {
        const auto& pl = d.get<PointLight>(e2);
        CHECK_V3(pl.color, 0.3f, 0.6f, 0.9f);
        CHECK_F(pl.intensity, 3.0f);
        const auto& rb = d.get<RigidBody>(e2);
        CHECK(rb.motionType == MotionType::Dynamic);
        CHECK_F(rb.mass, 2.0f);
        const auto& bc = d.get<BoxCollider>(e2);
        CHECK_V3(bc.halfExtents, 0.5f, 0.5f, 0.5f);
    }
}

static void Test_DataComponent()
{
    Case<DataComponent>(
        [](entt::registry& r, entt::entity e) {
            DataComponent dc;
            DataValue n; n.type = DataValue::Type::Number; n.num = 42.5;              dc.values["hp"]    = n;
            DataValue b; b.type = DataValue::Type::Bool;   b.b   = true;              dc.values["alive"] = b;
            DataValue s; s.type = DataValue::Type::String; s.str = "red";             dc.values["team"]  = s;
            DataValue v; v.type = DataValue::Type::Vec3;   v.vec = {1.0f, 2.0f, 3.0f};dc.values["spawn"] = v;
            r.emplace<DataComponent>(e, dc);
        },
        [](const DataComponent& dc) {
            CHECK(dc.values.size() == 4);
            auto hp = dc.values.find("hp");
            CHECK(hp != dc.values.end());
            if (hp != dc.values.end()) { CHECK(hp->second.type == DataValue::Type::Number); CHECK_D(hp->second.num, 42.5); }
            auto al = dc.values.find("alive");
            CHECK(al != dc.values.end() && al->second.type == DataValue::Type::Bool && al->second.b == true);
            auto tm = dc.values.find("team");
            CHECK(tm != dc.values.end() && tm->second.type == DataValue::Type::String && tm->second.str == "red");
            auto sp = dc.values.find("spawn");
            CHECK(sp != dc.values.end());
            if (sp != dc.values.end()) { CHECK(sp->second.type == DataValue::Type::Vec3); CHECK_V3(sp->second.vec, 1.0f, 2.0f, 3.0f); }
        });
}

static void Test_Sprite2D()
{
    Case<Sprite2D>(
        [](entt::registry& r, entt::entity e) {
            Sprite2D sp;
            sp.texturePath = "textures/hero.png";
            sp.layer = 5;
            sp.size  = { 2.0f, 3.0f };
            sp.uvMin = { 0.1f, 0.2f };
            sp.uvMax = { 0.7f, 0.9f };
            sp.color = { 0.5f, 0.6f, 0.7f, 0.8f };
            sp.worldSpace = false;
            r.emplace<Sprite2D>(e, sp);
        },
        [](const Sprite2D& sp) {
            CHECK(sp.texturePath == "textures/hero.png");
            CHECK(sp.layer == 5);
            CHECK_F(sp.size.x, 2.0f);  CHECK_F(sp.size.y, 3.0f);
            CHECK_F(sp.uvMin.x, 0.1f); CHECK_F(sp.uvMin.y, 0.2f);
            CHECK_F(sp.uvMax.x, 0.7f); CHECK_F(sp.uvMax.y, 0.9f);
            CHECK_F(sp.color.x, 0.5f); CHECK_F(sp.color.y, 0.6f);
            CHECK_F(sp.color.z, 0.7f); CHECK_F(sp.color.w, 0.8f);
            CHECK(sp.worldSpace == false);
        });
}

static void Test_UICanvas()
{
    Case<UICanvas>(
        [](entt::registry& r, entt::entity e) {
            UICanvas cv;
            cv.refWidth  = 1280.0f;
            cv.refHeight = 720.0f;
            cv.scaleMode = 1;
            cv.sortOrder = 3;
            cv.visible   = false;
            r.emplace<UICanvas>(e, cv);
        },
        [](const UICanvas& cv) {
            CHECK_F(cv.refWidth, 1280.0f);
            CHECK_F(cv.refHeight, 720.0f);
            CHECK(cv.scaleMode == 1);
            CHECK(cv.sortOrder == 3);
            CHECK(cv.visible == false);
        });
}

static void Test_UIRect()
{
    Case<UIRect>(
        [](entt::registry& r, entt::entity e) {
            UIRect rc;
            rc.anchorMin = { 0.0f, 0.0f };
            rc.anchorMax = { 1.0f, 0.5f };
            rc.pivot     = { 0.25f, 0.75f };
            rc.offsetMin = { 10.0f, -20.0f };
            rc.offsetMax = { -30.0f, 40.0f };
            rc.visible   = false;
            r.emplace<UIRect>(e, rc);
        },
        [](const UIRect& rc) {
            CHECK_F(rc.anchorMin.x, 0.0f);  CHECK_F(rc.anchorMin.y, 0.0f);
            CHECK_F(rc.anchorMax.x, 1.0f);  CHECK_F(rc.anchorMax.y, 0.5f);
            CHECK_F(rc.pivot.x, 0.25f);     CHECK_F(rc.pivot.y, 0.75f);
            CHECK_F(rc.offsetMin.x, 10.0f); CHECK_F(rc.offsetMin.y, -20.0f);
            CHECK_F(rc.offsetMax.x, -30.0f);CHECK_F(rc.offsetMax.y, 40.0f);
            CHECK(rc.visible == false);
        });
}

static void Test_UIImage()
{
    Case<UIImage>(
        [](entt::registry& r, entt::entity e) {
            UIImage im;
            im.texturePath  = "textures/panel.png";
            im.color        = { 0.1f, 0.2f, 0.3f, 0.4f };
            im.uvMin        = { 0.1f, 0.2f };
            im.uvMax        = { 0.8f, 0.9f };
            im.sliceBorder  = { 8.0f, 16.0f, 8.0f, 16.0f };
            im.cornerRadius = 6.0f;
            im.raycastBlock = false;   // 既定 true から変えて往復を確認
            im.fillAmount   = 0.75f;   // 既定 1 から変えて往復を確認
            im.fillDir      = 2;       // 下から
            r.emplace<UIImage>(e, im);
        },
        [](const UIImage& im) {
            CHECK(im.texturePath == "textures/panel.png");
            CHECK_F(im.color.x, 0.1f); CHECK_F(im.color.y, 0.2f);
            CHECK_F(im.color.z, 0.3f); CHECK_F(im.color.w, 0.4f);
            CHECK_F(im.uvMin.x, 0.1f); CHECK_F(im.uvMin.y, 0.2f);
            CHECK_F(im.uvMax.x, 0.8f); CHECK_F(im.uvMax.y, 0.9f);
            CHECK_F(im.sliceBorder.x, 8.0f);  CHECK_F(im.sliceBorder.y, 16.0f);
            CHECK_F(im.sliceBorder.z, 8.0f);  CHECK_F(im.sliceBorder.w, 16.0f);
            CHECK_F(im.cornerRadius, 6.0f);
            CHECK(im.raycastBlock == false);
            CHECK_F(im.fillAmount, 0.75f);
            CHECK(im.fillDir == 2);
        });
}

static void Test_UIText()
{
    Case<UIText>(
        [](entt::registry& r, entt::entity e) {
            UIText tx;
            tx.text     = "スコア: 100";
            tx.fontSize = 32.0f;
            tx.color    = { 1.0f, 0.9f, 0.2f, 1.0f };
            tx.alignH   = 2;
            tx.alignV   = 0;
            tx.wrap     = true;
            r.emplace<UIText>(e, tx);
        },
        [](const UIText& tx) {
            CHECK(tx.text == "スコア: 100");
            CHECK_F(tx.fontSize, 32.0f);
            CHECK_F(tx.color.x, 1.0f); CHECK_F(tx.color.y, 0.9f);
            CHECK_F(tx.color.z, 0.2f); CHECK_F(tx.color.w, 1.0f);
            CHECK(tx.alignH == 2);
            CHECK(tx.alignV == 0);
            CHECK(tx.wrap == true);
        });
}

static void Test_UIButton()
{
    Case<UIButton>(
        [](entt::registry& r, entt::entity e) {
            UIButton bt;
            bt.onClickEvent = "start_clicked";
            bt.normalColor  = { 0.9f, 0.9f, 0.9f, 1.0f };
            bt.hoverColor   = { 0.8f, 0.8f, 0.8f, 1.0f };
            bt.pressedColor = { 0.5f, 0.5f, 0.5f, 1.0f };
            bt.interactable = false;
            bt._hovered = true;   // ランタイム専有 → シリアライズされないこと
            bt._pressed = true;
            r.emplace<UIButton>(e, bt);
        },
        [](const UIButton& bt) {
            CHECK(bt.onClickEvent == "start_clicked");
            CHECK_F(bt.normalColor.x, 0.9f);  CHECK_F(bt.normalColor.w, 1.0f);
            CHECK_F(bt.hoverColor.x, 0.8f);
            CHECK_F(bt.pressedColor.x, 0.5f);
            CHECK(bt.interactable == false);
            // ランタイム専有はシリアライズされず既定値のまま
            CHECK(bt._hovered == false);
            CHECK(bt._pressed == false);
        });
}

static void Test_Tag()
{
    Case<Tag>(
        [](entt::registry& r, entt::entity e) {
            Tag t;
            t.tags = { "enemy", "flying", "boss" };
            r.emplace<Tag>(e, t);
        },
        [](const Tag& t) {
            CHECK(t.tags.size() == 3);
            if (t.tags.size() == 3) {
                CHECK(t.tags[0] == "enemy");
                CHECK(t.tags[1] == "flying");
                CHECK(t.tags[2] == "boss");
            }
        });
}

// Scene::QueryByTag のヘッドレス検証（device 不要、registry のみ）。
static void Test_TagQuery()
{
    Scene s;
    auto& reg = s.GetRegistry();
    entt::entity a = reg.create();
    reg.emplace<NameTag>(a, NameTag{"A"});
    { Tag t; t.tags = { "enemy", "red" };  reg.emplace<Tag>(a, t); }
    entt::entity b = reg.create();
    reg.emplace<NameTag>(b, NameTag{"B"});
    { Tag t; t.tags = { "enemy", "blue" }; reg.emplace<Tag>(b, t); }
    entt::entity c = reg.create();
    reg.emplace<NameTag>(c, NameTag{"C"});
    { Tag t; t.tags = { "ally" };          reg.emplace<Tag>(c, t); }
    (void)b; (void)c;

    CHECK(s.QueryByTag("enemy").size() == 2);
    const auto reds = s.QueryByTag("red");
    CHECK(reds.size() == 1);
    if (reds.size() == 1) CHECK(reds[0] == a);
    CHECK(s.QueryByTag("nonexistent").empty());
}

// Scene::QueryInBox（矩形＋任意タグ）のヘッドレス検証。
static void Test_QueryInBox()
{
    Scene s;
    auto& reg = s.GetRegistry();
    auto mk = [&](const char* name, float x, float z, const char* tag) -> entt::entity {
        entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{name});
        auto& tf = reg.emplace<Transform>(e);
        tf.position = { x, 0.0f, z };
        if (tag) { Tag t; t.tags = { tag }; reg.emplace<Tag>(e, t); }
        return e;
    };
    entt::entity a = mk("A", 1.0f, 1.0f, "unit");
    entt::entity b = mk("B", 5.0f, 5.0f, "unit");
    mk("C", 1.0f, 1.0f, "tree");   // 矩形内だが別タグ
    (void)b;

    CHECK(s.QueryInBox(0.0f, 0.0f, 2.0f, 2.0f).size() == 2);          // A, C
    const auto units = s.QueryInBox(0.0f, 0.0f, 2.0f, 2.0f, "unit");
    CHECK(units.size() == 1);
    if (units.size() == 1) CHECK(units[0] == a);
    CHECK(s.QueryInBox(0.0f, 0.0f, 6.0f, 6.0f, "unit").size() == 2);  // A, B
    CHECK(s.QueryInBox(10.0f, 10.0f, 20.0f, 20.0f).empty());
}

// シーン単位の SkyboxSettings（IBL/環境マップ）の往復。device 不要の SaveToString/LoadFromString 経路で検証。
static void Test_SkyboxSettings()
{
    Scene src;
    auto& sk = src.GetSkyboxSettings();
    sk.envMapPath      = "skybox/studio.dds";
    sk.iblIntensity    = 1.5f;
    sk.skyboxIntensity = 0.8f;
    sk.drawSkybox      = false;

    const std::string js = SceneSerializer::SaveToString(src, "");
    CHECK(!js.empty());

    Scene dst;
    const bool ok = SceneSerializer::LoadFromString(dst, js, "");
    CHECK(ok);
    if (ok)
    {
        const auto& d = dst.GetSkyboxSettings();
        CHECK(d.envMapPath == "skybox/studio.dds");
        CHECK_F(d.iblIntensity, 1.5f);
        CHECK_F(d.skyboxIntensity, 0.8f);
        CHECK(d.drawSkybox == false);
    }

    // skybox キーが無い JSON でもデフォルトへ復元（後方互換）
    Scene dst2;
    const bool ok2 = SceneSerializer::LoadFromString(dst2, "{\"entities\":[]}", "");
    CHECK(ok2);
    if (ok2)
    {
        const auto& d2 = dst2.GetSkyboxSettings();
        CHECK(d2.envMapPath.empty());
        CHECK_F(d2.iblIntensity, 1.0f);
        CHECK_F(d2.skyboxIntensity, 1.0f);
        CHECK(d2.drawSkybox == true);
    }
}

// シーン単位の SSAOSettings の往復。device 不要の SaveToString/LoadFromString 経路で検証。
static void Test_SSAOSettings()
{
    Scene src;
    auto& ss = src.GetSSAOSettings();
    ss.enabled     = true;
    ss.radius      = 0.8f;
    ss.bias        = 0.04f;
    ss.intensity   = 1.3f;
    ss.power       = 2.0f;
    ss.sampleCount = 8;
    ss.blur        = false;

    const std::string js = SceneSerializer::SaveToString(src, "");
    CHECK(!js.empty());

    Scene dst;
    const bool ok = SceneSerializer::LoadFromString(dst, js, "");
    CHECK(ok);
    if (ok)
    {
        const auto& d = dst.GetSSAOSettings();
        CHECK(d.enabled == true);
        CHECK_F(d.radius, 0.8f);
        CHECK_F(d.bias, 0.04f);
        CHECK_F(d.intensity, 1.3f);
        CHECK_F(d.power, 2.0f);
        CHECK(d.sampleCount == 8);
        CHECK(d.blur == false);
    }

    // ssao キーが無い JSON でもデフォルトへ復元（後方互換）
    Scene dst2;
    const bool ok2 = SceneSerializer::LoadFromString(dst2, "{\"entities\":[]}", "");
    CHECK(ok2);
    if (ok2)
    {
        const auto& d2 = dst2.GetSSAOSettings();
        CHECK(d2.enabled == false);     // 既定 OFF
        CHECK_F(d2.radius, 0.5f);
        CHECK_F(d2.bias, 0.025f);
        CHECK_F(d2.intensity, 1.0f);
        CHECK_F(d2.power, 1.5f);
        CHECK(d2.sampleCount == 16);
        CHECK(d2.blur == true);
    }
}

// エンティティ並び順の往復安定性（Play→Stop で Hierarchy が逆転しないこと）。
// Hierarchy 表示順は registry の view<NameTag>().each()（プール順）で決まる。Play→Stop は
// SaveToString→Clear→LoadFromString で再生成するため、保存順が往復ごとに反転すると
// 停止のたびに並びが逆転する（実バグ）。SaveToString が「プール挿入順」で保存することで
// 往復が同一順序になる。ここでは 2 回の往復で view 列挙順が不変であることを検証する。
static std::vector<std::string> NameOrder(Scene& s)
{
    std::vector<std::string> names;
    auto& reg = s.GetRegistry();
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        (void)e;
        names.push_back(tag.name);
    }
    return names;
}

static void Test_EntityOrderStable()
{
    Scene src;
    {
        auto& reg = src.GetRegistry();
        for (int i = 0; i < 6; ++i)
        {
            entt::entity e = reg.create();
            reg.emplace<NameTag>(e, NameTag{"E" + std::to_string(i)});
            reg.emplace<Transform>(e);
        }
    }

    // 1 回目の Play→Stop 相当: src を保存 → dst1 へ復元
    const std::string js1 = SceneSerializer::SaveToString(src, "");
    CHECK(!js1.empty());
    Scene dst1;
    CHECK(SceneSerializer::LoadFromString(dst1, js1, ""));
    const std::vector<std::string> order1 = NameOrder(dst1);

    // 2 回目の Play→Stop 相当: dst1 を保存 → dst2 へ復元
    const std::string js2 = SceneSerializer::SaveToString(dst1, "");
    Scene dst2;
    CHECK(SceneSerializer::LoadFromString(dst2, js2, ""));
    const std::vector<std::string> order2 = NameOrder(dst2);

    // 6 体そろっていて、表示順が往復で不変（停止のたびに逆転しない）。
    CHECK(order1.size() == 6);
    CHECK(order1 == order2);
}

// 中立ヘッダ(ComponentRegistry.h)が /WX で通り、型が使えることの最小確認。
static void Test_RegistryHeaderCompiles()
{
    RuntimeComponentInfo info;
    info.typeName = "Probe";
    CHECK(info.source == ComponentSource::Core);
    CHECK(info.typeName == "Probe");
}

int main()
{
    Test_Transform();
    Test_PointLight();
    Test_DirectionalLight();
    Test_SpotLight();
    Test_Camera();
    Test_Gimmick();
    Test_AudioSource();
    Test_ParticleEmitter();
    Test_Trigger();
    Test_RigidBody();
    Test_NetworkIdentity();
    Test_NetworkTransform();
    Test_BoxCollider();
    Test_SphereCollider();
    Test_CapsuleCollider();
    Test_CharacterController();
    Test_LuaScript();
    Test_Combined();
    Test_Tag();
    Test_TagQuery();
    Test_QueryInBox();
    Test_DataComponent();
    Test_Sprite2D();
    Test_UICanvas();
    Test_UIRect();
    Test_UIImage();
    Test_UIText();
    Test_UIButton();
    Test_SkyboxSettings();
    Test_SSAOSettings();
    Test_EntityOrderStable();
    Test_RegistryHeaderCompiles();

    std::printf("serialize_roundtrip: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
