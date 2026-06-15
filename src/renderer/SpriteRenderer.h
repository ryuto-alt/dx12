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
};

} // namespace dx12e
