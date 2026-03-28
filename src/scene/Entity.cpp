#include "scene/Entity.h"

#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"

namespace dx12e
{

Entity::Entity() = default;
Entity::~Entity() = default;
Entity::Entity(Entity&&) noexcept = default;
Entity& Entity::operator=(Entity&&) noexcept = default;

} // namespace dx12e
