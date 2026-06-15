#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{
class GraphicsDevice;
class DescriptorHeap;
class CommandList;

// オフスクリーンのカラーレンダーターゲット。
// 自前の RTV（rtvHeap から確保）と SRV（srvHeap から確保）を持ち、
// シーンを一旦ここへ描いてからポストプロセスでサンプルする用途。
class RenderTarget
{
public:
    void Initialize(GraphicsDevice& device,
                    DescriptorHeap* rtvHeap,
                    DescriptorHeap* srvHeap,
                    u32 width, u32 height,
                    DXGI_FORMAT format,
                    const float clearColor[4]);

    // ウィンドウリサイズ時にリソースを作り直す（RTV ハンドル / SRV インデックスは再利用）。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    // 現在の状態から newState へ遷移（同一状態ならスキップ）。
    void Transition(CommandList& cmd, D3D12_RESOURCE_STATES newState);

    ID3D12Resource*             GetResource() const { return m_resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv()      const { return m_rtv; }
    u32                         GetSrvIndex() const { return m_srvIndex; }
    DXGI_FORMAT                 GetFormat()   const { return m_format; }
    u32                         GetWidth()    const { return m_width; }
    u32                         GetHeight()   const { return m_height; }

private:
    void CreateResourceAndViews(GraphicsDevice& device, bool allocateViews);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    DescriptorHeap*             m_rtvHeap  = nullptr;
    DescriptorHeap*             m_srvHeap  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtv{};
    u32                         m_srvIndex = 0xFFFFFFFFu;
    u32                         m_width    = 0;
    u32                         m_height   = 0;
    DXGI_FORMAT                 m_format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    float                       m_clear[4] = {0, 0, 0, 1};
    D3D12_RESOURCE_STATES       m_state    = D3D12_RESOURCE_STATE_RENDER_TARGET;
};

} // namespace dx12e
