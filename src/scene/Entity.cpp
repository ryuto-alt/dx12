#include "scene/Entity.h"

namespace dx12e
{

Entity::Entity(entt::entity handle, entt::registry* registry)
    : m_handle(handle)
    , m_registry(registry)
{
}

} // namespace dx12e
