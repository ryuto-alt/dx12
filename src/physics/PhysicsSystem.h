#pragma once

#include <memory>
#include <cstdint>
#include <unordered_map>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"
#include "engine/core/EventBus.h"   // EngineEvent / EventBus（接触イベントの配信先）

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

    // CharacterController（CharacterVirtual）の生成/破棄。RegisterBody と同じライフサイクル
    // （Play 開始/シーンロードで生成、Stop/Shutdown で破棄）。
    void RegisterCharacter(entt::registry& registry, entt::entity entity);
    void UnregisterCharacter(entt::registry& registry, entt::entity entity);
    void UnregisterAllCharacters(entt::registry& registry);

    // CharacterVirtual を 1 ステップ進める（固定 dt ループ内から呼ぶ）。
    // 入力（_desiredVel/_jumpQueued）→ 速度合成 → ExtendedUpdate → _grounded/_verticalVel 更新。
    void StepCharacters(f32 fixedDt, entt::registry& registry);

    // CharacterVirtual の位置を Transform に書き戻す（accumulator ループ後に 1 回）。
    void SyncCharactersToTransforms(entt::registry& registry);

    // 物理操作 API
    void ApplyForce(uint32_t bodyId, DirectX::XMFLOAT3 force);
    void ApplyImpulse(uint32_t bodyId, DirectX::XMFLOAT3 impulse);
    void SetLinearVelocity(uint32_t bodyId, DirectX::XMFLOAT3 vel);
    DirectX::XMFLOAT3 GetLinearVelocity(uint32_t bodyId) const;
    void SetPosition(uint32_t bodyId, DirectX::XMFLOAT3 pos);

    RaycastHit Raycast(DirectX::XMFLOAT3 origin,
                       DirectX::XMFLOAT3 direction,
                       f32 maxDistance = 1000.0f) const;

    // --- 空間クエリ（Broadphase）---
    // out[0..cap) に entity を書き込み、実際に書いた数を返す。cap 超えは切り捨て。
    // physics 未初期化 / out が null / cap==0 / bodyId→entity マップが空なら 0 を返す。
    size_t OverlapBox(const DirectX::XMFLOAT3& center,
                      const DirectX::XMFLOAT3& halfExtents,
                      entt::entity* out, size_t cap) const;

    size_t OverlapSphere(const DirectX::XMFLOAT3& center,
                         float radius,
                         entt::entity* out, size_t cap) const;

    // --- 接触コールバック（EventBus 経由）---
    // EventBus を設定する。Initialize より前でも後でも可。
    // Null を渡すと以後のコールバック発火を停止する（PhysicsSystem は所有しない）。
    void SetEventBus(EventBus* bus) { m_eventBus = bus; }

    // --- 停止/ステップ（時間モデル用、Phase 4 の他機能と共用）---
    void SetPaused(bool paused) { m_paused = paused; }
    void Step(float dt);                  // 1固定ステップを即時実行（manual モード用）
    void SetGravity(DirectX::XMFLOAT3 g);

    bool IsInitialized() const { return m_initialized; }
    void ResetAccumulator() { m_accumulator = 0.0f; }

private:
    void SyncTransformsToPhysics(entt::registry& registry);
    void SyncPhysicsToTransforms(entt::registry& registry);
    // ContactListener の pending をメインスレッドで EventBus へ流す。
    void FlushPendingContacts();

    struct JoltImpl;
    std::unique_ptr<JoltImpl> m_impl;

    // bodyId → entt::entity の逆引き（RegisterBody 時に追加、Unregister 時に削除）。
    std::unordered_map<uint32_t, entt::entity> m_bodyToEntity;

    EventBus* m_eventBus = nullptr;   // 外部所有。PhysicsSystem は解放しない。
    bool      m_paused   = false;

    bool  m_initialized = false;
    f32   m_accumulator = 0.0f;

    static constexpr f32 kFixedTimeStep  = 1.0f / 60.0f;
    static constexpr int kCollisionSteps = 1;
};

} // namespace dx12e
