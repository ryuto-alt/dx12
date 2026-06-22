#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;

// スクリーン空間の 2D スプライト描画（テクスチャ付きクアッド）。
// Submit でリクエストを溜め、Render で 1 パスにまとめて描く（テクスチャ単位でバッチ）。
// HUD / タイトル / ゲーム内 2D の土台。
struct SpriteDesc
{
    DirectX::XMFLOAT2 pos{0, 0};          // 左上ピクセル座標
    DirectX::XMFLOAT2 size{100, 100};     // ピクセルサイズ
    DirectX::XMFLOAT2 uvMin{0, 0};
    DirectX::XMFLOAT2 uvMax{1, 1};
    DirectX::XMFLOAT4 color{1, 1, 1, 1};  // 乗算カラー
    u32   srvIndex = 0;                    // m_srvHeap 上のテクスチャ SRV インデックス
    float layer    = 0.0f;                 // 小さいほど奥（描画順）
};

// ワールド空間 2D スプライト（カメラ連動）。center/size は世界単位（XY 平面, z=0）。
// 純2D（見下ろし＝カメラ-Z正対 / 横スク）向け。前後は layer（小さいほど奥）で CPU ソート。
struct WorldSpriteDesc
{
    DirectX::XMFLOAT2 center{0, 0};       // ワールド中心 (X, Y)
    DirectX::XMFLOAT2 size{1, 1};         // ワールドサイズ
    DirectX::XMFLOAT2 uvMin{0, 0};
    DirectX::XMFLOAT2 uvMax{1, 1};
    DirectX::XMFLOAT4 color{1, 1, 1, 1};
    u32   srvIndex = 0;
    float layer    = 0.0f;
};

class SpriteRenderer
{
public:
    void Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                    DXGI_FORMAT rtvFormat, const std::wstring& shaderDir);

    void BeginFrame();
    void Submit(const SpriteDesc& s);
    bool HasAny() const { return !m_sprites.empty(); }

    // 呼び出し側で対象 RTV を先にバインドしておくこと。
    void Render(ID3D12GraphicsCommandList* cmd, u32 screenW, u32 screenH);

    // --- ワールド空間 2D（カメラ連動・layer ソート・アルファブレンド）---
    // HUD 経路（上）とは別の PSO/頂点バッファ/リストで隔離。Initialize の後に呼ぶ。
    // sceneRtvFormat はシーン RT のフォーマット（HDR の kSceneColorFormat）。
    void InitializeWorld(GraphicsDevice& device, DXGI_FORMAT sceneRtvFormat,
                         const std::wstring& shaderDir);
    void BeginWorldFrame();
    void SubmitWorld(const WorldSpriteDesc& s);
    bool HasAnyWorld() const { return !m_worldSprites.empty(); }
    // viewProj = アクティブカメラの ViewProj。呼び出し側で対象 RTV(sceneRT) をバインド済みのこと。
    void RenderWorld(ID3D12GraphicsCommandList* cmd, DirectX::XMMATRIX viewProj);

private:
    struct Vertex { DirectX::XMFLOAT2 pos; DirectX::XMFLOAT2 uv; DirectX::XMFLOAT4 col; };

    static constexpr u32 kMaxSprites  = 4096;
    static constexpr u32 kMaxVertices = kMaxSprites * 6;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_vbView{};

    DescriptorHeap* m_srvHeap = nullptr;
    std::vector<SpriteDesc> m_sprites;
    bool m_initialized = false;

    // ワールド空間経路（HUD と隔離）。RootSig は m_rootSig を共有。
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_worldPso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      m_worldVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW                     m_worldVbView{};
    std::vector<WorldSpriteDesc>                m_worldSprites;
    bool m_worldInitialized = false;
};

} // namespace dx12e
