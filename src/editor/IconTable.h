#pragma once

// ===== 種別 -> アイコン・色・ラベルの唯一の表（W1-A6 / RM-A4）=====
// ヒエラルキーの行頭、インスペクタの見出し、アセットブラウザ、コマンドパレットが同じ表を引く。
// 以前は strstr の連鎖（"UI" を含む名前が全部 UI アイコンになる等）と、画面ごとの switch が別々にあった。
// ここは「名前の完全一致」で引く。表に無い名前は汎用（Empty）になり、部分一致では決して決めない。
//
//   ForEntity(reg, e)        エンティティの代表種別（持っているコンポーネントの優先順で 1 つ）
//   ForComponent(metaName)   コンポーネント名（シリアライザの typeName / entt meta 名 / インスペクタの見出し）
//   ForAsset(AssetKind)      アセットの種別
//
// glyph は Lucide のアイコンフォントのグリフ、gg は Graphite Gauge の製図ペンのアイコンの id（GgIconDraw.h で描ける）。
// 色は現在のテーマのトークン（theme::Type*）。Graphite のときだけ GG 仕様の kind.material / kind.texture を使う。
// ★色の ImVec4 はテーマ切り替えで中身が変わる。ポインタ（tint）を持っておけば切り替えに追従する。

#include "editor/AssetBrowserLogic.h"
#include "editor/EditorIcons.h"
#include "editor/EditorTheme.h"
#include "ecs/Components.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <entt/entt.hpp>

#include <cstring>
#include <string>
#include <string_view>

namespace dx12e::icons
{

using AssetKind = abl::Kind;

struct KindVisual
{
    const char* glyph;   // ICON_*（UTF-8 の Lucide グリフ）
    ImU32       color;   // 呼んだ時点のテーマの色（アルファ 1）
    const char* label;   // 短い種別名（英語の識別名）
    const char* gg;      // GG の製図ペンのアイコン id（"i-mesh" 等。無ければ nullptr）
    const ImVec4* tint;  // テーマのトークンへのポインタ（切り替えに追従したい呼び出し側用）
};

// Graphite のときだけ使う GG 仕様の kind 色（IconTable.cpp。ThemeGraphite.gen.h を重ヘッダから隠すため）。
// slot: 0 = material, 1 = texture。値が無ければ nullptr。
const ImVec4* GraphiteKindTint(int slot);

enum class Id : int
{
    Empty, Group, Folder, Music, Transform, Mesh, Material, Texture, Light, Sun, Camera, Audio, Reverb, Script, Brain,
    Physics, Collider, Character, Ui, UiText, UiButton, Particle, Trail, Decal, Terrain, Sprite, Anim,
    Net, Trigger, Grid, Prefab, Scene, Shader, MaterialGraph, Sequence, Vfx, Water, Tag, File,
    kCount
};

namespace detail
{
struct Row { const char* glyph; const ImVec4* tint; const char* label; const char* gg; };

inline Row RowOf(Id id)
{
    namespace th = dx12e::theme;
    switch (id)
    {
    case Id::Empty:         return { ICON_T_EMPTY,     &th::TypeEmpty,   "Empty",         "i-empty" };
    case Id::Group:         return { ICON_T_GROUP,     &th::TypeFolder,  "Group",         "i-folder" };
    case Id::Folder:        return { ICON_FOLDER,      &th::TypeFolder,  "Folder",        "i-folder" };
    case Id::Transform:     return { ICON_T_TRANSFORM, &th::TypeEmpty,   "Transform",     "i-move" };
    case Id::Mesh:          return { ICON_T_MESH,      &th::TypeMesh,    "Mesh",          "i-mesh" };
    case Id::Material:      return { ICON_T_MATERIAL,  &th::TypeMesh,    "Material",      "i-mat" };
    case Id::Texture:       return { ICON_IMAGE,       &th::TypePhysics, "Texture",       "i-tex" };
    case Id::Light:         return { ICON_T_LIGHT,     &th::TypeLight,   "Light",         "i-light" };
    case Id::Sun:           return { ICON_T_SUN,       &th::TypeLight,   "Sun",           "i-light" };
    case Id::Camera:        return { ICON_T_CAMERA,    &th::TypeCamera,  "Camera",        "i-cam" };
    case Id::Audio:         return { ICON_T_AUDIO,     &th::TypeAudio,   "Audio",         "i-audio" };
    case Id::Music:         return { ICON_MUSIC,       &th::TypeAudio,   "Audio",         "i-wave" };
    case Id::Reverb:        return { ICON_T_AUDIO,     &th::TypeAudio,   "Reverb",        "i-wave" };
    case Id::Script:        return { ICON_T_SCRIPT,    &th::TypeScript,  "Script",        "i-script" };
    case Id::Brain:         return { ICON_T_BRAIN,     &th::TypeScript,  "Brain",         "i-nodes" };
    case Id::Physics:       return { ICON_T_PHYSICS,   &th::TypePhysics, "Physics",       "i-phys" };
    case Id::Collider:      return { ICON_T_COLLIDER,  &th::TypePhysics, "Collider",      "i-vol" };
    case Id::Character:     return { ICON_T_CHARACTER, &th::TypePhysics, "Character",     "i-person" };
    case Id::Ui:            return { ICON_T_UI,        &th::TypeUi,      "UI",            "i-ui" };
    case Id::UiText:        return { ICON_T_TEXT,      &th::TypeUi,      "UIText",        "i-font" };
    case Id::UiButton:      return { ICON_T_BUTTON,    &th::TypeUi,      "UIButton",      "i-ui" };
    case Id::Particle:      return { ICON_T_PARTICLE,  &th::TypeLight,   "Particle",      "i-particle" };
    case Id::Trail:         return { ICON_T_TRAIL,     &th::TypeLight,   "Trail",         "i-curve" };
    case Id::Decal:         return { ICON_T_DECAL,     &th::TypeMesh,    "Decal",         "i-plane" };
    case Id::Terrain:       return { ICON_T_TERRAIN,   &th::TypeMesh,    "Terrain",       "i-plane" };
    case Id::Sprite:        return { ICON_T_SPRITE,    &th::TypeMesh,    "Sprite",        "i-tex" };
    case Id::Anim:          return { ICON_T_ANIM,      &th::TypeScript,  "Animation",     "i-anim" };
    case Id::Net:           return { ICON_T_NET,       &th::TypeEmpty,   "Network",       "i-link" };
    case Id::Trigger:       return { ICON_T_TRIGGER,   &th::TypeScript,  "Trigger",       "i-event" };
    case Id::Grid:          return { ICON_T_GRID,      &th::TypeEmpty,   "Grid",          "i-grid" };
    case Id::Prefab:        return { ICON_PACKAGE,     &th::TypePrefab,  "Prefab",        "i-prefab" };
    case Id::Scene:         return { ICON_FILM,        &th::TypeScene,   "Scene",         "i-scene" };
    case Id::Shader:        return { ICON_T_SHADER,    &th::TypeScript,  "Shader",        "i-cmd" };
    case Id::MaterialGraph: return { ICON_T_SPLINE,    &th::TypeLight,   "MaterialGraph", "i-nodes" };
    case Id::Sequence:      return { ICON_T_SEQUENCE,  &th::TypeUi,      "Sequence",      "i-cut" };
    case Id::Vfx:           return { ICON_T_PARTICLE,  &th::TypeFolder,  "VFX",           "i-particle" };
    case Id::Water:         return { ICON_T_REVERB,    &th::TypeMesh,    "Water",         "i-wave" };
    case Id::Tag:           return { ICON_T_EMPTY,     &th::TypeEmpty,   "Tag",           "i-tag" };
    case Id::File:
    default:                return { ICON_FILE,        &th::TypeEmpty,   "File",          "i-doc" };
    }
}

inline const ImVec4* TintOf(Id id)
{
    return RowOf(id).tint;
}

inline KindVisual Make(Id id, const ImVec4* tintOverride = nullptr)
{
    const Row r = RowOf(id);
    const ImVec4* t = tintOverride ? tintOverride : r.tint;
    // Graphite のときだけ Material / Texture は GG 仕様の kind 色にする（画面によらず同じ色）。
    if (dx12e::theme::CurrentVariant() == dx12e::theme::Variant::Graphite)
    {
        if (id == Id::Material) { if (const ImVec4* g = GraphiteKindTint(0)) t = g; }
        if (id == Id::Texture)  { if (const ImVec4* g = GraphiteKindTint(1)) t = g; }
    }
    return { r.glyph, ImGui::ColorConvertFloat4ToU32(*t), r.label, r.gg, t };
}

// コンポーネント名 -> 種別。名前は typeName（シリアライザ / entt meta）とインスペクタの見出し（空白あり）の両方を受ける。
struct NameRow { const char* name; Id id; };
inline constexpr NameRow kComponentNames[] = {
    // 基本
    { "NameTag", Id::Tag }, { "Tag", Id::Tag }, { "EditorFlags", Id::Tag }, { "EntityDisabled", Id::Tag },
    { "EntityGuid", Id::Tag }, { "DataComponent", Id::Tag },
    { "Transform", Id::Transform },
    { "PrefabLink", Id::Prefab },
    // 描画
    { "MeshRenderer", Id::Mesh }, { "SculptMesh", Id::Mesh }, { "VirtualGeometry", Id::Mesh },
    { "FoliageLayer", Id::Mesh }, { "InstanceGroup", Id::Mesh },
    { "Material", Id::Material },
    { "Terrain", Id::Terrain }, { "WaterBody", Id::Water },
    { "DecalComponent", Id::Decal }, { "Decal", Id::Decal },
    { "Sprite2D", Id::Sprite }, { "SpriteAnimator", Id::Sprite },
    { "ParticleEmitter", Id::Particle }, { "vfx", Id::Particle },
    { "TrailRenderer", Id::Trail },
    // ライト・カメラ
    { "DirectionalLight", Id::Sun }, { "PointLight", Id::Light }, { "SpotLight", Id::Light },
    { "ReflectionProbe", Id::Light },
    { "CameraComponent", Id::Camera }, { "Camera", Id::Camera },
    // 音
    { "AudioSource", Id::Audio }, { "AudioReverbZone", Id::Audio },
    // 物理
    { "RigidBody", Id::Physics }, { "CharacterController", Id::Character },
    { "BoxCollider", Id::Collider }, { "SphereCollider", Id::Collider }, { "CapsuleCollider", Id::Collider },
    { "MeshCollider", Id::Collider }, { "ConvexHullCollider", Id::Collider },
    // スクリプト・AI・ゲームプレイ
    { "LuaScript", Id::Script }, { "Brain", Id::Brain },
    { "Trigger", Id::Trigger }, { "Gimmick", Id::Trigger },
    // アニメーション
    { "AnimatorController", Id::Anim }, { "FootIK", Id::Anim }, { "SkeletalAnimation", Id::Anim },
    { "NodeAnimation", Id::Anim }, { "Animator", Id::Anim },
    // UI（完全一致の一覧。"UI" を含むだけの名前はここに載らない）
    { "UICanvas", Id::Ui }, { "UIRect", Id::Ui }, { "UIImage", Id::Ui }, { "UISlider", Id::Ui },
    { "UIScrollView", Id::Ui }, { "UILayout", Id::Ui }, { "UIToggle", Id::Ui }, { "UIAnimator", Id::Ui },
    { "UIAnimPlayer", Id::Ui },
    { "UIText", Id::UiText }, { "UIButton", Id::UiButton },
    // ネットワーク・その他
    { "NetworkIdentity", Id::Net }, { "NetworkTransform", Id::Net }, { "Network", Id::Net },
    { "GridPlane", Id::Grid },
};

// 空白と "##" 以降（ImGui の ID 部分）を除いた名前で引く。"Box Collider" -> "BoxCollider"、"Shader##Sprite2D" -> "Shader"。
inline bool FindComponent(std::string_view name, Id& out)
{
    char buf[64];
    size_t n = 0;
    for (size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '#' && i + 1 < name.size() && name[i + 1] == '#') break;
        if (name[i] == ' ') continue;
        if (n + 1 >= sizeof(buf)) return false;
        buf[n++] = name[i];
    }
    buf[n] = '\0';
    for (const NameRow& r : kComponentNames)
        if (std::strcmp(r.name, buf) == 0) { out = r.id; return true; }
    return false;
}
} // namespace detail

// コンポーネント名の完全一致。表に無ければ汎用（Empty）。found で一致したかを返せる。
inline KindVisual ForComponent(std::string_view metaName, bool* found = nullptr)
{
    Id id = Id::Empty;
    const bool ok = detail::FindComponent(metaName, id);
    if (found) *found = ok;
    return detail::Make(ok ? id : Id::Empty);
}

// エンティティの代表種別。優先順は従来のヒエラルキーと同じ（カメラ > 太陽 > ライト > UI > 地形 > …）。
// hasChildren: コンポーネントを持たない空の親を「グループ」にする。
inline Id EntityId(const entt::registry& reg, entt::entity e, bool hasChildren = false)
{
    if (reg.all_of<CameraComponent>(e))                              return Id::Camera;
    if (reg.all_of<DirectionalLight>(e))                             return Id::Sun;
    if (reg.any_of<PointLight, SpotLight>(e))                        return Id::Light;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton, UISlider, UIScrollView, UILayout, UIToggle>(e)) return Id::Ui;
    if (reg.all_of<Terrain>(e))                                      return Id::Terrain;
    if (reg.all_of<ParticleEmitter>(e))                              return Id::Particle;
    if (reg.all_of<DecalComponent>(e))                               return Id::Decal;
    if (reg.all_of<ReflectionProbe>(e))                              return Id::Light;
    if (reg.all_of<MeshRenderer>(e) || reg.all_of<SculptMesh>(e))    return Id::Mesh;
    if (reg.all_of<Sprite2D>(e))                                     return Id::Sprite;
    if (reg.all_of<TrailRenderer>(e))                                return Id::Trail;
    if (reg.all_of<WaterBody>(e))                                    return Id::Water;
    if (reg.any_of<AudioSource, AudioReverbZone>(e))                 return Id::Audio;
    if (reg.any_of<RigidBody, CharacterController>(e))               return Id::Physics;
    if (reg.any_of<BoxCollider, SphereCollider, CapsuleCollider, ConvexHullCollider, MeshCollider>(e)) return Id::Collider;
    if (reg.all_of<Brain>(e))                                        return Id::Brain;
    if (reg.all_of<LuaScript>(e))                                    return Id::Script;
    if (hasChildren)                                                 return Id::Group;
    return Id::Empty;
}

inline KindVisual ForEntity(const entt::registry& reg, entt::entity e, bool hasChildren = false)
{
    return detail::Make(EntityId(reg, e, hasChildren));
}

inline Id AssetId(AssetKind k)
{
    switch (k)
    {
    case AssetKind::Folder:        return Id::Folder;
    case AssetKind::Model:         return Id::Mesh;
    case AssetKind::Texture:       return Id::Texture;
    case AssetKind::Scene:         return Id::Scene;
    case AssetKind::Script:        return Id::Script;
    case AssetKind::Audio:         return Id::Music;
    case AssetKind::Prefab:        return Id::Prefab;
    case AssetKind::Shader:        return Id::Shader;
    case AssetKind::Material:      return Id::Material;
    case AssetKind::UiAnim:        return Id::Anim;
    case AssetKind::SpriteSheet:   return Id::Grid;
    case AssetKind::MaterialGraph: return Id::MaterialGraph;
    case AssetKind::Sequence:      return Id::Sequence;
    case AssetKind::Vfx:           return Id::Vfx;
    default:                       return Id::File;
    }
}

// アセットの種別。グリフと色は従来のアセットブラウザと同じ（Model=青 / Texture=緑 / Scene=橙 / Script=紫 / Audio=ティール …）。
// Graphite のときの Material / Texture の色は detail::Make が差し替える。
inline KindVisual ForAsset(AssetKind k)
{
    namespace th = dx12e::theme;
    const Id id = AssetId(k);
    const ImVec4* tint = nullptr;
    switch (k)
    {
    case AssetKind::Model:         tint = &th::TypeCamera; break;    // 青
    case AssetKind::Material:      tint = &th::TypeLight;  break;    // 琥珀
    case AssetKind::UiAnim:        tint = &th::TypeUi;     break;    // アニメ系はマゼンタ寄りで統一
    case AssetKind::SpriteSheet:   tint = &th::TypeUi;     break;
    case AssetKind::MaterialGraph: tint = &th::TypeLight;  break;
    default: break;
    }
    return detail::Make(id, tint);
}

} // namespace dx12e::icons
