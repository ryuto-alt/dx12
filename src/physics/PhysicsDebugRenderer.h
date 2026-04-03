#pragma once

#include <string>
#include <vector>
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class PhysicsSystem;

struct DebugLineVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 color;
};

class PhysicsDebugRenderer
{
public:
    PhysicsDebugRenderer() = default;
    ~PhysicsDebugRenderer() = default;

    PhysicsDebugRenderer(const PhysicsDebugRenderer&) = delete;
    PhysicsDebugRenderer& operator=(const PhysicsDebugRenderer&) = delete;

    void Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat,
                    const std::wstring& shaderDir);

    void BeginFrame();

    void AddLine(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b,
                 DirectX::XMFLOAT3 color = {0.0f, 1.0f, 0.0f});

    void AddBox(DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 halfExtents,
                DirectX::XMFLOAT4 quat = {0,0,0,1},
                DirectX::XMFLOAT3 color = {0.0f, 1.0f, 0.0f});

    void AddSphere(DirectX::XMFLOAT3 center, f32 radius, u32 segments = 16,
                   DirectX::XMFLOAT3 color = {0.0f, 1.0f, 0.0f});

    void AddCapsule(DirectX::XMFLOAT3 center, f32 radius, f32 halfHeight,
                    DirectX::XMFLOAT4 quat = {0,0,0,1},
                    DirectX::XMFLOAT3 color = {0.0f, 1.0f, 0.0f});

    // PhysicsSystem + ECS から全コライダーを収集
    void CollectFromRegistry(entt::registry& registry);

    void Render(ID3D12GraphicsCommandList* cmdList,
                const DirectX::XMFLOAT4X4& viewProj);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const        { return m_enabled; }

private:
    static constexpr u32 kMaxVertices = 131072; // 65536 lines * 2 vertices

    std::vector<DebugLineVertex> m_vertices;

    // GPU resources
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;

    bool m_enabled     = false;
    bool m_initialized = false;
};

} // namespace dx12e
