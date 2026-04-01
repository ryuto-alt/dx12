#pragma once

#include <entt/entt.hpp>
#include <string>

namespace dx12e
{

class Entity
{
public:
    Entity() = default;
    Entity(entt::entity handle, entt::registry* registry);

    template<typename T, typename... Args>
    T& AddComponent(Args&&... args)
    {
        return m_registry->emplace<T>(m_handle, std::forward<Args>(args)...);
    }

    template<typename T>
    T& GetComponent()
    {
        return m_registry->get<T>(m_handle);
    }

    template<typename T>
    const T& GetComponent() const
    {
        return m_registry->get<T>(m_handle);
    }

    template<typename T>
    bool HasComponent() const
    {
        return m_registry->all_of<T>(m_handle);
    }

    template<typename T>
    void RemoveComponent()
    {
        m_registry->remove<T>(m_handle);
    }

    entt::entity GetHandle() const { return m_handle; }
    bool IsValid() const { return m_registry && m_registry->valid(m_handle); }

    bool operator==(const Entity& other) const { return m_handle == other.m_handle; }
    bool operator!=(const Entity& other) const { return m_handle != other.m_handle; }

private:
    entt::entity    m_handle   = entt::null;
    entt::registry* m_registry = nullptr;
};

} // namespace dx12e
