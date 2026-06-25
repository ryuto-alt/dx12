#include "scene/SceneSerializer.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "core/Logger.h"
#include "engine/ecs/ComponentRegistry.h"  // Phase 1: コア部品の直列化をレジストリ走査へ

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
using namespace DirectX;

namespace dx12e
{

// assetsDir プレフィックスを除去して相対パスにする
static std::string MakeRelative(const std::string& absPath,
                                const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    auto abs = fs::path(absPath).lexically_normal().string();
    auto base = fs::path(assetsDir).lexically_normal().string();
    // パス区切りを統一
    std::replace(abs.begin(), abs.end(), '\\', '/');
    std::replace(base.begin(), base.end(), '\\', '/');
    if (abs.rfind(base, 0) == 0)
        return abs.substr(base.size());
    return abs; // assetsDir 配下でなければそのまま返す
}

static json SerializeFloat3(const XMFLOAT3& v)
{
    return json::array({v.x, v.y, v.z});
}

static XMFLOAT3 DeserializeFloat3(const json& j,
                                   XMFLOAT3 defaultVal = {0, 0, 0})
{
    if (!j.is_array() || j.size() < 3) return defaultVal;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

// --- スクリプトプロパティ型 ↔ 文字列（自己記述的に保存するため）---
static const char* ScriptPropTypeStr(ScriptPropType t)
{
    switch (t)
    {
    case ScriptPropType::Int:    return "int";
    case ScriptPropType::Bool:   return "bool";
    case ScriptPropType::String: return "string";
    case ScriptPropType::Vec3:   return "vec3";
    case ScriptPropType::Color:  return "color";
    case ScriptPropType::Entity: return "entity";
    case ScriptPropType::Float:
    default:                     return "float";
    }
}

static ScriptPropType ScriptPropTypeFromStr(const std::string& s)
{
    if (s == "int")    return ScriptPropType::Int;
    if (s == "bool")   return ScriptPropType::Bool;
    if (s == "string") return ScriptPropType::String;
    if (s == "vec3")   return ScriptPropType::Vec3;
    if (s == "color")  return ScriptPropType::Color;
    if (s == "entity") return ScriptPropType::Entity;
    return ScriptPropType::Float;
}

// コア部品の直列化/復元を RuntimeComponentRegistry へ登録する（Phase 1）。
// 既存の per-type コード（SerializeEntityJson / InstantiateEntityJson 内の if 連鎖）を
// そのままラムダへ移設したもの＝挙動は不変。serialize_roundtrip_test が同値性を担保する。
// 段階移行のため、現状は light×3 / camera / rigidBody / 各 collider のみ登録。
// 残り（gimmick/audioSource/particleEmitter/trigger/convexHull/luaScript）は順次移設する。
static void RegisterCoreComponentSerializers()
{
    static bool done = false;
    if (done) return;
    done = true;

    auto& R = RuntimeComponentRegistry::Get();

    R.Register({ "PointLight", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<PointLight>(entity)) {
                const auto& pl = reg.get<PointLight>(entity);
                ej["pointLight"] = {
                    {"color",     SerializeFloat3(pl.color)},
                    {"intensity", pl.intensity},
                    {"range",     pl.range}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("pointLight")) {
                const auto& plj = ej["pointLight"];
                PointLight pl;
                if (plj.contains("color"))     pl.color     = DeserializeFloat3(plj["color"], {1,1,1});
                if (plj.contains("intensity")) pl.intensity = plj["intensity"].get<f32>();
                if (plj.contains("range"))     pl.range     = plj["range"].get<f32>();
                if (!reg.all_of<PointLight>(e))
                    reg.emplace<PointLight>(e, pl);
            }
        }, {}, {} });

    R.Register({ "DirectionalLight", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<DirectionalLight>(entity)) {
                const auto& dl = reg.get<DirectionalLight>(entity);
                ej["directionalLight"] = {
                    {"direction", SerializeFloat3(dl.direction)},
                    {"color",     SerializeFloat3(dl.color)},
                    {"intensity", dl.intensity},
                    {"ambient",   dl.ambient}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("directionalLight")) {
                const auto& dlj = ej["directionalLight"];
                DirectionalLight dl;
                if (dlj.contains("direction")) dl.direction = DeserializeFloat3(dlj["direction"], {0,-1,0});
                if (dlj.contains("color"))     dl.color     = DeserializeFloat3(dlj["color"], {1,1,1});
                if (dlj.contains("intensity")) dl.intensity = dlj["intensity"].get<f32>();
                dl.ambient = dlj.value("ambient", 0.25f);
                if (!reg.all_of<DirectionalLight>(e))
                    reg.emplace<DirectionalLight>(e, dl);
            }
        }, {}, {} });

    R.Register({ "SpotLight", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<SpotLight>(entity)) {
                const auto& sl = reg.get<SpotLight>(entity);
                ej["spotLight"] = {
                    {"color",        SerializeFloat3(sl.color)},
                    {"intensity",    sl.intensity},
                    {"range",        sl.range},
                    {"direction",    SerializeFloat3(sl.direction)},
                    {"innerConeDeg", sl.innerConeDeg},
                    {"outerConeDeg", sl.outerConeDeg}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("spotLight")) {
                const auto& slj = ej["spotLight"];
                SpotLight sl;
                if (slj.contains("color"))     sl.color     = DeserializeFloat3(slj["color"], {1,1,1});
                sl.intensity    = slj.value("intensity", 3.0f);
                sl.range        = slj.value("range", 15.0f);
                if (slj.contains("direction")) sl.direction = DeserializeFloat3(slj["direction"], {0,-1,0});
                sl.innerConeDeg = slj.value("innerConeDeg", 18.0f);
                sl.outerConeDeg = slj.value("outerConeDeg", 28.0f);
                if (!reg.all_of<SpotLight>(e))
                    reg.emplace<SpotLight>(e, sl);
            }
        }, {}, {} });

    R.Register({ "CameraComponent", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<CameraComponent>(entity)) {
                const auto& cam = reg.get<CameraComponent>(entity);
                ej["camera"] = {
                    {"fovDegrees", cam.fovDegrees},
                    {"nearClip",   cam.nearClip},
                    {"farClip",    cam.farClip},
                    {"isActive",   cam.isActive},
                    {"projection", static_cast<int>(cam.projection)},
                    {"orthoSize",  cam.orthoSize}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("camera")) {
                const auto& cj = ej["camera"];
                CameraComponent cam;
                if (cj.contains("fovDegrees")) cam.fovDegrees = cj["fovDegrees"].get<f32>();
                if (cj.contains("nearClip"))   cam.nearClip   = cj["nearClip"].get<f32>();
                if (cj.contains("farClip"))    cam.farClip    = cj["farClip"].get<f32>();
                if (cj.contains("isActive"))   cam.isActive   = cj["isActive"].get<bool>();
                if (cj.contains("projection")) cam.projection = static_cast<CameraProjection>(cj["projection"].get<int>());
                if (cj.contains("orthoSize"))  cam.orthoSize  = cj["orthoSize"].get<f32>();
                // アクティブカメラの重複防止（複製時など）
                if (cam.isActive) {
                    for (auto [oe, oc] : reg.view<const CameraComponent>().each()) {
                        if (oe != e && oc.isActive) { cam.isActive = false; break; }
                    }
                }
                if (!reg.all_of<CameraComponent>(e))
                    reg.emplace<CameraComponent>(e, cam);
            }
        }, {}, {} });

    R.Register({ "RigidBody", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<RigidBody>(entity)) {
                const auto& rb = reg.get<RigidBody>(entity);
                ej["rigidBody"] = {
                    {"motionType",    static_cast<int>(rb.motionType)},
                    {"mass",          rb.mass},
                    {"restitution",   rb.restitution},
                    {"friction",      rb.friction},
                    {"linearDamping", rb.linearDamping},
                    {"angularDamping",rb.angularDamping},
                    {"useGravity",    rb.useGravity}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("rigidBody")) {
                const auto& rbj = ej["rigidBody"];
                RigidBody rb;
                if (rbj.contains("motionType"))    rb.motionType    = static_cast<MotionType>(rbj["motionType"].get<int>());
                if (rbj.contains("mass"))          rb.mass          = rbj["mass"].get<f32>();
                if (rbj.contains("restitution"))   rb.restitution   = rbj["restitution"].get<f32>();
                if (rbj.contains("friction"))      rb.friction      = rbj["friction"].get<f32>();
                if (rbj.contains("linearDamping")) rb.linearDamping = rbj["linearDamping"].get<f32>();
                if (rbj.contains("angularDamping"))rb.angularDamping= rbj["angularDamping"].get<f32>();
                if (rbj.contains("useGravity"))    rb.useGravity    = rbj["useGravity"].get<bool>();
                reg.emplace_or_replace<RigidBody>(e, rb);
            }
        }, {}, {} });

    R.Register({ "BoxCollider", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<BoxCollider>(entity)) {
                const auto& col = reg.get<BoxCollider>(entity);
                ej["boxCollider"] = {
                    {"halfExtents", SerializeFloat3(col.halfExtents)},
                    {"offset",      SerializeFloat3(col.offset)}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("boxCollider")) {
                const auto& cj = ej["boxCollider"];
                BoxCollider col;
                if (cj.contains("halfExtents")) col.halfExtents = DeserializeFloat3(cj["halfExtents"], {0.5f, 0.5f, 0.5f});
                if (cj.contains("offset"))      col.offset      = DeserializeFloat3(cj["offset"]);
                reg.emplace_or_replace<BoxCollider>(e, col);
            }
        }, {}, {} });

    R.Register({ "SphereCollider", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<SphereCollider>(entity)) {
                const auto& col = reg.get<SphereCollider>(entity);
                ej["sphereCollider"] = {
                    {"radius", col.radius},
                    {"offset", SerializeFloat3(col.offset)}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("sphereCollider")) {
                const auto& cj = ej["sphereCollider"];
                SphereCollider col;
                if (cj.contains("radius")) col.radius = cj["radius"].get<f32>();
                if (cj.contains("offset")) col.offset = DeserializeFloat3(cj["offset"]);
                reg.emplace_or_replace<SphereCollider>(e, col);
            }
        }, {}, {} });

    R.Register({ "CapsuleCollider", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<CapsuleCollider>(entity)) {
                const auto& col = reg.get<CapsuleCollider>(entity);
                ej["capsuleCollider"] = {
                    {"radius",     col.radius},
                    {"halfHeight", col.halfHeight},
                    {"offset",     SerializeFloat3(col.offset)}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("capsuleCollider")) {
                const auto& cj = ej["capsuleCollider"];
                CapsuleCollider col;
                if (cj.contains("radius"))     col.radius     = cj["radius"].get<f32>();
                if (cj.contains("halfHeight")) col.halfHeight = cj["halfHeight"].get<f32>();
                if (cj.contains("offset"))     col.offset     = DeserializeFloat3(cj["offset"]);
                reg.emplace_or_replace<CapsuleCollider>(e, col);
            }
        }, {}, {} });

    R.Register({ "CharacterController", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<CharacterController>(entity)) {
                const auto& cc = reg.get<CharacterController>(entity);
                ej["characterController"] = {
                    {"radius",       cc.radius},
                    {"halfHeight",   cc.halfHeight},
                    {"offset",       SerializeFloat3(cc.offset)},
                    {"mass",         cc.mass},
                    {"maxSlopeDeg",  cc.maxSlopeDeg},
                    {"stepHeight",   cc.stepHeight},
                    {"jumpSpeed",    cc.jumpSpeed},
                    {"gravityScale", cc.gravityScale}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("characterController")) {
                const auto& cj = ej["characterController"];
                CharacterController cc;
                cc.radius       = cj.value("radius",       0.4f);
                cc.halfHeight   = cj.value("halfHeight",   0.6f);
                if (cj.contains("offset")) cc.offset = DeserializeFloat3(cj["offset"]);
                cc.mass         = cj.value("mass",         70.0f);
                cc.maxSlopeDeg  = cj.value("maxSlopeDeg",  50.0f);
                cc.stepHeight   = cj.value("stepHeight",   0.3f);
                cc.jumpSpeed    = cj.value("jumpSpeed",    6.0f);
                cc.gravityScale = cj.value("gravityScale", 1.0f);
                reg.emplace_or_replace<CharacterController>(e, cc);
            }
        }, {}, {} });

    R.Register({ "Tag", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<Tag>(entity)) {
                const auto& t = reg.get<Tag>(entity);
                if (!t.tags.empty()) {
                    json arr = json::array();
                    for (const auto& s : t.tags) arr.push_back(s);
                    ej["tags"] = std::move(arr);
                }
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("tags") && ej["tags"].is_array()) {
                Tag t;
                for (const auto& s : ej["tags"]) {
                    if (s.is_string()) t.tags.push_back(s.get<std::string>());
                }
                if (!t.tags.empty())
                    reg.emplace_or_replace<Tag>(e, std::move(t));
            }
        }, {}, {} });

    R.Register({ "DataComponent", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<DataComponent>(entity)) {
                const auto& dc = reg.get<DataComponent>(entity);
                if (!dc.values.empty()) {
                    json obj = json::object();
                    for (const auto& [k, v] : dc.values) {
                        json vj;
                        switch (v.type) {
                        case DataValue::Type::Number: vj = {{"t", "number"}, {"v", v.num}}; break;
                        case DataValue::Type::Bool:   vj = {{"t", "bool"},   {"v", v.b}};   break;
                        case DataValue::Type::String: vj = {{"t", "string"}, {"v", v.str}}; break;
                        case DataValue::Type::Vec3:   vj = {{"t", "vec3"},   {"v", SerializeFloat3(v.vec)}}; break;
                        }
                        obj[k] = std::move(vj);
                    }
                    ej["data"] = std::move(obj);
                }
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("data") && ej["data"].is_object()) {
                DataComponent dc;
                for (auto it = ej["data"].begin(); it != ej["data"].end(); ++it) {
                    const json& vj = it.value();
                    DataValue dv;
                    const std::string t = vj.value("t", "number");
                    if (t == "bool")        { dv.type = DataValue::Type::Bool;   dv.b   = vj.value("v", false); }
                    else if (t == "string") { dv.type = DataValue::Type::String; dv.str = vj.value("v", std::string{}); }
                    else if (t == "vec3")   { dv.type = DataValue::Type::Vec3;   if (vj.contains("v")) dv.vec = DeserializeFloat3(vj["v"]); }
                    else                    { dv.type = DataValue::Type::Number; dv.num = vj.value("v", 0.0); }
                    dc.values[it.key()] = std::move(dv);
                }
                if (!dc.values.empty())
                    reg.emplace_or_replace<DataComponent>(e, std::move(dc));
            }
        }, {}, {} });

    R.Register({ "Sprite2D", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<Sprite2D>(entity)) {
                const auto& sp = reg.get<Sprite2D>(entity);
                ej["sprite2d"] = {
                    {"texturePath", sp.texturePath},
                    {"layer",       sp.layer},
                    {"size",        json::array({sp.size.x, sp.size.y})},
                    {"uvMin",       json::array({sp.uvMin.x, sp.uvMin.y})},
                    {"uvMax",       json::array({sp.uvMax.x, sp.uvMax.y})},
                    {"color",       json::array({sp.color.x, sp.color.y, sp.color.z, sp.color.w})},
                    {"worldSpace",  sp.worldSpace},
                    {"billboard",   sp.billboard}
                };
            }
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("sprite2d")) {
                const auto& sj = ej["sprite2d"];
                Sprite2D sp;
                sp.texturePath = sj.value("texturePath", "");
                sp.layer       = sj.value("layer", 0);
                if (sj.contains("size")  && sj["size"].is_array()  && sj["size"].size()  >= 2)
                    sp.size  = { sj["size"][0].get<float>(),  sj["size"][1].get<float>() };
                if (sj.contains("uvMin") && sj["uvMin"].is_array() && sj["uvMin"].size() >= 2)
                    sp.uvMin = { sj["uvMin"][0].get<float>(), sj["uvMin"][1].get<float>() };
                if (sj.contains("uvMax") && sj["uvMax"].is_array() && sj["uvMax"].size() >= 2)
                    sp.uvMax = { sj["uvMax"][0].get<float>(), sj["uvMax"][1].get<float>() };
                if (sj.contains("color") && sj["color"].is_array() && sj["color"].size() >= 4)
                    sp.color = { sj["color"][0].get<float>(), sj["color"][1].get<float>(),
                                 sj["color"][2].get<float>(), sj["color"][3].get<float>() };
                sp.worldSpace = sj.value("worldSpace", true);
                sp.billboard  = sj.value("billboard", false);
                reg.emplace_or_replace<Sprite2D>(e, sp);
            }
        }, {}, {} });
}

// 単一エンティティを JSON ノードに直列化（parent は含まない）
static json SerializeEntityJson(const entt::registry& reg, entt::entity entity,
                                const std::string& assetsDir)
{
    RegisterCoreComponentSerializers();
    const auto& tag       = reg.get<NameTag>(entity);
    const auto& transform = reg.get<Transform>(entity);

    json ej;
    {
        ej["name"] = tag.name;
        ej["transform"] = {
            {"position", SerializeFloat3(transform.position)},
            {"rotation", SerializeFloat3(transform.rotation)},
            {"scale",    SerializeFloat3(transform.scale)}
        };

        if (reg.all_of<MeshRenderer>(entity))
        {
            const auto& mr = reg.get<MeshRenderer>(entity);
            // プリミティブマーカー（"__primitive_box__" 等）は種別として保存
            if (mr.modelPath.rfind("__primitive_", 0) == 0)
            {
                if      (mr.modelPath == "__primitive_sphere__") ej["primitive"] = "sphere";
                else if (mr.modelPath == "__primitive_plane__")  ej["primitive"] = "plane";
                else                                              ej["primitive"] = "box";
            }
            else
            {
                std::string relPath = MakeRelative(mr.modelPath, assetsDir);
                if (!relPath.empty())
                {
                    ej["meshRenderer"] = {
                        {"modelPath", relPath}
                    };
                }
                else
                {
                    ej["primitive"] = "box";
                }
            }

            // 頂点カラー保存（プリミティブのみ。モデルは頂点ごとの色を壊さないよう除外）
            if (mr.modelPath.rfind("__primitive_", 0) == 0 && !mr.meshes.empty() && mr.meshes[0])
            {
                auto c = mr.meshes[0]->GetVertexColor();
                if (c.x < 0.999f || c.y < 0.999f || c.z < 0.999f)
                    ej["color"] = json::array({ c.x, c.y, c.z });
            }

            // PBR Material パラメータ保存（オーバーライド値優先）
            if (!mr.meshes.empty() && mr.meshes[0] && mr.meshes[0]->GetMaterial())
            {
                const auto* mat = mr.meshes[0]->GetMaterial();
                f32 metallic  = (mr.overrideMetallic  >= 0.0f) ? mr.overrideMetallic  : mat->defaultMetallic;
                f32 roughness = (mr.overrideRoughness >= 0.0f) ? mr.overrideRoughness : mat->defaultRoughness;
                ej["material"] = {
                    {"metallic",  metallic},
                    {"roughness", roughness}
                };
            }

            // UV タイリング
            if (mr.uvScaleU != 1.0f || mr.uvScaleV != 1.0f)
            {
                ej["uvTiling"] = {{"u", mr.uvScaleU}, {"v", mr.uvScaleV}};
            }
        }

        if (reg.all_of<GridPlane>(entity))
        {
            ej["gridPlane"] = {{"size", 50.0f}};
        }

        // レジストリ登録済みコア部品をまとめて直列化（脱 if(all_of<T>) 連鎖）。
        // 現状 light×3 / camera / rigidBody / 各 collider を担当。
        RuntimeComponentRegistry::Get().ForEach([&](const RuntimeComponentInfo& info) {
            if (info.serialize) info.serialize(reg, entity, ej);
        });

        if (reg.all_of<Gimmick>(entity))
        {
            const auto& gm = reg.get<Gimmick>(entity);
            ej["gimmick"] = {
                {"kind",      gm.kind},
                {"period",    gm.period},
                {"phase",     gm.phase},
                {"amplitude", gm.amplitude},
                {"threshold", gm.threshold},
                {"solid",     gm.solid},
                {"deadly",    gm.deadly}
            };
        }

        if (reg.all_of<AudioSource>(entity))
        {
            const auto& as = reg.get<AudioSource>(entity);
            ej["audioSource"] = {
                {"clipPath",    as.clipPath},
                {"volume",      as.volume},
                {"loop",        as.loop},
                {"spatial",     as.spatial},
                {"playOnStart", as.playOnStart},
                {"minDistance", as.minDistance},
                {"maxDistance", as.maxDistance}
            };
        }

        if (reg.all_of<ParticleEmitter>(entity))
        {
            const auto& pe = reg.get<ParticleEmitter>(entity);
            ej["particleEmitter"] = {
                {"kind", pe.kind}, {"blend", pe.blend}, {"rate", pe.rate},
                {"playOnStart", pe.playOnStart}, {"looping", pe.looping}, {"duration", pe.duration},
                {"dir", SerializeFloat3(pe.dir)}, {"spread", pe.spread},
                {"speed", pe.speed}, {"speedVar", pe.speedVar},
                {"size", pe.size}, {"sizeEnd", pe.sizeEnd},
                {"life", pe.life}, {"lifeVar", pe.lifeVar},
                {"color", SerializeFloat3(pe.color)}, {"colorEnd", SerializeFloat3(pe.colorEnd)},
                {"intensity", pe.intensity}, {"gravity", pe.gravity},
                {"drag", pe.drag}, {"up", pe.up}, {"stretch", pe.stretch}
            };
        }

        if (reg.all_of<Trigger>(entity))
        {
            const auto& tr = reg.get<Trigger>(entity);
            json acts = json::array();
            for (const auto& a : tr.actions)
            {
                acts.push_back({
                    {"when", a.when}, {"type", a.type}, {"target", a.target},
                    {"str", a.str}, {"num", a.num}, {"vec", SerializeFloat3(a.vec)}
                });
            }
            ej["trigger"] = {
                {"shape", tr.shape}, {"halfExtents", SerializeFloat3(tr.halfExtents)},
                {"radius", tr.radius}, {"offset", SerializeFloat3(tr.offset)},
                {"filter", tr.filter}, {"once", tr.once}, {"actions", acts}
            };
        }

        // --- Physics ---（RigidBody / 各 Collider の直列化は上の ForEach レジストリ走査が担当）

        // ConvexHullCollider: autoCollider フラグだけ保存（頂点は起動時にメッシュから再生成）
        if (reg.all_of<ConvexHullCollider>(entity))
        {
            ej["convexHullCollider"] = true;
        }

        // --- LuaScript ---
        if (reg.all_of<LuaScript>(entity))
        {
            const auto& ls = reg.get<LuaScript>(entity);
            if (!ls.scriptPath.empty())
            {
                ej["luaScript"] = {
                    {"scriptPath", ls.scriptPath},
                    {"enabled",    ls.enabled}
                };

                // 公開プロパティのインスタンス値（型込みで自己記述的に保存）
                if (!ls.props.empty())
                {
                    json pa = json::array();
                    for (const auto& p : ls.props)
                    {
                        json pj;
                        pj["name"] = p.name;
                        pj["type"] = ScriptPropTypeStr(p.type);
                        switch (p.type)
                        {
                        case ScriptPropType::Float:  pj["value"] = p.num; break;
                        case ScriptPropType::Int:    pj["value"] = static_cast<long long>(p.num); break;
                        case ScriptPropType::Bool:   pj["value"] = p.b; break;
                        case ScriptPropType::String:
                        case ScriptPropType::Entity: pj["value"] = p.str; break;
                        case ScriptPropType::Vec3:
                        case ScriptPropType::Color:
                            pj["value"] = json::array({p.vec.x, p.vec.y, p.vec.z}); break;
                        }
                        pa.push_back(std::move(pj));
                    }
                    ej["luaScript"]["props"] = std::move(pa);
                }
            }
        }
    }

    return ej;
}

// シーン全エンティティを JSON ノードに直列化（共通処理）
static json BuildSceneJson(const Scene& scene, const std::string& assetsDir)
{
    json root;
    root["version"] = 1;
    root["entities"] = json::array();

    const auto& reg = scene.GetRegistry();

    // 1パス目: 保存順を確定して entity → 配列インデックスの対応を作る
    std::vector<entt::entity> order;
    std::unordered_map<entt::entity, int> indexOf;
    auto view = reg.view<const NameTag, const Transform>();
    for (auto [entity, tag, transform] : view.each())
    {
        (void)tag; (void)transform;
        indexOf[entity] = static_cast<int>(order.size());
        order.push_back(entity);
    }

    // 2パス目: 直列化 + 親子関係をインデックス参照で保存
    for (auto entity : order)
    {
        json ej = SerializeEntityJson(reg, entity, assetsDir);

        const auto& transform = reg.get<Transform>(entity);
        if (transform.parent != entt::null && reg.valid(transform.parent))
        {
            auto it = indexOf.find(transform.parent);
            if (it != indexOf.end())
                ej["parent"] = it->second;
        }

        root["entities"].push_back(ej);
    }

    // ポストプロセス設定（シーン単位）
    {
        const auto& pp = scene.GetPostSettings();
        root["postProcess"] = {
            {"enabled",        pp.enabled},
            {"exposureOn",   pp.exposureOn},   {"exposure",   pp.exposure},
            {"contrastOn",   pp.contrastOn},   {"contrast",   pp.contrast},
            {"brightnessOn", pp.brightnessOn}, {"brightness", pp.brightness},
            {"saturationOn", pp.saturationOn}, {"saturation", pp.saturation},
            {"warmthOn",     pp.warmthOn},     {"warmth",     pp.warmth},
            {"hueOn",        pp.hueOn},        {"hueShift",   pp.hueShift},
            {"tintOn",       pp.tintOn},       {"tint",       {pp.tint.x, pp.tint.y, pp.tint.z}},
            {"bloomOn",      pp.bloomOn},      {"bloom",      pp.bloom}, {"bloomThreshold", pp.bloomThreshold},
            {"vignetteOn",   pp.vignetteOn},   {"vignette",   pp.vignette},
            {"chromaticOn",  pp.chromaticOn},  {"chromatic",  pp.chromatic},
            {"pixelizeOn",   pp.pixelizeOn},   {"pixelSize",  pp.pixelSize},
            {"posterizeOn",  pp.posterizeOn},  {"posterize",  pp.posterize},
            {"ditherOn",     pp.ditherOn},     {"ditherLevels", pp.ditherLevels},
            {"scanlineOn",   pp.scanlineOn},   {"scanline",   pp.scanline},
            {"sharpenOn",    pp.sharpenOn},    {"sharpen",    pp.sharpen},
            {"grainOn",      pp.grainOn},      {"grain",      pp.grain},
            {"invertOn",     pp.invertOn},     {"invert",     pp.invert},
            {"sepiaOn",      pp.sepiaOn},      {"sepia",      pp.sepia},
            {"grayscaleOn",  pp.grayscaleOn},  {"grayscale",  pp.grayscale},
            {"lensOn",       pp.lensOn},       {"lens",       pp.lens},
            {"waveOn",       pp.waveOn},       {"waveAmp",    pp.waveAmp},
            {"waveFreq",     pp.waveFreq},     {"waveSpeed",  pp.waveSpeed},
            {"radialOn",     pp.radialOn},     {"radial",     pp.radial},
            {"glitchOn",     pp.glitchOn},     {"glitch",     pp.glitch},
            {"outlineOn",    pp.outlineOn},    {"outline",    pp.outline},
            {"outlineColor", {pp.outlineColor.x, pp.outlineColor.y, pp.outlineColor.z}},
            {"fxaaOn",       pp.fxaaOn},
        };
    }

    // スカイボックス / IBL 設定（シーン単位）
    {
        const auto& sk = scene.GetSkyboxSettings();
        root["skybox"] = {
            {"envMapPath",      sk.envMapPath},
            {"iblIntensity",    sk.iblIntensity},
            {"skyboxIntensity", sk.skyboxIntensity},
            {"drawSkybox",      sk.drawSkybox},
        };
    }

    return root;
}

// JSON から ポストプロセス設定を復元（postProcess が無ければデフォルト）
static void LoadPostSettings(Scene& scene, const json& root)
{
    PostProcessSettings pp;  // デフォルト（未指定キーは既定値を維持）
    if (root.contains("postProcess"))
    {
        const auto& pj = root["postProcess"];
        pp.enabled      = pj.value("enabled", pp.enabled);
        pp.exposureOn   = pj.value("exposureOn",   pp.exposureOn);   pp.exposure   = pj.value("exposure",   pp.exposure);
        pp.contrastOn   = pj.value("contrastOn",   pp.contrastOn);   pp.contrast   = pj.value("contrast",   pp.contrast);
        pp.brightnessOn = pj.value("brightnessOn", pp.brightnessOn); pp.brightness = pj.value("brightness", pp.brightness);
        pp.saturationOn = pj.value("saturationOn", pp.saturationOn); pp.saturation = pj.value("saturation", pp.saturation);
        pp.warmthOn     = pj.value("warmthOn",     pp.warmthOn);     pp.warmth     = pj.value("warmth",     pp.warmth);
        pp.hueOn        = pj.value("hueOn",        pp.hueOn);        pp.hueShift   = pj.value("hueShift",   pp.hueShift);
        pp.tintOn       = pj.value("tintOn",       pp.tintOn);
        if (pj.contains("tint")) pp.tint = DeserializeFloat3(pj["tint"], {1, 1, 1});
        pp.bloomOn      = pj.value("bloomOn",      pp.bloomOn);      pp.bloom      = pj.value("bloom",      pp.bloom);
        pp.bloomThreshold = pj.value("bloomThreshold", pp.bloomThreshold);
        pp.vignetteOn   = pj.value("vignetteOn",   pp.vignetteOn);   pp.vignette   = pj.value("vignette",   pp.vignette);
        pp.chromaticOn  = pj.value("chromaticOn",  pp.chromaticOn);  pp.chromatic  = pj.value("chromatic",  pp.chromatic);
        pp.pixelizeOn   = pj.value("pixelizeOn",   pp.pixelizeOn);   pp.pixelSize  = pj.value("pixelSize",  pp.pixelSize);
        pp.posterizeOn  = pj.value("posterizeOn",  pp.posterizeOn);  pp.posterize  = pj.value("posterize",  pp.posterize);
        pp.ditherOn     = pj.value("ditherOn",     pp.ditherOn);     pp.ditherLevels = pj.value("ditherLevels", pp.ditherLevels);
        pp.scanlineOn   = pj.value("scanlineOn",   pp.scanlineOn);   pp.scanline   = pj.value("scanline",   pp.scanline);
        pp.sharpenOn    = pj.value("sharpenOn",    pp.sharpenOn);    pp.sharpen    = pj.value("sharpen",    pp.sharpen);
        pp.grainOn      = pj.value("grainOn",      pp.grainOn);      pp.grain      = pj.value("grain",      pp.grain);
        pp.invertOn     = pj.value("invertOn",     pp.invertOn);     pp.invert     = pj.value("invert",     pp.invert);
        pp.sepiaOn      = pj.value("sepiaOn",      pp.sepiaOn);      pp.sepia      = pj.value("sepia",      pp.sepia);
        pp.grayscaleOn  = pj.value("grayscaleOn",  pp.grayscaleOn);  pp.grayscale  = pj.value("grayscale",  pp.grayscale);
        pp.lensOn       = pj.value("lensOn",       pp.lensOn);       pp.lens       = pj.value("lens",       pp.lens);
        pp.waveOn       = pj.value("waveOn",       pp.waveOn);       pp.waveAmp    = pj.value("waveAmp",    pp.waveAmp);
        pp.waveFreq     = pj.value("waveFreq",     pp.waveFreq);     pp.waveSpeed  = pj.value("waveSpeed",  pp.waveSpeed);
        pp.radialOn     = pj.value("radialOn",     pp.radialOn);     pp.radial     = pj.value("radial",     pp.radial);
        pp.glitchOn     = pj.value("glitchOn",     pp.glitchOn);     pp.glitch     = pj.value("glitch",     pp.glitch);
        pp.outlineOn    = pj.value("outlineOn",    pp.outlineOn);    pp.outline    = pj.value("outline",    pp.outline);
        if (pj.contains("outlineColor")) pp.outlineColor = DeserializeFloat3(pj["outlineColor"], {0, 0, 0});
        pp.fxaaOn       = pj.value("fxaaOn",       pp.fxaaOn);
    }
    scene.GetPostSettings() = pp;
}

// JSON から スカイボックス / IBL 設定を復元（skybox が無ければデフォルト）
static void LoadSkyboxSettings(Scene& scene, const json& root)
{
    SkyboxSettings sk;  // デフォルト
    if (root.contains("skybox"))
    {
        const auto& sj = root["skybox"];
        sk.envMapPath      = sj.value("envMapPath", sk.envMapPath);
        sk.iblIntensity    = sj.value("iblIntensity", sk.iblIntensity);
        sk.skyboxIntensity = sj.value("skyboxIntensity", sk.skyboxIntensity);
        sk.drawSkybox      = sj.value("drawSkybox", sk.drawSkybox);
    }
    scene.GetSkyboxSettings() = sk;
}

// JSON ノードから 1 エンティティを既存シーンに追加生成（Clear しない）
// 失敗時は entt::null
static entt::entity InstantiateEntityJson(Scene& scene, const json& ej,
                                          const std::string& assetsDir)
{
    RegisterCoreComponentSerializers();
    std::string name = ej.value("name", "Unnamed");

    XMFLOAT3 pos   = {0, 0, 0};
    XMFLOAT3 rot   = {0, 0, 0};
    XMFLOAT3 scale = {1, 1, 1};

    if (ej.contains("transform"))
    {
        const auto& tj = ej["transform"];
        if (tj.contains("position")) pos   = DeserializeFloat3(tj["position"]);
        if (tj.contains("rotation")) rot   = DeserializeFloat3(tj["rotation"]);
        if (tj.contains("scale"))    scale = DeserializeFloat3(tj["scale"], {1, 1, 1});
    }

    entt::entity e = entt::null;

    if (ej.contains("gridPlane"))
    {
        f32 size = ej["gridPlane"].value("size", 50.0f);
        e = scene.SpawnPlane(name, pos, size, true).GetHandle();
        OutputDebugStringA(("[Load] SpawnPlane: " + name + "\n").c_str());
    }
    else if (ej.contains("meshRenderer"))
    {
        std::string relPath = ej["meshRenderer"].value("modelPath", "");
        std::string absPath = assetsDir + relPath;
        auto entity = scene.Spawn(name, absPath, pos, rot, scale);
        if (!entity.IsValid())
        {
            OutputDebugStringA(("[Load] FAILED Spawn: " + name + " path=" + absPath + "\n").c_str());
            return entt::null;
        }
        e = entity.GetHandle();
        OutputDebugStringA(("[Load] Spawn: " + name + "\n").c_str());
    }
    else if (ej.contains("primitive"))
    {
        std::string prim = ej["primitive"].get<std::string>();
        if (prim == "sphere")
            e = scene.SpawnSphere(name, pos, 0.5f).GetHandle();
        else if (prim == "plane")
            e = scene.SpawnPlane(name, pos, 50.0f, false).GetHandle();
        else
            e = scene.SpawnBox(name, pos, rot, scale).GetHandle();
        OutputDebugStringA(("[Load] SpawnPrimitive: " + name + " type=" + prim + "\n").c_str());
    }
    else
    {
        // ライトやカメラのみのエンティティ
        auto& reg = scene.GetRegistry();
        e = reg.create();
        reg.emplace<NameTag>(e, NameTag{name});
        OutputDebugStringA(("[Load] CreateBasic: " + name + "\n").c_str());
    }

    if (e != entt::null)
    {
        auto& reg = scene.GetRegistry();

        // Spawn 系が引数を反映しないケースもあるため Transform を確定値で統一
        if (!reg.all_of<Transform>(e))
            reg.emplace<Transform>(e);
        auto& t = reg.get<Transform>(e);
        t.position = pos;
        t.rotation = rot;
        t.scale    = scale;

        {
            // レジストリ登録済みコア部品をまとめて復元（脱 if(ej.contains) 連鎖）。
            // 現状 light×3 / camera / rigidBody / 各 collider を担当。
            RuntimeComponentRegistry::Get().ForEach([&](const RuntimeComponentInfo& info) {
                if (info.deserialize) info.deserialize(reg, e, ej);
            });

            if (ej.contains("gimmick"))
            {
                const auto& gj = ej["gimmick"];
                Gimmick gm;
                gm.kind      = gj.value("kind", 0);
                gm.period    = gj.value("period", 4.0f);
                gm.phase     = gj.value("phase", 0.0f);
                gm.amplitude = gj.value("amplitude", 1.6f);
                gm.threshold = gj.value("threshold", 0.5f);
                gm.solid     = gj.value("solid", true);
                gm.deadly    = gj.value("deadly", false);
                reg.emplace_or_replace<Gimmick>(e, gm);
            }

            if (ej.contains("particleEmitter"))
            {
                const auto& pj = ej["particleEmitter"];
                ParticleEmitter pe;
                pe.kind        = pj.value("kind", 0);
                pe.blend       = pj.value("blend", 0);
                pe.rate        = pj.value("rate", 30.0f);
                pe.playOnStart = pj.value("playOnStart", true);
                pe.looping     = pj.value("looping", true);
                pe.duration    = pj.value("duration", 1.0f);
                if (pj.contains("dir")) pe.dir = DeserializeFloat3(pj["dir"], {0.0f, 1.0f, 0.0f});
                pe.spread   = pj.value("spread", 0.4f);
                pe.speed    = pj.value("speed", 3.0f);
                pe.speedVar = pj.value("speedVar", 0.4f);
                pe.size     = pj.value("size", 0.3f);
                pe.sizeEnd  = pj.value("sizeEnd", 0.0f);
                pe.life     = pj.value("life", 0.8f);
                pe.lifeVar  = pj.value("lifeVar", 0.3f);
                if (pj.contains("color"))    pe.color    = DeserializeFloat3(pj["color"], {1.0f, 0.6f, 0.2f});
                if (pj.contains("colorEnd")) pe.colorEnd = DeserializeFloat3(pj["colorEnd"], {1.0f, 0.12f, 0.05f});
                pe.intensity = pj.value("intensity", 3.0f);
                pe.gravity   = pj.value("gravity", 0.0f);
                pe.drag      = pj.value("drag", 1.0f);
                pe.up        = pj.value("up", 0.0f);
                pe.stretch   = pj.value("stretch", 0.0f);
                reg.emplace_or_replace<ParticleEmitter>(e, pe);
            }

            if (ej.contains("trigger"))
            {
                const auto& tj = ej["trigger"];
                Trigger tr;
                tr.shape = tj.value("shape", 0);
                if (tj.contains("halfExtents")) tr.halfExtents = DeserializeFloat3(tj["halfExtents"], {1.0f, 1.0f, 1.0f});
                tr.radius = tj.value("radius", 1.0f);
                if (tj.contains("offset")) tr.offset = DeserializeFloat3(tj["offset"]);
                tr.filter = tj.value("filter", std::string{});
                tr.once   = tj.value("once", false);
                if (tj.contains("actions") && tj["actions"].is_array())
                {
                    for (const auto& aj : tj["actions"])
                    {
                        TriggerAction a;
                        a.when   = aj.value("when", 0);
                        a.type   = aj.value("type", 0);
                        a.target = aj.value("target", std::string{});
                        a.str    = aj.value("str", std::string{});
                        a.num    = aj.value("num", 0.0);
                        if (aj.contains("vec")) a.vec = DeserializeFloat3(aj["vec"]);
                        tr.actions.push_back(std::move(a));
                    }
                }
                reg.emplace_or_replace<Trigger>(e, std::move(tr));
            }

            // --- Physics ---（RigidBody / 各 Collider の復元は上の ForEach レジストリ走査が担当）

            // ConvexHullCollider: メッシュ頂点から再生成（MeshRendererが必要）
            if (ej.contains("convexHullCollider") && ej["convexHullCollider"].get<bool>())
            {
                if (reg.all_of<MeshRenderer>(e) && reg.all_of<Transform>(e))
                {
                    const auto& mr = reg.get<MeshRenderer>(e);
                    const auto& tf = reg.get<Transform>(e);
                    std::vector<XMFLOAT3> allPoints;
                    for (const auto* mesh : mr.meshes)
                    {
                        if (!mesh) continue;
                        for (const auto& p : mesh->GetPositions())
                            allPoints.push_back({
                                p.x * tf.scale.x,
                                p.y * tf.scale.y,
                                p.z * tf.scale.z });
                    }
                    constexpr size_t kMax = 256;
                    if (allPoints.size() > kMax)
                    {
                        size_t step = allPoints.size() / kMax;
                        std::vector<XMFLOAT3> sampled;
                        for (size_t i = 0; i < allPoints.size() && sampled.size() < kMax; i += step)
                            sampled.push_back(allPoints[i]);
                        allPoints = std::move(sampled);
                    }
                    if (!allPoints.empty())
                    {
                        ConvexHullCollider col;
                        col.points = std::move(allPoints);
                        reg.emplace_or_replace<ConvexHullCollider>(e, std::move(col));
                    }
                }
            }

            // Material PBR パラメータ復元（MeshRenderer のオーバーライド値に設定）
            if (ej.contains("material"))
            {
                const auto& mj = ej["material"];
                if (reg.all_of<MeshRenderer>(e))
                {
                    auto& mr = reg.get<MeshRenderer>(e);
                    if (mj.contains("metallic"))  mr.overrideMetallic  = mj["metallic"].get<f32>();
                    if (mj.contains("roughness")) mr.overrideRoughness = mj["roughness"].get<f32>();
                }
            }

            // 頂点カラー復元（uvTiling より先に。色は m_verticesCache に焼くため）
            if (ej.contains("color") && reg.all_of<MeshRenderer>(e))
            {
                auto c = DeserializeFloat3(ej["color"], {1, 1, 1});
                auto& mr = reg.get<MeshRenderer>(e);
                if (auto* dev = scene.GetDevice())
                    for (auto* mesh : mr.meshes)
                        if (mesh) mesh->SetVertexColor(*dev, c.x, c.y, c.z, 1.0f);
            }

            // UV タイリング復元
            if (ej.contains("uvTiling") && reg.all_of<MeshRenderer>(e))
            {
                const auto& uvj = ej["uvTiling"];
                auto& mr = reg.get<MeshRenderer>(e);
                mr.uvScaleU = uvj.value("u", 1.0f);
                mr.uvScaleV = uvj.value("v", 1.0f);
                if (mr.uvScaleU != 1.0f || mr.uvScaleV != 1.0f)
                {
                    for (auto* mesh : mr.meshes)
                    {
                        if (mesh)
                            mesh->ApplyUVScale(*scene.GetDevice(), mr.uvScaleU, mr.uvScaleV);
                    }
                }
            }

            // LuaScript 復元（env は構築しない。Play 開始時に初期化される）
            if (ej.contains("luaScript"))
            {
                const auto& lsj = ej["luaScript"];
                LuaScript ls;
                ls.scriptPath = lsj.value("scriptPath", "");
                ls.enabled    = lsj.value("enabled", true);

                // 公開プロパティのインスタンス値（型は JSON に書いてあるのでスキーマ不要で復元できる）
                if (lsj.contains("props") && lsj["props"].is_array())
                {
                    for (const auto& pj : lsj["props"])
                    {
                        ScriptProp p;
                        p.name = pj.value("name", "");
                        if (p.name.empty()) continue;
                        p.type = ScriptPropTypeFromStr(pj.value("type", "float"));
                        const json& v = pj.contains("value") ? pj["value"] : json();
                        switch (p.type)
                        {
                        case ScriptPropType::Float:
                        case ScriptPropType::Int:
                            p.num = v.is_number() ? v.get<double>() : 0.0; break;
                        case ScriptPropType::Bool:
                            p.b = v.is_boolean() ? v.get<bool>() : false; break;
                        case ScriptPropType::String:
                        case ScriptPropType::Entity:
                            p.str = v.is_string() ? v.get<std::string>() : std::string{}; break;
                        case ScriptPropType::Vec3:
                            p.vec = DeserializeFloat3(v, {0.0f, 0.0f, 0.0f}); break;
                        case ScriptPropType::Color:
                            p.vec = DeserializeFloat3(v, {1.0f, 1.0f, 1.0f}); break;
                        }
                        ls.props.push_back(std::move(p));
                    }
                }

                if (!ls.scriptPath.empty() && !reg.all_of<LuaScript>(e))
                    reg.emplace<LuaScript>(e, std::move(ls));
            }

            // AudioSource 復元
            if (ej.contains("audioSource"))
            {
                const auto& aj = ej["audioSource"];
                AudioSource as;
                as.clipPath    = aj.value("clipPath", "");
                as.volume      = aj.value("volume", 1.0f);
                as.loop        = aj.value("loop", false);
                as.spatial     = aj.value("spatial", true);
                as.playOnStart = aj.value("playOnStart", true);
                as.minDistance = aj.value("minDistance", 1.0f);
                as.maxDistance = aj.value("maxDistance", 30.0f);
                reg.emplace_or_replace<AudioSource>(e, std::move(as));
            }
        }
    }

    return e;
}

// JSON ノードを既存シーンに展開（共通処理）
static bool ApplySceneJson(Scene& scene, const json& root, const std::string& assetsDir)
{
    scene.Clear();

    // ポストプロセス設定（entities が無くても復元する）
    LoadPostSettings(scene, root);
    // スカイボックス / IBL 設定
    LoadSkyboxSettings(scene, root);

    if (!root.contains("entities") || !root["entities"].is_array())
    {
        Logger::Warn("Scene JSON has no entities array");
        return true;
    }

    // 1パス目: 生成（配列インデックス → entity の対応を保持）
    std::vector<entt::entity> created;
    created.reserve(root["entities"].size());
    for (const auto& ej : root["entities"])
        created.push_back(InstantiateEntityJson(scene, ej, assetsDir));

    // 2パス目: 親子関係の復元
    auto& reg = scene.GetRegistry();
    size_t idx = 0;
    for (const auto& ej : root["entities"])
    {
        entt::entity e = created[idx++];
        if (e == entt::null || !ej.contains("parent")) continue;

        int parentIdx = ej["parent"].get<int>();
        if (parentIdx < 0 || parentIdx >= static_cast<int>(created.size())) continue;

        entt::entity parent = created[static_cast<size_t>(parentIdx)];
        if (parent == entt::null || parent == e) continue;

        if (reg.all_of<Transform>(e))
            reg.get<Transform>(e).parent = parent;
    }

    return true;
}

bool SceneSerializer::Save(const Scene& scene, const std::string& filePath,
                           const std::string& assetsDir)
{
    namespace fs = std::filesystem;

    fs::path dir = fs::path(filePath).parent_path();
    if (!dir.empty())
        fs::create_directories(dir);

    json root = BuildSceneJson(scene, assetsDir);

    std::ofstream ofs(filePath);
    if (!ofs.is_open())
    {
        Logger::Error("Failed to open file for writing: {}", filePath);
        return false;
    }

    ofs << root.dump(2);
    ofs.close();
    Logger::Info("Scene saved ({} entities): {}",
                 root["entities"].size(), filePath);
    return true;
}

bool SceneSerializer::Load(Scene& scene, const std::string& filePath,
                           const std::string& assetsDir)
{
    std::ifstream ifs(filePath);
    if (!ifs.is_open())
    {
        Logger::Error("Failed to open scene file: {}", filePath);
        return false;
    }

    json root;
    try
    {
        ifs >> root;
    }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON parse error: {}", e.what());
        return false;
    }
    ifs.close();

    bool ok = ApplySceneJson(scene, root, assetsDir);
    if (ok)
        Logger::Info("Scene loaded ({} entities): {}",
                     root.contains("entities") ? root["entities"].size() : 0, filePath);
    return ok;
}

std::string SceneSerializer::SaveToString(const Scene& scene, const std::string& assetsDir)
{
    json root = BuildSceneJson(scene, assetsDir);
    return root.dump();
}

bool SceneSerializer::LoadFromString(Scene& scene, const std::string& jsonStr,
                                     const std::string& assetsDir)
{
    json root;
    try
    {
        root = json::parse(jsonStr);
    }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON parse error (snapshot): {}", e.what());
        return false;
    }

    bool ok = ApplySceneJson(scene, root, assetsDir);
    if (ok)
        Logger::Info("Scene restored from snapshot ({} entities)",
                     root.contains("entities") ? root["entities"].size() : 0);
    return ok;
}

bool SceneSerializer::ApplyOverrides(Scene& scene, const std::string& filePath,
                                     const std::string& /*assetsDir*/)
{
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) return false;

    json root;
    try { root = json::parse(ifs); }
    catch (...) { return false; }
    ifs.close();

    if (!root.contains("entities") || !root["entities"].is_array())
        return false;

    auto& reg = scene.GetRegistry();

    for (const auto& ej : root["entities"])
    {
        std::string name = ej.value("name", "");
        if (name.empty()) continue;

        // 名前でエンティティを検索
        Entity entity = scene.FindEntity(name);
        if (!entity.IsValid()) continue;
        auto e = entity.GetHandle();

        // Transform 上書き
        if (ej.contains("transform") && reg.all_of<Transform>(e))
        {
            auto& t = reg.get<Transform>(e);
            const auto& tj = ej["transform"];
            t.position = DeserializeFloat3(tj["position"], t.position);
            t.rotation = DeserializeFloat3(tj["rotation"], t.rotation);
            t.scale    = DeserializeFloat3(tj["scale"],    t.scale);
        }

        // Material PBR オーバーライド
        if (ej.contains("material") && reg.all_of<MeshRenderer>(e))
        {
            const auto& mj = ej["material"];
            auto& mr = reg.get<MeshRenderer>(e);
            if (mj.contains("metallic"))  mr.overrideMetallic  = mj["metallic"].get<f32>();
            if (mj.contains("roughness")) mr.overrideRoughness = mj["roughness"].get<f32>();
        }

        // RigidBody
        if (ej.contains("rigidBody") && reg.all_of<RigidBody>(e))
        {
            auto& rb = reg.get<RigidBody>(e);
            const auto& rj = ej["rigidBody"];
            rb.motionType     = static_cast<MotionType>(rj.value("motionType", 2));
            rb.mass           = rj.value("mass", 1.0f);
            rb.friction       = rj.value("friction", 0.3f);
            rb.restitution    = rj.value("restitution", 0.4f);
            rb.linearDamping  = rj.value("linearDamping", 0.02f);
            rb.angularDamping = rj.value("angularDamping", 0.01f);
            rb.useGravity     = rj.value("useGravity", true);
        }
    }

    Logger::Info("Scene overrides applied: {}", filePath);
    return true;
}

std::string SceneSerializer::SerializeEntity(const Scene& scene, entt::entity e,
                                             const std::string& assetsDir)
{
    const auto& reg = scene.GetRegistry();
    if (!reg.valid(e) || !reg.all_of<NameTag>(e) || !reg.all_of<Transform>(e))
        return {};
    return SerializeEntityJson(reg, e, assetsDir).dump();
}

static std::string MakeUniqueName(const Scene& scene, const std::string& base);

entt::entity SceneSerializer::InstantiateEntity(Scene& scene, const std::string& jsonStr,
                                                const std::string& assetsDir)
{
    json ej;
    try { ej = json::parse(jsonStr); }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON parse error (entity): {}", e.what());
        return entt::null;
    }
    ej["name"] = MakeUniqueName(scene, ej.value("name", "Unnamed"));
    return InstantiateEntityJson(scene, ej, assetsDir);
}

// "Box" → "Box (1)" → "Box (2)" のように重複しない名前を作る
static std::string MakeUniqueName(const Scene& scene, const std::string& base)
{
    const auto& reg = scene.GetRegistry();
    auto exists = [&](const std::string& n)
    {
        for (auto [e, tag] : reg.view<const NameTag>().each())
            if (tag.name == n) return true;
        return false;
    };

    if (!exists(base)) return base;

    // 末尾の " (N)" を除去してベース名にする
    std::string stem = base;
    auto p = stem.rfind(" (");
    if (p != std::string::npos && stem.back() == ')')
        stem = stem.substr(0, p);

    for (int i = 1; i < 1000; ++i)
    {
        std::string candidate = stem + " (" + std::to_string(i) + ")";
        if (!exists(candidate)) return candidate;
    }
    return base;
}

entt::entity SceneSerializer::DuplicateEntity(Scene& scene, entt::entity src,
                                              const std::string& assetsDir)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(src) || !reg.all_of<NameTag>(src) || !reg.all_of<Transform>(src))
        return entt::null;

    json ej = SerializeEntityJson(reg, src, assetsDir);
    ej["name"] = MakeUniqueName(scene, reg.get<NameTag>(src).name);

    entt::entity copy = InstantiateEntityJson(scene, ej, assetsDir);
    if (copy == entt::null) return entt::null;

    // 親は元エンティティと同じにする
    reg.get<Transform>(copy).parent = reg.get<Transform>(src).parent;
    return copy;
}

// ── Prefab / サブツリー ──
// root とその全子孫を 1 つの JSON（シーンと同形式 + parent をローカル index 参照）に直列化する。
// root の親（サブツリー外）は含めない＝プレハブは自己完結する。
std::string SceneSerializer::SerializeSubtree(const Scene& scene, entt::entity root,
                                              const std::string& assetsDir)
{
    const auto& reg = scene.GetRegistry();
    if (!reg.valid(root) || !reg.all_of<NameTag>(root) || !reg.all_of<Transform>(root))
        return {};

    // root を先頭に、BFS で root + 子孫を列挙
    std::vector<entt::entity> order;
    std::unordered_map<entt::entity, int> indexOf;
    order.push_back(root);
    indexOf[root] = 0;
    for (size_t head = 0; head < order.size(); ++head)
    {
        entt::entity cur = order[head];
        for (auto [child, tf] : reg.view<const Transform>().each())
        {
            if (tf.parent == cur && indexOf.find(child) == indexOf.end())
            {
                indexOf[child] = static_cast<int>(order.size());
                order.push_back(child);
            }
        }
    }

    json root_j;
    root_j["version"] = 1;
    root_j["prefab"]  = true;
    root_j["entities"] = json::array();
    for (auto e : order)
    {
        json ej = SerializeEntityJson(reg, e, assetsDir);
        const auto& tf = reg.get<Transform>(e);
        if (e != root && tf.parent != entt::null)
        {
            auto it = indexOf.find(tf.parent);
            if (it != indexOf.end())
                ej["parent"] = it->second;
        }
        root_j["entities"].push_back(std::move(ej));
    }
    return root_j.dump(2);
}

// サブツリー JSON を既存シーンへ展開（Clear しない）。戻り値 = root エンティティ。
// outAll に生成した全エンティティ（root 先頭）を返す（Undo 用）。
entt::entity SceneSerializer::InstantiateSubtree(Scene& scene, const std::string& jsonStr,
                                                 const std::string& assetsDir,
                                                 std::vector<entt::entity>* outAll)
{
    json root;
    try { root = json::parse(jsonStr); }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON parse error (subtree): {}", e.what());
        return entt::null;
    }
    if (!root.contains("entities") || !root["entities"].is_array() || root["entities"].empty())
        return entt::null;

    auto& reg = scene.GetRegistry();

    // 1パス目: 生成（名前は重複しないよう連番付与）
    std::vector<entt::entity> created;
    created.reserve(root["entities"].size());
    for (const auto& ej : root["entities"])
    {
        json copy = ej;
        copy["name"] = MakeUniqueName(scene, ej.value("name", std::string("Unnamed")));
        created.push_back(InstantiateEntityJson(scene, copy, assetsDir));
    }

    // 2パス目: 親子関係の復元（root はサブツリー外の親を持たない）
    size_t idx = 0;
    for (const auto& ej : root["entities"])
    {
        entt::entity e = created[idx++];
        if (e == entt::null || !ej.contains("parent")) continue;
        int p = ej["parent"].get<int>();
        if (p < 0 || p >= static_cast<int>(created.size())) continue;
        entt::entity parent = created[static_cast<size_t>(p)];
        if (parent != entt::null && parent != e && reg.all_of<Transform>(e))
            reg.get<Transform>(e).parent = parent;
    }

    if (outAll) *outAll = created;
    return created.empty() ? entt::null : created[0];
}

bool SceneSerializer::SavePrefab(const Scene& scene, entt::entity root,
                                 const std::string& filePath, const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    std::string s = SerializeSubtree(scene, root, assetsDir);
    if (s.empty()) return false;

    fs::path dir = fs::path(filePath).parent_path();
    if (!dir.empty()) fs::create_directories(dir);

    std::ofstream ofs(filePath);
    if (!ofs.is_open())
    {
        Logger::Error("Failed to write prefab: {}", filePath);
        return false;
    }
    ofs << s;
    Logger::Info("Prefab saved: {}", filePath);
    return true;
}

entt::entity SceneSerializer::InstantiatePrefab(Scene& scene, const std::string& filePath,
                                                const std::string& assetsDir,
                                                std::vector<entt::entity>* outAll)
{
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open())
    {
        Logger::Error("Failed to open prefab: {}", filePath);
        return entt::null;
    }
    std::stringstream ss; ss << ifs.rdbuf();
    return InstantiateSubtree(scene, ss.str(), assetsDir, outAll);
}

} // namespace dx12e
