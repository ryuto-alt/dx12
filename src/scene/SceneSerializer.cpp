#include "scene/SceneSerializer.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "core/Logger.h"

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

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
            ej["meshRenderer"] = {
                {"modelPath", MakeRelative(mr.modelPath, assetsDir)}
            };
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
        }
        else if (ej.contains("meshRenderer"))
        {
            std::string relPath = ej["meshRenderer"].value("modelPath", "");
            std::string absPath = assetsDir + relPath;
            scene.Spawn(name, absPath, pos, rot, scale);
        }
        else
        {
            // ライトやカメラのみのエンティティ
            auto& reg = scene.GetRegistry();
            auto e = reg.create();
            reg.emplace<NameTag>(e, NameTag{name});
            reg.emplace<Transform>(e, Transform{pos, rot, scale});
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
        }
    }

    Logger::Info("Scene loaded ({} entities): {}",
                 root["entities"].size(), filePath);
    return true;
}

} // namespace dx12e
