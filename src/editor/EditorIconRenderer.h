#pragma once

#include <vector>
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class EditorContext;

struct IconLineVertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 color;
};

class EditorIconRenderer
{
public:
    EditorIconRenderer() = default;
    ~EditorIconRenderer() = default;

    EditorIconRenderer(const EditorIconRenderer&) = delete;
    EditorIconRenderer& operator=(const EditorIconRenderer&) = delete;

    void Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat,
                    const std::wstring& shaderDir);

    void BeginFrame();

    void CollectFromRegistry(entt::registry& registry, const EditorContext& ctx);

    void Render(ID3D12GraphicsCommandList* cmdList,
                const DirectX::XMFLOAT4X4& viewProj);

private:
    void AddLine(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b, DirectX::XMFLOAT3 color);

    // 常時表示: 小さなアイコン（クリック可能なサイズ）
    void AddCameraIcon(const DirectX::XMFLOAT3& pos,
                       const DirectX::XMFLOAT4& quat,
                       const DirectX::XMFLOAT3& color);

    void AddDirLightIcon(const DirectX::XMFLOAT3& pos,
                         const DirectX::XMFLOAT3& color);

    void AddPointLightIcon(const DirectX::XMFLOAT3& pos,
                           const DirectX::XMFLOAT3& color);

    // 選択時のみ: 詳細表示
    void AddCameraFrustum(const DirectX::XMFLOAT3& pos,
                          const DirectX::XMFLOAT4& quat,
                          f32 fovDegrees, f32 nearClip, f32 farClip,
                          const DirectX::XMFLOAT3& color);

    void AddDirectionalArrow(const DirectX::XMFLOAT3& origin,
                             const DirectX::XMFLOAT3& direction,
                             f32 length,
                             const DirectX::XMFLOAT3& color);

    void AddPointLightSphere(const DirectX::XMFLOAT3& center,
                             f32 radius, u32 segments,
                             const DirectX::XMFLOAT3& color);

    static constexpr u32 kMaxVertices = 65536;

    std::vector<IconLineVertex> m_vertices;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;
    bool m_initialized = false;
};

} // namespace dx12e
