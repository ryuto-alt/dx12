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
#include <filesystem>
#include <fstream>             // .prefab に guid が漏れていないかをファイルで見る
#include <functional>
#include <iterator>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>   // guid の JSON を直接検査する

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

static void Test_Decal()
{
    Case<DecalComponent>(
        [](entt::registry& r, entt::entity e) {
            DecalComponent d;
            d.atlasUV       = {0.25f, 0.5f, 0.25f, 0.5f};
            d.atlasUVNormal = {0.75f, 0.5f, 0.25f, 0.5f};
            d.tint          = {0.9f, 0.2f, 0.1f};
            d.opacity       = 0.75f;
            d.emissive      = {0.0f, 1.5f, 0.25f};
            d.normalStrength = 0.5f;
            d.roughness     = 0.2f;
            d.metallic      = 0.0f;
            d.angleFadeDeg  = 45.0f;
            d.fadeEdge      = 0.2f;
            d.sortOrder     = 7;
            r.emplace<DecalComponent>(e, d);
        },
        [](const DecalComponent& d) {
            CHECK_F(d.atlasUV.x, 0.25f);       CHECK_F(d.atlasUV.w, 0.5f);
            CHECK_F(d.atlasUVNormal.x, 0.75f); CHECK_F(d.atlasUVNormal.z, 0.25f);
            CHECK_V3(d.tint, 0.9f, 0.2f, 0.1f);
            CHECK_F(d.opacity, 0.75f);
            CHECK_V3(d.emissive, 0.0f, 1.5f, 0.25f);
            CHECK_F(d.normalStrength, 0.5f);
            CHECK_F(d.roughness, 0.2f);
            CHECK_F(d.metallic, 0.0f);
            CHECK_F(d.angleFadeDeg, 45.0f);
            CHECK_F(d.fadeEdge, 0.2f);
            CHECK(d.sortOrder == 7);
        });
}

// アニメーションステートマシン（.animfsm へのパス + 実行時パラメータ）。
// _state / _loaded はランタイム専有なので meta 未登録＝往復に出てこないこと。
static void Test_AnimatorController()
{
    Case<AnimatorController>(
        [](entt::registry& r, entt::entity e) {
            AnimatorController ac;
            ac.graphPath       = "animfsm/humanoid_locomotion.animfsm";
            ac.playOnStart     = false;
            ac.speed           = 1.25f;
            ac.applyRootMotion = true;
            ac.eventChannel    = "player.";
            r.emplace<AnimatorController>(e, std::move(ac));
        },
        [](const AnimatorController& ac) {
            CHECK(ac.graphPath == "animfsm/humanoid_locomotion.animfsm");
            CHECK(ac.playOnStart == false);
            CHECK_F(ac.speed, 1.25f);
            CHECK(ac.applyRootMotion == true);
            CHECK(ac.eventChannel == "player.");
            // 復元直後は必ず未ロード（複製先が元の FSM 実行状態を引き継がないこと）
            CHECK(ac._state == nullptr);
            CHECK(ac._loaded == false);
        });
}

// フット IK（接地補正）。_ 付きのランタイム状態は往復に出てこないこと。
static void Test_FootIK()
{
    Case<FootIK>(
        [](entt::registry& r, entt::entity e) {
            FootIK ik;
            ik.enabled         = false;
            ik.weight          = 0.75f;
            ik.leftFootBone    = "mixamorig:LeftFoot";
            ik.rightFootBone   = "mixamorig:RightFoot";
            ik.pelvisBone      = "mixamorig:Hips";
            ik.rayUpOffset     = 0.6f;
            ik.rayLength       = 1.4f;
            ik.footHeight      = 0.12f;
            ik.maxPelvisDrop   = 0.4f;
            ik.maxFootPitchDeg = 30.0f;
            ik.smoothTime      = 0.08f;
            ik.fadeOutTime     = 0.2f;
            ik.alignToNormal   = false;
            ik.kneeForward     = {0.0f, 0.0f, -1.0f};
            r.emplace<FootIK>(e, ik);
        },
        [](const FootIK& ik) {
            CHECK(ik.enabled == false);
            CHECK_F(ik.weight, 0.75f);
            CHECK(ik.leftFootBone == "mixamorig:LeftFoot");
            CHECK(ik.rightFootBone == "mixamorig:RightFoot");
            CHECK(ik.pelvisBone == "mixamorig:Hips");
            CHECK_F(ik.rayUpOffset, 0.6f);
            CHECK_F(ik.rayLength, 1.4f);
            CHECK_F(ik.footHeight, 0.12f);
            CHECK_F(ik.maxPelvisDrop, 0.4f);
            CHECK_F(ik.maxFootPitchDeg, 30.0f);
            CHECK_F(ik.smoothTime, 0.08f);
            CHECK_F(ik.fadeOutTime, 0.2f);
            CHECK(ik.alignToNormal == false);
            CHECK_V3(ik.kneeForward, 0.0f, 0.0f, -1.0f);
            // 復元直後は未解決（複製先が元の解決結果を引き継がないこと）
            CHECK(ik._resolved == false);
            CHECK(ik._lFoot == -1 && ik._rFoot == -1);
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
            sp.animFrames = 8;
            sp.animFps    = 12.0f;
            sp.animCols   = 4;
            sp.animRow    = 1;
            sp.animRows   = 2;
            sp.animMode   = 2;
            sp.scrollU    = 0.5f;
            sp.scrollV    = -0.25f;
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
            CHECK(sp.animFrames == 8);
            CHECK_F(sp.animFps, 12.0f);
            CHECK(sp.animCols == 4);
            CHECK(sp.animRow == 1);
            CHECK(sp.animRows == 2);
            CHECK(sp.animMode == 2);
            CHECK_F(sp.scrollU, 0.5f); CHECK_F(sp.scrollV, -0.25f);
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
            rc.rotation  = -12.5f;   // 既定 0 から変えて往復を確認
            rc.skewX     = 8.0f;
            rc.clipChildren = true;
            r.emplace<UIRect>(e, rc);
        },
        [](const UIRect& rc) {
            CHECK_F(rc.anchorMin.x, 0.0f);  CHECK_F(rc.anchorMin.y, 0.0f);
            CHECK_F(rc.anchorMax.x, 1.0f);  CHECK_F(rc.anchorMax.y, 0.5f);
            CHECK_F(rc.pivot.x, 0.25f);     CHECK_F(rc.pivot.y, 0.75f);
            CHECK_F(rc.offsetMin.x, 10.0f); CHECK_F(rc.offsetMin.y, -20.0f);
            CHECK_F(rc.offsetMax.x, -30.0f);CHECK_F(rc.offsetMax.y, 40.0f);
            CHECK(rc.visible == false);
            CHECK_F(rc.rotation, -12.5f);
            CHECK_F(rc.skewX, 8.0f);
            CHECK(rc.clipChildren == true);
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
            im.fillDir      = 4;       // 放射（時計回り）
            im.fillOrigin   = 90.0f;
            im.shape        = 4;       // 六角形
            im.ringThickness = 12.0f;
            im.uvScroll     = { 0.5f, -0.25f };
            im.outlineStyle = 2;       // コーナーブラケット
            im.outlineDash  = 20.0f;
            im.segments     = 5;
            im.segmentGap   = 2.0f;
            im.segmentColor = { 0.1f, 0.2f, 0.3f, 0.9f };
            im.gradientDir    = 2;
            im.gradientColor2 = { 0.5f, 0.6f, 0.7f, 1.0f };
            im.gradientScrollSpeed = 0.8f;
            im.outlineWidth   = 3.0f;
            im.outlineColor   = { 0.9f, 0.8f, 0.1f, 1.0f };
            im.shadowColor    = { 0.0f, 0.0f, 0.0f, 0.6f };
            im.shadowOffset   = { 4.0f, 5.0f };
            im.shadowSoftness = 7.0f;
            im.animFrames = 6;
            im.animFps    = 10.0f;
            im.animCols   = 3;
            im.animRow    = 1;
            im.animRows   = 2;
            im.animMode   = 1;
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
            CHECK(im.fillDir == 4);
            CHECK_F(im.fillOrigin, 90.0f);
            CHECK(im.shape == 4);
            CHECK_F(im.ringThickness, 12.0f);
            CHECK_F(im.uvScroll.x, 0.5f); CHECK_F(im.uvScroll.y, -0.25f);
            CHECK(im.outlineStyle == 2);
            CHECK_F(im.outlineDash, 20.0f);
            CHECK(im.segments == 5);
            CHECK_F(im.segmentGap, 2.0f);
            CHECK_F(im.segmentColor.w, 0.9f);
            CHECK(im.gradientDir == 2);
            CHECK_F(im.gradientColor2.x, 0.5f); CHECK_F(im.gradientColor2.y, 0.6f);
            CHECK_F(im.gradientColor2.z, 0.7f);
            CHECK_F(im.gradientScrollSpeed, 0.8f);
            CHECK_F(im.outlineWidth, 3.0f);
            CHECK_F(im.outlineColor.x, 0.9f); CHECK_F(im.outlineColor.y, 0.8f);
            CHECK_F(im.shadowColor.w, 0.6f);
            CHECK_F(im.shadowOffset.x, 4.0f); CHECK_F(im.shadowOffset.y, 5.0f);
            CHECK_F(im.shadowSoftness, 7.0f);
            CHECK(im.animFrames == 6);
            CHECK_F(im.animFps, 10.0f);
            CHECK(im.animCols == 3);
            CHECK(im.animRow == 1);
            CHECK(im.animRows == 2);
            CHECK(im.animMode == 1);
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
            tx.outlineWidth = 2.0f;
            tx.outlineColor = { 0.1f, 0.1f, 0.2f, 1.0f };
            tx.shadowColor  = { 0.0f, 0.0f, 0.0f, 0.5f };
            tx.shadowOffset = { 2.0f, 3.0f };
            tx.fontPath     = "fonts/title.ttf";
            tx.typewriterSpeed = 20.0f;
            tx.letterSpacing  = 3.5f;
            tx.charAnim       = 1;
            tx.charAnimAmount = 6.0f;
            tx.charAnimSpeed  = 3.0f;
            tx.gradientDir    = 2;
            tx.gradientColor2 = { 1.0f, 0.8f, 0.2f, 1.0f };
            tx.rich           = true;
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
            CHECK_F(tx.outlineWidth, 2.0f);
            CHECK_F(tx.outlineColor.z, 0.2f);
            CHECK_F(tx.shadowColor.w, 0.5f);
            CHECK_F(tx.shadowOffset.x, 2.0f); CHECK_F(tx.shadowOffset.y, 3.0f);
            CHECK(tx.fontPath == "fonts/title.ttf");
            CHECK_F(tx.typewriterSpeed, 20.0f);
            CHECK_F(tx.letterSpacing, 3.5f);
            CHECK(tx.charAnim == 1);
            CHECK_F(tx.charAnimAmount, 6.0f);
            CHECK_F(tx.charAnimSpeed, 3.0f);
            CHECK(tx.gradientDir == 2);
            CHECK_F(tx.gradientColor2.y, 0.8f);
            CHECK(tx.rich == true);
        });
}

static void Test_UILayout()
{
    Case<UILayout>(
        [](entt::registry& r, entt::entity e) {
            UILayout ly;
            ly.mode     = 2;
            ly.cellW    = 120.0f;
            ly.cellH    = 90.0f;
            ly.spacing  = 4.0f;
            ly.padding  = { 10.0f, 20.0f, 30.0f, 40.0f };
            ly.gridCols = 5;
            r.emplace<UILayout>(e, ly);
        },
        [](const UILayout& ly) {
            CHECK(ly.mode == 2);
            CHECK_F(ly.cellW, 120.0f);
            CHECK_F(ly.cellH, 90.0f);
            CHECK_F(ly.spacing, 4.0f);
            CHECK_F(ly.padding.x, 10.0f); CHECK_F(ly.padding.y, 20.0f);
            CHECK_F(ly.padding.z, 30.0f); CHECK_F(ly.padding.w, 40.0f);
            CHECK(ly.gridCols == 5);
        });
}

static void Test_UIScrollView()
{
    Case<UIScrollView>(
        [](entt::registry& r, entt::entity e) {
            UIScrollView sv;
            sv.vertical   = false;   // 既定 true から変えて往復を確認
            sv.horizontal = true;
            sv.scrollX    = 120.0f;
            sv.scrollY    = 340.0f;
            sv.wheelSpeed = 64.0f;
            sv.showBar    = false;
            sv.barColor   = { 0.2f, 0.4f, 0.6f, 0.8f };
            sv.dragScroll = false;   // 既定 true から変えて往復を確認
            sv.flickDecay = 7.5f;
            sv._dragging  = true;    // ランタイム専有 → シリアライズされないこと
            sv._dragMoved = true;
            sv._velX      = 500.0f;
            r.emplace<UIScrollView>(e, sv);
        },
        [](const UIScrollView& sv) {
            CHECK(sv.vertical == false);
            CHECK(sv.horizontal == true);
            CHECK_F(sv.scrollX, 120.0f);
            CHECK_F(sv.scrollY, 340.0f);
            CHECK_F(sv.wheelSpeed, 64.0f);
            CHECK(sv.showBar == false);
            CHECK_F(sv.barColor.x, 0.2f); CHECK_F(sv.barColor.y, 0.4f);
            CHECK_F(sv.barColor.z, 0.6f); CHECK_F(sv.barColor.w, 0.8f);
            CHECK(sv.dragScroll == false);
            CHECK_F(sv.flickDecay, 7.5f);
            // ランタイム専有はシリアライズされず既定値のまま
            CHECK(sv._dragging == false);
            CHECK(sv._dragMoved == false);
            CHECK_F(sv._velX, 0.0f);
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

static void Test_UIAnimator()
{
    Case<UIAnimator>(
        [](entt::registry& r, entt::entity e) {
            UIAnimator an;
            an.showAnim     = 3;
            an.showDuration = 0.5f;
            an.showDelay    = 0.2f;
            an.showEasing   = 4;
            an.slideOffset  = 120.0f;
            an.hoverScale   = 1.1f;
            an.pressScale   = 0.9f;
            an.hoverSpeed   = 20.0f;
            an.loopAnim     = 1;
            an.loopSpeed    = 2.0f;
            an.loopAmount   = 12.0f;
            an._mode = 2;      // ランタイム専有 → シリアライズされないこと
            an._t    = 1.5f;
            an._curAlpha = 0.5f;
            r.emplace<UIAnimator>(e, an);
        },
        [](const UIAnimator& an) {
            CHECK(an.showAnim == 3);
            CHECK_F(an.showDuration, 0.5f);
            CHECK_F(an.showDelay, 0.2f);
            CHECK(an.showEasing == 4);
            CHECK_F(an.slideOffset, 120.0f);
            CHECK_F(an.hoverScale, 1.1f);
            CHECK_F(an.pressScale, 0.9f);
            CHECK_F(an.hoverSpeed, 20.0f);
            CHECK(an.loopAnim == 1);
            CHECK_F(an.loopSpeed, 2.0f);
            CHECK_F(an.loopAmount, 12.0f);
            // ランタイム専有はシリアライズされず既定値のまま
            CHECK(an._mode == 0);
            CHECK_F(an._t, 0.0f);
            CHECK_F(an._curAlpha, 1.0f);
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
        // ★ここは「skybox キーが無ければ SkyboxSettings の構造体既定値になる」ことの確認。
        //   既定は空文字ではなく手続きの空(kProceduralSkyPath)。空文字は「IBL も skybox も
        //   無効」という別の意味を持つ値なので、取り違えると屋内シーンが真っ暗になる。
        CHECK(d2.envMapPath == kProceduralSkyPath);
        CHECK_F(d2.iblIntensity, 1.0f);
        CHECK_F(d2.skyboxIntensity, 1.0f);
        CHECK(d2.drawSkybox == true);
    }
}

// シーン単位の SSAOSettings の往復。device 不要の SaveToString/LoadFromString 経路で検証。
// DDGI（計画09 Step 6 / 段階1）。raytracing の入れ子に保存される。
static void Test_DdgiSettings()
{
    Scene src;
    auto& dg = src.GetDdgiSettings();
    dg.enabled     = true;
    dg.probeCountX = 12; dg.probeCountY = 6; dg.probeCountZ = 10;
    dg.spacing     = 1.5f;
    dg.originX     = -6.0f; dg.originY = 1.25f; dg.originZ = -4.5f;
    dg.rayLength   = 45.0f;
    dg.hysteresis  = 0.9f;
    dg.intensity   = 1.4f;
    dg.normalBias  = 0.05f;

    const std::string js = SceneSerializer::SaveToString(src, "");
    CHECK(!js.empty());

    Scene dst;
    const bool ok = SceneSerializer::LoadFromString(dst, js, "");
    CHECK(ok);
    if (ok)
    {
        const auto& d = dst.GetDdgiSettings();
        CHECK(d.enabled == true);
        CHECK(d.probeCountX == 12);
        CHECK(d.probeCountY == 6);
        CHECK(d.probeCountZ == 10);
        CHECK_F(d.spacing, 1.5f);
        CHECK_F(d.originX, -6.0f);
        CHECK_F(d.originY, 1.25f);
        CHECK_F(d.originZ, -4.5f);
        CHECK_F(d.rayLength, 45.0f);
        CHECK_F(d.hysteresis, 0.9f);
        CHECK_F(d.intensity, 1.4f);
        CHECK_F(d.normalBias, 0.05f);
    }

    // ddgi キーが無い JSON（＝段階1 より前に保存された全シーン）は既定 OFF へ。
    // ★ここが赤くなると「既存シーンを開いた瞬間に絵が変わる」ことを意味する。
    Scene dst2;
    const bool ok2 = SceneSerializer::LoadFromString(dst2, "{\"entities\":[]}", "");
    CHECK(ok2);
    if (ok2)
    {
        const auto& d2 = dst2.GetDdgiSettings();
        CHECK(d2.enabled == false);
        CHECK(d2.probeCountX == 8 && d2.probeCountY == 4 && d2.probeCountZ == 8);
        CHECK_F(d2.spacing, 2.0f);
        CHECK_F(d2.intensity, 1.0f);
    }

    // 手書き JSON の範囲外はロード時に丸める（プローブ数 999 で 4096 上限を割らない）。
    Scene dst3;
    const bool ok3 = SceneSerializer::LoadFromString(
        dst3, "{\"entities\":[],\"raytracing\":{\"ddgi\":{\"enabled\":true,"
              "\"probeCountX\":999,\"probeCountY\":-5,\"spacing\":0.0,\"hysteresis\":2.0}}}", "");
    CHECK(ok3);
    if (ok3)
    {
        const auto& d3 = dst3.GetDdgiSettings();
        CHECK(d3.probeCountX == 32);
        CHECK(d3.probeCountY == 1);
        CHECK_F(d3.spacing, 0.1f);
        CHECK_F(d3.hysteresis, 0.995f);
    }
}

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

// ---------------------------------------------------------------------------
// エンティティ GUID — このコミットが解く問題そのものを再現する。
//
// シーン JSON の親子は長らく「entities 配列のインデックス」だった。その index は
// entt のプール順に依存するので、2 人が別々の場所にエンティティを足すと
// git は行単位でどちらも通し、**コンフリクトを出さずに以降の全 parent がズレる**。
// ここではその「マージ後の JSON」を手で作り、guid があれば階層が保たれることを見る。
// ---------------------------------------------------------------------------
static void Test_GuidSurvivesArrayInsertion()
{
    // A(親) と B(子) を作る
    Scene src;
    {
        auto& reg = src.GetRegistry();
        entt::entity a = reg.create();
        reg.emplace<NameTag>(a, NameTag{"Parent"});
        reg.emplace<Transform>(a);
        entt::entity b = reg.create();
        reg.emplace<NameTag>(b, NameTag{"Child"});
        reg.emplace<Transform>(b).parent = a;
    }
    const std::string js = SceneSerializer::SaveToString(src, "");
    CHECK(!js.empty());

    nlohmann::json root = nlohmann::json::parse(js);
    CHECK(root.contains("entities"));
    CHECK(root["entities"].size() == 2);

    // 保存されたら guid と parentGuid が入っている（これが無いと以下は無意味）
    bool sawGuid = false, sawParentGuid = false;
    for (const auto& ej : root["entities"])
    {
        if (ej.contains("guid") && ej["guid"].is_string()) sawGuid = true;
        if (ej.contains("parentGuid") && ej["parentGuid"].is_string()) sawParentGuid = true;
    }
    CHECK(sawGuid);
    CHECK(sawParentGuid);

    // ★他人が entities 配列の【先頭】へエンティティを 1 個足した状態を作る。
    //   これで既存要素の index は全部 +1 ズレる（git のテキストマージで起きること）。
    nlohmann::json inserted = nlohmann::json::object();
    inserted["name"] = "Intruder";
    inserted["transform"] = {{"position", {0.0, 0.0, 0.0}},
                             {"rotation", {0.0, 0.0, 0.0}},
                             {"scale",    {1.0, 1.0, 1.0}}};
    nlohmann::json merged = root;
    merged["entities"].insert(merged["entities"].begin(), inserted);

    Scene dst;
    CHECK(SceneSerializer::LoadFromString(dst, merged.dump(), ""));

    auto& reg = dst.GetRegistry();
    entt::entity parent = entt::null, child = entt::null, intruder = entt::null;
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        if (tag.name == "Parent")   parent   = e;
        if (tag.name == "Child")    child    = e;
        if (tag.name == "Intruder") intruder = e;
    }
    CHECK(parent != entt::null);
    CHECK(child != entt::null);
    CHECK(intruder != entt::null);

    // guid で解決しているので、index がズレても Child の親は Parent のまま。
    // （index フォールバックだけだと Intruder か別物を指してしまう）
    if (child != entt::null && reg.all_of<Transform>(child))
        CHECK(reg.get<Transform>(child).parent == parent);
    // 割り込んだ方は親を持たない
    if (intruder != entt::null && reg.all_of<Transform>(intruder))
        CHECK(reg.get<Transform>(intruder).parent == entt::null);
}

// guid が無い旧シーンでも従来どおり index で親子が復元されること（後方互換）。
static void Test_LegacySceneWithoutGuid()
{
    const char* legacy = R"({
      "version": 1,
      "entities": [
        { "name": "P", "transform": {"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]} },
        { "name": "C", "transform": {"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}, "parent": 0 }
      ]
    })";
    Scene dst;
    CHECK(SceneSerializer::LoadFromString(dst, legacy, ""));
    auto& reg = dst.GetRegistry();
    entt::entity p = entt::null, c = entt::null;
    for (auto [e, tag] : reg.view<const NameTag>().each())
    {
        if (tag.name == "P") p = e;
        if (tag.name == "C") c = e;
    }
    CHECK(p != entt::null && c != entt::null);
    if (c != entt::null && reg.all_of<Transform>(c))
        CHECK(reg.get<Transform>(c).parent == p);

    // 一度保存すれば guid が付く（既存プロジェクトの移行はこれで進む）
    const std::string saved = SceneSerializer::SaveToString(dst, "");
    nlohmann::json root = nlohmann::json::parse(saved);
    bool allHaveGuid = !root["entities"].empty();
    for (const auto& ej : root["entities"])
        if (!ej.contains("guid") || !ej["guid"].is_string()) allHaveGuid = false;
    CHECK(allHaveGuid);
}

// 複製すると guid は引き継がれない（同じ guid が 2 体できると参照先が曖昧になる）。
static void Test_DuplicateGetsNewGuid()
{
    Scene src;
    entt::entity a = entt::null;
    {
        auto& reg = src.GetRegistry();
        a = reg.create();
        reg.emplace<NameTag>(a, NameTag{"Orig"});
        reg.emplace<Transform>(a);
    }
    // 一度保存して guid を確定させる
    (void)SceneSerializer::SaveToString(src, "");
    const auto* g0 = src.GetRegistry().try_get<EntityGuid>(a);
    CHECK(g0 != nullptr && g0->value != 0);
    const uint64_t origGuid = g0 ? g0->value : 0;

    const entt::entity copy = SceneSerializer::DuplicateEntity(src, a, "");
    CHECK(copy != entt::null);
    // 複製直後は guid 未設定（次の保存で新しい値が振られる）か、少なくとも元と違う
    (void)SceneSerializer::SaveToString(src, "");
    const auto* g1 = src.GetRegistry().try_get<EntityGuid>(copy);
    CHECK(g1 != nullptr);
    if (g1) CHECK(g1->value != origGuid);
}

// 「同じエンティティを作り直す」経路（Undo の復元 / プレハブ適用の伝播）は guid を保つこと。
// ★これが無いと、参照が guid を向いた瞬間に Undo とプレハブ適用のたびに黙って参照が切れる。
//   実際 ApplyPrefabToInstances には guid を戻すコードが最初からあったが、元データを作る
//   SerializeSubtree が guid を書いていなかったので常に 0 を読んでいて一度も動いていなかった。
//   「復元コードがある」ことと「復元されている」ことは別物なので、ここで実際に見る。
static void Test_SubtreeRestoreKeepsGuid()
{
    Scene sc;
    entt::entity a = entt::null;
    {
        auto& reg = sc.GetRegistry();
        a = reg.create();
        reg.emplace<NameTag>(a, NameTag{"Barrel"});
        reg.emplace<Transform>(a);
    }
    (void)SceneSerializer::SaveToString(sc, "");   // ここで guid が確定する
    const auto* g0 = sc.GetRegistry().try_get<EntityGuid>(a);
    CHECK(g0 != nullptr && g0->value != 0);
    const uint64_t orig = g0 ? g0->value : 0;

    const std::string snap = SceneSerializer::SerializeSubtree(sc, a, "");
    CHECK(!snap.empty());
    {   // スナップショット自体に guid が入っていること（ここが空だと以下の検査は無意味）
        nlohmann::json j = nlohmann::json::parse(snap, nullptr, /*allow_exceptions=*/false);
        CHECK(!j.is_discarded());
        CHECK(!j.is_discarded() && j.contains("entities") && j["entities"].size() == 1
              && j["entities"][0].contains("guid"));
    }

    sc.GetRegistry().destroy(a);

    std::vector<entt::entity> all;
    const entt::entity restored =
        SceneSerializer::InstantiateSubtree(sc, snap, "", &all, /*keepGuids*/ true);
    CHECK(restored != entt::null);
    const auto* g1 = sc.GetRegistry().try_get<EntityGuid>(restored);
    CHECK(g1 != nullptr);
    if (g1) CHECK(g1->value == orig);
}

// 既定（keepGuids=false）は従来どおり guid を捨てること。
// 貼り付け・プレハブの新規配置は「別のエンティティ」なので、上の修正のついでに
// ここまで引き継ぐようになっていたら同じ guid が 2 体できて参照先が曖昧になる。
static void Test_SubtreeInstantiateDropsGuidByDefault()
{
    Scene sc;
    entt::entity a = entt::null;
    {
        auto& reg = sc.GetRegistry();
        a = reg.create();
        reg.emplace<NameTag>(a, NameTag{"Crate"});
        reg.emplace<Transform>(a);
    }
    (void)SceneSerializer::SaveToString(sc, "");
    const auto* g0 = sc.GetRegistry().try_get<EntityGuid>(a);
    CHECK(g0 != nullptr && g0->value != 0);
    const uint64_t orig = g0 ? g0->value : 0;

    const std::string snap = SceneSerializer::SerializeSubtree(sc, a, "");
    std::vector<entt::entity> all;
    const entt::entity pasted = SceneSerializer::InstantiateSubtree(sc, snap, "", &all);
    CHECK(pasted != entt::null && pasted != a);
    const auto* g1 = sc.GetRegistry().try_get<EntityGuid>(pasted);
    CHECK(g1 == nullptr || g1->value != orig);   // 未設定 = 次の保存で新しい値が振られる
}

// Trigger の参照が guid 基準になっていること。
// ★これが解く問題: 名前参照は「リネームで切れる」「同名で曖昧になる」。
//   RewriteEntityNameRefs はエディタのリネーム経路を守るだけで、手書き JSON や
//   別経路の改名は素通りする。guid が正なら、名前がどう変わっても参照は残る。
static void Test_TriggerRefSurvivesRenameViaGuid()
{
    // 1) 名前だけで書かれた（＝旧形式の）シーンを作る
    Scene src;
    {
        auto& reg = src.GetRegistry();
        entt::entity door = reg.create();
        reg.emplace<NameTag>(door, NameTag{"Door"});
        reg.emplace<Transform>(door);

        entt::entity zone = reg.create();
        reg.emplace<NameTag>(zone, NameTag{"Zone"});
        reg.emplace<Transform>(zone);
        Trigger tr;
        tr.filter = "Door";
        TriggerAction a; a.target = "Door"; tr.actions.push_back(a);
        reg.emplace<Trigger>(zone, std::move(tr));
    }
    const std::string js1 = SceneSerializer::SaveToString(src, "");
    CHECK(!js1.empty());

    // 2) 読み込むと名前参照が guid へ昇格する（旧シーンの自動移行）
    Scene sc;
    CHECK(SceneSerializer::LoadFromString(sc, js1, ""));
    auto& reg = sc.GetRegistry();

    entt::entity door = entt::null, zone = entt::null;
    for (auto [e, n] : reg.view<const NameTag>().each())
    {
        if (n.name == "Door") door = e;
        if (n.name == "Zone") zone = e;
    }
    CHECK(door != entt::null && zone != entt::null);
    if (door == entt::null || zone == entt::null) return;

    const auto* dg = reg.try_get<EntityGuid>(door);
    CHECK(dg != nullptr && dg->value != 0);
    const uint64_t doorGuid = dg ? dg->value : 0;

    auto& tr = reg.get<Trigger>(zone);
    CHECK(tr.filterGuid == doorGuid);
    CHECK(!tr.actions.empty() && tr.actions[0].targetGuid == doorGuid);

    // 3) ★RewriteEntityNameRefs を通さずに改名する（手書き JSON / 別経路の再現）。
    //    名前だけの参照ならここで黙って切れる。
    reg.get<NameTag>(door).name = "FrontDoor";

    // 4) 保存すると、名前は guid から引き直されて追従する（ドリフトしない）
    const std::string js2 = SceneSerializer::SaveToString(sc, "");
    nlohmann::json r2 = nlohmann::json::parse(js2, nullptr, /*allow_exceptions=*/false);
    CHECK(!r2.is_discarded());
    bool checkedZone = false;
    if (!r2.is_discarded())
    {
        for (const auto& ej : r2["entities"])
        {
            if (ej.value("name", std::string{}) != "Zone") continue;
            checkedZone = true;
            const auto& tj = ej["trigger"];
            CHECK(tj.value("filter", std::string{}) == "FrontDoor");
            CHECK(tj.value("filterGuid", std::string{}).size() == 16);
            CHECK(tj["actions"][0].value("target", std::string{}) == "FrontDoor");
        }
    }
    CHECK(checkedZone);

    // 5) 読み直しても同じエンティティを指したまま
    Scene sc3;
    CHECK(SceneSerializer::LoadFromString(sc3, js2, ""));
    auto& reg3 = sc3.GetRegistry();
    entt::entity zone3 = entt::null;
    for (auto [e, n] : reg3.view<const NameTag>().each())
        if (n.name == "Zone") zone3 = e;
    CHECK(zone3 != entt::null);
    if (zone3 == entt::null) return;
    const entt::entity resolved =
        ResolveEntityRef(reg3, reg3.get<Trigger>(zone3).filterGuid, "FrontDoor");
    CHECK(resolved != entt::null);
    if (resolved != entt::null) CHECK(reg3.get<NameTag>(resolved).name == "FrontDoor");
}

// guid が正で、名前が食い違っていても guid の先を指すこと（同名の曖昧さを断つ）。
static void Test_TriggerGuidWinsOverName()
{
    Scene sc;
    auto& reg = sc.GetRegistry();

    entt::entity a = reg.create();
    reg.emplace<NameTag>(a, NameTag{"Same"});
    reg.emplace<Transform>(a);
    reg.emplace<EntityGuid>(a, EntityGuid{0x1111222233334444ull});

    entt::entity b = reg.create();
    reg.emplace<NameTag>(b, NameTag{"Same"});   // 同名が 2 体
    reg.emplace<Transform>(b);
    reg.emplace<EntityGuid>(b, EntityGuid{0x5555666677778888ull});

    // 名前は両方に一致するが、guid は b を名指ししている
    CHECK(ResolveEntityRef(reg, 0x5555666677778888ull, "Same") == b);
    CHECK(ResolveEntityRef(reg, 0x1111222233334444ull, "Same") == a);
    // guid 0（旧データ）は名前フォールバック。見つかりはするが、どちらかは曖昧なまま
    CHECK(ResolveEntityRef(reg, 0, "Same") != entt::null);
    // guid が死んでいる（対象が消えた）ときも名前で拾える
    CHECK(ResolveEntityRef(reg, 0xdeadbeefdeadbeefull, "Same") != entt::null);
    // どちらも当たらなければ null
    CHECK(ResolveEntityRef(reg, 0xdeadbeefdeadbeefull, "Nope") == entt::null);
}

// ---------------------------------------------------------------------------
// リネームで名前ベースの参照が切れないこと。
// 実行時の解決は「名前一致」なので、切れてもエラーは出ず「なぜか動かない」だけになる。
// ---------------------------------------------------------------------------
static void Test_RenameRewritesNameRefs()
{
    Scene sc;
    auto& reg = sc.GetRegistry();

    entt::entity target = reg.create();
    reg.emplace<NameTag>(target, NameTag{"Player"});
    reg.emplace<Transform>(target);

    // Lua の entity プロパティが "Player" を指す
    entt::entity scripted = reg.create();
    reg.emplace<NameTag>(scripted, NameTag{"Door"});
    reg.emplace<Transform>(scripted);
    {
        LuaScript ls;
        ls.scriptPath = "components/Door.lua";
        ScriptProp p;
        p.name = "opener";
        p.type = ScriptPropType::Entity;
        p.str  = "Player";
        ls.props.push_back(p);
        // 巻き込まれてはいけない別型のプロパティ（文字列だが entity ではない）
        ScriptProp s2;
        s2.name = "label";
        s2.type = ScriptPropType::String;
        s2.str  = "Player";
        ls.props.push_back(s2);
        reg.emplace<LuaScript>(scripted, std::move(ls));
    }

    // Trigger の filter / action target が "Player" を指す
    entt::entity trig = reg.create();
    reg.emplace<NameTag>(trig, NameTag{"Zone"});
    reg.emplace<Transform>(trig);
    {
        Trigger t;
        t.filter = "Player";
        TriggerAction a;
        a.target = "Player";
        t.actions.push_back(a);
        reg.emplace<Trigger>(trig, std::move(t));
    }

    // filter が空（= 暗黙の "Player"）のトリガは触ってはいけない
    entt::entity trigDefault = reg.create();
    reg.emplace<NameTag>(trigDefault, NameTag{"Zone2"});
    reg.emplace<Transform>(trigDefault);
    reg.emplace<Trigger>(trigDefault, Trigger{});

    RewriteEntityNameRefs(reg, "Player", "Hero");

    const auto& ls = reg.get<LuaScript>(scripted);
    CHECK(ls.props.size() == 2);
    if (ls.props.size() == 2)
    {
        CHECK(ls.props[0].str == "Hero");     // entity 参照は追従する
        CHECK(ls.props[1].str == "Player");   // ただの文字列は触らない
    }

    const auto& tr = reg.get<Trigger>(trig);
    CHECK(tr.filter == "Hero");
    CHECK(tr.actions.size() == 1);
    if (tr.actions.size() == 1) CHECK(tr.actions[0].target == "Hero");

    // 空の filter は「Player の暗黙指定」なので埋めない
    CHECK(reg.get<Trigger>(trigDefault).filter.empty());

    // 逆向きに戻せる（Undo が対称に効くことの根拠）
    RewriteEntityNameRefs(reg, "Hero", "Player");
    CHECK(reg.get<LuaScript>(scripted).props[0].str == "Player");
    CHECK(reg.get<Trigger>(trig).filter == "Player");
}

// ---------------------------------------------------------------------------
// アセット参照の付け替え。境界判定を間違えると "tex" が "textures/a.png" を
// 巻き込んで、無関係なアセットの参照が壊れる（しかも実行時までエラーが出ない）。
// ---------------------------------------------------------------------------
static void Test_AssetRefRewrite()
{
    Scene sc;
    auto& reg = sc.GetRegistry();

    auto mkImage = [&](const char* name, const char* tex) {
        entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{name});
        reg.emplace<Transform>(e);
        UIImage img;
        img.texturePath = tex;
        reg.emplace<UIImage>(e, img);
        return e;
    };

    entt::entity exact  = mkImage("Exact",  "tex/a.png");
    entt::entity under  = mkImage("Under",  "tex/sub/b.png");
    entt::entity prefix = mkImage("Prefix", "texture/c.png");   // 前方一致だが別ディレクトリ
    entt::entity other  = mkImage("Other",  "ui/d.png");

    // 数え上げ: ディレクトリ "tex" は exact と under だけを拾う
    std::vector<std::string> who;
    const int n = SceneSerializer::CountAssetPathRefs(sc, "tex", &who);
    CHECK(n == 2);

    // 付け替え: ディレクトリごと移動しても配下の相対部分は保たれる
    const int rewritten = SceneSerializer::RewriteAssetPathRefs(sc, "tex", "assets2/tex");
    CHECK(rewritten == 2);
    CHECK(reg.get<UIImage>(exact).texturePath  == "assets2/tex/a.png");
    CHECK(reg.get<UIImage>(under).texturePath  == "assets2/tex/sub/b.png");
    // ★ここが本題: 前方一致するだけの別パスを巻き込まない
    CHECK(reg.get<UIImage>(prefix).texturePath == "texture/c.png");
    CHECK(reg.get<UIImage>(other).texturePath  == "ui/d.png");

    // 単一ファイルのリネーム
    const int one = SceneSerializer::RewriteAssetPathRefs(sc, "ui/d.png", "ui/renamed.png");
    CHECK(one == 1);
    CHECK(reg.get<UIImage>(other).texturePath == "ui/renamed.png");

    // 該当なしは 0（空文字で全部巻き込む、のような事故がないこと）
    CHECK(SceneSerializer::RewriteAssetPathRefs(sc, "", "x") == 0);
    CHECK(SceneSerializer::CountAssetPathRefs(sc, "") == 0);
}

// プレハブの「適用」が、他インスタンスの手直しを消さないこと（3-way マージ）。
//
// なぜ大事か: レベル制作では 1 つのプレハブを何十個も置き、それぞれ違う場所へ置く。
// ここが壊れていると「プレハブのマテリアルを直したら全部が原点へ戻った」が起きる。
// 以前は他インスタンスを Revert（＝プレハブの姿へ作り直し）していたので、実際に起きていた。
static void Test_PrefabApplyKeepsInstanceOverrides()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dx12_prefab_merge_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string assets = dir.string() + "/";
    const std::string rel    = "p.prefab";

    Scene sc;
    auto& reg = sc.GetRegistry();

    // テンプレートを作って .prefab へ保存 → 実体は消す
    {
        entt::entity t = reg.create();
        reg.emplace<NameTag>(t, NameTag{"Barrel"});
        reg.emplace<Transform>(t);
        PointLight pl; pl.intensity = 1.0f; pl.range = 10.0f;
        reg.emplace<PointLight>(t, pl);
        CHECK(SceneSerializer::SavePrefab(sc, t, assets + rel, assets));
        reg.destroy(t);
    }

    // .prefab は「型」であってインスタンスではないので guid を持ち込まないこと。
    // （SerializeSubtree は Undo/伝播のために guid を書くようになったので、
    //   SavePrefab がそれを落としていないとインスタンスの guid がファイルに残る）
    {
        std::ifstream ifs(assets + rel);
        const std::string body((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
        CHECK(!body.empty());
        CHECK(body.find("\"guid\"") == std::string::npos);
    }

    entt::entity a = SceneSerializer::InstantiatePrefab(sc, assets + rel, assets);
    entt::entity b = SceneSerializer::InstantiatePrefab(sc, assets + rel, assets);
    CHECK(a != entt::null && b != entt::null && a != b);

    // B はレベル上の別の場所へ置いた（インスタンス固有の手直し）＋子を手で足した
    reg.get<Transform>(b).position = {5.0f, 0.0f, 0.0f};
    const std::string bName = reg.get<NameTag>(b).name;
    {
        entt::entity extra = reg.create();
        reg.emplace<NameTag>(extra, NameTag{"HandAdded"});
        Transform tf; tf.parent = b; tf.position = {0.0f, 2.0f, 0.0f};
        reg.emplace<Transform>(extra, tf);
    }

    // 一度保存して guid を確定させ、B の guid を控える。
    // ★プレハブ適用は B を「消して作り直す」ので、guid を引き継がないと伝播のたびに
    //   全インスタンスの guid が回る。参照が guid を向いた瞬間に黙って切れる形になる。
    (void)SceneSerializer::SaveToString(sc, assets);
    const auto* bg = reg.try_get<EntityGuid>(b);
    CHECK(bg != nullptr && bg->value != 0);
    const uint64_t bGuid = bg ? bg->value : 0;

    // A 側でプレハブそのものを直して「適用」
    reg.get<PointLight>(a).intensity = 7.0f;
    int propagated = 0;
    CHECK(SceneSerializer::ApplyPrefabInstance(sc, a, assets, &propagated));
    CHECK(propagated == 1);

    // B は作り直されるので名前で引き直す（★名前が変わらないことも同時に見ている）
    auto findByName = [&](const std::string& nm) {
        entt::entity found = entt::null;
        for (auto [e, n] : reg.view<const NameTag>().each())
            if (n.name == nm) found = e;
        return found;
    };
    entt::entity b2 = findByName(bName);
    CHECK(b2 != entt::null);
    if (b2 == entt::null) { fs::remove_all(dir); return; }

    // ★本題: 置いた位置は残り、プレハブ側の変更は受け取っている
    CHECK_F(reg.get<Transform>(b2).position.x, 5.0f);
    CHECK_F(reg.get<PointLight>(b2).intensity, 7.0f);
    // プレハブで触っていないフィールドは巻き込まれない
    CHECK_F(reg.get<PointLight>(b2).range, 10.0f);
    // 紐付けが外れていない（外れると以後 適用/元に戻す が効かなくなる）
    CHECK(reg.all_of<PrefabLink>(b2));
    // ★guid が回っていない（伝播は「同じインスタンスの作り直し」であってコピーではない）
    {
        const auto* bg2 = reg.try_get<EntityGuid>(b2);
        CHECK(bg2 != nullptr);
        if (bg2) CHECK(bg2->value == bGuid);
    }

    // 手で足した子が生き残り、作り直した B にぶら下がったまま
    entt::entity extra2 = findByName("HandAdded");
    CHECK(extra2 != entt::null);
    if (extra2 != entt::null) CHECK(reg.get<Transform>(extra2).parent == b2);

    // 2 回目の適用で名前に連番が積み上がらない（Barrel_1 → Barrel_1_1 になる回帰の検出）
    reg.get<PointLight>(a).intensity = 9.0f;
    CHECK(SceneSerializer::ApplyPrefabInstance(sc, a, assets, &propagated));
    entt::entity b3 = findByName(bName);
    CHECK(b3 != entt::null);
    if (b3 != entt::null)
    {
        CHECK_F(reg.get<Transform>(b3).position.x, 5.0f);
        CHECK_F(reg.get<PointLight>(b3).intensity, 9.0f);
    }

    fs::remove_all(dir);
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
    Test_Decal();
    Test_AnimatorController();
    Test_FootIK();
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
    Test_UIScrollView();
    Test_UIButton();
    Test_UILayout();
    Test_UIAnimator();
    Test_SkyboxSettings();
    Test_SSAOSettings();
    Test_DdgiSettings();
    Test_EntityOrderStable();
    Test_GuidSurvivesArrayInsertion();
    Test_LegacySceneWithoutGuid();
    Test_DuplicateGetsNewGuid();
    Test_SubtreeRestoreKeepsGuid();
    Test_SubtreeInstantiateDropsGuidByDefault();
    Test_TriggerRefSurvivesRenameViaGuid();
    Test_TriggerGuidWinsOverName();
    Test_RenameRewritesNameRefs();
    Test_AssetRefRewrite();
    Test_PrefabApplyKeepsInstanceOverrides();
    Test_RegistryHeaderCompiles();

    std::printf("serialize_roundtrip: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
