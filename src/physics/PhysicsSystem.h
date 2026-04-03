#pragma once

#include <memory>
#include <cstdint>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e
{

struct RaycastHit
{
    bool               hit      = false;
    f32                distance = 0.0f;
    uint32_t           bodyId   = 0xFFFFFFFF;
    DirectX::XMFLOAT3  point    = {};
    DirectX::XMFLOAT3  normal   = {};
};

class PhysicsSystem
{
public:
    PhysicsSystem();
    ~PhysicsSystem();

    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    void Initialize();
    void Update(f32 dt, entt::registry& registry);
    void Shutdown();

    // Entity の物理体を登録/解除
    void RegisterBody(entt::registry& registry, entt::entity entity);
    void UnregisterBody(entt::registry& registry, entt::entity entity);
    void UnregisterAllBodies(entt::registry& registry);

    // 物理操作 API
    void ApplyForce(uint32_t bodyId, DirectX::XMFLOAT3 force);
    void ApplyImpulse(uint32_t bodyId, DirectX::XMFLOAT3 impulse);
    void SetLinearVelocity(uint32_t bodyId, DirectX::XMFLOAT3 vel);
    DirectX::XMFLOAT3 GetLinearVelocity(uint32_t bodyId) const;
    void SetPosition(uint32_t bodyId, DirectX::XMFLOAT3 pos);

    RaycastHit Raycast(DirectX::XMFLOAT3 origin,
                       DirectX::XMFLOAT3 direction,
                       f32 maxDistance = 1000.0f) const;

    bool IsInitialized() const { return m_initialized; }
    void ResetAccumulator() { m_accumulator = 0.0f; }

private:
    void SyncTransformsToPhysics(entt::registry& registry);
    void SyncPhysicsToTransforms(entt::registry& registry);

    struct JoltImpl;
    std::unique_ptr<JoltImpl> m_impl;

    bool  m_initialized = false;
    f32   m_accumulator = 0.0f;

    static constexpr f32 kFixedTimeStep  = 1.0f / 60.0f;
    static constexpr int kCollisionSteps = 1;
};

} // namespace dx12e
