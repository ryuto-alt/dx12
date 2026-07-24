#pragma once

#include <entt/entt.hpp>
#include <stdexcept>
#include <string>
#include <typeinfo>

namespace dx12e
{

class Entity
{
public:
    Entity() = default;
    Entity(entt::entity handle, entt::registry* registry);

    // 無効な Entity（findEntity 失敗の戻り値や削除済みハンドル）への操作は
    // ヌルポインタ参照で即死せず例外にする。Lua から呼ばれた場合は sol2 が
    // 捕捉して Lua エラー（スクリプト側で原因が読めるメッセージ）に変換される。
    template<typename T, typename... Args>
    T& AddComponent(Args&&... args)
    {
        EnsureValid("AddComponent");
        return m_registry->emplace<T>(m_handle, std::forward<Args>(args)...);
    }

    template<typename T>
    T& GetComponent()
    {
        EnsureHas<T>("GetComponent");
        return m_registry->get<T>(m_handle);
    }

    template<typename T>
    const T& GetComponent() const
    {
        EnsureHas<T>("GetComponent");
        return m_registry->get<T>(m_handle);
    }

    // 既にあればそれを、無ければ既定構築して返す。Lua から「まず付けてから設定」を
    // 1 行で書けるようにするためのもの（scene:playUiAnim 等が使う）。
    template<typename T>
    T& GetOrAddComponent()
    {
        EnsureValid("GetOrAddComponent");
        return m_registry->get_or_emplace<T>(m_handle);
    }

    template<typename T>
    bool HasComponent() const
    {
        return IsValid() && m_registry->all_of<T>(m_handle);
    }

    template<typename T>
    void RemoveComponent()
    {
        if (IsValid())
            m_registry->remove<T>(m_handle);
    }

    entt::entity GetHandle() const { return m_handle; }
    bool IsValid() const { return m_registry && m_registry->valid(m_handle); }

    bool operator==(const Entity& other) const { return m_handle == other.m_handle; }
    bool operator!=(const Entity& other) const { return m_handle != other.m_handle; }

private:
    void EnsureValid(const char* op) const
    {
        if (!IsValid())
            throw std::runtime_error(std::string("Entity::") + op +
                ": invalid entity (destroyed or findEntity returned nothing). Check isValid() first.");
    }

    template<typename T>
    void EnsureHas(const char* op) const
    {
        EnsureValid(op);
        if (!m_registry->all_of<T>(m_handle))
            throw std::runtime_error(std::string("Entity::") + op +
                ": missing component " + typeid(T).name());
    }

    entt::entity    m_handle   = entt::null;
    entt::registry* m_registry = nullptr;
};

} // namespace dx12e
