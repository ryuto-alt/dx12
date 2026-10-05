#include "scene/SceneSerializer.h"
#include "core/AtomicFileJson.h"
#include "core/SceneBackup.h"
#include "scene/SceneFormatV2.h"
#include "scene/ScenePartition.h"   // シーンの分割保存（§4.3）
#include "scene/Scene.h"
#include "ecs/Components.h"
#include "physics/ColliderShape.h"         // ConvexHullCollider の頂点の間引き（collider::ReduceHullPoints。ヘッダのみ）
#include "ecs/ComponentMeta.h"             // entt::meta フィールド反映（シリアライズの単一ソース）
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "core/Logger.h"
#include "core/PathResolver.h"
#include "core/vfs/Vfs.h"
#include "engine/ecs/ComponentRegistry.h"  // Phase 1: コア部品の直列化をレジストリ走査へ
#include "terrain/TerrainIO.h"             // 複製時に .hf のパスを振り直す
#include "terrain/SculptIO.h"              // 複製時に .smsh のパスを振り直す
#include "renderer/foliage/FoliageIO.h"
#include "renderer/foliage/FoliageLayerOps.h"   // 植生: .dxfoliage の書き出し / 複製時のパス振り直し
#include "renderer/foliage/SceneWind.h"
#include "ecs/InstanceGroup.h"
#include "scene/InstanceGroupIO.h"   // インスタンス群のサイドカー
#include "scene/MissingModel.h"      // モデルが読めなかったエンティティの描画データを保持して書き戻す

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <Windows.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <optional>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cctype>
#include <random>    // エンティティ GUID の生成
#include <cstdio>     // GUID の hex 整形
#include <chrono>      // 読み込み時間の内訳ログ
#include <functional>

using json = nlohmann::json;
using namespace DirectX;

namespace dx12e
{

// シーン JSON の "material" ブロックから透明のオーバーライドを読む。
// ★読み込みは 2 経路（InstantiateEntityJson / ApplyOverrides）あるので必ずここへ一本化すること。
//   キーが無ければ何もしない＝既存シーンは全部 OPAQUE のまま。
static void ReadAlphaOverrides(const nlohmann::json& mj, MeshRenderer& mr)
{
    if (mj.contains("alphaMode"))
    {
        const auto& v = mj["alphaMode"];
        int mode = -1;
        if (v.is_string())
        {
            std::string m = v.get<std::string>();
            std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (m == "opaque")      mode = 0;
            else if (m == "mask")   mode = 1;
            else if (m == "blend")  mode = 2;
            else if (m == "auto" || m.empty()) mode = -1;
        }
        else if (v.is_number_integer()) mode = v.get<int>();
        mr.alphaModeOverride = (mode >= -1 && mode <= 2) ? mode : -1;
    }
    if (mj.contains("alphaCutoff")) mr.alphaCutoffOverride = mj["alphaCutoff"].get<f32>();
    if (mj.contains("opacity"))     mr.opacity             = mj["opacity"].get<f32>();
}

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

// シーン JSON の "material" ブロックから自己発光のオーバーライドを読む。
// 透明と同じく読み込み経路が 2 つあるのでここへ一本化する。
// キーが無ければ何もしない＝既存シーンは全部「継承（＝無発光）」のまま。
static void ReadEmissiveOverrides(const nlohmann::json& mj, MeshRenderer& mr)
{
    if (mj.contains("emissiveColor"))
        mr.overrideEmissiveColor = DeserializeFloat3(mj["emissiveColor"], mr.overrideEmissiveColor);
    if (mj.contains("emissiveIntensity"))
        mr.overrideEmissiveIntensity = mj["emissiveIntensity"].get<f32>();
    // マテリアル AO の強さの上書き（無ければ <0 = モデルに従う）
    if (mj.contains("aoStrength"))
        mr.overrideAoStrength = std::clamp(mj["aoStrength"].get<f32>(), 0.0f, 1.0f);
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

// ---- シーン形式 v2 の既定値表の初期値を作るための「既定構築 → 実際の保存処理」プローブ ----
// 反射登録（MakeReflectedInfo）と同じ場所で積むので、コンポーネントを足しても一覧の更新漏れが起きない。
// 表そのものは凍結データ（src/scene/scene_defaults_v2.json）で、これは生成ツール（SceneSerializer::BuildDefaultsTableV2Json）専用。
static json SerializeEntityJson(const entt::registry& reg, entt::entity entity, const std::string& assetsDir);

// MeshRenderer があるときだけ書かれる描画まわりのキー（SerializeEntityJson の MeshRenderer 節）。
// モデルが読めなかったエンティティはこれを MissingModel に取っておき、保存時に書き戻す。
static const char* const kRendererKeys[] = {
    "meshRenderer", "material", "color", "uvTiling", "uvScroll", "flipbook",
    "shader", "shaderAlphaBlend", "shaderEffectValue", "shaderParams", "shaderParamsB",
    "materialTextureOverrides", "materialAssets",
};

struct DefaultsProbe { std::string key; std::function<json()> run; };
static std::vector<DefaultsProbe>& DefaultsProbes()
{
    static std::vector<DefaultsProbe> v;
    return v;
}

template <typename T>
static RuntimeComponentInfo MakeReflectedInfo(const char* typeName, const char* jsonKey,
                                              bool replaceExisting)
{
    const std::string key = jsonKey;
    DefaultsProbes().push_back({ key, [key]() -> json {
        entt::registry reg;
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ "probe" });
        reg.emplace<Transform>(e);
        reg.emplace<T>(e);
        json ej = SerializeEntityJson(reg, e, std::string{});
        return ej.contains(key) ? ej[key] : json();
    } });
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
    R.Register(MakeReflectedInfo<MeshCollider>("MeshCollider", "meshCollider", true));
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
    R.Register(MakeReflectedInfo<Brain>("Brain", "brain", true));
    // リバーブ域（音の担当）。純フィールドなので反射で直列化する
    R.Register(MakeReflectedInfo<AudioReverbZone>("AudioReverbZone", "audioReverbZone", true));
    // 仮想ジオメトリ（.vgeo）。パスと有効フラグだけなので反射で直列化する
    R.Register(MakeReflectedInfo<VirtualGeometry>("VirtualGeometry", "virtualGeometry", true));
    // 植生（F1）。パラメータは反射で直列化する。インスタンスの実体は .dxfoliage（シーン JSON へ直書きしない）。
    //   ★保存のたびに、編集済み（_needsSave）または未保存（instancePath 空）の実体をここで書き出してからパスを JSON へ出す。
    {
        auto info = MakeReflectedInfo<FoliageLayer>("FoliageLayer", "foliageLayer", true);
        auto generic = info.serialize;
        info.serialize = [generic](const entt::registry& reg, entt::entity e, json& ej)
        {
            if (!reg.all_of<FoliageLayer>(e)) return;
            FoliageLayer& l = const_cast<entt::registry&>(reg).get<FoliageLayer>(e);
            if (l._set && (l._needsSave || l.instancePath.empty() || l._set.get() != l._diskSet))
            {
                const NameTag* nt = reg.try_get<NameTag>(e);
                std::string err;
                if (!foliage::FlushSidecar(l, nt ? nt->name : std::string("Foliage"), PathResolver::AssetsDir(), &err))
                    Logger::Warn("植生: .dxfoliage を書き出せませんでした（{}）", err);
            }
            generic(reg, e, ej);
        };
        R.Register(std::move(info));
    }
    // インスタンス群（docs/SCENE_FORMAT_DESIGN.md §4.1 の 4-1）。JSON には個数だけを書く。実体（InstanceSet）は:
    //   ・ファイル保存（Save）の直列化中 … 収集器へ (guid, 実体) を積む。Save が <シーン>.inst/<guid>.jsonl へ書く。
    //   ・それ以外（Play のスナップショット / 複製 / Undo の JSON）… 台帳へ預けて "mem":id を書く（配列の再直列化をしない）。
    //   読み込みは "mem" があれば台帳から、無ければ読み込み中のシーンの隣のサイドカーから。
    {
        RuntimeComponentInfo info;
        info.typeName = "InstanceGroup";
        info.source   = ComponentSource::Core;
        info.serialize = [](const entt::registry& reg, entt::entity e, json& ej)
        {
            const auto* g = reg.try_get<InstanceGroup>(e);
            if (!g) return;
            json out = json::object();
            out["count"] = g->_set ? g->_set->Count() : 0u;
            if (g->_set)
            {
                if (auto* col = instgroup::CurrentCollector())
                {
                    const auto* gu = reg.try_get<EntityGuid>(e);
                    if (gu && gu->value != 0) col->sets.emplace_back(FormatEntityGuidHex(gu->value), g->_set);
                }
                else
                {
                    instgroup::StoreRegister(g->_set);
                    out["mem"] = g->_set->id;
                }
            }
            ej["instanceGroup"] = std::move(out);
        };
        info.deserialize = [](entt::registry& reg, entt::entity e, const json& ej)
        {
            if (!ej.contains("instanceGroup") || !ej["instanceGroup"].is_object()) return;
            const json& cj = ej["instanceGroup"];
            instgroup::InstanceSetPtr set;
            if (cj.contains("mem") && cj["mem"].is_number_unsigned())
                set = instgroup::StoreFind(cj["mem"].get<u64>());
            if (!set)
            {
                const std::string& scenePath = instgroup::CurrentLoadScenePath();
                // .prefab は guid を落として保存するので、サイドカー名は "sidecar" に残す（SavePrefab が書く）。
                std::string key;
                if (cj.contains("sidecar") && cj["sidecar"].is_string()) key = cj["sidecar"].get<std::string>();
                else
                {
                    const uint64_t g = (ej.contains("guid") && ej["guid"].is_string()) ? ParseEntityGuidHex(ej["guid"].get<std::string>()) : 0ull;
                    if (g != 0) key = FormatEntityGuidHex(g);
                }
                if (!scenePath.empty() && !key.empty() && key.find_first_of("/\\.:") == std::string::npos)
                    set = instgroup::LoadSidecar(scenePath, key);
            }
            if (!set)
            {
                const u64 want = cj.value("count", 0ull);
                if (want > 0)
                    Logger::Warn("インスタンス群のデータが見つかりません（{} 個の想定）: {}", want, ej.value("name", std::string("?")));
                set = instgroup::NewSet(std::vector<instgroup::InstanceTRS>{});
            }
            reg.emplace_or_replace<InstanceGroup>(e, InstanceGroup{ set });
        };
        R.Register(std::move(info));
    }
    // 水面（W1）。純フィールドなので反射で直列化する（波は種 + パラメータから決定論的に合成するので配列は持たない）。
    R.Register(MakeReflectedInfo<WaterBody>("WaterBody", "waterBody", true));
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

    // ---- [H] エディタ専用フラグ（フェーズ 1b）と兄弟順 ----
    // ★立っている（0 以外の）時だけキーを書く＝キーが無い旧シーンは従来どおり、フラグの無いシーンの JSON は 1 バイトも変わらない。
    //   editorHidden / editorLocked はエディタだけが見る（ゲームランタイムは読んでも描画に使わない）。
    R.Register({ "EditorFlags", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<EditorHidden>(entity)) ej["editorHidden"] = true;
            if (reg.all_of<EditorLocked>(entity)) ej["editorLocked"] = true;
            if (reg.all_of<EditorFolder>(entity)) ej["editorFolder"] = true;
            if (reg.all_of<PartitionRoot>(entity)) ej["partition"] = "root";   // 分割保存でも foo.json 側に置く印（§4.3）
            if (const auto* t = reg.try_get<Transform>(entity); t && t->siblingOrder != 0)
                ej["siblingOrder"] = t->siblingOrder;
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            auto flag = [&](const char* key) { return ej.contains(key) && ej[key].is_boolean() && ej[key].get<bool>(); };
            if (flag("editorHidden")) reg.emplace_or_replace<EditorHidden>(e);
            if (flag("editorLocked")) reg.emplace_or_replace<EditorLocked>(e);
            if (flag("editorFolder")) reg.emplace_or_replace<EditorFolder>(e);
            if (ej.contains("partition") && ej["partition"].is_string() && ej["partition"].get<std::string>() == "root")
                reg.emplace_or_replace<PartitionRoot>(e);
            if (ej.contains("siblingOrder") && ej["siblingOrder"].is_number_integer())
                if (auto* t = reg.try_get<Transform>(e)) t->siblingOrder = ej["siblingOrder"].get<int>();
        }, {}, {} });

    // ---- [I] エンティティの有効 / 無効（インスペクタの「有効」チェック。EntityDisabled）----
    // 無効の時だけ "disabled": true を書く（キーが無い旧シーンは従来どおり有効）。描画 / ライト / Lua / 物理から外れる（Play にも効く）。
    R.Register({ "EntityDisabled", ComponentSource::Core,
        [](const entt::registry& reg, entt::entity entity, json& ej) {
            if (reg.all_of<EntityDisabled>(entity)) ej["disabled"] = true;
        },
        [](entt::registry& reg, entt::entity e, const json& ej) {
            if (ej.contains("disabled") && ej["disabled"].is_boolean() && ej["disabled"].get<bool>())
                reg.emplace_or_replace<EntityDisabled>(e);
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

// hex 変換の実体は ecs/Components.{h,cpp}（MCP の set_component も同じものを使う。
// 2 箇所に別実装があると片方だけ直して静かに食い違う）。
std::string GuidToHex(uint64_t v) { return FormatEntityGuidHex(v); }

uint64_t GuidFromJson(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return 0;
    return ParseEntityGuidHex(it->get<std::string>());
}

} // namespace

// シーケンサー(エディタのスクラブ)が書き換えた値を、直列化の直前に元へ戻すためのフック。
// SerializeEntityJson は Save / SaveToString(Play のスナップショット・オートセーブ・ビルド)/ SerializeEntity / SerializeSubtree
// (Undo・複製・プレハブ)の全部が通る唯一の入口なので、ここに 1 つ置けば保存の経路の網羅漏れが構造的に起きない。
// フックが無い(nullptr)ときは何もしない。フックは registry を見て「自分の担当のシーンか」を判断する。
static SceneSerializer::PreSerializeHook g_preSerializeHook = nullptr;
static void* g_preSerializeCtx = nullptr;

void SceneSerializer::SetPreSerializeHook(PreSerializeHook fn, void* ctx)
{
    g_preSerializeHook = fn;
    g_preSerializeCtx = ctx;
}

static json SerializeEntityJson(const entt::registry& reg, entt::entity entity,
                                const std::string& assetsDir)
{
    if (g_preSerializeHook) g_preSerializeHook(g_preSerializeCtx, reg);
    // エンティティ参照の名前を guid から引き直す。★参照の正は guid で、名前は
    // 人間と git diff のための派生値。ここで引き直すことで「guid は A を指すのに
    // 名前は B」というドリフト状態が原理的に作れない。guid が 0（旧データ）か
    // 指す先が消えているときだけ、元の名前をそのまま残す。
    const auto refName = [&reg](uint64_t g, const std::string& fallback) -> std::string {
        if (g == 0) return fallback;
        const entt::entity t = FindEntityByGuid(reg, g);
        if (t == entt::null) return fallback;
        const auto* n = reg.try_get<NameTag>(t);
        return n ? n->name : fallback;
    };

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
            //
            // ★種別しか書いていなかったので、球の半径と平面の一辺が保存されず、
            //   読み込み側のハードコード（0.5 / 50）に化けていた。既定シーンの
            //   Ground は SpawnPlane(..., 20.0f) なので、**開いて保存し直すだけで
            //   20m→50m に広がる**（実測で再現）。box は scale が Transform に乗るので無事。
            //   半径/一辺はメッシュに焼かれていて別途持っていないため AABB から復元する
            //   （平面は 4 隅なので厳密、球は頂点分割ぶんだけ内側に出る可能性がある）。
            else if (mr.modelPath.rfind("__primitive_", 0) == 0)
            {
                const Mesh* pmesh = (!mr.meshes.empty()) ? mr.meshes[0] : nullptr;
                if (mr.modelPath == "__primitive_sphere__")
                {
                    ej["primitive"] = "sphere";
                    if (pmesh) ej["primitiveSize"] = pmesh->GetAABBMax().x;          // 半径
                }
                else if (mr.modelPath == "__primitive_plane__")
                {
                    ej["primitive"] = "plane";
                    if (pmesh) ej["primitiveSize"] = pmesh->GetAABBMax().x * 2.0f;   // 一辺
                    // ★分割数も保存する。書かないと読み直したとき 4 頂点の板に戻り、
                    //   水/海など「頂点を動かすシェーダー」を貼った平面が
                    //   保存して開くだけで【波が消える】。
                    //   一辺 n 分割の平面のインデックス数は n*n*6 なので、そこから戻す
                    //   （primitiveSize を AABB から戻しているのと同じ流儀＝専用フィールドを増やさない）。
                    if (pmesh)
                    {
                        const u32 idx = pmesh->GetIndexCountLod(0);
                        const u32 sub = (idx >= 6)
                            ? static_cast<u32>(std::llround(std::sqrt(static_cast<double>(idx) / 6.0)))
                            : 1u;
                        if (sub > 1) ej["primitiveSubdiv"] = sub;
                    }
                }
                else
                {
                    ej["primitive"] = "box";
                }
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
                // 名前付きパラメーターで使えるようになった旧 _pad(3 float)。
                // 全部 0（＝従来のシェーダー）なら書かないので、既存シーンの差分は増えない。
                if (mr.shaderParamsB.x != 0.0f || mr.shaderParamsB.y != 0.0f
                    || mr.shaderParamsB.z != 0.0f)
                    ej["shaderParamsB"] = {mr.shaderParamsB.x, mr.shaderParamsB.y,
                                           mr.shaderParamsB.z};
            }

            // マテリアルテクスチャ上書き（アセットブラウザからテクスチャをD&Dして割当、サブメッシュ単位）。
            // Material 自体は同一モデルの全インスタンスで共有されるため、上書きは MeshRenderer 側に
            // インスタンス単位で保持している(Application::EnsureMaterialOverrideSrv 参照)。
            {
                size_t maxLen = (std::max)({mr.overrideAlbedoTexture.size(),
                                             mr.overrideNormalTexture.size(),
                                             mr.overrideMetalRoughnessTexture.size(),
                                             mr.overrideEmissiveTexture.size()});
                bool anyOverride = false;
                json overridesJson = json::array();
                for (size_t i = 0; i < maxLen; ++i)
                {
                    json entry = json::object();
                    const std::string& a = MeshRenderer::SafeGetOverride(mr.overrideAlbedoTexture, static_cast<u32>(i));
                    const std::string& n = MeshRenderer::SafeGetOverride(mr.overrideNormalTexture, static_cast<u32>(i));
                    const std::string& m = MeshRenderer::SafeGetOverride(mr.overrideMetalRoughnessTexture, static_cast<u32>(i));
                    const std::string& em = MeshRenderer::SafeGetOverride(mr.overrideEmissiveTexture, static_cast<u32>(i));
                    if (!a.empty()) { entry["albedo"] = a; anyOverride = true; }
                    if (!n.empty()) { entry["normal"] = n; anyOverride = true; }
                    if (!m.empty()) { entry["metalRoughness"] = m; anyOverride = true; }
                    if (!em.empty()) { entry["emissive"] = em; anyOverride = true; }
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
            //
            // ★以前は `GetMaterial()` が非 null のときだけ書いていた。Material を持つのは
            //   ModelLoader が読んだメッシュだけ（SetMaterial の呼び出しは ModelLoader.cpp:1145
            //   の 1 箇所のみ）で、プリミティブ・地形・スカルプトは常に null。
            //   ところが描画側は mat の有無に関わらず override を優先する
            //   （ApplicationRender.cpp:553）ので、dx12_set_pbr でボックスを金属にすると
            //   **絵は変わり MCP も成功を返すのに、保存すると消える**（実測で再現）。
            //   override が入っているなら mat が無くても書く。
            const auto* mat = (!mr.meshes.empty() && mr.meshes[0]) ? mr.meshes[0]->GetMaterial() : nullptr;
            const bool  hasPbrOverride = (mr.overrideMetallic >= 0.0f) || (mr.overrideRoughness >= 0.0f);
            // 自己発光は「上書きが入っているときだけ」書く。既定（継承）のままなら
            // キーが 1 つも増えない＝既存シーンを開いて保存し直しても JSON は変わらない。
            const bool  hasEmissive = (mr.overrideEmissiveIntensity >= 0.0f)
                                   || (mr.overrideEmissiveColor.x >= 0.0f);
            const bool  hasAoOverride = (mr.overrideAoStrength >= 0.0f);
            if (mat || hasPbrOverride)
            {
                // 既定値は描画側（ApplicationRender.cpp:553-556）と同じものを使う。
                f32 metallic  = (mr.overrideMetallic  >= 0.0f) ? mr.overrideMetallic
                              : (mat ? mat->defaultMetallic  : 0.0f);
                f32 roughness = (mr.overrideRoughness >= 0.0f) ? mr.overrideRoughness
                              : (mat ? mat->defaultRoughness : 0.5f);
                ej["material"] = {
                    {"metallic",  metallic},
                    {"roughness", roughness}
                };
            }
            if (hasEmissive)
            {
                if (!ej.contains("material")) ej["material"] = nlohmann::json::object();
                if (mr.overrideEmissiveColor.x >= 0.0f)
                    ej["material"]["emissiveColor"] = json::array({ mr.overrideEmissiveColor.x,
                                                                    mr.overrideEmissiveColor.y,
                                                                    mr.overrideEmissiveColor.z });
                if (mr.overrideEmissiveIntensity >= 0.0f)
                    ej["material"]["emissiveIntensity"] = mr.overrideEmissiveIntensity;
            }

            // マテリアル AO の強さ（上書きが入っているときだけ書く＝既存シーンの JSON は増えない）
            if (hasAoOverride)
            {
                if (!ej.contains("material")) ej["material"] = nlohmann::json::object();
                ej["material"]["aoStrength"] = mr.overrideAoStrength;
            }

            // ---- 透明（アルファクリップ / アルファブレンド）----
            // ★書くのは「エンティティ側のオーバーライド」だけ。モデル焼き込みの alphaMode は
            //   毎回 ModelLoader が読み直すので保存しない（既定のままなら 1 キーも増えない
            //   ＝既存シーンを開いて保存し直しても JSON は変わらない）。
            if (mr.alphaModeOverride >= 0 || mr.alphaCutoffOverride >= 0.0f || mr.opacity != 1.0f)
            {
                if (!ej.contains("material")) ej["material"] = nlohmann::json::object();
                if (mr.alphaModeOverride >= 0)
                {
                    const char* names[3] = {"opaque", "mask", "blend"};
                    ej["material"]["alphaMode"] = names[(std::min)(mr.alphaModeOverride, 2)];
                }
                if (mr.alphaCutoffOverride >= 0.0f) ej["material"]["alphaCutoff"] = mr.alphaCutoffOverride;
                if (mr.opacity != 1.0f)             ej["material"]["opacity"]     = mr.opacity;
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
            // テクスチャスプラット。
            // ★条件に splatPath を足してある。以前は layerSetPath が空だと 1 つも書かず、
            //   **レイヤーセットを外しただけで splatPath がシーンから消えた**。
            //   再割り当てすると TerrainPanel は「splatPath が空 → 新規作成」と判断して
            //   傾斜/標高からの自動ペイントを走らせ、しかも SaveSplat がエンティティ名から
            //   同じファイル名を導くので、**手描きのスプラットがディスクごと上書きされる**。
            //   （比べるために一時的にレイヤーを外す、が破壊操作になっていた）
            //   layerSetPath も splatPath も空＝レイヤーを一度も使っていない地形では
            //   従来どおり 1 つも書かないので、既存シーンの JSON は変わらない。
            if (!tr.layerSetPath.empty() || !tr.splatPath.empty())
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
            // 空（= sfx）のときは書かない。既存シーンの保存結果を変えないため。
            if (!as.bus.empty()) ej["audioSource"]["bus"] = as.bus;
            if (as.priority != 128) ej["audioSource"]["priority"] = as.priority;
        }

        if (reg.all_of<ParticleEmitter>(entity))
        {
            const auto& emitter = reg.get<ParticleEmitter>(entity);
            // レイヤー 1 枚を JSON へ。旧形式（フラット）とキー名は完全に同じにしてある。
            // ★こうしておくと「1 枚だけのエミッタ」は旧エンジンでもそのまま読める。
            auto layerJson = [](const ParticleLayer& pe) {
                json j = {
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
                // レイヤー化で増えた分は「既定値なら書かない」。1 枚だけのエミッタの
                // JSON が旧形式とバイト単位で同じになり、無関係な差分が git に出ない。
                if (!pe.name.empty())       j["name"]       = pe.name;
                if (!pe.shaderPath.empty()) j["shaderPath"] = pe.shaderPath;
                if (!pe.vfxPath.empty()) j["vfxPath"] = pe.vfxPath;
                if (pe.offset.x != 0.0f || pe.offset.y != 0.0f || pe.offset.z != 0.0f)
                    j["offset"] = SerializeFloat3(pe.offset);
                return j;
            };

            if (emitter.layers.size() == 1)
            {
                // ★1 枚のときは旧形式のまま書く（前方互換 + 既存シーンの差分ゼロ）。
                ej["particleEmitter"] = layerJson(emitter.layers[0]);
            }
            else
            {
                // 2 枚以上のときだけ layers 配列にする。
                json arr = json::array();
                for (const auto& l : emitter.layers) arr.push_back(layerJson(l));
                ej["particleEmitter"] = json{{"layers", arr}};
            }
        }

        if (reg.all_of<Trigger>(entity))
        {
            const auto& tr = reg.get<Trigger>(entity);
            // ★参照の正は guid。名前は「人間と git diff のための派生値」なので、
            //   guid が生きているならその時点の NameTag で書き直す。こうしておくと
            //   両者がドリフトした状態（guid は A を指すのに名前は B）が原理的に作れない。
            //   guid が 0（旧データ）か指す先が消えているときだけ、元の名前をそのまま残す。
            json acts = json::array();
            for (const auto& a : tr.actions)
            {
                json aj = {
                    {"when", a.when}, {"type", a.type}, {"target", refName(a.targetGuid, a.target)},
                    {"str", a.str}, {"num", a.num}, {"vec", SerializeFloat3(a.vec)}
                };
                if (a.targetGuid != 0) aj["targetGuid"] = GuidToHex(a.targetGuid);
                acts.push_back(std::move(aj));
            }
            ej["trigger"] = {
                {"shape", tr.shape}, {"halfExtents", SerializeFloat3(tr.halfExtents)},
                {"radius", tr.radius}, {"offset", SerializeFloat3(tr.offset)},
                {"filter", refName(tr.filterGuid, tr.filter)}, {"once", tr.once}, {"actions", acts}
            };
            if (tr.filterGuid != 0) ej["trigger"]["filterGuid"] = GuidToHex(tr.filterGuid);
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
                        case ScriptPropType::String: pj["value"] = p.str; break;
                        case ScriptPropType::Entity:
                            // ★参照の正は guid。名前は保存時に引き直す派生値（Trigger と同じ流儀）。
                            pj["value"] = refName(p.guid, p.str);
                            if (p.guid != 0) pj["valueGuid"] = GuidToHex(p.guid);
                            break;
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

    // モデルが読めなかったエンティティ: 読み込み時に取っておいた描画まわりのキーを書き戻す
    // （MeshRenderer が無いので上では何も書かれない。scene/MissingModel.h）。
    if (const auto* mm = reg.try_get<MissingModel>(entity); mm && !reg.all_of<MeshRenderer>(entity))
    {
        const json keep = json::parse(mm->rendererJson, nullptr, /*allow_exceptions=*/false);
        if (keep.is_object())
            for (auto it = keep.begin(); it != keep.end(); ++it)
                if (!ej.contains(it.key())) ej[it.key()] = it.value();
    }

    return ej;
}

// シーン全エンティティを JSON ノードに直列化（共通処理）

// ---- 物理ベース大気 A1（"atmosphere"）。既定と同じ値も含めて全項目を書く（読み戻しで欠けが出ないように）----
static json Rgb3ToJson(const float* v) { return json::array({v[0], v[1], v[2]}); }
static void JsonToRgb3(const json& j, const char* key, float* v)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() < 3) return;
    for (int i = 0; i < 3; ++i) if (j[key][i].is_number()) v[i] = j[key][i].get<float>();
}
static json SerializeAtmosphere(const AtmosphereSettings& a)
{
    return {
        {"enabled", a.enabled}, {"timeOfDay", a.timeOfDay}, {"timeSpeed", a.timeSpeed},
        {"latitudeDeg", a.latitudeDeg}, {"dayOfYear", a.dayOfYear}, {"northYawDeg", a.northYawDeg},
        {"sunMode", a.sunMode}, {"driveSun", a.driveSun}, {"driveIBL", a.driveIBL},
        {"drawStars", a.drawStars}, {"drawMoon", a.drawMoon},
        {"planetRadiusKm", a.planetRadiusKm}, {"atmosphereHeightKm", a.atmosphereHeightKm},
        {"rayleighScattering", Rgb3ToJson(a.rayleighScattering)}, {"rayleighScaleHeightKm", a.rayleighScaleHeightKm},
        {"mieScattering", Rgb3ToJson(a.mieScattering)}, {"mieAbsorption", Rgb3ToJson(a.mieAbsorption)},
        {"mieScaleHeightKm", a.mieScaleHeightKm}, {"mieG", a.mieG},
        {"ozoneAbsorption", Rgb3ToJson(a.ozoneAbsorption)}, {"ozoneCenterKm", a.ozoneCenterKm}, {"ozoneWidthKm", a.ozoneWidthKm},
        {"groundAlbedo", Rgb3ToJson(a.groundAlbedo)}, {"sunAngularRadius", a.sunAngularRadius},
        {"sunIlluminance", a.sunIlluminance}, {"sunTint", Rgb3ToJson(a.sunTint)},
        {"moonIlluminance", a.moonIlluminance}, {"moonTint", Rgb3ToJson(a.moonTint)},
        {"multiScatteringFactor", a.multiScatteringFactor}, {"seaLevelY", a.seaLevelY},
        {"nightSkyNits", a.nightSkyNits}, {"skyLuminanceScale", a.skyLuminanceScale},
        {"aerialPerspective", a.aerialPerspective}, {"apStartDepth", a.apStartDepth},
        {"apMaxDistanceKm", a.apMaxDistanceKm}, {"apStrength", a.apStrength},
        {"iblRebakeThresholdDeg", a.iblRebakeThresholdDeg}, {"iblRebakeMaxHz", a.iblRebakeMaxHz},
    };
}

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

        // guid が出そろった直後に、Trigger の名前参照を guid へ昇格させる。
        // ★ここでやらないと「このセッションで作った参照」は次に開き直すまで
        //   guid が付かない（読み込み時の昇格だけでは 1 往復ぶん遅れる）。
        PromoteEntityRefsToGuid(mutableReg);
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
    // ★キーを手書きで並べるのはやめた。唯一の名前表である DX12E_POST_FIELDS
    //   （renderer/PostProcessSettings.h）から生成する＝フィールドを足したのに
    //   保存だけ書き忘れて「エディタで詰めた設定が次に開くと消える」事故を構造的に潰す。
    {
        const auto& pp = scene.GetPostSettings();
        json pj = json::object();
#define DX12E_PP_SAVE_B(f) pj[#f] = pp.f;
#define DX12E_PP_SAVE_F(f) pj[#f] = pp.f;
#define DX12E_PP_SAVE_I(f) pj[#f] = pp.f;
#define DX12E_PP_SAVE_V(f) pj[#f] = json::array({pp.f.x, pp.f.y, pp.f.z});
#define DX12E_PP_SAVE_S(f) pj[#f] = pp.f;
        DX12E_POST_FIELDS(DX12E_PP_SAVE_B, DX12E_PP_SAVE_F, DX12E_PP_SAVE_I,
                          DX12E_PP_SAVE_V, DX12E_PP_SAVE_S)
#undef DX12E_PP_SAVE_B
#undef DX12E_PP_SAVE_F
#undef DX12E_PP_SAVE_I
#undef DX12E_PP_SAVE_V
#undef DX12E_PP_SAVE_S
        root["postProcess"] = std::move(pj);
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

    // ナビメッシュの生成パラメータ（シーン単位）。焼いた実体は隣の .nav サイドカー。
    {
        const auto& nc = scene.GetNavConfig();
        root["navmesh"] = {
            {"cellSize",             nc.cellSize},
            {"cellHeight",           nc.cellHeight},
            {"agentHeight",          nc.agentHeight},
            {"agentRadius",          nc.agentRadius},
            {"agentMaxClimb",        nc.agentMaxClimb},
            {"agentMaxSlope",        nc.agentMaxSlope},
            {"minRegionArea",        nc.minRegionArea},
            {"mergeRegionArea",      nc.mergeRegionArea},
            {"maxEdgeLen",           nc.maxEdgeLen},
            {"maxSimplificationErr", nc.maxSimplificationErr},
            {"maxVertsPerPoly",      nc.maxVertsPerPoly},
            {"monotonePartition",    nc.monotonePartition},
            {"filterLedgeSpans",     nc.filterLedgeSpans},
            {"filterLowHanging",     nc.filterLowHanging},
            {"useBounds",            nc.useBounds},
            {"boundsMin",            {nc.boundsMin[0], nc.boundsMin[1], nc.boundsMin[2]}},
            {"boundsMax",            {nc.boundsMax[0], nc.boundsMax[1], nc.boundsMax[2]}},
        };
    }

    // シーケンサーの自動再生（空なら書かない = 既存シーンの JSON は 1 バイトも変わらない）
    if (!scene.GetSequenceAutoPlay().empty())
    {
        json arr = json::array();
        for (const auto& sp : scene.GetSequenceAutoPlay())
        {
            json o = { {"sequence", sp.sequence}, {"loop", sp.loop}, {"rate", sp.rate} };
            if (sp.startDelay != 0.0f) o["startDelay"] = sp.startDelay;
            if (sp.clockGame) o["clock"] = "game";
            arr.push_back(std::move(o));
        }
        root["sequencePlayers"] = std::move(arr);
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

    // 法線マップフィルタリング（シーン単位）
    {
        const auto& nf = scene.GetNormalFilterSettings();
        root["normalFilter"] = {
            {"enabled",        nf.enabled},
            {"strength",       nf.strength},
            {"varianceClamp",  nf.varianceClamp},
            {"geometricBlend", nf.geometricBlend},
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
            {"bounceIntensity", dg.bounceIntensity},
        };
        // GI S4: カメラ追従 / カスケード 1 の間隔 / 予算。既定のままなら書かない（旧シーンの JSON は 1 バイトも変わらない）。
        {
            auto& jd = root["raytracing"]["ddgi"];
            const DdgiSettings def{};
            if (dg.followCamera)              jd["followCamera"] = true;
            if (dg.spacing1 != def.spacing1)  jd["spacing1"]     = dg.spacing1;
            if (dg.budgetMs != def.budgetMs)  jd["budgetMs"]     = dg.budgetMs;
        }
    }

    // GI モード（シーン単位）。Legacy（既定）は書かない＝既存シーンの JSON は 1 バイトも変わらない。
    if (scene.GetGiSettings().mode != GiMode::Legacy)
        root["gi"] = {{"mode", "new"}};

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

    // 仮想ジオメトリ（Nanite 風）。既定値のときは書かない＝旧シーンと差分ゼロ。
    if (scene.GetVirtualGeometrySettings() != vg::VirtualGeometrySettings{})
    {
        const auto& vgs = scene.GetVirtualGeometrySettings();
        root["virtualGeometry"] = {
            {"enabled",       vgs.enabled},
            {"lodPixelError", vgs.lodPixelError},
            {"hzbCulling",    vgs.hzbCulling},
            {"coneCulling",   vgs.coneCulling},
            {"instanceMinPx", vgs.instanceMinPx},
            {"vramBudgetMB",  vgs.vramBudgetMB},
        };
    }

    // シーンの風（植生 F1）。既定値のときは書かない＝旧シーンと差分ゼロ。
    if (scene.GetWind() != foliage::SceneWind{})
    {
        const auto& w = scene.GetWind();
        root["wind"] = {
            {"enabled",       w.enabled},
            {"directionDeg",  w.directionDeg},
            {"speed",         w.speed},
            {"gustStrength",  w.gustStrength},
            {"gustFrequency", w.gustFrequency},
            {"turbulence",    w.turbulence},
            {"phaseOffset",   w.phaseOffset},
        };
    }

    // 物理ベース大気 A1。既定（OFF・全項目が既定値）のときは書かない＝旧シーンと差分ゼロ。
    if (scene.GetAtmosphereSettings() != AtmosphereSettings{})
        root["atmosphere"] = SerializeAtmosphere(scene.GetAtmosphereSettings());

    // シーンファイルの分割保存（§4.3）。0（既定）のときは書かない＝既存シーンの JSON は 1 バイトも変わらない。
    if (scene.GetPartitionCellSize() > 0.0f)
    {
        const double cs = static_cast<double>(scene.GetPartitionCellSize());
        root["partition"] = {{"cellSize", (cs == std::floor(cs) && cs < 1.0e9) ? json(static_cast<long long>(cs)) : json(cs)}};
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
        // ★保存側と同じく DX12E_POST_FIELDS から生成する。
        //   「保存はされているのに読み込みだけ書き忘れて、開き直すと既定値に戻る」を構造的に潰す。
#define DX12E_PP_LOAD_B(f) pp.f = pj.value(#f, pp.f);
#define DX12E_PP_LOAD_F(f) pp.f = pj.value(#f, pp.f);
#define DX12E_PP_LOAD_I(f) pp.f = pj.value(#f, pp.f);
#define DX12E_PP_LOAD_V(f) if (pj.contains(#f)) pp.f = DeserializeFloat3(pj[#f], pp.f);
#define DX12E_PP_LOAD_S(f) pp.f = pj.value(#f, pp.f);
        DX12E_POST_FIELDS(DX12E_PP_LOAD_B, DX12E_PP_LOAD_F, DX12E_PP_LOAD_I,
                          DX12E_PP_LOAD_V, DX12E_PP_LOAD_S)
#undef DX12E_PP_LOAD_B
#undef DX12E_PP_LOAD_F
#undef DX12E_PP_LOAD_I
#undef DX12E_PP_LOAD_V
#undef DX12E_PP_LOAD_S
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

// JSON からナビメッシュの生成パラメータを復元（navmesh が無ければ既定 = 後方互換）
static void LoadNavSettings(Scene& scene, const json& root)
{
    nav::NavBuildConfig nc;   // 既定
    if (root.contains("navmesh"))
    {
        const auto& nj = root["navmesh"];
        nc.cellSize             = nj.value("cellSize",             nc.cellSize);
        nc.cellHeight           = nj.value("cellHeight",           nc.cellHeight);
        nc.agentHeight          = nj.value("agentHeight",          nc.agentHeight);
        nc.agentRadius          = nj.value("agentRadius",          nc.agentRadius);
        nc.agentMaxClimb        = nj.value("agentMaxClimb",        nc.agentMaxClimb);
        nc.agentMaxSlope        = nj.value("agentMaxSlope",        nc.agentMaxSlope);
        nc.minRegionArea        = nj.value("minRegionArea",        nc.minRegionArea);
        nc.mergeRegionArea      = nj.value("mergeRegionArea",      nc.mergeRegionArea);
        nc.maxEdgeLen           = nj.value("maxEdgeLen",           nc.maxEdgeLen);
        nc.maxSimplificationErr = nj.value("maxSimplificationErr", nc.maxSimplificationErr);
        nc.maxVertsPerPoly      = nj.value("maxVertsPerPoly",      nc.maxVertsPerPoly);
        nc.monotonePartition    = nj.value("monotonePartition",    nc.monotonePartition);
        nc.filterLedgeSpans     = nj.value("filterLedgeSpans",     nc.filterLedgeSpans);
        nc.filterLowHanging     = nj.value("filterLowHanging",     nc.filterLowHanging);
        nc.useBounds            = nj.value("useBounds",            nc.useBounds);
        if (nj.contains("boundsMin"))
        {
            const DirectX::XMFLOAT3 v = DeserializeFloat3(nj["boundsMin"], {0, 0, 0});
            nc.boundsMin[0] = v.x; nc.boundsMin[1] = v.y; nc.boundsMin[2] = v.z;
        }
        if (nj.contains("boundsMax"))
        {
            const DirectX::XMFLOAT3 v = DeserializeFloat3(nj["boundsMax"], {0, 0, 0});
            nc.boundsMax[0] = v.x; nc.boundsMax[1] = v.y; nc.boundsMax[2] = v.z;
        }
    }
    scene.GetNavConfig() = nc;
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

// JSON から法線マップフィルタリング設定を復元（normalFilter が無ければ既定 ON）
static void LoadNormalFilterSettings(Scene& scene, const json& root)
{
    NormalFilterSettings nf;
    if (root.contains("normalFilter"))
    {
        const auto& j = root["normalFilter"];
        nf.enabled        = j.value("enabled",        nf.enabled);
        nf.strength       = j.value("strength",       nf.strength);
        nf.varianceClamp  = j.value("varianceClamp",  nf.varianceClamp);
        nf.geometricBlend = j.value("geometricBlend", nf.geometricBlend);
    }
    scene.GetNormalFilterSettings() = nf;
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
        dg.bounceIntensity = j.value("bounceIntensity", dg.bounceIntensity);
        dg.followCamera = j.value("followCamera", dg.followCamera);
        dg.spacing1     = j.value("spacing1",     dg.spacing1);
        dg.budgetMs     = j.value("budgetMs",     dg.budgetMs);
        // ★MCP の set_dxr と同じ範囲へ丸める（手書き JSON でプローブ数 999 を書かれても落ちない）
        dg.probeCountX = std::clamp(dg.probeCountX, 1, 32);
        dg.probeCountY = std::clamp(dg.probeCountY, 1, 32);
        dg.probeCountZ = std::clamp(dg.probeCountZ, 1, 32);
        dg.spacing     = std::clamp(dg.spacing,    0.1f, 100.0f);
        dg.rayLength   = std::clamp(dg.rayLength,  0.1f, 10000.0f);
        dg.hysteresis  = std::clamp(dg.hysteresis, 0.0f, 0.995f);
        dg.intensity   = std::clamp(dg.intensity,  0.0f, 10.0f);
        dg.normalBias  = std::clamp(dg.normalBias, 0.0f, 1.0f);
        dg.spacing1    = std::clamp(dg.spacing1,   0.1f, 100.0f);
        dg.budgetMs    = std::clamp(dg.budgetMs,   0.05f, 20.0f);
        // 1 を超えさせない。E/(1-ρ·b) の幾何級数が発散する。
        dg.bounceIntensity = std::clamp(dg.bounceIntensity, 0.0f, 1.0f);
    }
    scene.GetDdgiSettings() = dg;
}

// JSON から GI モードを復元（キーが無い / 不明な値 = legacy）
static void LoadGiSettings(Scene& scene, const json& root)
{
    GiSettings g;
    if (root.contains("gi") && root["gi"].is_object())
    {
        const std::string m = root["gi"].value("mode", std::string("legacy"));
        g.mode = (m == "new") ? GiMode::New : GiMode::Legacy;
    }
    scene.GetGiSettings() = g;
}

// JSON から仮想ジオメトリ設定を復元（virtualGeometry が無ければ既定 OFF = 後方互換）
static void LoadVirtualGeometrySettings(Scene& scene, const json& root)
{
    vg::VirtualGeometrySettings v;
    if (root.contains("virtualGeometry") && root["virtualGeometry"].is_object())
    {
        const auto& j = root["virtualGeometry"];
        v.enabled       = j.value("enabled",       v.enabled);
        v.lodPixelError = j.value("lodPixelError", v.lodPixelError);
        v.hzbCulling    = j.value("hzbCulling",    v.hzbCulling);
        v.coneCulling   = j.value("coneCulling",   v.coneCulling);
        v.instanceMinPx = j.value("instanceMinPx", v.instanceMinPx);
        v.vramBudgetMB  = j.value("vramBudgetMB",  v.vramBudgetMB);
        // 手書き JSON でも範囲外を持ち込まない（実行時の使用範囲と同じ）。
        v.lodPixelError = std::clamp(v.lodPixelError, 0.25f, 8.0f);
        v.instanceMinPx = std::clamp(v.instanceMinPx, 0.0f, 64.0f);
        v.vramBudgetMB  = std::clamp(v.vramBudgetMB, 64, 65536);
    }
    scene.GetVirtualGeometrySettings() = v;
}

// JSON からシーンの風を復元（wind が無ければ既定 = 後方互換）
static void LoadWindSettings(Scene& scene, const json& root)
{
    foliage::SceneWind w;
    if (root.contains("wind") && root["wind"].is_object())
    {
        const auto& j = root["wind"];
        w.enabled       = j.value("enabled",       w.enabled);
        w.directionDeg  = j.value("directionDeg",  w.directionDeg);
        w.speed         = j.value("speed",         w.speed);
        w.gustStrength  = j.value("gustStrength",  w.gustStrength);
        w.gustFrequency = j.value("gustFrequency", w.gustFrequency);
        w.turbulence    = j.value("turbulence",    w.turbulence);
        w.phaseOffset   = j.value("phaseOffset",   w.phaseOffset);
        // 手書き JSON でも範囲外を持ち込まない（NaN / 巨大値で GPU の位置が壊れないように）
        auto fin = [](f32 v, f32 lo, f32 hi, f32 dflt) { return std::isfinite(v) ? std::clamp(v, lo, hi) : dflt; };
        w.directionDeg  = fin(w.directionDeg, -100000.0f, 100000.0f, 45.0f);
        w.speed         = fin(w.speed, 0.0f, 200.0f, 3.0f);
        w.gustStrength  = fin(w.gustStrength, 0.0f, 1.0f, 0.5f);
        w.gustFrequency = fin(w.gustFrequency, 0.0f, 20.0f, 0.35f);
        w.turbulence    = fin(w.turbulence, 0.0f, 1.0f, 0.25f);
        w.phaseOffset   = fin(w.phaseOffset, -1.0e6f, 1.0e6f, 0.0f);
    }
    scene.GetWind() = w;
}

// JSON から物理ベース大気を復元（atmosphere が無ければ既定 = OFF・後方互換）。手書き JSON の範囲外・NaN は既定へ丸める。
static void LoadAtmosphereSettings(Scene& scene, const json& root)
{
    AtmosphereSettings a;
    if (root.contains("atmosphere") && root["atmosphere"].is_object())
    {
        const auto& j = root["atmosphere"];
        auto fin = [](float v, float lo, float hi, float d) { return std::isfinite(v) ? std::clamp(v, lo, hi) : d; };
        a.enabled     = j.value("enabled", a.enabled);
        a.timeOfDay   = fin(j.value("timeOfDay", a.timeOfDay), 0.0f, 24.0f, 12.0f);
        a.timeSpeed   = fin(j.value("timeSpeed", a.timeSpeed), -240.0f, 240.0f, 0.0f);
        a.latitudeDeg = fin(j.value("latitudeDeg", a.latitudeDeg), -90.0f, 90.0f, 35.0f);
        a.dayOfYear   = std::clamp(j.value("dayOfYear", a.dayOfYear), 1, 366);
        a.northYawDeg = fin(j.value("northYawDeg", a.northYawDeg), -360.0f, 360.0f, 0.0f);
        a.sunMode     = std::clamp(j.value("sunMode", a.sunMode), 0, 1);
        a.driveSun    = j.value("driveSun", a.driveSun);
        a.driveIBL    = j.value("driveIBL", a.driveIBL);
        a.drawStars   = j.value("drawStars", a.drawStars);
        a.drawMoon    = j.value("drawMoon", a.drawMoon);
        a.planetRadiusKm     = fin(j.value("planetRadiusKm", a.planetRadiusKm), 100.0f, 100000.0f, 6360.0f);
        a.atmosphereHeightKm = fin(j.value("atmosphereHeightKm", a.atmosphereHeightKm), 1.0f, 2000.0f, 100.0f);
        JsonToRgb3(j, "rayleighScattering", a.rayleighScattering);
        a.rayleighScaleHeightKm = fin(j.value("rayleighScaleHeightKm", a.rayleighScaleHeightKm), 0.1f, 500.0f, 8.0f);
        JsonToRgb3(j, "mieScattering", a.mieScattering);
        JsonToRgb3(j, "mieAbsorption", a.mieAbsorption);
        a.mieScaleHeightKm = fin(j.value("mieScaleHeightKm", a.mieScaleHeightKm), 0.1f, 500.0f, 1.2f);
        a.mieG             = fin(j.value("mieG", a.mieG), -0.99f, 0.99f, 0.8f);
        JsonToRgb3(j, "ozoneAbsorption", a.ozoneAbsorption);
        a.ozoneCenterKm = fin(j.value("ozoneCenterKm", a.ozoneCenterKm), 0.0f, 1000.0f, 25.0f);
        a.ozoneWidthKm  = fin(j.value("ozoneWidthKm", a.ozoneWidthKm), 0.0f, 500.0f, 15.0f);
        JsonToRgb3(j, "groundAlbedo", a.groundAlbedo);
        a.sunAngularRadius = fin(j.value("sunAngularRadius", a.sunAngularRadius), 0.0005f, 0.2f, 0.004675f);
        a.sunIlluminance   = fin(j.value("sunIlluminance", a.sunIlluminance), 0.0f, 1.0e7f, 128000.0f);
        JsonToRgb3(j, "sunTint", a.sunTint);
        a.moonIlluminance  = fin(j.value("moonIlluminance", a.moonIlluminance), 0.0f, 1.0e5f, 0.25f);
        JsonToRgb3(j, "moonTint", a.moonTint);
        a.multiScatteringFactor = fin(j.value("multiScatteringFactor", a.multiScatteringFactor), 0.0f, 4.0f, 1.0f);
        a.seaLevelY        = fin(j.value("seaLevelY", a.seaLevelY), -1.0e5f, 1.0e5f, 0.0f);
        a.nightSkyNits     = fin(j.value("nightSkyNits", a.nightSkyNits), 0.0f, 1000.0f, 0.0015f);
        a.skyLuminanceScale = fin(j.value("skyLuminanceScale", a.skyLuminanceScale), 0.0f, 100.0f, 1.0f);
        a.aerialPerspective = j.value("aerialPerspective", a.aerialPerspective);
        a.apStartDepth      = fin(j.value("apStartDepth", a.apStartDepth), 0.0f, 1.0e6f, 100.0f);
        a.apMaxDistanceKm   = fin(j.value("apMaxDistanceKm", a.apMaxDistanceKm), 1.0f, 2000.0f, 64.0f);
        a.apStrength        = fin(j.value("apStrength", a.apStrength), 0.0f, 20.0f, 1.0f);
        a.iblRebakeThresholdDeg = fin(j.value("iblRebakeThresholdDeg", a.iblRebakeThresholdDeg), 0.0f, 90.0f, 0.25f);
        a.iblRebakeMaxHz        = fin(j.value("iblRebakeMaxHz", a.iblRebakeMaxHz), 0.1f, 60.0f, 2.0f);
    }
    scene.GetAtmosphereSettings() = a;
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

// ---- 読み込み時間の内訳（SceneSerializer::Load が 1 行のログに出す。段階 3 の判断材料）----
namespace {
using LoadClock = std::chrono::steady_clock;
struct LoadTimings
{
    double readMs = 0, parseMs = 0, inflateMs = 0, entitiesMs = 0, modelMs = 0, parentMs = 0;
    int    version = 1;
    size_t entities = 0;
    // 分割保存（§4.3）: セルファイルの読み込み（read はファイル読み・parse は並列パース・merge は並び順どおりの統合）
    size_t   partFiles = 0;
    unsigned partThreads = 0;
    double   partReadMs = 0, partParseMs = 0, partMergeMs = 0;
};
thread_local LoadTimings* g_loadTimings = nullptr;   // Load の間だけ非 null（Play 復元など他経路では計らない）
double LoadMsSince(LoadClock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(LoadClock::now() - t0).count();
}
struct LoadTimingsScope
{
    LoadTimings* prev;
    explicit LoadTimingsScope(LoadTimings* t) : prev(g_loadTimings) { g_loadTimings = t; }
    ~LoadTimingsScope() { g_loadTimings = prev; }
};
} // namespace

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
        const auto tSpawn = g_loadTimings ? LoadClock::now() : LoadClock::time_point{};
        auto entity = scene.Spawn(name, absPath, pos, rot, scale);
        if (g_loadTimings) g_loadTimings->modelMs += LoadMsSince(tSpawn);
        if (!entity.IsValid())
        {
            // ★エンティティごと捨てない（scene/MissingModel.h）。捨てると次の保存でシーンから消える。
            //   本体は作り、描画まわりのキーだけ元の JSON のまま持って保存時に書き戻す。
            OutputDebugStringA(("[Load] FAILED Spawn: " + name + " path=" + absPath + "\n").c_str());
            auto& reg = scene.GetRegistry();
            e = reg.create();
            reg.emplace<NameTag>(e, NameTag{name});
            json keep = json::object();
            for (const char* k : kRendererKeys)
                if (ej.contains(k)) keep[k] = ej[k];
            reg.emplace<MissingModel>(e, MissingModel{relPath, keep.dump()});
            Logger::Warn("モデルが見つからないため、描画せずに残します（保存しても消えません）: {} ({})", name, relPath);
        }
        else
        {
            e = entity.GetHandle();
            if (IsDebuggerPresent()) OutputDebugStringA(("[Load] Spawn: " + name + "\n").c_str());
        }
    }
    else if (ej.contains("primitive"))
    {
        std::string prim = ej["primitive"].get<std::string>();
        // primitiveSize が無い旧シーンは従来どおりの既定値（0.5 / 50）で読む。
        const f32 psize = ej.value("primitiveSize", 0.0f);
        if (prim == "sphere")
            e = scene.SpawnSphere(name, pos, psize > 0.0f ? psize : 0.5f).GetHandle();
        else if (prim == "plane")
            e = scene.SpawnPlane(name, pos, psize > 0.0f ? psize : 50.0f, false,
                                 ej.value("primitiveSubdiv", 1u)).GetHandle();
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
                const auto& pej = ej["particleEmitter"];
                // ★後方互換: 旧形式はフラット（キーが直接並ぶ）、新形式は "layers" 配列。
                //   旧シーンを開いたときはフラットな 1 枚を layers[0] として読む。
                //   ここを両対応にしておかないと、レイヤー化した瞬間に
                //   既存プロジェクトのエフェクトが全部消える。
                ParticleEmitter emitter;
                std::vector<const json*> layerNodes;
                if (pej.contains("layers") && pej["layers"].is_array())
                    for (const auto& lj : pej["layers"]) layerNodes.push_back(&lj);
                else
                    layerNodes.push_back(&pej);

                for (const json* pjp : layerNodes)
                {
                const auto& pj = *pjp;
                ParticleLayer pe;
                pe.name    = pj.value("name", std::string());
                pe.vfxPath = pj.value("vfxPath", std::string());
                if (pj.contains("offset")) pe.offset = DeserializeFloat3(pj["offset"]);
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
                pe.shaderPath  = pj.value("shaderPath", std::string());
                emitter.layers.push_back(std::move(pe));
                }   // レイヤーループ
                reg.emplace_or_replace<ParticleEmitter>(e, std::move(emitter));
            }

            if (ej.contains("trigger"))
            {
                const auto& tj = ej["trigger"];
                Trigger tr;
                tr.shape = tj.value("shape", 0);
                if (tj.contains("halfExtents")) tr.halfExtents = DeserializeFloat3(tj["halfExtents"], {1.0f, 1.0f, 1.0f});
                tr.radius = tj.value("radius", 1.0f);
                if (tj.contains("offset")) tr.offset = DeserializeFloat3(tj["offset"]);
                tr.filter     = tj.value("filter", std::string{});
                tr.filterGuid = GuidFromJson(tj, "filterGuid");
                tr.once   = tj.value("once", false);
                if (tj.contains("actions") && tj["actions"].is_array())
                {
                    for (const auto& aj : tj["actions"])
                    {
                        TriggerAction a;
                        a.when   = aj.value("when", 0);
                        a.type   = aj.value("type", 0);
                        a.target = aj.value("target", std::string{});
                        a.targetGuid = GuidFromJson(aj, "targetGuid");
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
                    allPoints = collider::ReduceHullPoints(allPoints, 256);   // 方向ごとの極値を残す（外形を保つ）
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
                    ReadEmissiveOverrides(mj, mr);
                    ReadAlphaOverrides(mj, mr);
                }
            }

            // 頂点カラー復元（uvTiling より先に。色は m_verticesCache に焼くため）
            if (ej.contains("color") && reg.all_of<MeshRenderer>(e))
            {
                auto c = DeserializeFloat3(ej["color"], {1, 1, 1});
                auto& mr = reg.get<MeshRenderer>(e);
                mr.colorTint    = {c.x, c.y, c.z, 1.0f};   // 保存で消えないよう意図を記録
                mr.hasColorTint = true;
                mr.instanceColor = mr.colorTint;
                // ★共有 Mesh は塗らない。ここが最悪で、読み込みはエンティティ順に同じ Mesh を
                //   塗り直すので **最後に読まれた 1 体の色が全員に残って**いた
                //   （色違い 3 体を保存して開き直すと 3 体とも同じ色。色未指定の兄弟まで染まる）。
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
                if (ej.contains("shaderParamsB") && ej["shaderParamsB"].is_array()
                    && ej["shaderParamsB"].size() >= 3)
                {
                    const auto& sb = ej["shaderParamsB"];
                    mrRestore.shaderParamsB = {sb[0].get<float>(), sb[1].get<float>(),
                                               sb[2].get<float>()};
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
                    if (entry.contains("emissive"))
                        MeshRenderer::SetOverride(mrOv.overrideEmissiveTexture, smi, entry.value("emissive", ""));
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
                            p.str = v.is_string() ? v.get<std::string>() : std::string{}; break;
                        case ScriptPropType::Entity:
                            p.str  = v.is_string() ? v.get<std::string>() : std::string{};
                            p.guid = GuidFromJson(pj, "valueGuid");
                            break;
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
                as.bus         = aj.value("bus", std::string());
                as.priority    = aj.value("priority", 128);
                reg.emplace_or_replace<AudioSource>(e, std::move(as));
            }
        }
    }

    return e;
}

// JSON ノードを既存シーンに展開（共通処理）
// root は非 const: version >= 2 のとき入口で既定値表から補完する（補完後は v1 保存と同じ JSON になるので、
// 下流の InstantiateEntityJson は v1/v2 を区別しない）。
static bool ApplySceneJson(Scene& scene, json& root, const std::string& assetsDir)
{
    scene.Clear();

    {
        const auto tInflate = LoadClock::now();
        scenefmt::InflateScene(root);
        if (g_loadTimings)
        {
            g_loadTimings->inflateMs += LoadMsSince(tInflate);
            g_loadTimings->version = scenefmt::IsV2(root) ? root.value("version", 2) : 1;
        }
    }

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
    try { LoadNavSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("navmesh 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadContactShadowSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("contactShadow 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadNormalFilterSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("normalFilter 設定をスキップしました（型不正）: {}", e.what()); }
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
    try { LoadGiSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("gi 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadVirtualGeometrySettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("virtualGeometry 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadWindSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("wind 設定をスキップしました（型不正）: {}", e.what()); }
    try { LoadAtmosphereSettings(scene, root); }
    catch (const json::exception& e) { Logger::Warn("atmosphere 設定をスキップしました（型不正）: {}", e.what()); }

    // シーケンサーの自動再生（キーが無ければ空 = 後方互換）
    try
    {
        if (root.contains("sequencePlayers") && root["sequencePlayers"].is_array())
        {
            for (const auto& o : root["sequencePlayers"])
            {
                if (!o.is_object() || !o.contains("sequence") || !o["sequence"].is_string()) continue;
                SequenceAutoPlay sp;
                sp.sequence   = o["sequence"].get<std::string>();
                sp.loop       = o.value("loop", false);
                sp.rate       = o.value("rate", 1.0f);
                sp.startDelay = o.value("startDelay", 0.0f);
                sp.clockGame  = o.value("clock", std::string("real")) == "game";
                scene.GetSequenceAutoPlay().push_back(std::move(sp));
            }
        }
    }
    catch (const json::exception& e) { Logger::Warn("sequencePlayers をスキップしました（型不正）: {}", e.what()); }

    // リアルタイム影 ON/OFF（キーが無ければ既定 ON ＝後方互換）
    scene.SetShadowsEnabled(root.value("shadows", true));

    // 分割保存のセルの大きさ（キーが無ければ 0 = 分割しない）
    scene.SetPartitionCellSize(0.0f);
    try
    {
        if (root.contains("partition") && root["partition"].is_object() && root["partition"].contains("cellSize")
            && root["partition"]["cellSize"].is_number())
            scene.SetPartitionCellSize(root["partition"]["cellSize"].get<float>());
    }
    catch (const json::exception& e) { Logger::Warn("partition 設定をスキップしました（型不正）: {}", e.what()); }

    if (!root.contains("entities") || !root["entities"].is_array())
    {
        Logger::Warn("シーン JSON に entities 配列がありません");
        return true;
    }

    // 1パス目: 生成（配列インデックス → entity の対応を保持）
    const auto tEntities = LoadClock::now();
    std::vector<entt::entity> created;
    created.reserve(root["entities"].size());
    scene.BeginBulkSpawn();   // Spawn の 1 体ごとのログは出さず、最後に「N 体配置」の 1 行へ
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
    scene.EndBulkSpawn();
    if (g_loadTimings)
    {
        g_loadTimings->entitiesMs += LoadMsSince(tEntities);
        g_loadTimings->entities = created.size();
    }

    // 2パス目: 親子関係の復元
    const auto tParents = LoadClock::now();
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

    // 4パス目: Trigger の名前参照を guid へ昇格させる（メモリ上だけ）。
    // 全エンティティが揃った後でないと名前を引けないのでここに置く。
    // これで旧シーンも「開いて保存」で自動的に guid 参照へ移行する。
    PromoteEntityRefsToGuid(reg);

    if (g_loadTimings) g_loadTimings->parentMs += LoadMsSince(tParents);
    return true;
}

static SceneSerializer::SaveReport g_lastSaveReport;
static SceneSerializer::SplitAdviceHook g_splitAdviceHook = nullptr;
static void* g_splitAdviceCtx = nullptr;

const SceneSerializer::SaveReport& SceneSerializer::LastSaveReport() { return g_lastSaveReport; }

static std::string g_lastLoadError;
const std::string& SceneSerializer::LastLoadError() { return g_lastLoadError; }

void SceneSerializer::SetSplitAdviceHook(SplitAdviceHook fn, void* ctx)
{
    g_splitAdviceHook = fn;
    g_splitAdviceCtx = ctx;
}

bool SceneSerializer::Save(const Scene& scene, const std::string& filePath,
                           const std::string& assetsDir)
{
    namespace fs = std::filesystem;

    // ★error_code 版を使う。投げる版だと不正なパス（Windows の予約名・使えない文字）で
    //   filesystem_error が Save を突き抜け、呼び出し側の `if (Save(...))` による
    //   失敗処理が一切走らない。ここは戻り値で失敗を返す約束の関数。
    fs::path dir = fs::path(filePath).parent_path();
    if (!dir.empty())
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec && !fs::exists(dir, ec))
        {
            Logger::Error("保存先のフォルダを作れません: {} ({})", dir.string(), ec.message());
            return false;
        }
    }

    // ★シーンファイルは常に v2（既定値の省略・最短 float・parent index 廃止・1 行 1 体）。
    //   Play のスナップショット（SaveToString）は v1 の完全形のまま。
    const auto tSave0 = LoadClock::now();
    // インスタンス群の実体はここで集めて、シーンの隣の <シーン名>.inst/ へ書く（シーン JSON には個数だけが出る）。
    instgroup::SerializeCollector instCollected;
    json root;
    {
        instgroup::ScopedFileSerialize instScope(instCollected);
        root = BuildSceneJson(scene, assetsDir);
    }
    const size_t entityCount = root["entities"].size();
    const double buildMs = LoadMsSince(tSave0);

    // 文字列化で例外（不正な UTF-8 など）が出ても、ファイルを開いて空にする前なので既存の保存は無傷で残る。
    const auto tSave1 = LoadClock::now();
    double convertMs = 0, dumpMs = 0;
    std::string text;
    // 分割保存（§4.3）: セルの大きさが設定されているシーンは foo.json（ルート設定＋割り当てなし）＋ foo.parts/cell_*.json へ。
    const float cellSize = scene.GetPartitionCellSize();
    scenepart::SplitOutput split;
    try
    {
        scenefmt::ConvertToV2(root);
        convertMs = LoadMsSince(tSave1);
        const auto tSave2 = LoadClock::now();
        if (cellSize > 0.0f)
        {
            scenepart::SplitAndDump(std::move(root), static_cast<double>(cellSize), split);
            text = split.rootText;
        }
        else
        {
            text = scenefmt::DumpSceneV2(root);
        }
        dumpMs = LoadMsSince(tSave2);
    }
    catch (const std::exception& e)
    {
        Logger::Error("シーンを JSON にできません（名前やパスに不正な文字が含まれていないか確認してください）: {}", e.what());
        return false;
    }

    // ---- ここから書き込み。シーン本体・セル・サイドカー・.nav を 1 回のコミットにまとめる ----
    //   ・全部を一時ファイルへ書いて検証し終えてから置き換える（1 つでも失敗したら何も置き換えない）
    //   ・ルート（foo.json）を最後に置き換える（「新しいルート＋古いセル」の食い違いを作らない）
    //   ・置き換えの途中でプロセスが死んでも、次回の読み込み・保存が RecoverPending で最後まで置き換える
    const fs::path txnPath = fs::path(filePath).concat(".dx12txn");
    if (atomicfile::RecoverPending(txnPath))
        Logger::Warn("前回の保存が途中で止まっていたので、最後まで置き換えて復旧しました: {}", filePath);
    // 残骸（前回死んだプロセスの一時ファイル・世代へ移し損ねた元の版）は、プロセスが死んだときにしか生じない。
    // このプロセスでそのシーンを最初に保存するときだけ掃除する（毎回ディレクトリを走査しない）。
    static std::unordered_set<std::string> s_sweptFor;
    if (s_sweptFor.insert(filePath).second)
    {
        // 前回死んだプロセスの一時ファイルの残骸を掃除（動いている別プロセスのものは触らない）
        int swept = atomicfile::SweepStaleTmp(dir.empty() ? fs::path(".") : dir);
        swept += atomicfile::SweepStaleTmp(scenepart::PartsDirFor(filePath));
        swept += atomicfile::SweepStaleTmp(instgroup::SidecarDirFor(filePath));
        // コミット後に取っておいた元の版（世代へ移し損ねたもの）の残り。RecoverPending の後なので進行中のコミットは無い。
        swept += atomicfile::SweepStaleOld(dir.empty() ? fs::path(".") : dir);
        swept += atomicfile::SweepStaleOld(scenepart::PartsDirFor(filePath));
        swept += atomicfile::SweepStaleOld(instgroup::SidecarDirFor(filePath));
        if (swept > 0) Logger::Info("書き込み途中で残った一時ファイルを {} 個消しました", swept);
    }
    atomicfile::TakeStats();   // この保存の内訳だけを数える

    // 直前の版を世代として残す（既定 10 世代・60 秒間隔。.dx12/backups/）。ここではコピーしない: 保存で置き換わる元のファイルを
    // コミットの後に「移動」して世代にする（置き換わらないファイルはハードリンク）。失敗しても保存は続ける。
    const auto tBackup0 = LoadClock::now();
    scenebackup::Pending backup;
    try { backup = scenebackup::Begin(PathResolver::BaseDir(), fs::path(filePath)); }
    catch (const std::exception& e) { Logger::Warn("バックアップの世代を作れませんでした: {}", e.what()); }
    double backupMs = LoadMsSince(tBackup0);

    atomicfile::Batch batch(txnPath);
    batch.RetainOld(backup.active);
    auto failSave = [&](const char* what) {
        Logger::Error("{}（元のファイルは変更していません）: {} ({})", what, filePath, batch.Error());
        return false;
    };

    const auto tSave3 = LoadClock::now();
    SaveReport report;
    report.path = filePath;
    report.entities = entityCount;
    report.cellSize = cellSize;

    // サイドカー（インスタンス群）→ セル → .nav → ルートの順にステージ（置き換えもこの順）。内容が同じファイルには触らない。
    const instgroup::SaveStats instStats = instgroup::StageSidecars(filePath, instCollected, batch);
    if (!instStats.ok) return failSave("インスタンス群のサイドカーを書けません");

    scenepart::WriteStats ws;
    if (cellSize > 0.0f)
    {
        ws = scenepart::StageSplit(filePath, split, batch, /*stageRoot=*/false);
        if (!ws.ok) return failSave("シーンの分割ファイルを書き込めません");
    }

    // ---- ナビメッシュのサイドカー（<シーン>.nav）----
    // シーン JSON にはパラメータだけを書き、焼いた実体はバイナリで隣に置く。
    // 焼いていない/消したシーンでは古い .nav を残さない（開き直すと蘇るため。コミットの後に消す）。
    const std::string navPath = fs::path(filePath).replace_extension(".nav").string();
    if (scene.HasNavMesh())
    {
        std::vector<u8> navBytes;
        scene.GetNavMesh().SerializeToBytes(navBytes);
        if (!batch.Add(fs::path(navPath), std::string_view(reinterpret_cast<const char*>(navBytes.data()), navBytes.size())))
            return failSave("ナビメッシュを書けません");
    }

    // ルート。中身が同じなら触らない（更新時刻・git の差分を動かさない。変わっていない保存し直しで世代も増えない）。
    bool rootSame = false;
    // 前回このプロセスが書いた版の指紋（ハッシュ・大きさ・更新時刻）。ファイルが前回のままなら、読まずに「同じか」を判断できる
    // （25MB でも 1 回の読みを省ける）。指紋が無い / ファイルが変わっていた場合だけ従来どおり読んで比べる。
    struct LastSaved { uint64_t hash = 0; uintmax_t size = 0; fs::file_time_type mtime{}; };
    static std::unordered_map<std::string, LastSaved> s_lastSaved;
    uint64_t textHash = 0;
    {
        // 8 バイトずつの高速ハッシュ（暗号用ではない。内容が違えば違う値になれば十分）
        uint64_t h1 = 0x9E3779B97F4A7C15ULL, h2 = 0xC2B2AE3D27D4EB4FULL;
        const char* p = text.data();
        size_t n = text.size();
        for (; n >= 16; n -= 16, p += 16)
        {
            uint64_t a, b;
            std::memcpy(&a, p, 8); std::memcpy(&b, p + 8, 8);
            h1 = (h1 ^ a) * 0xFF51AFD7ED558CCDULL; h1 ^= h1 >> 32;
            h2 = (h2 ^ b) * 0xC4CEB9FE1A85EC53ULL; h2 ^= h2 >> 29;
        }
        uint64_t tail = 0;
        for (size_t i = 0; i < n; ++i) tail = tail * 131 + static_cast<unsigned char>(p[i]);
        textHash = (h1 ^ (h2 * 31) ^ tail ^ text.size()) * 0x9E3779B97F4A7C15ULL;
    }
    {
        bool same = false;
        const auto last = s_lastSaved.find(filePath);
        bool known = false;   // 指紋で決着がついたか
        if (last != s_lastSaved.end())
        {
            std::error_code ec;
            if (last->second.hash != textHash || last->second.size != text.size()) { known = true; same = false; }   // 内容が違う（読まなくてよい）
            else if (fs::exists(filePath, ec) && fs::file_size(filePath, ec) == last->second.size
                     && fs::last_write_time(filePath, ec) == last->second.mtime) { known = true; same = true; }    // 前回書いたままのファイル
        }
        if (!known)
        {
            std::error_code ec;
            if (fs::exists(filePath, ec) && fs::file_size(filePath, ec) == text.size())
            {
                // 一括で読む（istreambuf_iterator の 1 文字ずつは 25MB で 0.15 秒かかる）
                std::ifstream in(filePath, std::ios::binary);
                std::string cur(text.size(), '\0');
                in.read(cur.data(), static_cast<std::streamsize>(cur.size()));
                same = in.gcount() == static_cast<std::streamsize>(cur.size()) && cur == text;
            }
        }
        rootSame = same;
        if (cellSize > 0.0f) { ws.maxBytes = (std::max)(ws.maxBytes, text.size()); ws.totalBytes += text.size(); }
        if (same) ++ws.unchanged;
        else
        {
            if (cellSize > 0.0f) ++ws.written;
            // ★書いた中身を再パースして確認する。通常のシーンは体数の一致まで確認する（分割のルートは一部の体だけ）。
            //   大きいシーン（4MB 超）は再パースしない（25MB で 0.2 秒以上かかり保存が目に見えて遅くなる）。書いた後に読み戻して
            //   送ったバイト列と 1 バイトずつ比較しているので、壊れていないことはそちらで確かめてあり、JSON は直前に dump した
            //   正しいものそのもの。末尾の形だけ確認する。
            atomicfile::Verifier rootVerify;
            if (text.size() > (4u << 20))
                rootVerify = [](std::string_view b, std::string& err) {
                    if (b.size() < 2 || b.front() != '{' || b.back() != '\n') { err = "シーンの末尾が不正です"; return false; }
                    return true;
                };
            else
                rootVerify = atomicfile::JsonVerifier(cellSize > 0.0f ? -1 : static_cast<long long>(entityCount));
            if (!batch.Add(fs::path(filePath), text, rootVerify))
                return failSave("シーンを書き込めません");
        }
    }

    const double stageMs = LoadMsSince(tSave3);   // ここまで = ステージ（一時ファイルへ書く・検証）
    const auto tCommit0 = LoadClock::now();
    const atomicfile::Result commit = batch.Commit();
    if (!commit.ok) return failSave("シーンを置き換えられません");
    const double commitMs = LoadMsSince(tCommit0);

    {
        // コミットが成功したときだけ、次の保存のための指紋（この版のハッシュ・大きさ・更新時刻）を控える
        std::error_code ec;
        LastSaved& ls = s_lastSaved[filePath];
        ls.hash = textHash;
        ls.size = text.size();
        ls.mtime = fs::last_write_time(filePath, ec);
    }

    // 保存前の版を世代にする（置き換わった元のファイルを移動・残りをリンク）。
    // 孤児の掃除（空になったセル・削除した群・.nav）が控えているときは、消えるファイルも世代に入れる必要があるので、掃除より前に
    // 同期で行う。控えていなければ別スレッドで行う（数百のセルのリンクで約 0.5 秒かかるため。保存の待ち時間に載せない。
    // 次の保存・読み込み・一覧は完了を待つ）。
    {
        const auto tB1 = LoadClock::now();
        try
        {
            bool stalePending = false;
            if (backup.active)
            {
                auto key = [](const fs::path& x) { return x.lexically_normal().generic_string(); };
                std::unordered_set<std::string> keep;
                keep.insert(key(fs::path(filePath)));
                if (cellSize > 0.0f)
                    for (const auto& pt : split.parts) keep.insert(key(fs::path(scenepart::PartsDirFor(filePath)) / pt.name));
                for (const auto& [guidHex, set] : instCollected.sets)
                    if (set) keep.insert(key(fs::path(instgroup::SidecarDirFor(filePath)) / (guidHex + ".jsonl")));
                if (scene.HasNavMesh()) keep.insert(key(fs::path(navPath)));
                for (const fs::path& L : backup.live)
                    if (!keep.count(key(L))) { stalePending = true; break; }
            }
            if (stalePending)
            {
                const std::string gen = scenebackup::Finish(backup, batch);
                if (!gen.empty()) Logger::Info("保存の前の版を世代として残しました: {}", gen);
            }
            else
            {
                scenebackup::FinishAsync(std::move(backup), batch);
            }
        }
        catch (const std::exception& e) { Logger::Warn("バックアップの世代を作れませんでした: {}", e.what()); batch.ReleaseRetained(); }
        backupMs += LoadMsSince(tB1);
    }

    // コミットの後の掃除（失敗しても保存は成功している）
    instgroup::CleanupSidecars(filePath, instCollected);
    if (cellSize > 0.0f)
    {
        ws.removed = scenepart::FinishSplit(filePath, split);
        report.partitioned = !split.parts.empty();
        report.files = ws.files;
        report.written = ws.written;
        report.unchanged = ws.unchanged;
        report.removed = ws.removed;
        report.maxFileBytes = ws.maxBytes;
        report.totalBytes = ws.totalBytes;
    }
    else
    {
        // 分割をやめたシーン: 古いセルファイルを消す（foo.json はもう parts を指していないので、置き換えた後に消す）
        report.removed = scenepart::RemoveParts(filePath);
        report.written = rootSame ? 0 : 1;
        report.unchanged = rootSame ? 1 : 0;
        report.maxFileBytes = report.totalBytes = text.size();
    }
    if (!scene.HasNavMesh())
    {
        std::error_code ec;
        fs::remove(navPath, ec);
    }
    const double writeMs = LoadMsSince(tSave3);

    report.ms = LoadMsSince(tSave0);
    {
        // 書き込みの内訳（原子的な書き込みの追加分: flush / 読み戻し / 検証 / 置き換え / 世代）。保存の遅さの調査用。
        const atomicfile::Stats st = atomicfile::TakeStats();
        Logger::Info("Scene save write stages: {} files {:.1f} MB | stage {:.0f} ms, commit {:.0f} ms | write {:.0f} ms, flush {:.0f}, readback {:.0f}, verify {:.0f}, replace {:.0f}, backup {:.0f} ms",
                     st.files, static_cast<double>(st.bytes) / (1024.0 * 1024.0), stageMs, commitMs, st.writeMs, st.flushMs, st.readbackMs, st.verifyMs, st.replaceMs, backupMs);
    }
    Logger::Info("Scene saved ({} entities, format v2, {} bytes): {} | build {:.0f} ms, convert {:.0f}, dump {:.0f}, write {:.0f}, total {:.0f} ms",
                 entityCount, text.size(), filePath, buildMs, convertMs, dumpMs, writeMs, report.ms);
    if (report.partitioned)
        Logger::Info("Scene split saved: {} cell files (cell {:.0f} m), written {}, unchanged {}, removed {}, max file {} bytes",
                     report.files, static_cast<double>(cellSize), report.written, report.unchanged, report.removed, report.maxFileBytes);
    g_lastSaveReport = report;

    // 大きいシーンを分割しないまま保存したときは、分割を勧める通知を（シーンごとに 1 回）出す。
    // "." で始まるフォルダ/ファイル（.autosave・.diagnostics_snapshot など生成物）では出さない。
    if (cellSize <= 0.0f && entityCount >= scenepart::kAdviceEntityThreshold && g_splitAdviceHook)
    {
        bool hidden = false;
        for (const auto& comp : fs::path(filePath))
        {
            const std::string c = comp.string();
            if (c.size() > 1 && c[0] == '.' && c != "..") { hidden = true; break; }
        }
        static std::unordered_set<std::string> advised;
        if (!hidden && advised.insert(filePath).second)
            g_splitAdviceHook(g_splitAdviceCtx,
                "このシーンは " + std::to_string(entityCount) + " 体あります。分割保存を有効にすると、変更した部分のファイルだけが書き換わり、"
                "git の差分も小さくなります（ライティング > シーンファイル）。");
    }
    return true;
}

// シーンの隣にある .nav を読む（あれば）。無くてもエラーにしない。
static void LoadNavMeshSidecar(Scene& scene, const std::string& scenePath)
{
    namespace fs = std::filesystem;
    const std::string navPath = fs::path(scenePath).replace_extension(".nav").string();
    auto bytes = vfs::ReadAssetAbs(navPath);          // ゲームモードは pak から復号
    if (bytes.empty())
    {
        std::error_code ec;
        if (!fs::exists(navPath, ec)) return;
        std::ifstream f(navPath, std::ios::binary);
        if (!f) return;
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) return;

    std::string err;
    if (!scene.GetNavMesh().LoadFromMemory(reinterpret_cast<const u8*>(bytes.data()),
                                           bytes.size(), err))
    {
        Logger::Warn("ナビメッシュを読めませんでした（焼き直しが必要）: {}", err);
        return;
    }
    Logger::Info("NavMesh loaded: {} polys, {} samples ({})",
                 scene.GetNavMesh().PolyCount(),
                 scene.GetNavMesh().GetStats().sampleCount, navPath);
}

// JSON テキスト → シーン（Load / LoadFromString 共通）。fromFile は失敗時のメッセージの出し分けだけに使う。
static bool LoadSceneText(Scene& scene, const std::string& text, const std::string& assetsDir,
                          bool fromFile, size_t* outEntityCount, const std::string* scenePath = nullptr)
{
    json root;
    const auto tParse = LoadClock::now();
    try
    {
        root = json::parse(text);
    }
    catch (const json::parse_error& e)
    {
        if (fromFile) Logger::Error("JSON の解析に失敗しました: {}", e.what());
        else          Logger::Error("JSON の解析に失敗しました（スナップショット）: {}", e.what());
        g_lastLoadError = text.empty() ? "ファイルが空です（保存の途中で止まった可能性があります）"
                                       : std::string("ファイルの内容が壊れています（保存の途中で切れた可能性があります）: ") + e.what();
        return false;
    }
    if (g_loadTimings) g_loadTimings->parseMs += LoadMsSince(tParse);

    // 分割保存のシーン（foo.json の "parts"）: セルファイルを全部読んで 1 つの entities へ統合する（パースは並列）。
    // 以降は 1 ファイルのシーンと同じ処理。読めないセルがあれば開かない（黙って欠けたまま保存して消すのを避ける）。
    bool hadParts = false;
    if (scenePath && root.is_object() && root.contains("parts"))
    {
        hadParts = true;
        const std::string partsDir = scenepart::PartsDirFor(*scenePath);
        scenepart::MergeStats ms;
        std::string err;
        const bool merged = scenepart::MergeParts(root, [&](const std::string& name, std::string& bytes) {
            const std::string p = (std::filesystem::path(partsDir) / name).string();
            auto b = vfs::ReadAssetAbs(p);   // ゲームモード: pak から復号。エディタ: ディスク
            if (!b.empty()) { bytes.assign(b.begin(), b.end()); return true; }
            std::ifstream f(p, std::ios::binary);
            if (!f) return false;
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return true;
        }, ms, err);
        if (!merged)
        {
            Logger::Error("シーンの分割ファイルを読み込めません（{}）: {}", *scenePath, err);
            g_lastLoadError = "分割ファイル（.parts）を読み込めません: " + err;
            return false;
        }
        if (g_loadTimings)
        {
            g_loadTimings->partFiles = ms.files;
            g_loadTimings->partThreads = ms.threads;
            g_loadTimings->partReadMs = ms.readMs;
            g_loadTimings->partParseMs = ms.parseMs;
            g_loadTimings->partMergeMs = ms.mergeMs;
        }
        if (!ms.seqUsed && ms.files > 0)
            Logger::Warn("シーンの分割ファイルに並び順（seq）が無いか壊れています。foo.json → セルの順につなぎました: {}", *scenePath);
    }

    bool ok = false;
    try
    {
        ok = ApplySceneJson(scene, root, assetsDir);
        // parts があるのに partition の設定が無い手書きのシーン: 既定のセルの大きさで分割保存を続ける
        if (ok && hadParts && scene.GetPartitionCellSize() <= 0.0f)
            scene.SetPartitionCellSize(static_cast<float>(scenepart::kDefaultCellSize));
    }
    catch (const json::exception& e)
    {
        // ApplySceneJson 内でセクション/エンティティ単位に捕捉しているが、最後の保険
        if (fromFile) Logger::Error("シーン読み込みを中断しました（JSON の値が不正）: {}", e.what());
        else          Logger::Error("シーン復元を中断しました（JSON の値が不正）: {}", e.what());
        g_lastLoadError = std::string("シーンの値が不正です: ") + e.what();
        return false;
    }
    if (outEntityCount)
        *outEntityCount = root.contains("entities") && root["entities"].is_array() ? root["entities"].size() : 0;
    return ok;
}

bool SceneSerializer::Load(Scene& scene, const std::string& filePath,
                           const std::string& assetsDir)
{
    LoadTimings tm;
    const auto tTotal = LoadClock::now();
    g_lastLoadError.clear();
    // 前回の保存が置き換えの途中で止まっていたら、読む前に最後まで置き換える（新旧が食い違った状態で開かない）
    if (atomicfile::RecoverPending(std::filesystem::path(filePath).concat(".dx12txn")))
        Logger::Warn("前回の保存が途中で止まっていたので、最後まで置き換えて復旧しました: {}", filePath);

    // 分割保存のセルファイル（foo.parts/cell_*.json）は単独では開かない（一部だけ読むと、保存で他が消えるため）。
    if (std::filesystem::path(filePath).parent_path().extension() == ".parts")
    {
        Logger::Error("分割保存のセルファイルは直接開けません。本体のシーン（{}.json）を開いてください: {}",
                      std::filesystem::path(filePath).parent_path().stem().string(), filePath);
        return false;
    }

    // VFS 経由で読む（ゲームモード: pak 復号。エディタ: ディスク）。
    const auto tRead = LoadClock::now();
    std::string text;
    {
        auto b = vfs::ReadAssetAbs(filePath);
        if (!b.empty())
        {
            text.assign(b.begin(), b.end());
        }
        else
        {
            // ディスクフォールバック
            std::ifstream ifs(filePath, std::ios::binary);
            if (!ifs.is_open())
            {
                Logger::Error("シーンファイルを開けません: {}", filePath);
                g_lastLoadError = "シーンファイルを開けません（ファイルが無いか、他のプログラムが使用中です）";
                return false;
            }
            ifs.seekg(0, std::ios::end);
            const std::streamoff sz = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            if (sz > 0)
            {
                text.resize(static_cast<size_t>(sz));
                ifs.read(text.data(), sz);
            }
        }
    }
    tm.readMs = LoadMsSince(tRead);

    size_t entityCount = 0;
    bool ok = false;
    {
        LoadTimingsScope scope(&tm);
        instgroup::ScopedLoadScene instLoadScene(filePath);   // インスタンス群のサイドカーを読む先
        ok = LoadSceneText(scene, text, assetsDir, /*fromFile=*/true, &entityCount, &filePath);
    }
    if (ok)
    {
        LoadNavMeshSidecar(scene, filePath);
        const double loadTotalMs = LoadMsSince(tTotal);   // 検証フックの時間を含めない

        // ★検証用の隠しフック（通常は何も起きない）。環境変数 DX12E_SCENE_FORMAT_CHECK_DIR があるとき、
        //   読み込み直後のシーンから <stem>.v1full.json（SaveToString = v1 完全形。F1 等価性の基準）と
        //   <stem>.v2.json（Save が書く v2 本体）をそのフォルダへ書き、v1(dump(2)) と v2 の保存時間を 1 行ログに出す。
        //   別ファイルを開き直して v1full を突き合わせる使い方は docs/SCENE_FORMAT_DESIGN.md §3.6 の F1 / F4。
        {
            char dirBuf[MAX_PATH] = {};
            const DWORD n = GetEnvironmentVariableA("DX12E_SCENE_FORMAT_CHECK_DIR", dirBuf, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                namespace fs = std::filesystem;
                const std::string stem = fs::path(filePath).stem().string();
                std::error_code ec;
                fs::create_directories(dirBuf, ec);
                auto writeBin = [&](const std::string& name, const std::string& body) {
                    std::ofstream o((fs::path(dirBuf) / name).string(), std::ios::binary | std::ios::trunc);
                    o.write(body.data(), static_cast<std::streamsize>(body.size()));
                };
                const auto t1 = LoadClock::now();
                const std::string v1full = SaveToString(scene, assetsDir);
                const double v1fullMs = LoadMsSince(t1);
                writeBin(stem + ".v1full.json", v1full);
                // 旧 Save 相当（BuildSceneJson + dump(2)）の時間と大きさ。
                const auto t2 = LoadClock::now();
                const std::string v1pretty = BuildSceneJson(scene, assetsDir).dump(2);
                const double v1prettyMs = LoadMsSince(t2);
                // 新 Save 相当（BuildSceneJson + v2 変換 + 整形）。
                const auto t3 = LoadClock::now();
                const std::string v2 = SaveToStringV2(scene, assetsDir);
                const double v2Ms = LoadMsSince(t3);
                writeBin(stem + ".v2.json", v2);
                writeBin(stem + ".v1save.json", v1pretty);   // 旧 Save が書いていたもの（F4: v1 と v2 の読み込み時間の比較用）
                Logger::Info("SceneFormatCheck {}: v1full {} bytes ({:.0f} ms) | v1 save (dump(2)) {} bytes {:.0f} ms | v2 save {} bytes {:.0f} ms",
                             stem, v1full.size(), v1fullMs, v1pretty.size(), v1prettyMs, v2.size(), v2Ms);
            }
        }

        // 読み込み時間の内訳（段階 3「実行用バイナリ」の判断材料）。entities は生成全体で、models はそのうち Spawn（モデル読み込み）の合計。
        {
            // インスタンス群（InstanceGroup）があれば 1 行で個数を残す（G6: pak から読めたことの確認にも使う）
            size_t groups = 0, instances = 0;
            for (auto [ge, gg] : scene.GetRegistry().view<const InstanceGroup>().each())
            {
                ++groups;
                if (gg._set) instances += gg._set->items.size();
            }
            if (groups > 0) Logger::Info("Instance groups loaded: {} groups, {} instances ({})", groups, instances, filePath);
        }
        Logger::Info("Scene loaded ({} entities, format v{}): {} | read {:.0f} ms, parse {:.0f}, inflate {:.0f}, entities {:.0f} (models {:.0f}), parents {:.0f}, total {:.0f} ms",
                     entityCount, tm.version, filePath, tm.readMs, tm.parseMs, tm.inflateMs,
                     tm.entitiesMs, tm.modelMs, tm.parentMs, loadTotalMs);
        if (tm.partFiles > 0)
            Logger::Info("Scene parts: {} files (cell {:.0f} m) | read {:.0f} ms, parse {:.0f} ms ({} threads), merge {:.0f} ms",
                         tm.partFiles, static_cast<double>(scene.GetPartitionCellSize()), tm.partReadMs, tm.partParseMs,
                         tm.partThreads, tm.partMergeMs);
    }
    return ok;
}

std::string SceneSerializer::SaveToString(const Scene& scene, const std::string& assetsDir)
{
    json root = BuildSceneJson(scene, assetsDir);
    return root.dump();
}

std::string SceneSerializer::SaveToStringV2(const Scene& scene, const std::string& assetsDir)
{
    json root = BuildSceneJson(scene, assetsDir);
    scenefmt::ConvertToV2(root);
    return scenefmt::DumpSceneV2(root);
}

bool SceneSerializer::LoadFromString(Scene& scene, const std::string& jsonStr,
                                     const std::string& assetsDir)
{
    size_t entityCount = 0;
    const bool ok = LoadSceneText(scene, jsonStr, assetsDir, /*fromFile=*/false, &entityCount);
    if (ok)
        Logger::Info("Scene restored from snapshot ({} entities)", entityCount);
    return ok;
}

std::string SceneSerializer::BuildDefaultsTableV2Json()
{
    RegisterCoreComponentSerializers();   // プローブはここで積まれる
    json table = json::object();
    {
        // transform: 位置は表に入れない（書く側が常に position を持つ）。回転 / スケールだけが対象。
        entt::registry reg;
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ "probe" });
        reg.emplace<Transform>(e);
        json ej = SerializeEntityJson(reg, e, std::string{});
        json tj = ej.value("transform", json::object());
        tj.erase("position");
        table["transform"] = std::move(tj);
    }
    for (const auto& pr : DefaultsProbes())
    {
        json j = pr.run();
        if (j.is_object()) table[pr.key] = std::move(j);
    }
    {
        // ★rigidBody だけは「既定構築」ではなく「静的コライダー」の姿を表にする（設計書 §3.1 の `"rigidBody":{}` = 静的）。
        //   実シーンの剛体はほぼ全部が静的コライダー（Dead Mall は 13522 体が同一設定）で、既定構築の動的剛体を表にすると
        //   静的の設定 5 項目が全員に残り、Dead Mall が 5MB（F3）に収まらない。表は凍結データで v2 のファイルにだけ効く
        //   （v1 の読み込みは従来どおり RigidBody{} = 動的）。人間 / AI が省略形 `rigidBody:{}` を書くと静的になる点に注意。
        entt::registry reg;
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ "probe" });
        reg.emplace<Transform>(e);
        RigidBody rb;
        rb.motionType  = MotionType::Static;
        rb.mass        = 0.0f;
        rb.friction    = 0.8f;
        rb.restitution = 0.0f;
        rb.useGravity  = false;
        reg.emplace<RigidBody>(e, rb);
        json ej = SerializeEntityJson(reg, e, std::string{});
        if (ej.contains("rigidBody")) table["rigidBody"] = ej["rigidBody"];
    }
    scenefmt::NormalizeFloats(table);
    return table.dump(2) + "\n";
}

bool SceneSerializer::ApplyOverrides(Scene& scene, const std::string& filePath,
                                     const std::string& /*assetsDir*/)
{
    // 分割保存のシーンはセルファイルもつなげて読む（§4.3）。
    json root;
    {
        std::string perr;
        if (!scenepart::ReadSceneFileMerged(filePath, root, perr)) return false;
    }

    // v2 は既定値を省略している。補完してから読む（補完後は v1 保存と同じ形）。
    scenefmt::InflateScene(root);

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
            // ★v2 は position が無いこともあり得る（const operator[] の欠損キーは未定義動作なので contains で守る）。
            if (tj.contains("position")) t.position = DeserializeFloat3(tj["position"], t.position);
            if (tj.contains("rotation")) t.rotation = DeserializeFloat3(tj["rotation"], t.rotation);
            if (tj.contains("scale"))    t.scale    = DeserializeFloat3(tj["scale"],    t.scale);
        }

        // Material PBR オーバーライド
        if (ej.contains("material") && reg.all_of<MeshRenderer>(e))
        {
            const auto& mj = ej["material"];
            auto& mr = reg.get<MeshRenderer>(e);
            if (mj.contains("metallic"))  mr.overrideMetallic  = mj["metallic"].get<f32>();
            if (mj.contains("roughness")) mr.overrideRoughness = mj["roughness"].get<f32>();
            ReadEmissiveOverrides(mj, mr);
            ReadAlphaOverrides(mj, mr);
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
    json ej = SerializeEntityJson(reg, e, assetsDir);
    // ★guid も載せる。BuildSceneJson / SerializeSubtree は書いているのにここだけ抜けていて、
    //   削除 → Undo で復元されたエンティティが guid を失っていた（次の保存で別 guid になる）。
    //   受け取り側が使うかは InstantiateEntity の keepGuid で決める。
    if (const auto* g = reg.try_get<EntityGuid>(e))
        ej["guid"] = GuidToHex(g->value);
    return ej.dump();
}

static std::string MakeUniqueName(const Scene& scene, const std::string& base);

// NameIndexScope の状態（スレッドごと）。active の間 MakeUniqueName は全走査せず names を引く。
namespace
{
struct NameIndexState
{
    const Scene* scene = nullptr;
    std::unordered_set<std::string> names;
};
thread_local NameIndexState g_nameIndex;
} // namespace

SceneSerializer::NameIndexScope::NameIndexScope(const Scene& scene)
{
    if (g_nameIndex.scene != nullptr) return;   // 入れ子は外側が持つ
    g_nameIndex.scene = &scene;
    g_nameIndex.names.clear();
    for (auto [e, tag] : scene.GetRegistry().view<const NameTag>().each())
        g_nameIndex.names.insert(tag.name);
    m_owner = true;
}

SceneSerializer::NameIndexScope::~NameIndexScope()
{
    if (!m_owner) return;
    g_nameIndex.scene = nullptr;
    g_nameIndex.names.clear();
}

entt::entity SceneSerializer::InstantiateEntity(Scene& scene, const std::string& jsonStr,
                                                const std::string& assetsDir,
                                                bool keepGuid)
{
    json ej;
    try { ej = json::parse(jsonStr); }
    catch (const json::parse_error& e)
    {
        Logger::Error("JSON の解析に失敗しました（エンティティ）: {}", e.what());
        return entt::null;
    }
    ej["name"] = MakeUniqueName(scene, ej.value("name", "Unnamed"));
    const uint64_t guid = keepGuid ? GuidFromJson(ej, "guid") : 0ull;
    const entt::entity e = InstantiateEntityJson(scene, ej, assetsDir);
    // 同じエンティティの作り直し（Undo/Redo）だけ guid を引き継ぐ。
    if (e != entt::null && guid != 0)
        scene.GetRegistry().emplace_or_replace<EntityGuid>(e, EntityGuid{ guid });
    return e;
}

// "Box" → "Box (1)" → "Box (2)" のように重複しない名前を作る
static std::string MakeUniqueName(const Scene& scene, const std::string& base)
{
    const auto& reg = scene.GetRegistry();
    const bool indexed = g_nameIndex.scene == &scene;   // NameIndexScope の間は索引（全走査しない）
    auto exists = [&](const std::string& n)
    {
        if (indexed) return g_nameIndex.names.count(n) != 0;
        for (auto [e, tag] : reg.view<const NameTag>().each())
            if (tag.name == n) return true;
        return false;
    };

    if (!exists(base)) { if (indexed) g_nameIndex.names.insert(base); return base; }

    // 末尾の " (N)" を除去してベース名にする
    std::string stem = base;
    auto p = stem.rfind(" (");
    if (p != std::string::npos && stem.back() == ')')
        stem = stem.substr(0, p);

    for (int i = 1; i < 1000; ++i)
    {
        std::string candidate = stem + " (" + std::to_string(i) + ")";
        if (!exists(candidate)) { if (indexed) g_nameIndex.names.insert(candidate); return candidate; }
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
    // 植生(.dxfoliage)も同じ理由で複製先に固有のパスを振る（実体は下でコピーオンライトのまま共有し、書き出しを予約する）。
    if (ej.contains("foliageLayer") && ej["foliageLayer"].is_object())
        ej["foliageLayer"]["instancePath"] = foliage::MakeFoliageRelPath(uniqueName);

    // 複製元の植生の実体は、複製の前に必ず読み込んでおく（遅延読込のまま新パスを指すと、実体の無い複製になる）。
    if (auto* srcF0 = reg.try_get<FoliageLayer>(src)) foliage::EnsureLoaded(*srcF0);

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

    if (auto* dstF = reg.try_get<FoliageLayer>(copy))
    {
        const auto* srcF = reg.try_get<FoliageLayer>(src);
        if (srcF && srcF->_set)
        {
            dstF->_set = srcF->_set;         // コピーオンライト: 編集すると新しい実体へ差し替わるので共有して安全
            dstF->_loadTried = true;
            dstF->_needsSave = true;         // 新しい .dxfoliage を書き出させる
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
    if (!ej.contains("terrain") && !ej.contains("sculpt") && !ej.contains("foliageLayer")) return;

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
        {
            const atomicfile::Result cr = atomicfile::CopyFileAtomic(src, dst, /*overwrite=*/true);
            if (!cr.ok) { ec = std::make_error_code(std::errc::io_error); Logger::Warn("複製時のコピー失敗: {}", cr.error); }
        }
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
    if (ej.contains("foliageLayer") && ej["foliageLayer"].is_object())
        repoint(ej["foliageLayer"], "instancePath", foliage::MakeFoliageRelPath(newName));
}

// ---- プレハブが持つ「生成アセット」（.smsh / .hf / .splat）の面倒を見る ------------
//
// 経緯: これらの実体はシーン JSON に入らず別ファイルにある。インスタンス化のときに
// RepointGeneratedAssets が **インスタンスごとに私物のコピー**を作る（1 個彫っても
// 他のインスタンスが変わらないようにするため）。
// ところが .prefab 自身はコピーを持たず、「作った元インスタンスのファイル」を指したままだった。
// そのせいで:
//   - 元インスタンスを後から彫ると、触っていない .prefab の中身が黙って変わる
//   - 「適用」で他インスタンスへ配るとき、3-way マージは meshPath の違いを
//     **インスタンス側の手直し**と判定してインスタンスの古いパスを残す
//     ＝形状だけ配られない。なのにログと UI は「他 N インスタンスへ反映」と言う。
// 対策: 保存時にプレハブ専用のコピーを作って .prefab をそこへ向け（自己完結させる）、
// 配布時は「そのインスタンスが自分で彫っていない」ときだけ中身を上書きする。
struct GeoField { const char* comp; const char* key; };
static const GeoField kGeoFields[] = {
    {"sculpt",  "meshPath"},
    {"terrain", "heightmapPath"},
    {"terrain", "splatPath"},
};

// key に応じた「その名前ならこの相対パス」を返す
static std::string MakeGeoRelPath(const char* key, const std::string& name)
{
    if (std::strcmp(key, "meshPath")      == 0) return sculpt::MakeSculptMeshRelPath(name);
    if (std::strcmp(key, "heightmapPath") == 0) return terrain::MakeHeightFieldRelPath(name);
    return terrain::MakeSplatRelPath(name);
}

static std::string ReadFileBytes(const std::string& abs)
{
    std::ifstream ifs(abs, std::ios::binary);
    if (!ifs) return {};
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

// entities 配列の各要素から生成アセットのパスを拾う。戻り値は "<index>/<comp>.<key>" → 相対パス。
static std::unordered_map<std::string, std::string> CollectGeoPaths(const json& entities)
{
    std::unordered_map<std::string, std::string> out;
    if (!entities.is_array()) return out;
    for (size_t i = 0; i < entities.size(); ++i)
    {
        for (const GeoField& g : kGeoFields)
        {
            const auto c = entities[i].find(g.comp);
            if (c == entities[i].end() || !c->is_object()) continue;
            const auto k = c->find(g.key);
            if (k == c->end() || !k->is_string()) continue;
            const std::string rel = k->get<std::string>();
            if (rel.empty()) continue;
            out[std::to_string(i) + "/" + g.comp + "." + g.key] = rel;
        }
    }
    return out;
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
        for (auto& p : mr.overrideEmissiveTexture)       fn(p, who, "emissive");
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
    for (auto [e, fl] : reg.view<FoliageLayer>().each()) fn(fl.instancePath, nameOf(e), "foliage");
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
    // レイヤーごとにテクスチャを持つので全部見る。
    // ★fn は「参照を書き換える」用途にも使われる（アセット移動時のパス追従）ので
    //   非 const 参照で渡すこと。const にすると書き換え経路が黙って効かなくなる。
    for (auto [e, p]  : reg.view<ParticleEmitter>().each())
        for (auto& l : p.layers) fn(l.texturePath, nameOf(e), "particle");
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
    // 開いているシーンが分割保存なら、そのセルファイル（<stem>.parts/）もメモリ側で処理済みなので飛ばす。
    const fs::path skipParts = skipAbsPath.empty() ? fs::path()
                                                   : fs::weakly_canonical(fs::path(scenepart::PartsDirFor(skipAbsPath)), ec);

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
        if (!skipParts.empty() && fs::weakly_canonical(p.parent_path(), ec) == skipParts) continue;

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

        // v2 のシーンは v2 の整形で書き戻す（dump(2) だと 1 体 45 行に崩れて差分が全体に広がる）。
        // 値は strip 済みのまま触らない（文字列の付け替えしかしていない）。
        // 原子的に置き換える（途中で落ちても元のファイルは無傷）。
        std::string rewritten;
        try
        {
            rewritten = (scenefmt::IsV2(doc) && doc.contains("entities") && doc["entities"].is_array())
                            ? scenefmt::DumpSceneV2(doc) : doc.dump(2);
        }
        catch (const std::exception&)
        {
            out.failed.push_back(MakeRelative(p.string(), assetsDir));
            continue;
        }
        if (!atomicfile::WriteFile(p, rewritten, atomicfile::JsonVerifier()).ok)
        {
            out.failed.push_back(MakeRelative(p.string(), assetsDir));
            continue;
        }
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

    // ★展開元 JSON に入っていた guid の集合。keepGuids=false のとき、参照がこの中の
    //   guid を指していたら 0 へ落とす（下の 3 パス目の直前）。落とさないと、複製した
    //   サブツリー内部の参照が「切れる」のではなく**複製元の方を正しく指し続ける**。
    //   名前だけの時代は MakeUniqueName の連番リネーム後に名前を付け替えれば済んでいたが、
    //   guid は連番と無関係なので付け替えが効かない。
    std::unordered_set<uint64_t> srcGuids;
    if (!keepGuids)
        for (const auto& ej : root["entities"])
            if (const uint64_t g = GuidFromJson(ej, "guid"); g != 0) srcGuids.insert(g);

    // 1パス目: 生成（名前は重複しないよう連番付与）
    // 件数が多いとき（体数 × 件数の 2 乗になる）だけ名前の索引を使う。少数なら全走査のほうが速い。
    std::optional<SceneSerializer::NameIndexScope> nameScope;
    if (root["entities"].size() > 32) nameScope.emplace(scene);
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
    // ★名前の付け替えより先に、サブツリー内部を指していた guid を落とす。
    //   guid が残っていると解決が guid 優先なので、名前を付け替えても効かない。
    ClearInternalEntityRefGuids(reg, created, srcGuids);

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
        if (ej.is_object())
        {
            ej.erase("prefabLink");
            // [H] エディタ専用フラグはプレハブ（配布物にもなる）へ持ち込まない
            ej.erase("editorHidden"); ej.erase("editorLocked"); ej.erase("editorFolder"); ej.erase("partition");
        }
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

// インスタンス群の「正規形」。比較（差分表示）と 3-way マージのために、3 者（インスタンスの今の姿 = "mem"、
// .prefab = "sidecar"）を同じ形 {"count":N,"mem":id} へそろえる。内容が同じ実体は 1 つの実体へ寄せるので、
// 触っていない群は JSON として一致し、群を編集したインスタンスだけが差分になる。
// 台帳（強い参照）が実体を預かるので、この JSON から InstantiateSubtree しても群が戻る。
struct GroupCanon
{
    std::unordered_map<uint64_t, std::vector<instgroup::InstanceSetPtr>> buckets;
};

instgroup::InstanceSetPtr CanonSet(GroupCanon& canon, const instgroup::InstanceSetPtr& s)
{
    uint64_t h = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const unsigned char*>(s->items.data());
    const size_t n = s->items.size() * sizeof(instgroup::InstanceTRS);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    h ^= s->items.size();
    auto& vec = canon.buckets[h];
    for (const auto& c : vec)
        if (c.get() == s.get()
            || (c->items.size() == s->items.size()
                && (n == 0 || std::memcmp(c->items.data(), s->items.data(), n) == 0)))
            return c;
    vec.push_back(s);
    return s;
}

// entities 配列の群を正規形へ。prefabAbs が空でない場合は "sidecar" をそのプレハブの隣から読む。
void CanonicalizeGroups(json& entities, const std::string& prefabAbs, GroupCanon& canon)
{
    if (!entities.is_array()) return;
    for (auto& ej : entities)
    {
        if (!ej.is_object() || !ej.contains("instanceGroup") || !ej["instanceGroup"].is_object()) continue;
        json& cj = ej["instanceGroup"];
        instgroup::InstanceSetPtr set;
        if (cj.contains("mem") && cj["mem"].is_number_unsigned())
            set = instgroup::StoreFind(cj["mem"].get<uint64_t>());
        if (!set && !prefabAbs.empty() && cj.contains("sidecar") && cj["sidecar"].is_string())
        {
            const std::string key = cj["sidecar"].get<std::string>();
            if (key.find_first_of("/\\.:") == std::string::npos)
                set = instgroup::LoadSidecar(prefabAbs, key);
        }
        if (!set) continue;   // 読めない（欠損）。そのまま残す
        set = CanonSet(canon, set);
        instgroup::StoreRegister(set);
        cj = json{{"count", set->Count()}, {"mem", set->id}};
    }
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
    static const char* const kIgnored[] = {"name", "prefabLink", "guid", "parentGuid",
                                            "editorHidden", "editorLocked", "editorFolder", "siblingOrder", "partition"};   // [H] エディタ専用（プレハブの差分ではない）
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
    static const char* const kInstanceOwned[] = {"name", "guid", "parentGuid", "prefabLink",
                                                   "editorHidden", "editorLocked", "editorFolder", "siblingOrder", "partition"};   // [H] エディタ専用
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
// oldGeoBytes: 「配る前のプレハブが持っていた生成アセットの中身」（キーは CollectGeoPaths と同じ）。
// インスタンスのファイルがこれと 1 バイト違わなければ「そのインスタンスは自分では彫っていない」
// と判断でき、新しい形状を安全に上書きできる。違えば個別の手直しなので残す。
int MergePrefabInstances(Scene& scene, const std::string& sourcePath, const json& oldBase,
                         const json& newBase, const std::string& assetsDir, entt::entity except,
                         const std::unordered_map<std::string, std::string>& oldGeoBytes,
                         int* outGeoPropagated, int* outGeoKept, GroupCanon& canon)
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
        CanonicalizeGroups(mine["entities"], std::string{}, canon);   // 群は "mem" の正規形で 3 者を比べる

        json merged = mine;
        merged["entities"] = Merge3WaySubtree(mine["entities"], oldBase, newBase);
        if (merged["entities"].empty()) continue;

        // ★形状（.smsh / .hf / .splat）の伝播。
        //   3-way マージはパス文字列しか見ないので、ここを通さないと形だけ配られない
        //   （インスタンスは私物のコピーを持つ＝パスが必ず違う＝常に「手直し」判定になる）。
        //   このインスタンスのファイルが「配る前のプレハブ」と同一なら未編集なので中身を差し替え、
        //   違えば個別に彫ったものなので残す。
        {
            namespace fs = std::filesystem;
            const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;
            const auto minePaths = CollectGeoPaths(merged["entities"]);
            const auto newPaths  = CollectGeoPaths(newBase);
            for (const auto& [key, mineRel] : minePaths)
            {
                const auto np = newPaths.find(key);
                const auto op = oldGeoBytes.find(key);
                if (np == newPaths.end() || op == oldGeoBytes.end()) continue;

                const std::string mineBytes = ReadFileBytes(base + mineRel);
                if (mineBytes.empty()) continue;
                if (mineBytes != op->second)
                {
                    if (outGeoKept) ++*outGeoKept;   // 個別に彫ってある＝手直しとして残す
                    continue;
                }
                std::error_code ec;
                const bool propagated = atomicfile::CopyFileAtomic(fs::path(base + np->second), fs::path(base + mineRel), /*overwrite=*/true).ok;
                (void)ec;
                if (propagated && outGeoPropagated) ++*outGeoPropagated;
            }
        }

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
    {
        GroupCanon canon;   // 群の内容が同じなら一致扱い（"mem" と "sidecar" の表記差を差分にしない）
        CanonicalizeGroups(base["entities"], abs, canon);
        CanonicalizeGroups(mine["entities"], std::string{}, canon);
    }

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
    // 群のサイドカーは SavePrefab が上書きするので、配る前の群もここで読んで台帳へ預ける（正規形へ）。
    GroupCanon groupCanon;
    if (haveOld) CanonicalizeGroups(oldBase["entities"], assetsDir + rel, groupCanon);

    // ★SavePrefab は生成アセットをプレハブ専用パスへ上書きコピーするので、
    //   「配る前の中身」はここで先に読んでおく必要がある。
    std::unordered_map<std::string, std::string> oldGeoBytes;
    if (haveOld && oldBase.contains("entities"))
    {
        const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;
        for (const auto& [key, rel2] : CollectGeoPaths(oldBase["entities"]))
        {
            std::string bytes = ReadFileBytes(base + rel2);
            if (!bytes.empty()) oldGeoBytes.emplace(key, std::move(bytes));
        }
    }

    if (!SavePrefab(scene, root, assetsDir + rel, assetsDir)) return false;

    if (haveOld)
    {
        json newBase;
        if (ReadPrefabJson(assetsDir + rel, newBase))
        {
            CanonicalizeGroups(newBase["entities"], assetsDir + rel, groupCanon);
            int geoProp = 0, geoKept = 0;
            const int n = MergePrefabInstances(scene, rel, oldBase["entities"],
                                               newBase["entities"], assetsDir, root,
                                               oldGeoBytes, &geoProp, &geoKept, groupCanon);
            if (outPropagated) *outPropagated = n;
            if (geoProp > 0 || geoKept > 0)
                Logger::Info("プレハブの形状(.smsh/.hf/.splat): {} 件へ反映 / {} 件は"
                             "個別に彫ってあるので据え置き", geoProp, geoKept);
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
    // インスタンス群の実体は <プレハブ名>.prefab.inst/<キー>.jsonl へ書く（シーンと同じ方式。docs/SCENE_FORMAT_DESIGN.md §4.1）。
    // ★以前は Undo と同じ "mem"（メモリ台帳の番号）を書いていたので、エンジンを再起動すると群が空になった。
    //   .prefab 本体には個数と "sidecar":"<キー>" だけが残る（1 行 1 インスタンスのテキストで grep・git の差分が効く）。
    instgroup::SerializeCollector instCollected;
    std::string s;
    {
        // サイドカー名は guid。展開したばかりのインスタンスなどまだ guid が無い群へここで振る
        // （BuildSceneJson と同じ理由の const_cast。シーン保存時にどうせ全員へ振られる値を前倒しするだけ）。
        auto& mutableReg = const_cast<entt::registry&>(scene.GetRegistry());
        for (const entt::entity ge : mutableReg.view<InstanceGroup>())
        {
            auto* g = mutableReg.try_get<EntityGuid>(ge);
            if (!g)                 mutableReg.emplace<EntityGuid>(ge, EntityGuid{ NewEntityGuid() });
            else if (g->value == 0) g->value = NewEntityGuid();
        }
        instgroup::ScopedFileSerialize instScope(instCollected);
        s = SerializeSubtree(scene, root, assetsDir);
    }
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
                for (auto& ej : j["entities"])
                {
                    // 群のサイドカー名は guid（収集器のキー）。guid を落とす前にここへ写す。
                    if (ej.contains("instanceGroup") && ej["instanceGroup"].is_object()
                        && ej.contains("guid") && ej["guid"].is_string())
                        ej["instanceGroup"]["sidecar"] = ej["guid"];
                    ej.erase("guid"); ej.erase("parentGuid");
                }

            // ★生成アセット（.smsh / .hf / .splat）は .prefab 専用のコピーへ向け直す。
            //   向け直さないと .prefab が「作った元インスタンスのファイル」を指したままになり、
            //   その元を後から彫るだけで、触っていないはずの .prefab の中身が黙って変わる。
            if (j.contains("entities") && j["entities"].is_array())
            {
                const std::string base = assetsDir.empty() ? PathResolver::AssetsDir() : assetsDir;
                const std::string stem = fs::path(filePath).stem().string();
                auto& arr = j["entities"];
                for (size_t i = 0; i < arr.size(); ++i)
                {
                    for (const GeoField& g : kGeoFields)
                    {
                        auto c = arr[i].find(g.comp);
                        if (c == arr[i].end() || !c->is_object()) continue;
                        auto k = c->find(g.key);
                        if (k == c->end() || !k->is_string()) continue;
                        const std::string srcRel = k->get<std::string>();
                        if (srcRel.empty()) continue;

                        const std::string dstRel =
                            MakeGeoRelPath(g.key, "__prefab_" + stem + "_" + std::to_string(i));
                        if (srcRel == dstRel) continue;

                        std::error_code ec;
                        const fs::path src(base + srcRel), dst(base + dstRel);
                        if (!fs::exists(src, ec)) continue;
                        fs::create_directories(dst.parent_path(), ec);
                        // 原子的にコピーする（上書きの途中で落ちても、いまの .prefab が指している古いコピーを壊さない）
                        std::string geoBytes;
                        {
                            std::ifstream gin(src, std::ios::binary);
                            geoBytes.assign(std::istreambuf_iterator<char>(gin), std::istreambuf_iterator<char>());
                            if (!gin.eof() && gin.fail()) geoBytes.clear();
                        }
                        const atomicfile::Result gr = geoBytes.empty() ? atomicfile::Result{false, "元ファイルを読めません"}
                                                                       : atomicfile::WriteFile(dst, geoBytes);
                        if (!gr.ok)
                        {
                            Logger::Warn("プレハブ用に {} を {} へコピーできませんでした"
                                         "（元インスタンスのファイルを共有します）: {}",
                                         srcRel, dstRel, gr.error);
                            continue;
                        }
                        *k = dstRel;
                    }
                }
            }
            s = j.dump(2);
        }
    }

    fs::path dir = fs::path(filePath).parent_path();
    if (!dir.empty()) fs::create_directories(dir);

    // サイドカーと .prefab を 1 回のコミットで置き換える（.prefab が「無いデータ」を指す瞬間・古いサイドカーと新しい .prefab の
    // 食い違いを作らない）。群が無くなったら古い .inst はコミットの後に消える。
    atomicfile::Batch batch(fs::path(filePath).concat(".dx12txn"));
    atomicfile::RecoverPending(fs::path(filePath).concat(".dx12txn"));
    const instgroup::SaveStats instStats = instgroup::StageSidecars(filePath, instCollected, batch);
    if (!instStats.ok || !batch.Add(fs::path(filePath), s, atomicfile::JsonVerifier()) || !batch.Commit().ok)
    {
        Logger::Error("プレハブの書き込みに失敗しました: {} ({})", filePath, batch.Error());
        return false;
    }
    instgroup::CleanupSidecars(filePath, instCollected);
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

    // 群のサイドカー（<プレハブ>.prefab.inst/）を読む先をこのプレハブにする（pak ではゲームモードの VFS から読む）。
    entt::entity root = entt::null;
    {
        instgroup::ScopedLoadScene instLoad(filePath);
        root = InstantiateSubtree(scene, jsonStr, assetsDir, outAll);
    }
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
