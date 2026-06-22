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

// ビルボード用: ワールド中心 + スクリーン空間ピクセルオフセット。
// VS で常にカメラを向く一定スクリーンサイズの 2D アイコンになる。
struct IconBillboardVertex
{
    DirectX::XMFLOAT3 center;
    DirectX::XMFLOAT2 offset;  // ピクセル単位の 2D オフセット
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
                const DirectX::XMFLOAT4X4& viewProj,
                u32 screenW, u32 screenH);

private:
    void AddLine(DirectX::XMFLOAT3 a, DirectX::XMFLOAT3 b, DirectX::XMFLOAT3 color);

    // ビルボード（常にカメラを向く・一定スクリーンサイズ）アイコン
    void AddBillboardLine(const DirectX::XMFLOAT3& center,
                          DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b,
                          const DirectX::XMFLOAT3& color);

    // 常時表示: カメラの向きに追従する 3D カメラアイコン（本体＋前方レンズ）
    void AddCameraIcon(const DirectX::XMFLOAT3& pos,
                       const DirectX::XMFLOAT4& quat,
                       const DirectX::XMFLOAT3& color);

    // 常時表示: 小さなビルボードアイコン（どの角度からも見える）
    void AddDirLightIcon(const DirectX::XMFLOAT3& pos,
                         const DirectX::XMFLOAT3& color);

    void AddPointLightIcon(const DirectX::XMFLOAT3& pos,
                           const DirectX::XMFLOAT3& color);

    // 選択時のみ: 詳細表示
    void AddCameraFrustum(const DirectX::XMFLOAT3& pos,
                          const DirectX::XMFLOAT4& quat,
                          f32 fovDegrees, f32 nearClip, f32 farClip,
                          const DirectX::XMFLOAT3& color,
                          bool ortho = false, f32 orthoSize = 10.0f);

    void AddDirectionalArrow(const DirectX::XMFLOAT3& origin,
                             const DirectX::XMFLOAT3& direction,
                             f32 length,
                             const DirectX::XMFLOAT3& color);

    void AddPointLightSphere(const DirectX::XMFLOAT3& center,
                             f32 radius, u32 segments,
                             const DirectX::XMFLOAT3& color);

    static constexpr u32 kMaxVertices = 65536;

    // 3D ライン（選択時の補助線: フラスタム / 範囲球 / 矢印）
    std::vector<IconLineVertex> m_vertices;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;

    // ビルボード（常時表示の基本アイコン）
    std::vector<IconBillboardVertex> m_billboardVerts;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_billboardVB;
    D3D12_VERTEX_BUFFER_VIEW                     m_billboardVBView{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_billboardRootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_billboardPSO;

    bool m_initialized = false;
};

} // namespace dx12e
