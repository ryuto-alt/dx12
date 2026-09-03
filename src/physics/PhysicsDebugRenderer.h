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

// 当たり判定表示のふるい。
// ★全部出すと、壁と床（Static）だけで画面が線で埋まって何も読めなくなる。
//   256 エンティティ規模の実シーンでは「全部表示」は事実上使えないので、
//   何を見たいかで絞れるようにする。
struct DebugDrawFilter
{
    enum class Scope : u8
    {
        All = 0,       // 全部（従来どおり）
        Selected,      // 選択中のエンティティとその子孫だけ ★これが一番よく使う
        NearCamera,    // カメラから maxDistance 以内だけ
    };
    Scope scope = Scope::All;
    f32   maxDistance = 20.0f;   // NearCamera のとき（m）

    // 種類ごとの表示。★Static を切れるのが効く（壁と床が線の大半を占めるため）。
    bool showStatic    = true;
    bool showDynamic   = true;
    bool showKinematic = true;
    bool showCharacter = true;
    bool showTriggers  = true;
    bool showOrphans   = true;   // コライダーはあるが RigidBody が無い＝当たらないもの
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

    // シェーダーホットリロード用。PSO のみ作り直す(ルートシグネチャ/頂点バッファは不変のため触らない)。
    void RecreatePipelines(GraphicsDevice& device);

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

    // ECS からコライダー/トリガーを収集する。
    // selected はスコープが Selected のときの基準（entt::null なら何も出ない）。
    // cameraPos は NearCamera のときの距離判定に使う。
    void CollectFromRegistry(entt::registry& registry,
                             entt::entity selected,
                             const DirectX::XMFLOAT3& cameraPos);

    void Render(ID3D12GraphicsCommandList* cmdList,
                const DirectX::XMFLOAT4X4& viewProj);

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const        { return m_enabled; }

    // 表示のふるい（UI から直接いじる）。
    DebugDrawFilter&       Filter()       { return m_filter; }
    const DebugDrawFilter& Filter() const { return m_filter; }

private:
    static constexpr u32 kMaxVertices = 131072; // 65536 lines * 2 vertices
    // in-flight 多重度（SwapChain::kFrameCount と一致）。動的VBを区画リングで書き分ける
    static constexpr u32 kFrames      = 3;

    std::vector<DebugLineVertex> m_vertices;

    // GPU resources
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pso;

    u32  m_frameIdx    = 0;   // 動的VBの書き込み区画（Render毎に巡回）
    bool m_enabled     = false;
    DebugDrawFilter m_filter;
    bool m_initialized = false;

    // RecreatePipelines 用に保持
    std::wstring m_shaderDir;
    DXGI_FORMAT  m_rtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT  m_dsvFormat = DXGI_FORMAT_UNKNOWN;
};

} // namespace dx12e
