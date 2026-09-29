#pragma once

// ===== エンティティの種別 → アイコングリフ + 種別カラー =====
// ヒエラルキーの行頭とインスペクタの見出し（エンティティ名）で共用する。
// 色は「控えめな色味」: メッシュは中性グレー、ライト=琥珀、カメラ=青、UI=マゼンタ、物理=緑 …
// （全部同じ濃青チップだった旧 PNG アイコンをやめ、モノクロのグリフ + 種別ごとの色にした）。

#include "editor/EditorTheme.h"
#include "editor/EditorIcons.h"
#include "ecs/Components.h"

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

#include <entt/entt.hpp>

namespace dx12e
{

struct EntityGlyph
{
    const char*   glyph;   // ICON_* （UTF-8 の Lucide グリフ）
    const ImVec4* tint;    // theme::Type*
};

// hasChildren: コンポーネントを持たない空の親を「グループ」（フォルダ）アイコンにする。
inline EntityGlyph PickEntityGlyph(entt::registry& reg, entt::entity e, bool hasChildren)
{
    using namespace dx12e::theme;
    if (reg.all_of<CameraComponent>(e))                            return { ICON_T_CAMERA,   &TypeCamera };
    if (reg.all_of<DirectionalLight>(e))                           return { ICON_T_SUN,      &TypeLight };
    if (reg.any_of<PointLight, SpotLight>(e))                      return { ICON_T_LIGHT,    &TypeLight };
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton>(e)) return { ICON_T_UI,       &TypeUi };
    if (reg.all_of<Terrain>(e))                                    return { ICON_T_TERRAIN,  &TypeMesh };
    if (reg.all_of<ParticleEmitter>(e))                            return { ICON_T_PARTICLE, &TypeLight };
    if (reg.all_of<DecalComponent>(e))                             return { ICON_T_DECAL,    &TypeMesh };
    if (reg.all_of<MeshRenderer>(e) || reg.all_of<SculptMesh>(e))  return { ICON_T_MESH,     &TypeMesh };
    if (reg.any_of<AudioSource, AudioReverbZone>(e))               return { ICON_T_AUDIO,    &TypeAudio };
    if (reg.any_of<RigidBody, CharacterController>(e))             return { ICON_T_PHYSICS,  &TypePhysics };
    if (reg.any_of<BoxCollider, SphereCollider, CapsuleCollider,
                   ConvexHullCollider, MeshCollider>(e))           return { ICON_T_COLLIDER, &TypePhysics };
    if (reg.all_of<Brain>(e))                                      return { ICON_T_BRAIN,    &TypeScript };
    if (reg.all_of<LuaScript>(e))                                  return { ICON_T_SCRIPT,   &TypeScript };
    if (hasChildren)                                               return { ICON_T_GROUP,    &TypeFolder };
    return { ICON_T_EMPTY, &TypeEmpty };
}

} // namespace dx12e
