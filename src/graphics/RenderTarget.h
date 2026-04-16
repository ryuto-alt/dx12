#pragma once

#include <Windows.h>
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <memory>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

// オフスクリーンレンダーターゲット。color (RGBA8) + depth (D32_FLOAT) を一体管理する。
// SRV は外部の shader-visible heap に登録し、RTV/DSV は専用ヒープを内部に持つ。
class RenderTarget
{
public:
    RenderTarget() = default;
    ~RenderTarget();

    RenderTarget(const RenderTarget&)            = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // 初回作成。srvHeap には ImGui 用に SRV スロットを 1 つ確保する。
    void Initialize(GraphicsDevice& device,
                    DescriptorHeap& srvHeap,
                    u32 width, u32 height);

    // サイズ変更。SRV slot は同インデックスに上書き再作成（解放しない）。
    void Resize(GraphicsDevice& device, u32 width, u32 height);

    void Release();

    bool IsValid()    const { return m_color && m_width > 0 && m_height > 0; }
    u32  GetWidth()   const { return m_width; }
    u32  GetHeight()  const { return m_height; }

    ID3D12Resource*             GetColorResource() const { return m_color.Get(); }
    ID3D12Resource*             GetDepthResource() const { return m_depth.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV() const { return m_rtvHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const { return m_dsvHandle; }

    // ImGui::Image() に渡す GPU ハンドル（D3D12_GPU_DESCRIPTOR_HANDLE.ptr 値）
    u64 GetImGuiTextureId() const { return m_imguiTextureId; }

    // ClearRenderTarget で使うべきデフォルトカラー (内部の OptimizedClearValue と一致)
    static const float* GetClearColor();

private:
    void CreateResources(GraphicsDevice& device);
    void CreateViews(GraphicsDevice& device);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_color;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;

    std::unique_ptr<DescriptorHeap> m_rtvHeap;  // 1 RTV スロット
    std::unique_ptr<DescriptorHeap> m_dsvHeap;  // 1 DSV スロット
    D3D12_CPU_DESCRIPTOR_HANDLE     m_rtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE     m_dsvHandle{};

    // 共有 SRV ヒープ (non-owning) と確保済みインデックス
    DescriptorHeap* m_srvHeap     = nullptr;
    u32             m_srvIndex    = 0xFFFFFFFFu;
    u64             m_imguiTextureId = 0;

    u32 m_width  = 0;
    u32 m_height = 0;
};

} // namespace dx12e
