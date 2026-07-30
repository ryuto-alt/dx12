#include "scene/SceneSerializer.h"
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "ecs/ComponentMeta.h"             // entt::meta フィールド反映（シリアライズの単一ソース）
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "engine/ecs/ComponentRegistry.h"  // Phase 1: コア部品の直列化をレジストリ走査へ
#include "terrain/TerrainIO.h"             // 複製時に .hf のパスを振り直す
#include "terrain/SculptIO.h"              // 複製時に .smsh のパスを振り直す

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <random>    // エンティティ GUID の生成
#include <cstdio>     // GUID の hex 整形

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

// ===== entt::meta 反射ベースの汎用シリアライズ（ComponentMeta が単一ソース）=====
// ComponentMeta.cpp に登録された「永続フィールドだけ」を JSON へ往復させる。
// 新規コンポーネントの追加手順:
//   ① Components.h に struct
//   ② ComponentMeta.cpp に meta_factory 登録（永続フィールドのみ。_ 付きランタイム状態は登録しない）
//   ③ 下の RegisterCoreComponentSerializers に MakeReflectedInfo 1 行
// これで保存/復元が揃う（従来の手書き serialize/deserialize ラムダは不要）。

// meta_any の値 → JSON。未対応型は null を返す（呼び出し側で警告スキップ）。
static json MetaFieldToJson(const entt::meta_any& v)
{
    const auto t = v.type();
    if (t == entt::resolve<f32>())         return v.cast<f32>();
    if (t == entt::resolve<i32>())         return v.cast<i32>();
    if (t == entt::resolve<u32>())         return v.cast<u32>();
    if (t == entt::resolve<bool>())        return v.cast<bool>();
    if (t == entt::resolve<std::string>()) return v.cast<std::string>();
    if (t == entt::resolve<XMFLOAT3>())    return SerializeFloat3(v.cast<XMFLOAT3>());
    if (t == entt::resolve<XMFLOAT2>())
    {
        const auto f = v.cast<XMFLOAT2>();
        return json::array({f.x, f.y});
    }
    if (t == entt::resolve<XMFLOAT4>())
    {
        const auto f = v.cast<XMFLOAT4>();
        return json::array({f.x, f.y, f.z, f.w});
    }
    if (t.is_enum())
    {
        entt::meta_any c = v;   // allow_cast は自己変換なのでコピーに対して行う
        if (c.allow_cast<int>())
            return c.cast<int>();
    }
    return json{};
}

// JSON → meta フィールド設定。型不一致や未対応型は false（フィールド単位でスキップ）。
static bool JsonToMetaField(entt::meta_any& obj, const entt::meta_data& data, const json& j)
{
    const auto t = data.type();
    try
    {
        if (t == entt::resolve<f32>())         return data.set(obj, j.get<f32>());
        if (t == entt::resolve<i32>())         return data.set(obj, j.get<i32>());
        if (t == entt::resolve<u32>())         return data.set(obj, j.get<u32>());
        if (t == entt::resolve<bool>())        return data.set(obj, j.get<bool>());
        if (t == entt::resolve<std::string>()) return data.set(obj, j.get<std::string>());
        if (t == entt::resolve<XMFLOAT3>())    return data.set(obj, DeserializeFloat3(j));
        if (t == entt::resolve<XMFLOAT2>())
        {
            if (!j.is_array() || j.size() < 2) return false;
            return data.set(obj, XMFLOAT2{j[0].get<f32>(), j[1].get<f32>()});
        }
        if (t == entt::resolve<XMFLOAT4>())
        {
            if (!j.is_array() || j.size() < 4) return false;
            return data.set(obj, XMFLOAT4{j[0].get<f32>(), j[1].get<f32>(),
                                          j[2].get<f32>(), j[3].get<f32>()});
        }
        if (t.is_enum())
        {
            entt::meta_any v{j.get<int>()};
            if (!v.allow_cast(t)) return false;
            return data.set(obj, v);
        }
    }
    catch (const json::exception&)
    {
        return false;   // 型不一致はフィールド単位でスキップ（シーン全体を巻き込まない）
    }
    return false;
}

// T の RuntimeComponentInfo を反射から生成する。
// replaceExisting: true = emplace_or_replace / false = 既に持っていたら何もしない
// （ライト/カメラは従来 emplace-if-absent だったので false、それ以外は true で挙動を維持）。
template <typename T>
static RuntimeComponentInfo MakeReflectedInfo(const char* typeName, const char* jsonKey,
                                              bool replaceExisting)
{
    const std::string key = jsonKey;
    RuntimeComponentInfo info;
    info.typeName = typeName;
    info.source   = ComponentSource::Core;

    info.serialize = [key](const entt::registry& reg, entt::entity e, json& ej)
    {
        if (!reg.all_of<T>(e)) return;
        T comp = reg.get<T>(e);   // 値コピー（const 越しの meta get を避ける）
        entt::meta_any handle = entt::forward_as_meta(comp);
        json out = json::object();
        for (auto&& [id, data] : entt::resolve<T>().data())
        {
            (void)id;
            const char* name = data.name();
            if (!name) continue;
            json jv = MetaFieldToJson(data.get(handle));
            if (jv.is_null())
            {
                Logger::Warn("MetaSerialize: 未対応のフィールド型です（{}.{}）", key, name);
                continue;
            }
            out[name] = std::move(jv);
        }
        ej[key] = std::move(out);
    };

    info.deserialize = [key, replaceExisting](entt::registry& reg, entt::entity e, const json& ej)
    {
        if (!ej.contains(key) || !ej[key].is_object()) return;
        const json& cj = ej[key];

        T comp{};   // 既定値から開始し、JSON に存在するフィールドだけ上書き（後方互換）
        entt::meta_any handle = entt::forward_as_meta(comp);
        for (auto&& [id, data] : entt::resolve<T>().data())
        {
            (void)id;
            const char* name = data.name();
            if (!name || !cj.contains(name)) continue;
            if (!JsonToMetaField(handle, data, cj[name]))
                Logger::Warn("MetaDeserialize: フィールドをスキップしました（{}.{}）", key, name);
        }

        if (replaceExisting)
            reg.emplace_or_replace<T>(e, std::move(comp));
        else if (!reg.all_of<T>(e))
            reg.emplace<T>(e, std::move(comp));
    };

    return info;
}

// コア部品の直列化/復元を RuntimeComponentRegistry へ登録する。
// 純フィールド型は entt::meta 反射（MakeReflectedInfo）で自動化し、独自フォーマットを
// 持つ型（Tag=配列直値 / DataComponent=型付きマップ）だけ手書きを残す。
// serialize_roundtrip_test が新旧の同値性を担保する。
// 残りのレガシーif連鎖（gimmick/audioSource/particleEmitter/trigger/convexHull/luaScript）は順次移設する。
static void RegisterCoreComponentSerializers()
{
    static bool done = false;
    if (done) return;
    done = true;

    // フィールド定義の単一ソース = ComponentMeta（entt::meta）。ここで確実に登録しておく
    RegisterCoreComponentMeta();

    auto& R = RuntimeComponentRegistry::Get();

    // ---- 反射ベース（純フィールド型）----
    // ライトは「既にあれば上書きしない」(emplace-if-absent) が従来挙動なので replaceExisting=false
    R.Register(MakeReflectedInfo<PointLight>("PointLight", "pointLight", false));
    R.Register(MakeReflectedInfo<DirectionalLight>("DirectionalLight", "directionalLight", false));
    R.Register(MakeReflectedInfo<SpotLight>("SpotLight", "spotLight", false));
    R.Register(MakeReflectedInfo<RigidBody>("RigidBody", "rigidBody", true));
    R.Register(MakeReflectedInfo<BoxCollider>("BoxCollider", "boxCollider", true));
    R.Register(MakeReflectedInfo<SphereCollider>("SphereCollider", "sphereCollider", true));
    R.Register(MakeReflectedInfo<CapsuleCollider>("CapsuleCollider", "capsuleCollider", true));
    R.Register(MakeReflectedInfo<CharacterController>("CharacterController", "characterController", true));
    R.Register(MakeReflectedInfo<Sprite2D>("Sprite2D", "sprite2d", true));
    R.Register(MakeReflectedInfo<TrailRenderer>("TrailRenderer", "trailRenderer", true));
    R.Register(MakeReflectedInfo<DecalComponent>("DecalComponent", "decal", true));
    R.Register(MakeReflectedInfo<NetworkIdentity>("NetworkIdentity", "networkIdentity", true));
    R.Register(MakeReflectedInfo<NetworkTransform>("NetworkTransform", "networkTransform", true));
    // ゲーム内UI（retained-mode）。ランタイム状態（_付き）は ComponentMeta 未登録なので保存されない
    R.Register(MakeReflectedInfo<UICanvas>("UICanvas", "uiCanvas", true));
    R.Register(MakeReflectedInfo<UIRect>("UIRect", "uiRect", true));
    R.Register(MakeReflectedInfo<UIImage>("UIImage", "uiImage", true));
    R.Register(MakeReflectedInfo<UIText>("UIText", "uiText", true));
    R.Register(MakeReflectedInfo<UIButton>("UIButton", "uiButton", true));
    R.Register(MakeReflectedInfo<UISlider>("UISlider", "uiSlider", true));
    R.Register(MakeReflectedInfo<UIToggle>("UIToggle", "uiToggle", true));
    R.Register(MakeReflectedInfo<UIScrollView>("UIScrollView", "uiScrollView", true));
    R.Register(MakeReflectedInfo<UILayout>("UILayout", "uiLayout", true));
    R.Register(MakeReflectedInfo<UIAnimator>("UIAnimator", "uiAnimator", true));
    // タイムライン製クリップ(.uianim) / スプライトシート(.spranim) の再生器
    R.Register(MakeReflectedInfo<UIAnimPlayer>("UIAnimPlayer", "uiAnimPlayer", true));
    R.Register(MakeReflectedInfo<SpriteAnimator>("SpriteAnimator", "spriteAnimator", true));
    // スケルタルアニメのステートマシン(.animfsm)。構造はアセット側、ここはパスとパラメータだけ
    R.Register(MakeReflectedInfo<AnimatorController>("AnimatorController", "animatorController", true));
    // フット IK（接地補正）。ボーン名が空なら一般的な命名から自動推定する
    R.Register(MakeReflectedInfo<FootIK>("FootIK", "footIK", true));
    // プレハブインスタンスの紐付け。.prefab 側へ書き出す時だけ StripPrefabLinks で落とす
    R.Register(MakeReflectedInfo<PrefabLink>("PrefabLink", "prefabLink", true));

    // ---- カメラ: フィールドは反射で復元し、新規追加時だけアクティブカメラの重複防止を行う ----
    {
        auto info = MakeReflectedInfo<CameraComponent>("CameraComponent", "camera", false);
        auto generic = info.deserialize;
        info.deserialize = [generic](entt::registry& reg, entt::entity e, const json& ej)
        {
            const bool had = reg.all_of<CameraComponent>(e);
            generic(reg, e, ej);
            if (had) return;   // 既存カメラには触らない（従来挙動）
            auto* cam = reg.try_get<CameraComponent>(e);
            if (!cam || !cam->isActive) return;
            // アクティブカメラの重複防止（複製時など）
            for (auto [oe, oc] : reg.view<const CameraComponent>().each())
            {
                if (oe != e && oc.isActive) { cam->isActive = false; break; }
            }
        };
        R.Register(std::move(info));
    }

    // ---- 独自フォーマット型（配列直値/型付きマップ）は手書きを維持 ----

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
            // 地形メッシュ / スカルプトメッシュは CPU 生成で modelPath は内部マーカー
            //（"__terrain__" / "__sculpt__"）。下の "terrain" / "sculpt" ブロックから作り直すので
            // meshRenderer/primitive としては書かない（書くと復元時に Box になってしまう）。
            // シェーダー/マテリアル割当は下でそのまま保存される。
            const bool isGeneratedMesh = reg.all_of<Terrain>(entity) || reg.all_of<SculptMesh>(entity);
            if (isGeneratedMesh)
            {
                // 何も書かない（"terrain" / "sculpt" ブロックが担当）
            }
            // プリミティブマーカー（"__primitive_box__" 等）は種別として保存
            else if (mr.modelPath.rfind("__primitive_", 0) == 0)
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

            // カスタムシェーダー割当（プリミティブ/モデル問わず。空なら既定 Forward）
            if (!mr.shaderPath.empty())
            {
                ej["shader"] = mr.shaderPath;
                if (mr.shaderAlphaBlend)
                    ej["shaderAlphaBlend"] = true;
                if (mr.effectValue != 0.0f)
                    ej["shaderEffectValue"] = mr.effectValue;
                if (mr.shaderParams.x != 0.0f || mr.shaderParams.y != 0.0f
                    || mr.shaderParams.z != 0.0f || mr.shaderParams.w != 0.0f)
                    ej["shaderParams"] = {mr.shaderParams.x, mr.shaderParams.y,
                                          mr.shaderParams.z, mr.shaderParams.w};
            }

            // マテリアルテクスチャ上書き（アセットブラウザからテクスチャをD&Dして割当、サブメッシュ単位）。
            // Material 自体は同一モデルの全インスタンスで共有されるため、上書きは MeshRenderer 側に
            // インスタンス単位で保持している(Application::EnsureMaterialOverrideSrv 参照)。
            {
                size_t maxLen = (std::max)({mr.overrideAlbedoTexture.size(),
                                             mr.overrideNormalTexture.size(),
                                             mr.overrideMetalRoughnessTexture.size()});
                bool anyOverride = false;
                json overridesJson = json::array();
                for (size_t i = 0; i < maxLen; ++i)
                {
                    json entry = json::object();
                    const std::string& a = MeshRenderer::SafeGetOverride(mr.overrideAlbedoTexture, static_cast<u32>(i));
                    const std::string& n = MeshRenderer::SafeGetOverride(mr.overrideNormalTexture, static_cast<u32>(i));
                    const std::string& m = MeshRenderer::SafeGetOverride(mr.overrideMetalRoughnessTexture, static_cast<u32>(i));
                    if (!a.empty()) { entry["albedo"] = a; anyOverride = true; }
                    if (!n.empty()) { entry["normal"] = n; anyOverride = true; }
                    if (!m.empty()) { entry["metalRoughness"] = m; anyOverride = true; }
                    overridesJson.push_back(entry);
                }
                if (anyOverride)
                    ej["materialTextureOverrides"] = overridesJson;
            }

            // マテリアルアセット割当（assets/materials/*.dxmat、サブメッシュ単位）。
            // materialTextureOverrides より優先されるので別キーで保存する(Application 描画ループ参照)。
            if (!mr.materialAsset.empty())
            {
                bool anyMaterial = false;
                json materialsJson = json::array();
                for (size_t i = 0; i < mr.materialAsset.size(); ++i)
                {
                    const std::string& path = MeshRenderer::SafeGetOverride(mr.materialAsset, static_cast<u32>(i));
                    materialsJson.push_back(path);
                    if (!path.empty()) anyMaterial = true;
                }
                if (anyMaterial)
                    ej["materialAssets"] = materialsJson;
            }

            // 色ティント保存。明示的な割当(hasColorTint: JSONのcolor/scene:setColor由来)は
            // モデルでも保存する(従来はプリミティブ限定で、エディタ保存のたびにモデルの
            // 色指定が消えていた)。旧シーン互換: フラグが無いプリミティブはメッシュの
            // 頂点色から従来どおり推定する(モデルは本来の頂点色と区別できないため除外)。
            if (mr.hasColorTint)
            {
                ej["color"] = json::array({ mr.colorTint.x, mr.colorTint.y, mr.colorTint.z });
            }
            else if (mr.modelPath.rfind("__primitive_", 0) == 0 && !mr.meshes.empty() && mr.meshes[0])
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

            // UV スクロール（既定 0 のときは書かない）
            if (mr.uvScrollU != 0.0f || mr.uvScrollV != 0.0f)
            {
                ej["uvScroll"] = {{"u", mr.uvScrollU}, {"v", mr.uvScrollV}};
            }

            // 連番アニメ（無効時は書かない）
            if (mr.animFrames > 0)
            {
                ej["flipbook"] = {{"frames", mr.animFrames}, {"fps", mr.animFps},
                                  {"cols",   mr.animCols},   {"row", mr.animRow},
                                  {"rows",   mr.animRows},   {"mode", mr.animMode}};
            }
        }

        // 地形（ハイトフィールド）。高さ配列そのものは assets/terrain/*.hf 側にあり、
        // ここにはパラメータとパスだけを書く（JSON に数万要素を入れない）。
        if (reg.all_of<Terrain>(entity))
        {
            const auto& tr = reg.get<Terrain>(entity);
            ej["terrain"] = {
                {"resolution",    tr.resolution},
                {"worldSize",     tr.worldSize},
                {"maxHeight",     tr.maxHeight},
                {"heightmapPath", tr.heightmapPath},
                {"uvScale",       tr.uvScale},
                {"color",         json::array({tr.color.x, tr.color.y, tr.color.z, tr.color.w})}
            };
            // テクスチャスプラット。layerSetPath が空のときは 1 つも書かない
            // ＝既存シーンの JSON が 1 バイトも変わらない（完全後方互換）。
            if (!tr.layerSetPath.empty())
            {
                auto& tj = ej["terrain"];
                tj["layerSetPath"]       = tr.layerSetPath;
                tj["splatPath"]          = tr.splatPath;
                tj["heightBlendDepth"]   = tr.heightBlendDepth;
                tj["triplanarSharpness"] = tr.triplanarSharpness;
                tj["terrainMatFlags"]    = tr.terrainMatFlags;
                tj["macroScale"]         = tr.macroScale;
                tj["macroStrength"]      = tr.macroStrength;
                tj["distTilingStart"]    = tr.distTilingStart;
                tj["distTilingFarScale"] = tr.distTilingFarScale;
                tj["normalStrength"]     = tr.normalStrength;
                tj["pomHeightScale"]     = tr.pomHeightScale;
                tj["pomFadeStart"]       = tr.pomFadeStart;
                tj["pomFadeEnd"]         = tr.pomFadeEnd;
                tj["pomMaxSteps"]        = tr.pomMaxSteps;
                tj["splatResolution"]    = tr.splatResolution;
            }
        }

        // スカルプトメッシュ（異形）。頂点配列そのものは assets/sculpt/*.smsh 側にあり、
        // ここにはパラメータとパスだけを書く（JSON に数万頂点を入れない）。
        if (reg.all_of<SculptMesh>(entity))
        {
            const auto& sc = reg.get<SculptMesh>(entity);
            ej["sculpt"] = {
                {"meshPath",  sc.meshPath},
                {"uvScale",   sc.uvScale},
                {"collision", sc.collision},
                {"color",     json::array({sc.color.x, sc.color.y, sc.color.z, sc.color.w})}
            };
        }

        if (reg.all_of<GridPlane>(entity))
        {
            ej["gridPlane"] = {{"size", kEditorGridSize}};
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
                {"kind", pe.kind}, {"blend", pe.blend}, {"orient", pe.orient}, {"rate", pe.rate},
                {"playOnStart", pe.playOnStart}, {"looping", pe.looping}, {"duration", pe.duration},
                {"dir", SerializeFloat3(pe.dir)}, {"spread", pe.spread},
                {"speed", pe.speed}, {"speedVar", pe.speedVar},
                {"size", pe.size}, {"sizeEnd", pe.sizeEnd},
                {"life", pe.life}, {"lifeVar", pe.lifeVar},
                {"color", SerializeFloat3(pe.color)}, {"colorEnd", SerializeFloat3(pe.colorEnd)},
                {"colorMid", SerializeFloat3(pe.colorMid)}, {"hasColorMid", pe.hasColorMid},
                {"intensity", pe.intensity}, {"gravity", pe.gravity},
                {"drag", pe.drag}, {"up", pe.up}, {"stretch", pe.stretch},
                {"turbStrength", pe.turbStrength}, {"turbFreq", pe.turbFreq},
                {"sizeMid", pe.sizeMid}, {"distort", pe.distort},
                {"light", pe.light}, {"lightRange", pe.lightRange},
                {"flicker", pe.flicker}, {"flickerFreq", pe.flickerFreq},
                {"gpu", pe.gpu}, {"texturePath", pe.texturePath}
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
// ---- エンティティ GUID -------------------------------------------------------
// 位置に依存しない参照。EntityGuid（ecs/Components.h）の解説を参照。
namespace {

uint64_t NewEntityGuid()
{
    // 0 は「未設定」の番兵なので絶対に返さない。
    static std::mt19937_64 rng{ std::random_device{}() };
    uint64_t v = 0;
    while (v == 0) v = rng();
    return v;
}

std::string GuidToHex(uint64_t v)
{
    // JSON では**文字列**にする。u64 を数値で書くと、JS/TS 側（dx12_scene_write の
    // 検証や外部ツール）が double へ丸めて下位ビットを失う。
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

uint64_t GuidFromJson(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return 0;
    const std::string sv = it->get<std::string>();
    if (sv.empty() || sv.size() > 16) return 0;
    uint64_t v = 0;
    for (char c : sv)
    {
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;   // 壊れた guid は「無い」扱い（index フォールバックへ落ちる）
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v;
}

} // namespace

static json BuildSceneJson(const Scene& scene, const std::string& assetsDir)
{
    json root;
    root["version"] = 1;
    root["entities"] = json::array();

    const auto& reg = scene.GetRegistry();

    // 1パス目: 保存順を確定して entity → 配列インデックスの対応を作る。
    // ★ entt の view.each() はプール末尾→先頭（逆挿入順）で列挙する。そのまま保存すると
    //   Play→Stop（SaveToString→Clear→LoadFromString で再生成）の往復ごとに生成順が反転し、
    //   Hierarchy の並びが停止のたびに逆転していた。view.each() の結果を reverse して
    //   「プール挿入順＝再生成時に再現される順」で保存することで、往復が同一順序になり安定する。
    std::vector<entt::entity> order;
    auto view = reg.view<const NameTag, const Transform>();
    for (auto [entity, tag, transform] : view.each())
    {
        (void)tag; (void)transform;
        order.push_back(entity);
    }
    std::reverse(order.begin(), order.end());

    std::unordered_map<entt::entity, int> indexOf;
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
        indexOf[order[i]] = i;

    // GUID の自動付与。まだ持っていないエンティティ（＝旧シーン、または reg.create() で
    // 直接作られたもの）へここで振る。開いて保存すれば既存シーンにも付く。
    // ★const_cast: 保存 API は歴史的に const Scene& を取る。ここを非 const へ変えると
    //   Save/SaveToString/SavePrefab とその 7 箇所以上の呼び出し側まで波及するので、
    //   「保存時に GUID を確定させる」ためだけに限定して使う。
    //   Scene の実体が const で構築されることは無いので未定義動作にはならない。
    {
        auto& mutableReg = const_cast<entt::registry&>(reg);
        for (auto entity : order)
        {
            auto* g = mutableReg.try_get<EntityGuid>(entity);
            if (!g)                       mutableReg.emplace<EntityGuid>(entity, EntityGuid{ NewEntityGuid() });
            else if (g->value == 0)       g->value = NewEntityGuid();
        }
    }

    // 2パス目: 直列化 + 親子関係を保存
    for (auto entity : order)
    {
        json ej = SerializeEntityJson(reg, entity, assetsDir);

        if (const auto* g = reg.try_get<EntityGuid>(entity))
            ej["guid"] = GuidToHex(g->value);

        const auto& transform = reg.get<Transform>(entity);
        if (transform.parent != entt::null && reg.valid(transform.parent))
        {
            // ★parentGuid が正。parent(index) は旧エンジンで開けるように残す互換用。
            //   index は entt のプール順に依存するので、他人が別の場所へエンティティを
            //   足した JSON と git がマージされると黙ってズレる。読み側は guid を優先する。
            if (const auto* pg = reg.try_get<EntityGuid>(transform.parent))
                ej["parentGuid"] = GuidToHex(pg->value);

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
            {"bloomKnee",    pp.bloomKnee},    {"bloomRadius", pp.bloomRadius},
            {"tonemapper",   pp.tonemapper},
            {"autoExposureOn", pp.autoExposureOn}, {"aeSpeed", pp.aeSpeed}, {"aeEvComp", pp.aeEvComp},
            {"aeLogMin",     pp.aeLogMin},     {"aeLogMax",   pp.aeLogMax},
            {"lutOn",        pp.lutOn},        {"lutPath",    pp.lutPath}, {"lutAmount", pp.lutAmount},
            {"debandOn",     pp.debandOn},
            {"godraysOn",    pp.godraysOn},    {"grIntensity", pp.grIntensity},
            {"grDensity",    pp.grDensity},    {"grDecay",     pp.grDecay},
            {"lensflareOn",  pp.lensflareOn},  {"lfIntensity", pp.lfIntensity},
            {"lfGhosts",     pp.lfGhosts},     {"lfDispersal", pp.lfDispersal},
            {"lfHalo",       pp.lfHalo},       {"lfChroma",    pp.lfChroma},
            {"dofOn",        pp.dofOn},        {"dofFocusDist", pp.dofFocusDist},
            {"dofFocusRange", pp.dofFocusRange}, {"dofBlurSize", pp.dofBlurSize},
            {"motionBlurOn", pp.motionBlurOn}, {"mbStrength",  pp.mbStrength},
            {"mbSamples",    pp.mbSamples},
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

    // リアルタイム影 ON/OFF（シーン単位）
    root["shadows"] = scene.GetShadowsEnabled();

    // SSAO 設定（シーン単位）
    {
        const auto& ss = scene.GetSSAOSettings();
        root["ssao"] = {
            {"enabled",     ss.enabled},
            {"radius",      ss.radius},
            {"bias",        ss.bias},
            {"intensity",   ss.intensity},
            {"power",       ss.power},
            {"sampleCount", ss.sampleCount},
            {"blur",        ss.blur},
        };
    }

    // コンタクトシャドウ設定（シーン単位）
    {
        const auto& cs = scene.GetContactShadowSettings();
        root["contactShadow"] = {
            {"enabled",      cs.enabled},
            {"rayLength",    cs.rayLength},
            {"thickness",    cs.thickness},
            {"bias",         cs.bias},
            {"intensity",    cs.intensity},
            {"steps",        cs.steps},
            {"maxDistance",  cs.maxDistance},
            {"fadeDistance", cs.fadeDistance},
        };
    }

    // SSR / SSGI 設定（シーン単位）。どちらも既定 OFF なので旧シーンは無変更で開く。
    {
        const auto& sr = scene.GetSsrSettings();
        root["ssr"] = {
            {"enabled",         sr.enabled},
            {"intensity",       sr.intensity},
            {"maxDistance",     sr.maxDistance},
            {"thickness",       sr.thickness},
            {"maxSteps",        sr.maxSteps},
            {"stride",          sr.stride},
            {"roughnessCutoff", sr.roughnessCutoff},
            {"edgeFade",        sr.edgeFade},
            {"bias",            sr.bias},
        };
        const auto& sg = scene.GetSsgiSettings();
        root["ssgi"] = {
            {"enabled",     sg.enabled},
            {"intensity",   sg.intensity},
            {"radius",      sg.radius},
            {"thickness",   sg.thickness},
            {"rayCount",    sg.rayCount},
            {"stepCount",   sg.stepCount},
            {"clampValue",  sg.clampValue},
            {"feedback",    sg.feedback},
            {"iblFallback", sg.iblFallback},
        };
    }

    // TAA 設定（シーン単位）。debugVelocity は目視検証用の一時トグルなので保存しない。
    {
        const auto& ta = scene.GetTaaSettings();
        root["taa"] = {
            {"enabled",       ta.enabled},
            {"sampleCount",   ta.sampleCount},
            {"feedbackMin",   ta.feedbackMin},
            {"feedbackMax",   ta.feedbackMax},
            {"varianceGamma", ta.varianceGamma},
            {"jitterScale",   ta.jitterScale},
        };
    }

    // PCSS（ソフトシャドウ・シーン単位）。既定 OFF なので、未設定シーンは従来どおり。
    {
        const auto& pc = scene.GetShadowPcssSettings();
        root["shadowPcss"] = {
            {"enabled",             pc.enabled},
            {"lightTanAngle",       pc.lightTanAngle},
            {"maxPenumbraTexels",   pc.maxPenumbraTexels},
            {"blockerSearchTexels", pc.blockerSearchTexels},
            {"temporalDither",      pc.temporalDither},
        };
    }

    // DXR レイトレーシング（シーン単位）。既定 OFF なので、未設定シーンは従来どおり。
    // forceBuildTlas は目視検証用の一時トグルなので保存しない。
    {
        const auto& rt = scene.GetRtSettings();
        root["raytracing"] = {
            {"shadowEnabled",      rt.shadowEnabled},
            {"shadowSunAngle",     rt.shadowSunAngle},
            {"shadowNormalBias",   rt.shadowNormalBias},
            {"shadowMaxDistance",  rt.shadowMaxDistance},
            {"shadowIntensity",    rt.shadowIntensity},
            {"aoEnabled",          rt.aoEnabled},
            {"aoRadius",           rt.aoRadius},
            {"aoRayCount",         rt.aoRayCount},
            {"aoIntensity",        rt.aoIntensity},
            {"aoPower",            rt.aoPower},
            {"aoCombineWithSsao",  rt.aoCombineWithSsao},
            {"aoDenoise",          rt.aoDenoise},
            {"aoDenoiseRadius",    rt.aoDenoiseRadius},
            {"maxInstances",       rt.maxInstances},
        };
        // DDGI（計画09 Step 6）。★ルートキーを新設せず raytracing の入れ子にする:
        //   TLAS が前提で MCP も set_dxr が捌いている＝同じものだから。
        //   （SCENE_ROOT_KEYS / sceneWrite.ts を触らずに済む副次効果もある）
        const auto& dg = scene.GetDdgiSettings();
        root["raytracing"]["ddgi"] = {
            {"enabled",     dg.enabled},
            {"probeCountX", dg.probeCountX},
            {"probeCountY", dg.probeCountY},
            {"probeCountZ", dg.probeCountZ},
            {"spacing",     dg.spacing},
            {"originX",     dg.originX},
            {"originY",     dg.originY},
            {"originZ",     dg.originZ},
            {"rayLength",   dg.rayLength},
            {"hysteresis",  dg.hysteresis},
            {"intensity",   dg.intensity},
            {"normalBias",  dg.normalBias},
        };
    }

    // ボリュメトリックフォグ（シーン単位）。debugMode は目視検証用の一時トグルなので保存しない。
    {
        const auto& vf = scene.GetVolumetricFogSettings();
        root["volumetricFog"] = {
            {"enabled",           vf.enabled},
            {"density",           vf.density},
            {"albedo",            {vf.albedo.x, vf.albedo.y, vf.albedo.z}},
            {"anisotropy",        vf.anisotropy},
            {"heightFalloff",     vf.heightFalloff},
            {"heightRef",         vf.heightRef},
            {"distance",          vf.distance},
            {"depthDistribution", vf.depthDistribution},
            {"ambient",           {vf.ambient.x, vf.ambient.y, vf.ambient.z}},
            {"sunIntensity",      vf.sunIntensity},
            {"lightScattering",   vf.lightScattering},
            {"temporal",          vf.temporal},
            {"temporalBlend",     vf.temporalBlend},
            {"extendBeyondRange", vf.extendBeyondRange},
        };
    }

    // デカールアトラス（assets 相対の 1 枚）。空のときは書かない＝旧シーンと差分ゼロ。
    if (!scene.GetDecalAtlasPath().empty())
        root["decalAtlas"] = scene.GetDecalAtlasPath();

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
        pp.bloomKnee    = pj.value("bloomKnee",    pp.bloomKnee);    pp.bloomRadius = pj.value("bloomRadius", pp.bloomRadius);
        pp.tonemapper   = pj.value("tonemapper",   pp.tonemapper);
        pp.autoExposureOn = pj.value("autoExposureOn", pp.autoExposureOn);
        pp.aeSpeed      = pj.value("aeSpeed",      pp.aeSpeed);      pp.aeEvComp   = pj.value("aeEvComp",   pp.aeEvComp);
        pp.aeLogMin     = pj.value("aeLogMin",     pp.aeLogMin);     pp.aeLogMax   = pj.value("aeLogMax",   pp.aeLogMax);
        pp.lutOn        = pj.value("lutOn",        pp.lutOn);        pp.lutPath    = pj.value("lutPath",    pp.lutPath);
        pp.lutAmount    = pj.value("lutAmount",    pp.lutAmount);
        pp.debandOn     = pj.value("debandOn",     pp.debandOn);
        pp.godraysOn    = pj.value("godraysOn",    pp.godraysOn);    pp.grIntensity = pj.value("grIntensity", pp.grIntensity);
        pp.grDensity    = pj.value("grDensity",    pp.grDensity);    pp.grDecay     = pj.value("grDecay",     pp.grDecay);
        pp.lensflareOn  = pj.value("lensflareOn",  pp.lensflareOn);  pp.lfIntensity = pj.value("lfIntensity", pp.lfIntensity);
        pp.lfGhosts     = pj.value("lfGhosts",     pp.lfGhosts);     pp.lfDispersal = pj.value("lfDispersal", pp.lfDispersal);
        pp.lfHalo       = pj.value("lfHalo",       pp.lfHalo);       pp.lfChroma    = pj.value("lfChroma",    pp.lfChroma);
        pp.dofOn        = pj.value("dofOn",        pp.dofOn);        pp.dofFocusDist = pj.value("dofFocusDist", pp.dofFocusDist);
        pp.dofFocusRange = pj.value("dofFocusRange", pp.dofFocusRange); pp.dofBlurSize = pj.value("dofBlurSize", pp.dofBlurSize);
        pp.motionBlurOn = pj.value("motionBlurOn", pp.motionBlurOn); pp.mbStrength  = pj.value("mbStrength",  pp.mbStrength);
        pp.mbSamples    = pj.value("mbSamples",    pp.mbSamples);
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

// JSON から SSAO 設定を復元（ssao が無ければデフォルト = 後方互換）
static void LoadSSAOSettings(Scene& scene, const json& root)
{
    SSAOSettings ss;  // デフォルト（未指定キーは既定値を維持）
    if (root.contains("ssao"))
    {
        const auto& sj = root["ssao"];
        ss.enabled     = sj.value("enabled",     ss.enabled);
        ss.radius      = sj.value("radius",      ss.radius);
        ss.bias        = sj.value("bias",        ss.bias);
        ss.intensity   = sj.value("intensity",   ss.intensity);
        ss.power       = sj.value("power",       ss.power);
        ss.sampleCount = sj.value("sampleCount", ss.sampleCount);
        ss.blur        = sj.value("blur",        ss.blur);
    }
    scene.GetSSAOSettings() = ss;
}

// JSON からコンタクトシャドウ設定を復元（contactShadow が無ければデフォルト = 後方互換）
static void LoadContactShadowSettings(Scene& scene, const json& root)
{
    ContactShadowSettings cs;  // デフォルト（未指定キーは既定値を維持）
    if (root.contains("contactShadow"))
    {
        const auto& cj = root["contactShadow"];
        cs.enabled      = cj.value("enabled",      cs.enabled);
        cs.rayLength    = cj.value("rayLength",    cs.rayLength);
        cs.thickness    = cj.value("thickness",    cs.thickness);
        cs.bias         = cj.value("bias",         cs.bias);
        cs.intensity    = cj.value("intensity",    cs.intensity);
        cs.steps        = cj.value("steps",        cs.steps);
        cs.maxDistance  = cj.value("maxDistance",  cs.maxDistance);
        cs.fadeDistance = cj.value("fadeDistance", cs.fadeDistance);
    }
    scene.GetContactShadowSettings() = cs;
}

// JSON から SSR / SSGI 設定を復元（キーが無ければデフォルト OFF = 後方互換）
static void LoadScreenSpaceGiSettings(Scene& scene, const json& root)
{
    SsrSettings sr;
    if (root.contains("ssr"))
    {
        const auto& j = root["ssr"];
        sr.enabled         = j.value("enabled",         sr.enabled);
        sr.intensity       = j.value("intensity",       sr.intensity);
        sr.maxDistance     = j.value("maxDistance",     sr.maxDistance);
        sr.thickness       = j.value("thickness",       sr.thickness);
        sr.maxSteps        = j.value("maxSteps",        sr.maxSteps);
        sr.stride          = j.value("stride",          sr.stride);
        sr.roughnessCutoff = j.value("roughnessCutoff", sr.roughnessCutoff);
        sr.edgeFade        = j.value("edgeFade",        sr.edgeFade);
        sr.bias            = j.value("bias",            sr.bias);
    }
    scene.GetSsrSettings() = sr;

    SsgiSettings sg;
    if (root.contains("ssgi"))
    {
        const auto& j = root["ssgi"];
        sg.enabled     = j.value("enabled",     sg.enabled);
        sg.intensity   = j.value("intensity",   sg.intensity);
        sg.radius      = j.value("radius",      sg.radius);
        sg.thickness   = j.value("thickness",   sg.thickness);
        sg.rayCount    = j.value("rayCount",    sg.rayCount);
        sg.stepCount   = j.value("stepCount",   sg.stepCount);
        sg.clampValue  = j.value("clampValue",  sg.clampValue);
        sg.feedback    = j.value("feedback",    sg.feedback);
        sg.iblFallback = j.value("iblFallback", sg.iblFallback);
    }
    scene.GetSsgiSettings() = sg;
}

// JSON から TAA 設定を復元（taa が無ければデフォルト = 後方互換。旧シーンは既定 OFF で開く）
static void LoadTaaSettings(Scene& scene, const json& root)
{
    TaaSettings ta;  // デフォルト（未指定キーは既定値を維持）
    if (root.contains("taa"))
    {
        const auto& tj = root["taa"];
        ta.enabled       = tj.value("enabled",       ta.enabled);
        ta.sampleCount   = tj.value("sampleCount",   ta.sampleCount);
        ta.feedbackMin   = tj.value("feedbackMin",   ta.feedbackMin);
        ta.feedbackMax   = tj.value("feedbackMax",   ta.feedbackMax);
        ta.varianceGamma = tj.value("varianceGamma", ta.varianceGamma);
        ta.jitterScale   = tj.value("jitterScale",   ta.jitterScale);
    }
    scene.GetTaaSettings() = ta;   // debugVelocity は常に既定 OFF（保存対象外）
}

// JSON から PCSS 設定を復元（shadowPcss が無ければデフォルト OFF = 後方互換）
static void LoadShadowPcssSettings(Scene& scene, const json& root)
{
    ShadowPcssSettings pc;
    if (root.contains("shadowPcss"))
    {
        const auto& pj = root["shadowPcss"];
        pc.enabled             = pj.value("enabled",             pc.enabled);
        pc.lightTanAngle       = pj.value("lightTanAngle",       pc.lightTanAngle);
        pc.maxPenumbraTexels   = pj.value("maxPenumbraTexels",   pc.maxPenumbraTexels);
        pc.blockerSearchTexels = pj.value("blockerSearchTexels", pc.blockerSearchTexels);
        pc.temporalDither      = pj.value("temporalDither",      pc.temporalDither);
    }
    scene.GetShadowPcssSettings() = pc;
}

// JSON から DXR 設定を復元（raytracing が無ければデフォルト OFF = 後方互換）
static void LoadRtSettings(Scene& scene, const json& root)
{
    RtSettings rt;
    if (root.contains("raytracing"))
    {
        const auto& j = root["raytracing"];
        rt.shadowEnabled     = j.value("shadowEnabled",     rt.shadowEnabled);
        rt.shadowSunAngle    = j.value("shadowSunAngle",    rt.shadowSunAngle);
        rt.shadowNormalBias  = j.value("shadowNormalBias",  rt.shadowNormalBias);
        rt.shadowMaxDistance = j.value("shadowMaxDistance", rt.shadowMaxDistance);
        rt.shadowIntensity   = j.value("shadowIntensity",   rt.shadowIntensity);
        rt.aoEnabled         = j.value("aoEnabled",         rt.aoEnabled);
        rt.aoRadius          = j.value("aoRadius",          rt.aoRadius);
        rt.aoRayCount        = j.value("aoRayCount",        rt.aoRayCount);
        rt.aoIntensity       = j.value("aoIntensity",       rt.aoIntensity);
        rt.aoPower           = j.value("aoPower",           rt.aoPower);
        rt.aoCombineWithSsao = j.value("aoCombineWithSsao", rt.aoCombineWithSsao);
        rt.aoDenoise         = j.value("aoDenoise",         rt.aoDenoise);
        rt.aoDenoiseRadius   = j.value("aoDenoiseRadius",   rt.aoDenoiseRadius);
        rt.maxInstances      = j.value("maxInstances",      rt.maxInstances);
    }
    scene.GetRtSettings() = rt;   // forceBuildTlas は常に既定 OFF（保存対象外）

    // DDGI（raytracing の入れ子。キーが無ければ既定 OFF＝旧シーンは無変更で開く）
    DdgiSettings dg;
    if (root.contains("raytracing") && root["raytracing"].contains("ddgi"))
    {
        const auto& j = root["raytracing"]["ddgi"];
        dg.enabled     = j.value("enabled",     dg.enabled);
        dg.probeCountX = j.value("probeCountX", dg.probeCountX);
        dg.probeCountY = j.value("probeCountY", dg.probeCountY);
        dg.probeCountZ = j.value("probeCountZ", dg.probeCountZ);
        dg.spacing     = j.value("spacing",     dg.spacing);
        dg.originX     = j.value("originX",     dg.originX);
        dg.originY     = j.value("originY",     dg.originY);
        dg.originZ     = j.value("originZ",     dg.originZ);
        dg.rayLength   = j.value("rayLength",   dg.rayLength);
        dg.hysteresis  = j.value("hysteresis",  dg.hysteresis);
        dg.intensity   = j.value("intensity",   dg.intensity);
        dg.normalBias  = j.value("normalBias",  dg.normalBias);
        // ★MCP の set_dxr と同じ範囲へ丸める（手書き JSON でプローブ数 999 を書かれても落ちない）
        dg.probeCountX = std::clamp(dg.probeCountX, 1, 32);
        dg.probeCountY = std::clamp(dg.probeCountY, 1, 32);
        dg.probeCountZ = std::clamp(dg.probeCountZ, 1, 32);
        dg.spacing     = std::clamp(dg.spacing,    0.1f, 100.0f);
        dg.rayLength   = std::clamp(dg.rayLength,  0.1f, 10000.0f);
        dg.hysteresis  = std::clamp(dg.hysteresis, 0.0f, 0.995f);
        dg.intensity   = std::clamp(dg.intensity,  0.0f, 10.0f);
        dg.normalBias  = std::clamp(dg.normalBias, 0.0f, 1.0f);
    }
    scene.GetDdgiSettings() = dg;
}

// JSON から ボリュメトリックフォグ設定を復元（volumetricFog が無ければデフォルト OFF = 後方互換）
static void LoadVolumetricFogSettings(Scene& scene, const json& root)
{
    VolumetricFogSettings vf;  // デフォルト（未指定キーは既定値を維持）
    if (root.contains("volumetricFog"))
    {
        const auto& j = root["volumetricFog"];
        auto readVec3 = [&](const char* key, DirectX::XMFLOAT3& dst)
        {
            if (j.contains(key) && j[key].is_array() && j[key].size() >= 3)
            {
                dst.x = j[key][0].get<float>();
                dst.y = j[key][1].get<float>();
                dst.z = j[key][2].get<float>();
            }
        };
        vf.enabled           = j.value("enabled",           vf.enabled);
        vf.density           = j.value("density",           vf.density);
        readVec3("albedo", vf.albedo);
        vf.anisotropy        = j.value("anisotropy",        vf.anisotropy);
        vf.heightFalloff     = j.value("heightFalloff",     vf.heightFalloff);
        vf.heightRef         = j.value("heightRef",         vf.heightRef);
        vf.distance          = j.value("distance",          vf.distance);
        vf.depthDistribution = j.value("depthDistribution", vf.depthDistribution);
        readVec3("ambient", vf.ambient);
        vf.sunIntensity      = j.value("sunIntensity",      vf.sunIntensity);
        vf.lightScattering   = j.value("lightScattering",   vf.lightScattering);
        vf.temporal          = j.value("temporal",          vf.temporal);
        vf.temporalBlend     = j.value("temporalBlend",     vf.temporalBlend);
        vf.extendBeyondRange = j.value("extendBeyondRange", vf.extendBeyondRange);
    }
    scene.GetVolumetricFogSettings() = vf;   // debugMode は常に 0（保存対象外）

    // デカールアトラス（assets 相対）。キーが無ければ空＝デカール無効。
    scene.SetDecalAtlasPath(root.contains("decalAtlas") && root["decalAtlas"].is_string()
                            ? root["decalAtlas"].get<std::string>() : std::string());
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
        // エディタグリッドは常に最新サイズで再生成する(保存値は無視)。
        // 旧シーン(size=50 が焼かれてる)を開いても矩形に途切れず広く表示されるようにするため。
        (void)ej["gridPlane"].value("size", kEditorGridSize);
        e = scene.SpawnPlane(name, pos, kEditorGridSize, true).GetHandle();
        OutputDebugStringA(("[Load] SpawnPlane: " + name + "\n").c_str());
    }
    else if (ej.contains("terrain"))
    {
        // ハイトフィールド地形。高さ配列は heightmapPath の .hf から vfs 経由で読む
        // （SpawnTerrain の中でロード。読めなければ平坦のまま警告して続行）。
        const auto& tj = ej["terrain"];
        Terrain tp;
        tp.resolution    = tj.value("resolution", 128u);
        tp.worldSize     = tj.value("worldSize", 200.0f);
        tp.maxHeight     = tj.value("maxHeight", 200.0f);
        tp.heightmapPath = tj.value("heightmapPath", std::string{});
        tp.uvScale       = tj.value("uvScale", 24.0f);
        if (tj.contains("color") && tj["color"].is_array() && tj["color"].size() >= 4)
        {
            tp.color = { tj["color"][0].get<float>(), tj["color"][1].get<float>(),
                         tj["color"][2].get<float>(), tj["color"][3].get<float>() };
        }
        // テクスチャスプラット（無ければ既定値のまま = layerSetPath 空 = 従来経路）
        tp.layerSetPath       = tj.value("layerSetPath", std::string{});
        tp.splatPath          = tj.value("splatPath", std::string{});
        tp.heightBlendDepth   = tj.value("heightBlendDepth", 0.2f);
        tp.triplanarSharpness = tj.value("triplanarSharpness", 4.0f);
        tp.terrainMatFlags    = tj.value("terrainMatFlags", 0x0Du);
        tp.macroScale         = tj.value("macroScale", 90.0f);
        tp.macroStrength      = tj.value("macroStrength", 0.45f);
        tp.distTilingStart    = tj.value("distTilingStart", 40.0f);
        tp.distTilingFarScale = tj.value("distTilingFarScale", 7.0f);
        tp.normalStrength     = tj.value("normalStrength", 1.0f);
        tp.pomHeightScale     = tj.value("pomHeightScale", 0.05f);
        tp.pomFadeStart       = tj.value("pomFadeStart", 8.0f);
        tp.pomFadeEnd         = tj.value("pomFadeEnd", 25.0f);
        tp.pomMaxSteps        = tj.value("pomMaxSteps", 24u);
        tp.splatResolution    = tj.value("splatResolution", 512u);
        e = scene.SpawnTerrain(name, pos, tp).GetHandle();
        OutputDebugStringA(("[Load] SpawnTerrain: " + name + "\n").c_str());
    }
    else if (ej.contains("sculpt"))
    {
        // スカルプトメッシュ（異形）。頂点配列は meshPath の .smsh から vfs 経由で読む
        //（SpawnSculpt の中でロード。読めなければ素体の球で続行）。
        const auto& sj = ej["sculpt"];
        SculptMesh sp;
        sp.meshPath  = sj.value("meshPath", std::string{});
        sp.uvScale   = sj.value("uvScale", 1.0f);
        sp.collision = sj.value("collision", true);
        if (sj.contains("color") && sj["color"].is_array() && sj["color"].size() >= 4)
        {
            sp.color = { sj["color"][0].get<float>(), sj["color"][1].get<float>(),
                         sj["color"][2].get<float>(), sj["color"][3].get<float>() };
        }
        e = scene.SpawnSculpt(name, pos, sp).GetHandle();
        OutputDebugStringA(("[Load] SpawnSculpt: " + name + "\n").c_str());
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
                pe.orient      = pj.value("orient", 0);
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
                if (pj.contains("colorMid")) pe.colorMid = DeserializeFloat3(pj["colorMid"], {1.0f, 0.6f, 0.2f});
                pe.hasColorMid = pj.value("hasColorMid", false);
                pe.intensity = pj.value("intensity", 3.0f);
                pe.gravity   = pj.value("gravity", 0.0f);
                pe.drag      = pj.value("drag", 1.0f);
                pe.up        = pj.value("up", 0.0f);
                pe.stretch   = pj.value("stretch", 0.0f);
                pe.turbStrength = pj.value("turbStrength", 0.0f);
                pe.turbFreq     = pj.value("turbFreq", 1.0f);
                pe.sizeMid   = pj.value("sizeMid", -1.0f);
                pe.distort   = pj.value("distort", 0.0f);
                pe.light     = pj.value("light", false);
                pe.lightRange = pj.value("lightRange", 3.0f);
                pe.flicker      = pj.value("flicker", 0.0f);
                pe.flickerFreq  = pj.value("flickerFreq", 18.0f);
                pe.gpu       = pj.value("gpu", false);
                pe.texturePath = pj.value("texturePath", std::string());
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
                mr.colorTint    = {c.x, c.y, c.z, 1.0f};   // 保存で消えないよう意図を記録
                mr.hasColorTint = true;
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

            // UV スクロール復元（頂点は触らないのでロード時の再構築は不要）
            if (ej.contains("uvScroll") && reg.all_of<MeshRenderer>(e))
            {
                const auto& sj = ej["uvScroll"];
                auto& mr = reg.get<MeshRenderer>(e);
                mr.uvScrollU = sj.value("u", 0.0f);
                mr.uvScrollV = sj.value("v", 0.0f);
            }

            // 連番アニメ復元
            if (ej.contains("flipbook") && reg.all_of<MeshRenderer>(e))
            {
                const auto& fj = ej["flipbook"];
                auto& mr = reg.get<MeshRenderer>(e);
                mr.animFrames = fj.value("frames", 0);
                mr.animFps    = fj.value("fps",    8.0f);
                mr.animCols   = fj.value("cols",   0);
                mr.animRow    = fj.value("row",    0);
                mr.animRows   = fj.value("rows",   0);
                mr.animMode   = fj.value("mode",   0);
            }

            // カスタムシェーダー割当復元
            if (ej.contains("shader") && reg.all_of<MeshRenderer>(e))
            {
                auto& mrRestore = reg.get<MeshRenderer>(e);
                mrRestore.shaderPath = ej.value("shader", "");
                mrRestore.shaderAlphaBlend = ej.value("shaderAlphaBlend", false);
                mrRestore.effectValue = ej.value("shaderEffectValue", 0.0f);
                if (ej.contains("shaderParams") && ej["shaderParams"].is_array()
                    && ej["shaderParams"].size() >= 4)
                {
                    const auto& sp = ej["shaderParams"];
                    mrRestore.shaderParams = {sp[0].get<float>(), sp[1].get<float>(),
                                              sp[2].get<float>(), sp[3].get<float>()};
                }
            }

            // マテリアルテクスチャ上書き復元（サブメッシュ単位）
            if (ej.contains("materialTextureOverrides") && reg.all_of<MeshRenderer>(e))
            {
                auto& mrOv = reg.get<MeshRenderer>(e);
                const auto& arr = ej["materialTextureOverrides"];
                for (size_t i = 0; i < arr.size(); ++i)
                {
                    const auto& entry = arr[i];
                    u32 smi = static_cast<u32>(i);
                    if (entry.contains("albedo"))
                        MeshRenderer::SetOverride(mrOv.overrideAlbedoTexture, smi, entry.value("albedo", ""));
                    if (entry.contains("normal"))
                        MeshRenderer::SetOverride(mrOv.overrideNormalTexture, smi, entry.value("normal", ""));
                    if (entry.contains("metalRoughness"))
                        MeshRenderer::SetOverride(mrOv.overrideMetalRoughnessTexture, smi, entry.value("metalRoughness", ""));
                }
            }

            // マテリアルアセット割当復元（サブメッシュ単位、assets/materials/*.dxmat）
            if (ej.contains("materialAssets") && reg.all_of<MeshRenderer>(e))
            {
                auto& mrMat = reg.get<MeshRenderer>(e);
                const auto& arr = ej["materialAssets"];
                for (size_t i = 0; i < arr.size(); ++i)
                    MeshRenderer::SetOverride(mrMat.materialAsset, static_cast<u32>(i), arr[i].get<std::string>());
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

    // JSON として valid でも「型」が想定と違う値（例: intensity が文字列）は
    // json::type_error を投げる。1 箇所のミスでシーン全体・アプリ全体を巻き込まず、
    // 該当セクション/エンティティだけスキップして警告に倒す。

    // ポストプロセス / スカイボックス / SSAO 設定（entities が無くても復元する）
    try { LoadPostSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("postProcess 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadSkyboxSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("skybox 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadSSAOSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("ssao 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadContactShadowSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("contactShadow 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadTaaSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("taa 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadScreenSpaceGiSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("ssr/ssgi 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadShadowPcssSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("shadowPcss 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadVolumetricFogSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("volumetricFog 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadRtSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("raytracing 設定をスキップしました（型不正）: {}", e.what()); }

    // リアルタイム影 ON/OFF（キーが無ければ既定 ON ＝後方互換）
    scene.SetShadowsEnabled(root.value("shadows", true));

    if (!root.contains("entities") || !root["entities"].is_array())
    {
        Logger::Warn("シーン JSON に entities 配列がありません");
        return true;
    }

    // 1パス目: 生成（配列インデックス → entity の対応を保持）
    std::vector<entt::entity> created;
    created.reserve(root["entities"].size());
    for (const auto& ej : root["entities"])
    {
        entt::entity e = entt::null;
        try
        {
            e = InstantiateEntityJson(scene, ej, assetsDir);
        }
        catch (const std::exception& ex)
        {
            const std::string nm = (ej.is_object() && ej.contains("name") && ej["name"].is_string())
                ? ej["name"].get<std::string>() : "?";
            Logger::Error("エンティティ [{}] \"{}\" をスキップしました（値不正）: {}", created.size(), nm, ex.what());
        }
        created.push_back(e);
    }

    // 2パス目: 親子関係の復元
    auto& reg = scene.GetRegistry();

    // guid → entity。JSON に guid があるものだけ載る（旧シーンは空のまま）。
    // guid を持つエンティティには EntityGuid を復元しておく（次の保存でそのまま残る）。
    std::unordered_map<uint64_t, entt::entity> byGuid;
    {
        size_t gi = 0;
        for (const auto& ej : root["entities"])
        {
            const entt::entity e = created[gi++];
            if (e == entt::null || !ej.is_object()) continue;
            const uint64_t g = GuidFromJson(ej, "guid");
            if (g == 0) continue;
            // 同じ guid が 2 つある = 壊れたマージか、複製時の振り直し漏れ。
            // 先勝ちにして警告する（黙って片方の参照を奪わせない）。
            auto [it, inserted] = byGuid.emplace(g, e);
            if (!inserted)
            {
                Logger::Warn("エンティティ [{}] の guid が重複しています（先に読んだ方を使います）", gi - 1);
                continue;
            }
            reg.emplace_or_replace<EntityGuid>(e, EntityGuid{ g });
        }
    }

    size_t idx = 0;
    for (const auto& ej : root["entities"])
    {
        entt::entity e = created[idx++];
        if (e == entt::null || !ej.is_object()) continue;

        // ★parentGuid を優先。index は entt のプール順に依存するので、他人が別の場所へ
        //   エンティティを足した JSON と git がマージされると黙ってズレる。
        //   guid が無い（旧シーン）ときだけ index へフォールバックする。
        entt::entity parent = entt::null;
        if (const uint64_t pg = GuidFromJson(ej, "parentGuid"); pg != 0)
        {
            auto it = byGuid.find(pg);
            if (it != byGuid.end()) parent = it->second;
            else Logger::Warn("エンティティ [{}] の parentGuid が見つかりません。index へフォールバックします", idx - 1);
        }

        if (parent == entt::null)
        {
            if (!ej.contains("parent")) continue;
            if (!ej["parent"].is_number_integer())
            {
                Logger::Warn("エンティティ [{}] の parent が整数ではないため、ルートのままにします", idx - 1);
                continue;
            }
            const int parentIdx = ej["parent"].get<int>();
            if (parentIdx < 0 || parentIdx >= static_cast<int>(created.size())) continue;
            parent = created[static_cast<size_t>(parentIdx)];
            if (parent == entt::null)
            {
                Logger::Warn("エンティティ [{}] の親（index {}）が読み込めなかったため、ルートのままにします", idx - 1, parentIdx);
                continue;
            }
        }

        if (parent == e) continue;
        if (reg.all_of<Transform>(e))
            reg.get<Transform>(e).parent = parent;
    }

    // 3パス目: 親子グラフの正規化。相互参照(A→B→A 等)のサイクルが成立していると、
    // 削除時のサブツリー収集や祖先走査が無限ループ/無限膨張するため、検出したら
    // そのエンティティをルートへ切り離す（旧バージョンで保存された壊れたデータ対策）。
    for (entt::entity e : created)
    {
        if (e == entt::null || !reg.all_of<Transform>(e)) continue;
        int depth = 0;
        entt::entity cur = reg.get<Transform>(e).parent;
        while (cur != entt::null && depth < 4096)
        {
            if (cur == e) break;   // 自分に戻ってきた = サイクル
            auto* t = reg.try_get<Transform>(cur);
            cur = t ? t->parent : entt::null;
            ++depth;
        }
        if (cur == e || depth >= 4096)
        {
            Logger::Warn("親子関係にサイクルを検出したため、エンティティ {} をルートへ切り離しました",
                         static_cast<u32>(e));
            reg.get<Transform>(e).parent = entt::null;
        }
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
        Logger::Error("ファイルを書き込み用に開けません: {}", filePath);
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
    // VFS 経由で読む（ゲームモード: pak 復号。エディタ: ディスク）。
    auto b = vfs::ReadAssetAbs(filePath);
    if (!b.empty())
        return LoadFromString(scene, std::string(b.begin(), b.end()), assetsDir);

    // ディスクフォールバック
    std::ifstream ifs(filePath);
    if (!ifs.is_open())
    {
        Logger::Error("シーンファイルを開けません: {}", filePath);
        return false;
    }

    json root;
    try
    {
        ifs >> root;
    }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON の解析に失敗しました: {}", e.what());
        return false;
    }
    ifs.close();

    bool ok = false;
    try
    {
        ok = ApplySceneJson(scene, root, assetsDir);
    }
    catch (const json::exception& e)
    {
        // ApplySceneJson 内でセクション/エンティティ単位に捕捉しているが、最後の保険
        Logger::Error("シーン読み込みを中断しました（JSON の値が不正）: {}", e.what());
        return false;
    }
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
        Logger::Error("JSON の解析に失敗しました（スナップショット）: {}", e.what());
        return false;
    }

    bool ok = false;
    try
    {
        ok = ApplySceneJson(scene, root, assetsDir);
    }
    catch (const json::exception& e)
    {
        Logger::Error("シーン復元を中断しました（JSON の値が不正）: {}", e.what());
        return false;
    }
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
            rb.continuousCollision = rj.value("continuousCollision", false);
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
        Logger::Error("JSON の解析に失敗しました（エンティティ）: {}", e.what());
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
    const std::string uniqueName = MakeUniqueName(scene, reg.get<NameTag>(src).name);
    ej["name"] = uniqueName;

    // 地形(.hf) / スカルプト(.smsh) の実データはシーン JSON に入らず assets 配下の外部ファイルにある。
    // JSON を素通しでコピーすると複製元と複製先が同じファイルを指し、どちらを彫っても
    // 両方が同じ .hf へ自動保存されて壊れる。→ 複製先には固有のパスを振る。
    // 実データはこの時点でまだ存在しないので、下で複製元の中身をメモリ上でコピーし、
    // _needsSave を立てて TerrainPanel / SculptPanel に書き出させる。
    if (ej.contains("terrain") && ej["terrain"].is_object())
    {
        ej["terrain"]["heightmapPath"] = terrain::MakeHeightFieldRelPath(uniqueName);
        if (ej["terrain"].contains("splatPath")
            && !ej["terrain"]["splatPath"].get<std::string>().empty())
            ej["terrain"]["splatPath"] = terrain::MakeSplatRelPath(uniqueName);
    }
    if (ej.contains("sculpt") && ej["sculpt"].is_object())
        ej["sculpt"]["meshPath"] = sculpt::MakeSculptMeshRelPath(uniqueName);

    entt::entity copy = InstantiateEntityJson(scene, ej, assetsDir);
    if (copy == entt::null) return entt::null;

    // 実データのコピー。Spawn* は「新パスのファイルが無い」ので平坦 / 素体へフォールバック
    // しているため、ここで複製元の中身を流し込んで見た目とコリジョンを一致させる。
    // 注: InstantiateEntityJson でコンポーネントプールが再確保されうるので、
    //     複製元側のポインタもここで取り直すこと。
    if (auto* dstT = reg.try_get<Terrain>(copy))
    {
        const auto* srcT = reg.try_get<Terrain>(src);
        if (srcT && srcT->_hf && dstT->_hf && srcT->_hf->IsValid())
        {
            *dstT->_hf     = *srcT->_hf;
            dstT->resolution = dstT->_hf->Resolution();
            dstT->worldSize  = dstT->_hf->WorldSize();
            dstT->ClearDirtyRect();          // 矩形無効 = 全面再構築
            dstT->_meshDirty     = true;
            dstT->_colliderDirty = true;
            dstT->_needsSave     = true;     // 新しい .hf を書き出させる
        }
        if (srcT && srcT->_splat && srcT->_splat->IsValid())
        {
            if (!dstT->_splat) dstT->_splat = std::make_shared<TerrainSplatMap>();
            *dstT->_splat = *srcT->_splat;
            dstT->_splat->Touch();
            dstT->_splatNeedsSave = true;    // 新しい .splat を書き出させる
        }
    }
    if (auto* dstS = reg.try_get<SculptMesh>(copy))
    {
        const auto* srcS = reg.try_get<SculptMesh>(src);
        if (srcS && srcS->_data && dstS->_data && srcS->_data->IsValid())
        {
            *dstS->_data = *srcS->_data;
            dstS->_meshDirty     = true;
            dstS->_colliderDirty = true;
            dstS->_needsSave     = true;     // 新しい .smsh を書き出させる
        }
    }

    // 親は元エンティティと同じにする
    reg.get<Transform>(copy).parent = reg.get<Transform>(src).parent;
    return copy;
}

entt::entity SceneSerializer::SwapEntityModel(Scene& scene, entt::entity e,
                                              const std::string& newModelPath,
                                              const std::string& assetsDir)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(e) || !reg.all_of<NameTag>(e) || !reg.all_of<Transform>(e)
        || !reg.all_of<MeshRenderer>(e))
        return entt::null;

    json ej = SerializeEntityJson(reg, e, assetsDir);

    // モデルパスを差し替え（プリミティブ⇔モデルの変更も許可。プリミティブ専用の
    // 頂点カラーはモデルでは意味を持たないので一緒に落とす）
    ej.erase("primitive");
    ej.erase("color");
    ej.erase("meshRenderer");
    if (newModelPath.rfind("__primitive_", 0) == 0)
    {
        // Undo でプリミティブへ戻すケース（modelPath にはマーカーが入っている）
        if      (newModelPath == "__primitive_sphere__") ej["primitive"] = "sphere";
        else if (newModelPath == "__primitive_plane__")  ej["primitive"] = "plane";
        else                                              ej["primitive"] = "box";
    }
    else
    {
        std::string rel = MakeRelative(newModelPath, assetsDir);
        if (rel.empty()) rel = newModelPath;
        ej["meshRenderer"] = json{{"modelPath", rel}};
    }

    const entt::entity parent = reg.get<Transform>(e).parent;

    // 先に新エンティティを生成し、成功してから旧を消す（ロード失敗時は旧を維持）
    entt::entity ne = InstantiateEntityJson(scene, ej, assetsDir);
    if (ne == entt::null)
    {
        Logger::Error("モデル差し替え失敗（ロードエラー）: {}", newModelPath);
        return entt::null;
    }

    reg.get<Transform>(ne).parent = parent;

    // 子エンティティの親参照を新エンティティへ付け替え
    for (auto [c, tf] : reg.view<Transform>().each())
        if (c != ne && tf.parent == e)
            tf.parent = ne;

    scene.Remove(Entity(e, &reg));
    return ne;
}

// ── Prefab / サブツリー ──
// root とその全子孫を 1 つの JSON（シーンと同形式 + parent をローカル index 参照）に直列化する。
// root の親（サブツリー外）は含めない＝プレハブは自己完結する。
// 地形(.hf) / スプラット(.splat) / スカルプト(.smsh) の実データはシーン JSON に入らず
// assets 配下の外部ファイルにある。複製 / ペースト / プレハブ展開で名前が連番リネームされたのに
// パスを素通しでコピーすると、複製元と複製先が同じファイルを指す。以後どちらを彫っても
// 同じファイルへ自動保存され、両方が壊れる。
// → リネームされたら実ファイルを新しい名前へコピーし、パスもそこへ付け替える。
//   コピーできなかった場合はパスを付け替えない（＝従来どおり共有。参照切れで平坦になるより安全）。
static void RepointGeneratedAssets(json& ej, const std::string& oldName,
                                   const std::string& newName, const std::string& assetsDir)
{
    if (newName.empty() || oldName == newName) return;
    if (!ej.contains("terrain") && !ej.contains("sculpt")) return;

    namespace fs = std::filesystem;
    const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;

    auto repoint = [&](json& node, const char* key, const std::string& newRel)
    {
        if (!node.is_object() || !node.contains(key) || !node[key].is_string()) return;
        const std::string oldRel = node[key].get<std::string>();
        if (oldRel.empty() || oldRel == newRel) return;   // 未保存はそのまま（保存時に自分の名前で決まる）

        std::error_code ec;
        const fs::path src(base + oldRel);
        const fs::path dst(base + newRel);
        if (!fs::exists(src, ec)) return;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            Logger::Warn("複製時に {} を {} へコピーできませんでした（元と共有します）: {}",
                         oldRel, newRel, ec.message());
            return;
        }
        node[key] = newRel;
    };

    if (ej.contains("terrain") && ej["terrain"].is_object())
    {
        repoint(ej["terrain"], "heightmapPath", terrain::MakeHeightFieldRelPath(newName));
        repoint(ej["terrain"], "splatPath",     terrain::MakeSplatRelPath(newName));
    }
    if (ej.contains("sculpt") && ej["sculpt"].is_object())
        repoint(ej["sculpt"], "meshPath", sculpt::MakeSculptMeshRelPath(newName));
}

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
        // ★guid も書く。書かないと、この JSON から作り直す経路（Undo の復元・
        //   プレハブ適用の伝播）が「同じエンティティ」を復元しているのに guid だけ
        //   別物になる。ApplyPrefabToInstances には元々 guid を戻すコードがあるが、
        //   ここが書いていなかったので常に 0 を読んでいて一度も動いていなかった。
        //   .prefab ファイルには残したくないので SavePrefab 側で落とす。
        if (const auto* g = reg.try_get<EntityGuid>(e))
            ej["guid"] = GuidToHex(g->value);
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
// ---------------------------------------------------------------------------
// アセット参照（assets 相対パス）の走査。
//
// 参照フィールドは 20 箇所以上あり、増えるたびにここへ足す必要がある。
// 「1 箇所ずつ書く」代わりに、フィールドを訪問する関数を 1 本にして
// 付け替え(Rewrite)と数え上げ(Count)の両方から使う＝片方だけ更新して
// ズレる事故を防ぐ。
// ---------------------------------------------------------------------------
namespace {

// rel が target と一致するか、target ディレクトリ配下か。
// 前方一致だけだと "tex" が "textures/a.png" に誤爆するので区切りを見る。
bool PathMatches(const std::string& rel, const std::string& target)
{
    if (rel.empty() || target.empty()) return false;
    if (rel == target) return true;
    if (rel.size() > target.size() && rel.compare(0, target.size(), target) == 0)
        return rel[target.size()] == '/';
    return false;
}

// 一致したら新しいパスへ差し替える。oldRel がディレクトリなら配下の相対部分を保つ。
// fn(field, who, kind) の形で全参照フィールドを訪問する。
template <typename Fn>
void ForEachAssetPathField(Scene& scene, Fn&& fn)
{
    auto& reg = scene.GetRegistry();
    auto nameOf = [&reg](entt::entity e) -> std::string {
        const auto* n = reg.try_get<NameTag>(e);
        return n ? n->name : std::string("(no name)");
    };

    for (auto [e, mr] : reg.view<MeshRenderer>().each())
    {
        const std::string who = nameOf(e);
        fn(mr.modelPath,  who, "model");
        fn(mr.shaderPath, who, "shader");
        for (auto& p : mr.overrideAlbedoTexture)         fn(p, who, "albedo");
        for (auto& p : mr.overrideNormalTexture)         fn(p, who, "normal");
        for (auto& p : mr.overrideMetalRoughnessTexture) fn(p, who, "metalRoughness");
        for (auto& p : mr.materialAsset)                 fn(p, who, "material");
    }
    for (auto [e, t] : reg.view<Terrain>().each())
    {
        const std::string who = nameOf(e);
        fn(t.heightmapPath, who, "heightmap");
        fn(t.layerSetPath,  who, "terrainLayers");
        fn(t.splatPath,     who, "splat");
    }
    for (auto [e, sm] : reg.view<SculptMesh>().each()) fn(sm.meshPath, nameOf(e), "sculpt");
    for (auto [e, a]  : reg.view<AudioSource>().each()) fn(a.clipPath, nameOf(e), "audio");
    for (auto [e, sp] : reg.view<Sprite2D>().each())
    {
        const std::string who = nameOf(e);
        fn(sp.texturePath, who, "sprite");
        fn(sp.shaderPath,  who, "spriteShader");
    }
    for (auto [e, i]  : reg.view<UIImage>().each())     fn(i.texturePath, nameOf(e), "uiImage");
    for (auto [e, t]  : reg.view<UIText>().each())      fn(t.fontPath,    nameOf(e), "font");
    for (auto [e, b]  : reg.view<UIButton>().each())
    {
        const std::string who = nameOf(e);
        fn(b.hoverSound, who, "hoverSound");
        fn(b.clickSound, who, "clickSound");
    }
    for (auto [e, p]  : reg.view<ParticleEmitter>().each()) fn(p.texturePath, nameOf(e), "particle");
    for (auto [e, ap] : reg.view<UIAnimPlayer>().each())    fn(ap.clipPath,   nameOf(e), "uianim");
    for (auto [e, sa] : reg.view<SpriteAnimator>().each())  fn(sa.sheetPath,  nameOf(e), "spriteSheet");
    for (auto [e, ac] : reg.view<AnimatorController>().each()) fn(ac.graphPath, nameOf(e), "animGraph");
    for (auto [e, pl] : reg.view<PrefabLink>().each())      fn(pl.sourcePath, nameOf(e), "prefab");
    for (auto [e, ls] : reg.view<LuaScript>().each())       fn(ls.scriptPath, nameOf(e), "luaScript");

    // シーン単位
    fn(scene.GetSkyboxSettings().envMapPath, "(scene)", "envMap");
    fn(scene.GetPostSettings().lutPath,      "(scene)", "lut");
    // デカールアトラスはセッタ経由でしか触れないので個別に扱う（下の Rewrite で対応）
}

} // namespace

namespace {

// JSON を再帰で歩き、文字列値のうち target に一致するものを付け替える。
// ★キー名は見ない。アセット参照フィールドは 30 箇所以上あり、増えるたびに
//   列挙を更新し忘れて静かに漏れるため（sceneWrite.ts の表が実際そうなっていた）。
//   誤爆は「たまたまアセットパスと完全一致する非パス文字列」だけで、実質起きない。
int RewriteJsonStrings(json& node, const std::string& oldRel, const std::string& newRel)
{
    int n = 0;
    if (node.is_string())
    {
        const std::string v = node.get<std::string>();
        if (PathMatches(v, oldRel))
        {
            node = (v.size() == oldRel.size()) ? newRel : newRel + v.substr(oldRel.size());
            ++n;
        }
        return n;
    }
    if (node.is_array() || node.is_object())
        for (auto& child : node) n += RewriteJsonStrings(child, oldRel, newRel);
    return n;
}

// assets 配下で「中身が JSON でアセットを参照しうる」拡張子。
bool IsRewritableAssetFile(const std::filesystem::path& p)
{
    static const char* kExts[] = {
        ".json",          // シーン / vfx プリセット / sceneflow
        ".prefab",
        ".dxmat",         // albedo / normal / metalRoughness
        ".animfsm",       // clip パス
        ".spranim",       // texture
        ".terrainlayers", // layers[].albedo など
        ".uianim",
    };
    const std::string ext = p.extension().string();
    for (const char* e : kExts) if (ext == e) return true;
    return false;
}

} // namespace

SceneSerializer::AssetRefFileRewrite
SceneSerializer::RewriteAssetPathRefsInFiles(const std::string& assetsDir,
                                             const std::string& oldRel,
                                             const std::string& newRel,
                                             const std::string& skipAbsPath)
{
    AssetRefFileRewrite out;
    if (oldRel.empty() || oldRel == newRel || assetsDir.empty()) return out;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root(assetsDir);
    if (!fs::exists(root, ec)) return out;

    const fs::path skip = skipAbsPath.empty() ? fs::path()
                                              : fs::weakly_canonical(fs::path(skipAbsPath), ec);

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        // .texcache / .thumbcache / .autosave など「生成物の置き場」は丸ごと飛ばす。
        // 中身は再生成できるし、走査するだけ無駄で遅い。
        if (it->is_directory(ec) && it->path().filename().string().rfind('.', 0) == 0)
        {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        const fs::path& p = it->path();
        if (!IsRewritableAssetFile(p)) continue;
        if (!skip.empty() && fs::weakly_canonical(p, ec) == skip) continue;

        json doc;
        try
        {
            std::ifstream ifs(p, std::ios::binary);
            if (!ifs) { out.failed.push_back(MakeRelative(p.string(), assetsDir)); continue; }
            ifs >> doc;
        }
        catch (const std::exception&)
        {
            // JSON でないファイル（拡張子が同じだけ）は黙って飛ばす。
            // ここでエラーを積むと「壊れていないのに失敗した」ように見える。
            continue;
        }

        const int changed = RewriteJsonStrings(doc, oldRel, newRel);
        if (changed == 0) continue;

        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs)
        {
            out.failed.push_back(MakeRelative(p.string(), assetsDir));
            continue;
        }
        ofs << doc.dump(2);
        ++out.filesChanged;
        out.refsChanged += changed;
        if (static_cast<int>(out.files.size()) < kMaxReportedFiles)
            out.files.push_back(MakeRelative(p.string(), assetsDir));
    }
    return out;
}

int SceneSerializer::RewriteAssetPathRefs(Scene& scene, const std::string& oldRel,
                                          const std::string& newRel)
{
    if (oldRel.empty() || oldRel == newRel) return 0;
    int n = 0;
    auto rewrite = [&](std::string& field, const std::string&, const char*) {
        if (!PathMatches(field, oldRel)) return;
        // ディレクトリ配下なら、配下の相対部分を保ったまま付け替える
        field = (field.size() == oldRel.size()) ? newRel
                                                : newRel + field.substr(oldRel.size());
        ++n;
    };
    ForEachAssetPathField(scene, rewrite);

    // デカールアトラス（getter が const 参照なので個別）
    {
        std::string atlas = scene.GetDecalAtlasPath();
        if (PathMatches(atlas, oldRel))
        {
            atlas = (atlas.size() == oldRel.size()) ? newRel
                                                    : newRel + atlas.substr(oldRel.size());
            scene.SetDecalAtlasPath(atlas);
            ++n;
        }
    }
    return n;
}

int SceneSerializer::CountAssetPathRefs(const Scene& scene, const std::string& rel,
                                        std::vector<std::string>* outWho, int maxNames)
{
    if (rel.empty()) return 0;
    int n = 0;
    // ForEachAssetPathField は非 const 参照を配るので、数えるだけでも非 const が要る。
    // 実際には書き換えないので const_cast で足りる（Scene の実体は const ではない）。
    auto& mutableScene = const_cast<Scene&>(scene);
    auto count = [&](std::string& field, const std::string& who, const char* kind) {
        if (!PathMatches(field, rel)) return;
        ++n;
        if (outWho && static_cast<int>(outWho->size()) < maxNames)
            outWho->push_back(who + ": " + kind);
    };
    ForEachAssetPathField(mutableScene, count);

    if (PathMatches(scene.GetDecalAtlasPath(), rel))
    {
        ++n;
        if (outWho && static_cast<int>(outWho->size()) < maxNames)
            outWho->push_back("(scene): decalAtlas");
    }
    return n;
}

entt::entity SceneSerializer::InstantiateSubtree(Scene& scene, const std::string& jsonStr,
                                                 const std::string& assetsDir,
                                                 std::vector<entt::entity>* outAll,
                                                 bool keepGuids)
{
    json root;
    try { root = json::parse(jsonStr); }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON の解析に失敗しました（サブツリー）: {}", e.what());
        return entt::null;
    }
    // 旧クリップボード形式（単一エンティティの JSON オブジェクト）も受け付ける
    if (root.is_object() && !root.contains("entities") && root.contains("name"))
        root = json{{"entities", json::array({std::move(root)})}};
    if (!root.contains("entities") || !root["entities"].is_array() || root["entities"].empty())
        return entt::null;

    auto& reg = scene.GetRegistry();

    // 1パス目: 生成（名前は重複しないよう連番付与）
    std::vector<entt::entity> created;
    created.reserve(root["entities"].size());
    for (const auto& ej : root["entities"])
    {
        entt::entity e = entt::null;
        try
        {
            json copy = ej;
            const std::string oldName = ej.value("name", std::string("Unnamed"));
            const std::string newName = MakeUniqueName(scene, oldName);
            copy["name"] = newName;
            // ★GUID は JSON からは渡さない。InstantiateEntityJson は複製・貼り付け・
            //   モデル差し替えからも呼ばれるので、そちらで guid が重複しないよう常に消す。
            //   keepGuids のときは生成後に下で emplace し直す（この関数の中だけで完結させる）。
            copy.erase("guid");
            copy.erase("parentGuid");
            // 地形/スカルプトの外部ファイルを複製先専用にする（共有バグの根治はここ）
            RepointGeneratedAssets(copy, oldName, newName, assetsDir);
            e = InstantiateEntityJson(scene, copy, assetsDir);
        }
        catch (const std::exception& ex)
        {
            Logger::Error("サブツリーのエンティティ [{}] をスキップしました（値不正）: {}",
                          created.size(), ex.what());
        }
        // 同じエンティティを作り直している経路（Undo の復元 / プレハブ適用の伝播）だけ
        // guid を戻す。ここを忘れると、参照が guid を向いた瞬間に Undo とプレハブ適用の
        // たびに黙って参照が切れる。
        if (keepGuids && e != entt::null)
        {
            if (const uint64_t g = GuidFromJson(ej, "guid"); g != 0)
                reg.emplace_or_replace<EntityGuid>(e, EntityGuid{g});
        }
        created.push_back(e);
    }

    // 2パス目: 親子関係の復元（root はサブツリー外の親を持たない）
    // parent は ApplySceneJson と同じく整数型チェックしてから読む（旧データで
    // null/文字列が入っていると get<int> が type_error を投げてフレーム境界処理ごと落ちる）
    size_t idx = 0;
    for (const auto& ej : root["entities"])
    {
        entt::entity e = created[idx++];
        if (e == entt::null || !ej.is_object() || !ej.contains("parent")) continue;
        if (!ej["parent"].is_number_integer()) continue;
        int p = ej["parent"].get<int>();
        if (p < 0 || p >= static_cast<int>(created.size())) continue;
        entt::entity parent = created[static_cast<size_t>(p)];
        if (parent != entt::null && parent != e && reg.all_of<Transform>(e))
            reg.get<Transform>(e).parent = parent;
    }

    // 3パス目: サブツリー内部への名前参照を連番リネーム後の名前へ付け替える。
    // Lua の entity プロパティ / Trigger の filter・actions[].target は名前文字列で
    // 参照するため、リネームで内部参照が切れる（外部エンティティへの参照はそのまま）。
    std::unordered_map<std::string, std::string> renamed;
    idx = 0;
    for (const auto& ej : root["entities"])
    {
        entt::entity e = created[idx++];
        if (e == entt::null || !ej.is_object() || !reg.all_of<NameTag>(e)) continue;
        std::string oldName = ej.value("name", std::string());
        const std::string& newName = reg.get<NameTag>(e).name;
        if (!oldName.empty() && newName != oldName)
            renamed.emplace(std::move(oldName), newName);   // 同名は先勝ち（root 優先）
    }
    if (!renamed.empty())
    {
        auto remap = [&](std::string& ref)
        {
            auto it = renamed.find(ref);
            if (it != renamed.end()) ref = it->second;
        };
        for (auto e : created)
        {
            if (e == entt::null) continue;
            if (reg.all_of<LuaScript>(e))
                for (auto& p : reg.get<LuaScript>(e).props)
                    if (p.type == ScriptPropType::Entity) remap(p.str);
            if (reg.all_of<Trigger>(e))
            {
                auto& tr = reg.get<Trigger>(e);
                remap(tr.filter);
                for (auto& a : tr.actions) remap(a.target);
            }
        }
    }

    if (outAll) *outAll = created;
    return created.empty() ? entt::null : created[0];
}

std::vector<entt::entity> SceneSerializer::TopmostRoots(const Scene& scene,
                                                        const std::vector<entt::entity>& entities)
{
    const auto& reg = scene.GetRegistry();
    std::unordered_set<entt::entity> set(entities.begin(), entities.end());
    std::vector<entt::entity> out;
    for (auto e : entities)
    {
        if (!reg.valid(e) || !reg.all_of<Transform>(e)) continue;
        bool covered = false;
        entt::entity p = reg.get<Transform>(e).parent;
        int guard = 0;   // 親参照が循環していても抜けられるように
        while (p != entt::null && reg.valid(p) && guard++ < 1024)
        {
            if (set.count(p)) { covered = true; break; }
            p = reg.all_of<Transform>(p) ? reg.get<Transform>(p).parent : entt::null;
        }
        if (!covered) out.push_back(e);
    }
    return out;
}

entt::entity SceneSerializer::DuplicateSubtree(Scene& scene, entt::entity src,
                                               const std::string& assetsDir,
                                               std::vector<entt::entity>* outAll)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(src) || !reg.all_of<NameTag>(src) || !reg.all_of<Transform>(src))
        return entt::null;

    const entt::entity parent = reg.get<Transform>(src).parent;
    std::string subtreeJson = SerializeSubtree(scene, src, assetsDir);
    if (subtreeJson.empty()) return entt::null;

    entt::entity copy = InstantiateSubtree(scene, subtreeJson, assetsDir, outAll);
    if (copy == entt::null) return entt::null;

    // 親は元エンティティと同じにする（子孫の親子は InstantiateSubtree が復元済み）
    reg.get<Transform>(copy).parent = parent;
    return copy;
}

namespace
{

// .prefab へ書き出す JSON から PrefabLink を落とす。
// 残すと「自分自身を指すプレハブ」ができ、展開のたびにリンクが二重になる。
void StripPrefabLinks(json& prefabJson)
{
    if (!prefabJson.contains("entities") || !prefabJson["entities"].is_array()) return;
    for (auto& ej : prefabJson["entities"])
        if (ej.is_object()) ej.erase("prefabLink");
}

// assets ルートからの相対パスへ正規化（区切りは '/' に統一）。
// assetsDir の外にあるファイルは絶対パスのまま返す（相対にできないため）。
std::string ToAssetRelative(const std::string& absPath, const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    std::string abs  = fs::path(absPath).lexically_normal().string();
    std::string base = fs::path(assetsDir).lexically_normal().string();
    std::replace(abs.begin(), abs.end(), '\\', '/');
    std::replace(base.begin(), base.end(), '\\', '/');
    if (!base.empty() && base.back() != '/') base += '/';
    return (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
}

// プレハブ JSON を読む（VFS 優先 → ディスクフォールバック。InstantiatePrefab と同じ順序）。
bool ReadPrefabJson(const std::string& absPath, json& out)
{
    auto b = vfs::ReadAssetAbs(absPath);
    if (b.empty())
    {
        std::ifstream ifs(absPath, std::ios::binary);
        if (!ifs.is_open()) return false;
        std::stringstream ss; ss << ifs.rdbuf();
        const std::string s = ss.str();
        b.assign(s.begin(), s.end());
    }
    out = json::parse(b.begin(), b.end(), nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded() && out.is_object() && out.contains("entities")
        && out["entities"].is_array();
}

// エンティティ 1 個ぶんの JSON を比較して差分を積む。
// name は展開時に連番が付くので比較しない（毎回全インスタンスが差分だらけになる）。
// parent はローカル index なので構造が同じなら一致する = 比較対象に残してよい。
void DiffEntityJson(const json& mine, const json& base, int index, const std::string& name,
                    std::vector<SceneSerializer::PrefabOverride>& out)
{
    // ★guid / parentGuid を除外する理由: インスタンスは展開時に guid を振り直すので、
    //   .prefab 側の値と必ず食い違う。除外しないと全インスタンスに「guid が違う」という
    //   偽の差分が出続け、本当の上書きが埋もれる。
    //   name / prefabLink も同じ理由（展開時の連番リネームで毎回全差分になる）。
    static const char* const kIgnored[] = {"name", "prefabLink", "guid", "parentGuid"};
    auto ignored = [&](const std::string& key)
    {
        for (const char* k : kIgnored) if (key == k) return true;
        return false;
    };

    for (auto it = mine.begin(); it != mine.end(); ++it)
    {
        if (ignored(it.key())) continue;
        const auto bit = base.find(it.key());
        if (bit == base.end())
        {
            out.push_back({index, name, it.key(), "(追加)"});
            continue;
        }
        if (*bit == it.value()) continue;

        // コンポーネントがオブジェクトならフィールド単位まで降りる（どこを触ったか分かるように）
        if (it.value().is_object() && bit->is_object())
        {
            for (auto f = it.value().begin(); f != it.value().end(); ++f)
            {
                const auto bf = bit->find(f.key());
                if (bf == bit->end() || *bf != f.value())
                    out.push_back({index, name, it.key(), f.key()});
            }
            // ベース側にしか無いフィールドは「消された」ではなく既定値化なので拾わない
            // （反射シリアライズは既定値も必ず書くため、実際にはここへ来ない）
        }
        else
        {
            out.push_back({index, name, it.key(), "(値)"});
        }
    }
    for (auto it = base.begin(); it != base.end(); ++it)
    {
        if (ignored(it.key())) continue;
        if (!mine.contains(it.key())) out.push_back({index, name, it.key(), "(削除)"});
    }
}

// mine（インスタンスの今の姿）が oldBase から動かしたところだけを newBase の上へ載せ直す。
// いわゆる 3-way マージ。oldBase が要るのは、これが無いと「インスタンス側が上書きした」のか
// 「プレハブ側が変わった」のかを区別できず、どちらかを必ず捨てることになるため。
json Merge3WayEntity(const json& mine, const json& oldBase, const json& newBase)
{
    // インスタンス固有で、プレハブ側の値に上書きされては困るもの。
    //   name       : 名前ベースの参照（Lua の entity プロパティ / Trigger）が切れる
    //   guid       : 安定 ID が変わると parentGuid などの参照が切れる
    //   prefabLink : .prefab 側は Strip 済みなので、こちらから持ち込まないと紐付けが外れる
    static const char* const kInstanceOwned[] = {"name", "guid", "parentGuid", "prefabLink"};
    auto instanceOwned = [](const std::string& key)
    {
        for (const char* k : kInstanceOwned) if (key == k) return true;
        return false;
    };

    json out = newBase;
    for (const char* k : kInstanceOwned)
    {
        const auto it = mine.find(k);
        if (it != mine.end()) out[k] = *it;
        else                  out.erase(k);
    }

    for (auto it = mine.begin(); it != mine.end(); ++it)
    {
        const std::string& key = it.key();
        // parent はサブツリー内のローカル index。構造が同じなら三者一致するので触らない
        if (key == "parent" || instanceOwned(key)) continue;

        const auto ob = oldBase.find(key);
        if (ob == oldBase.end()) { out[key] = it.value(); continue; }  // インスタンスが足した
        if (*ob == it.value()) continue;                               // 触っていない→プレハブ側を採用

        const auto nb = out.find(key);
        if (it.value().is_object() && ob->is_object() && nb != out.end() && nb->is_object())
        {
            // フィールド単位まで降りる。「位置だけ動かしたインスタンス」が
            // プレハブ側の scale 変更をちゃんと受け取れるようにするため
            for (auto f = it.value().begin(); f != it.value().end(); ++f)
            {
                const auto obf = ob->find(f.key());
                if (obf == ob->end() || *obf != f.value()) (*nb)[f.key()] = f.value();
            }
        }
        else
        {
            out[key] = it.value();
        }
    }

    // インスタンス側で消したコンポーネントは消したままにする
    for (auto it = oldBase.begin(); it != oldBase.end(); ++it)
        if (!mine.contains(it.key())) out.erase(it.key());

    return out;
}

// entities 配列ぜんたいの 3-way マージ。
json Merge3WaySubtree(const json& mine, const json& oldBase, const json& newBase)
{
    const size_t nc = (std::min)({mine.size(), oldBase.size(), newBase.size()});
    json out = json::array();
    for (size_t i = 0; i < nc; ++i)
        out.push_back(Merge3WayEntity(mine[i], oldBase[i], newBase[i]));

    // プレハブ側で増えた子。merged での index が newBase での index と同じなので parent は直さなくてよい。
    // なお mine の方が短い（インスタンスが子を消した）場合はここで復活する。
    // 「プレハブに戻ってくる」方が「プレハブ側の追加を黙って落とす」より事故が小さいので、これでよい。
    for (size_t i = nc; i < newBase.size(); ++i)
        out.push_back(newBase[i]);

    // インスタンス側で足した子（プレハブ配下へ手で足したもの）。ここだけ index がずれるので直す
    for (size_t i = nc; i < mine.size(); ++i)
    {
        json ej = mine[i];
        if (ej.contains("parent") && ej["parent"].is_number_integer())
        {
            const int p = ej["parent"].get<int>();
            if (p >= 0)
                ej["parent"] = static_cast<int>(static_cast<size_t>(p) < nc
                                                    ? static_cast<size_t>(p)
                                                    : newBase.size() + (static_cast<size_t>(p) - nc));
        }
        out.push_back(std::move(ej));
    }
    return out;
}

// 他のインスタンスへプレハブの変更を配る。各インスタンスの上書きは 3-way マージで残す。
int MergePrefabInstances(Scene& scene, const std::string& sourcePath, const json& oldBase,
                         const json& newBase, const std::string& assetsDir, entt::entity except)
{
    auto& reg = scene.GetRegistry();
    // 作り直すのでビューを回しながらだと壊れる。先に対象を集める
    std::vector<entt::entity> targets;
    for (auto [e, link] : reg.view<const PrefabLink>().each())
        if (link.sourcePath == sourcePath && e != except) targets.push_back(e);

    int n = 0;
    for (entt::entity root : targets)
    {
        if (!reg.valid(root)) continue;

        json mine = json::parse(SceneSerializer::SerializeSubtree(scene, root, assetsDir),
                                nullptr, /*allow_exceptions=*/false);
        if (mine.is_discarded() || !mine.contains("entities") || !mine["entities"].is_array())
            continue;

        json merged = mine;
        merged["entities"] = Merge3WaySubtree(mine["entities"], oldBase, newBase);
        if (merged["entities"].empty()) continue;

        const entt::entity externalParent =
            reg.all_of<Transform>(root) ? reg.get<Transform>(root).parent : entt::null;

        // ★先に消してから作る。逆にすると MakeUniqueName が名前の重複を避けて連番を足し、
        //   伝播のたびに Barrel → Barrel_1 → Barrel_1_1 と名前が伸びて、名前ベースの参照が切れる。
        //   中身は merged（parse 済みの JSON）由来なので、作る側が丸ごと失敗する経路は無い。
        std::vector<entt::entity> old{root};
        for (size_t head = 0; head < old.size(); ++head)
            for (auto [child, tf] : reg.view<const Transform>().each())
                if (tf.parent == old[head] && std::find(old.begin(), old.end(), child) == old.end())
                    old.push_back(child);
        for (auto it = old.rbegin(); it != old.rend(); ++it)
            if (reg.valid(*it)) reg.destroy(*it);

        // ★keepGuids=true。ここは「同じインスタンスの作り直し」なので guid を引き継ぐ。
        //   以前は生成後に自前で戻していたが、その元データ（SerializeSubtree の出力）が
        //   guid を書いていなかったので常に 0 を読んでいて、伝播のたびに全インスタンスの
        //   guid が回っていた（参照が guid を向いた瞬間に効く地雷だった）。
        std::vector<entt::entity> created;
        const entt::entity newRoot = SceneSerializer::InstantiateSubtree(
            scene, merged.dump(), assetsDir, &created, /*keepGuids*/ true);
        if (newRoot == entt::null) continue;

        if (externalParent != entt::null && reg.valid(externalParent)
            && reg.all_of<Transform>(newRoot))
            reg.get<Transform>(newRoot).parent = externalParent;
        ++n;
    }
    return n;
}

} // namespace

bool SceneSerializer::ComputePrefabOverrides(const Scene& scene, entt::entity root,
                                             const std::string& assetsDir,
                                             std::vector<PrefabOverride>& out)
{
    out.clear();
    const auto& reg = scene.GetRegistry();
    if (!reg.valid(root) || !reg.all_of<PrefabLink>(root)) return false;

    const std::string abs = assetsDir + reg.get<PrefabLink>(root).sourcePath;
    json base;
    if (!ReadPrefabJson(abs, base)) return false;

    json mine = json::parse(SerializeSubtree(scene, root, assetsDir), nullptr, false);
    if (mine.is_discarded() || !mine.contains("entities")) return false;

    const auto& mineArr = mine["entities"];
    const auto& baseArr = base["entities"];
    const size_t n = (std::min)(mineArr.size(), baseArr.size());
    for (size_t i = 0; i < n; ++i)
    {
        const std::string nm = mineArr[i].value("name", std::string("?"));
        DiffEntityJson(mineArr[i], baseArr[i], static_cast<int>(i), nm, out);
    }
    // 要素数が違う = 子を足した/消した。フィールド差分より重い変更なので明示的に出す
    for (size_t i = n; i < mineArr.size(); ++i)
        out.push_back({static_cast<int>(i), mineArr[i].value("name", std::string("?")),
                       "(構成)", "(子を追加)"});
    for (size_t i = n; i < baseArr.size(); ++i)
        out.push_back({static_cast<int>(i), baseArr[i].value("name", std::string("?")),
                       "(構成)", "(子を削除)"});
    return true;
}

bool SceneSerializer::ApplyPrefabInstance(Scene& scene, entt::entity root,
                                          const std::string& assetsDir, int* outPropagated)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(root) || !reg.all_of<PrefabLink>(root)) return false;
    const std::string rel = reg.get<PrefabLink>(root).sourcePath;
    if (rel.empty()) return false;

    // 上書きする前に元の中身を控える。これが 3-way マージの base になる。
    // 読めない（新規プレハブ）ときは配る相手もいないので、そのまま書くだけ。
    json oldBase;
    const bool haveOld = ReadPrefabJson(assetsDir + rel, oldBase);

    if (!SavePrefab(scene, root, assetsDir + rel, assetsDir)) return false;

    if (haveOld)
    {
        json newBase;
        if (ReadPrefabJson(assetsDir + rel, newBase))
        {
            const int n = MergePrefabInstances(scene, rel, oldBase["entities"],
                                               newBase["entities"], assetsDir, root);
            if (outPropagated) *outPropagated = n;
        }
    }
    return true;
}

entt::entity SceneSerializer::RevertPrefabInstance(Scene& scene, entt::entity root,
                                                   const std::string& assetsDir)
{
    auto& reg = scene.GetRegistry();
    if (!reg.valid(root) || !reg.all_of<PrefabLink>(root)) return entt::null;
    const std::string rel = reg.get<PrefabLink>(root).sourcePath;

    // サブツリーの外側にいる親だけ引き継ぐ（戻した拍子に階層のトップへ飛ばさないため）。
    // Transform や UIRect は「元に戻す」の意味どおりプレハブの値へ戻す（Unity と同じ）。
    const entt::entity externalParent =
        reg.all_of<Transform>(root) ? reg.get<Transform>(root).parent : entt::null;

    // 先に新しい方を作る。失敗しても元のインスタンスが無傷で残るようにするため。
    std::vector<entt::entity> created;
    const entt::entity newRoot = InstantiatePrefab(scene, assetsDir + rel, assetsDir, &created);
    if (newRoot == entt::null) return entt::null;

    // 古いサブツリーを削除（Scene::Remove はカスケードしないので子から順に消す）
    std::vector<entt::entity> old;
    old.push_back(root);
    for (size_t head = 0; head < old.size(); ++head)
    {
        for (auto [child, tf] : reg.view<const Transform>().each())
            if (tf.parent == old[head]
                && std::find(old.begin(), old.end(), child) == old.end())
                old.push_back(child);
    }
    for (auto it = old.rbegin(); it != old.rend(); ++it)
        if (reg.valid(*it)) reg.destroy(*it);

    if (externalParent != entt::null && reg.valid(externalParent)
        && reg.all_of<Transform>(newRoot))
        reg.get<Transform>(newRoot).parent = externalParent;

    return newRoot;
}

int SceneSerializer::RefreshPrefabInstances(Scene& scene, const std::string& sourcePath,
                                            const std::string& assetsDir, entt::entity except)
{
    auto& reg = scene.GetRegistry();
    // Revert は destroy/create するのでビューを回しながらだと壊れる。先に対象を集める
    std::vector<entt::entity> targets;
    for (auto [e, link] : reg.view<const PrefabLink>().each())
        if (link.sourcePath == sourcePath && e != except) targets.push_back(e);

    int n = 0;
    for (entt::entity e : targets)
        if (reg.valid(e) && RevertPrefabInstance(scene, e, assetsDir) != entt::null) ++n;
    return n;
}

bool SceneSerializer::SavePrefab(const Scene& scene, entt::entity root,
                                 const std::string& filePath, const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    std::string s = SerializeSubtree(scene, root, assetsDir);
    if (s.empty()) return false;

    // 自己参照リンクを落としてから書く（下の StripPrefabLinks のコメント参照）。
    // guid も落とす: SerializeSubtree は Undo/プレハブ伝播のために guid を書くが、
    // .prefab は「型」であってインスタンスではないので、特定インスタンスの guid が
    // ファイルに残ると git の差分が無意味に動くし、読む人を誤解させる。
    // （InstantiateSubtree も既定で guid を捨てるので、残っていても動作は変わらない）
    {
        json j = json::parse(s, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded())
        {
            StripPrefabLinks(j);
            if (j.contains("entities") && j["entities"].is_array())
                for (auto& ej : j["entities"]) { ej.erase("guid"); ej.erase("parentGuid"); }
            s = j.dump(2);
        }
    }

    fs::path dir = fs::path(filePath).parent_path();
    if (!dir.empty()) fs::create_directories(dir);

    std::ofstream ofs(filePath);
    if (!ofs.is_open())
    {
        Logger::Error("プレハブの書き込みに失敗しました: {}", filePath);
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
    // VFS 経由で読む（ゲームモード: pak 復号。エディタ: ディスク）。
    std::string jsonStr;
    auto b = vfs::ReadAssetAbs(filePath);
    if (!b.empty())
    {
        jsonStr.assign(b.begin(), b.end());
    }
    else
    {
        // ディスクフォールバック
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs.is_open())
        {
            Logger::Error("プレハブを開けません: {}", filePath);
            return entt::null;
        }
        std::stringstream ss; ss << ifs.rdbuf();
        jsonStr = ss.str();
    }

    const entt::entity root = InstantiateSubtree(scene, jsonStr, assetsDir, outAll);
    // 元 .prefab への紐付けをルートへ張る（Apply/Revert/差分表示はこれが起点）。
    // ここで一括して付けるので、エディタ D&D / MCP / ネットワーク spawn のどの経路でも効く。
    if (root != entt::null)
    {
        auto& reg = scene.GetRegistry();
        if (reg.valid(root))
            reg.emplace_or_replace<PrefabLink>(root, PrefabLink{ToAssetRelative(filePath, assetsDir)});
    }
    return root;
}

} // namespace dx12e
