#include "scene/SceneSerializer.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <Windows.h>
#include <fstream>
#include <filesystem>

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

bool SceneSerializer::Save(const Scene& scene, const std::string& filePath,
                           const std::string& assetsDir)
{
    namespace fs = std::filesystem;

    // ディレクトリが無ければ作成
    fs::path dir = fs::path(filePath).parent_path();
    if (!dir.empty())
        fs::create_directories(dir);

    json root;
    root["version"] = 1;
    root["entities"] = json::array();

    const auto& reg = scene.GetRegistry();

    auto view = reg.view<const NameTag, const Transform>();
    for (auto [entity, tag, transform] : view.each())
    {
        json ej;
        ej["name"] = tag.name;
        ej["transform"] = {
            {"position", SerializeFloat3(transform.position)},
            {"rotation", SerializeFloat3(transform.rotation)},
            {"scale",    SerializeFloat3(transform.scale)}
        };

        if (reg.all_of<MeshRenderer>(entity))
        {
            const auto& mr = reg.get<MeshRenderer>(entity);
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
        }

        if (reg.all_of<GridPlane>(entity))
        {
            ej["gridPlane"] = {{"size", 50.0f}};
        }

        if (reg.all_of<PointLight>(entity))
        {
            const auto& pl = reg.get<PointLight>(entity);
            ej["pointLight"] = {
                {"color",     SerializeFloat3(pl.color)},
                {"intensity", pl.intensity},
                {"range",     pl.range}
            };
        }

        if (reg.all_of<DirectionalLight>(entity))
        {
            const auto& dl = reg.get<DirectionalLight>(entity);
            ej["directionalLight"] = {
                {"direction", SerializeFloat3(dl.direction)},
                {"color",     SerializeFloat3(dl.color)},
                {"intensity", dl.intensity}
            };
        }

        if (reg.all_of<CameraComponent>(entity))
        {
            const auto& cam = reg.get<CameraComponent>(entity);
            ej["camera"] = {
                {"fovDegrees", cam.fovDegrees},
                {"nearClip",   cam.nearClip},
                {"farClip",    cam.farClip},
                {"isActive",   cam.isActive}
            };
        }

        // --- Physics ---
        if (reg.all_of<RigidBody>(entity))
        {
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

        if (reg.all_of<BoxCollider>(entity))
        {
            const auto& col = reg.get<BoxCollider>(entity);
            ej["boxCollider"] = {
                {"halfExtents", SerializeFloat3(col.halfExtents)},
                {"offset",      SerializeFloat3(col.offset)}
            };
        }

        if (reg.all_of<SphereCollider>(entity))
        {
            const auto& col = reg.get<SphereCollider>(entity);
            ej["sphereCollider"] = {
                {"radius", col.radius},
                {"offset", SerializeFloat3(col.offset)}
            };
        }

        if (reg.all_of<CapsuleCollider>(entity))
        {
            const auto& col = reg.get<CapsuleCollider>(entity);
            ej["capsuleCollider"] = {
                {"radius",     col.radius},
                {"halfHeight", col.halfHeight},
                {"offset",     SerializeFloat3(col.offset)}
            };
        }

        // ConvexHullCollider: autoCollider フラグだけ保存（頂点は起動時にメッシュから再生成）
        if (reg.all_of<ConvexHullCollider>(entity))
        {
            ej["convexHullCollider"] = true;
        }

        root["entities"].push_back(ej);
    }

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

    scene.Clear();

    if (!root.contains("entities") || !root["entities"].is_array())
    {
        Logger::Warn("Scene file has no entities array");
        return true;
    }

    for (const auto& ej : root["entities"])
    {
        std::string name = ej.value("name", "Unnamed");

        XMFLOAT3 pos   = {0, 0, 0};
        XMFLOAT3 rot   = {0, 0, 0};
        XMFLOAT3 scale = {1, 1, 1};

        if (ej.contains("transform"))
        {
            const auto& tj = ej["transform"];
            if (tj.contains("position")) pos   = DeserializeFloat3(tj["position"]);
            if (tj.contains("rotation")) rot   = DeserializeFloat3(tj["rotation"]);
            if (tj.contains("scale"))    scale = DeserializeFloat3(tj["scale"]);
        }

        if (ej.contains("gridPlane"))
        {
            f32 size = ej["gridPlane"].value("size", 50.0f);
            scene.SpawnPlane(name, pos, size, true);
            OutputDebugStringA(("[Load] SpawnPlane: " + name + "\n").c_str());
        }
        else if (ej.contains("meshRenderer"))
        {
            std::string relPath = ej["meshRenderer"].value("modelPath", "");
            std::string absPath = assetsDir + relPath;
            auto entity = scene.Spawn(name, absPath, pos, rot, scale);
            if (!entity.IsValid())
                OutputDebugStringA(("[Load] FAILED Spawn: " + name + " path=" + absPath + "\n").c_str());
            else
                OutputDebugStringA(("[Load] Spawn: " + name + "\n").c_str());
        }
        else if (ej.contains("primitive"))
        {
            std::string prim = ej["primitive"].get<std::string>();
            if (prim == "sphere")
                scene.SpawnSphere(name, pos, 0.5f);
            else
                scene.SpawnBox(name, pos, rot, scale);
            OutputDebugStringA(("[Load] SpawnPrimitive: " + name + " type=" + prim + "\n").c_str());
        }
        else
        {
            // ライトやカメラのみのエンティティ
            auto& reg = scene.GetRegistry();
            auto e = reg.create();
            reg.emplace<NameTag>(e, NameTag{name});
            reg.emplace<Transform>(e, Transform{pos, rot, scale});
            OutputDebugStringA(("[Load] CreateBasic: " + name + "\n").c_str());
        }

        // 追加コンポーネント（最後に追加されたエンティティに付与）
        // FindEntity で名前検索して取得
        auto found = scene.FindEntity(name);
        if (found.IsValid())
        {
            auto& reg = scene.GetRegistry();
            auto e = found.GetHandle();

            if (ej.contains("pointLight"))
            {
                const auto& plj = ej["pointLight"];
                PointLight pl;
                if (plj.contains("color"))     pl.color     = DeserializeFloat3(plj["color"], {1,1,1});
                if (plj.contains("intensity")) pl.intensity = plj["intensity"].get<f32>();
                if (plj.contains("range"))     pl.range     = plj["range"].get<f32>();
                if (!reg.all_of<PointLight>(e))
                    reg.emplace<PointLight>(e, pl);
            }

            if (ej.contains("directionalLight"))
            {
                const auto& dlj = ej["directionalLight"];
                DirectionalLight dl;
                if (dlj.contains("direction")) dl.direction = DeserializeFloat3(dlj["direction"], {0,-1,0});
                if (dlj.contains("color"))     dl.color     = DeserializeFloat3(dlj["color"], {1,1,1});
                if (dlj.contains("intensity")) dl.intensity = dlj["intensity"].get<f32>();
                if (!reg.all_of<DirectionalLight>(e))
                    reg.emplace<DirectionalLight>(e, dl);
            }

            if (ej.contains("camera"))
            {
                const auto& cj = ej["camera"];
                CameraComponent cam;
                if (cj.contains("fovDegrees")) cam.fovDegrees = cj["fovDegrees"].get<f32>();
                if (cj.contains("nearClip"))   cam.nearClip   = cj["nearClip"].get<f32>();
                if (cj.contains("farClip"))    cam.farClip    = cj["farClip"].get<f32>();
                if (cj.contains("isActive"))   cam.isActive   = cj["isActive"].get<bool>();
                if (!reg.all_of<CameraComponent>(e))
                    reg.emplace<CameraComponent>(e, cam);
            }

            // --- Physics ---
            if (ej.contains("rigidBody"))
            {
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

            if (ej.contains("boxCollider"))
            {
                const auto& cj = ej["boxCollider"];
                BoxCollider col;
                if (cj.contains("halfExtents")) col.halfExtents = DeserializeFloat3(cj["halfExtents"], {0.5f, 0.5f, 0.5f});
                if (cj.contains("offset"))      col.offset      = DeserializeFloat3(cj["offset"]);
                reg.emplace_or_replace<BoxCollider>(e, col);
            }

            if (ej.contains("sphereCollider"))
            {
                const auto& cj = ej["sphereCollider"];
                SphereCollider col;
                if (cj.contains("radius")) col.radius = cj["radius"].get<f32>();
                if (cj.contains("offset")) col.offset = DeserializeFloat3(cj["offset"]);
                reg.emplace_or_replace<SphereCollider>(e, col);
            }

            if (ej.contains("capsuleCollider"))
            {
                const auto& cj = ej["capsuleCollider"];
                CapsuleCollider col;
                if (cj.contains("radius"))     col.radius     = cj["radius"].get<f32>();
                if (cj.contains("halfHeight")) col.halfHeight = cj["halfHeight"].get<f32>();
                if (cj.contains("offset"))     col.offset     = DeserializeFloat3(cj["offset"]);
                reg.emplace_or_replace<CapsuleCollider>(e, col);
            }

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
        }
    }

    Logger::Info("Scene loaded ({} entities): {}",
                 root["entities"].size(), filePath);
    return true;
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

} // namespace dx12e
