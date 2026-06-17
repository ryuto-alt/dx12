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
#include <unordered_map>

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

// 単一エンティティを JSON ノードに直列化（parent は含まない）
static json SerializeEntityJson(const entt::registry& reg, entt::entity entity,
                                const std::string& assetsDir)
{
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
                {"intensity", dl.intensity},
                {"ambient",   dl.ambient}
            };
        }

        if (reg.all_of<SpotLight>(entity))
        {
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

// JSON ノードから 1 エンティティを既存シーンに追加生成（Clear しない）
// 失敗時は entt::null
static entt::entity InstantiateEntityJson(Scene& scene, const json& ej,
                                          const std::string& assetsDir)
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
                dl.ambient = dlj.value("ambient", 0.25f);
                if (!reg.all_of<DirectionalLight>(e))
                    reg.emplace<DirectionalLight>(e, dl);
            }

            if (ej.contains("spotLight"))
            {
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

            if (ej.contains("camera"))
            {
                const auto& cj = ej["camera"];
                CameraComponent cam;
                if (cj.contains("fovDegrees")) cam.fovDegrees = cj["fovDegrees"].get<f32>();
                if (cj.contains("nearClip"))   cam.nearClip   = cj["nearClip"].get<f32>();
                if (cj.contains("farClip"))    cam.farClip    = cj["farClip"].get<f32>();
                if (cj.contains("isActive"))   cam.isActive   = cj["isActive"].get<bool>();
                // アクティブカメラの重複防止（複製時など）
                if (cam.isActive)
                {
                    for (auto [oe, oc] : reg.view<const CameraComponent>().each())
                    {
                        if (oe != e && oc.isActive) { cam.isActive = false; break; }
                    }
                }
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

} // namespace dx12e
