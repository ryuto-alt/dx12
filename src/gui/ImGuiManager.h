#pragma once

#include <Windows.h>
#include <directx/d3d12.h>
#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;
class DescriptorHeap;

class ImGuiManager
{
public:
    ImGuiManager() = default;
    ~ImGuiManager() = default;

    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    void Initialize(
        HWND hwnd,
        GraphicsDevice& device,
        ID3D12CommandQueue* commandQueue,
        DescriptorHeap& srvHeap,
        DXGI_FORMAT rtvFormat,
        u32 frameCount);

    void BeginFrame();
    void EndFrame(ID3D12GraphicsCommandList* cmdList);
    void Shutdown();

    static LRESULT WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    u32 m_srvIndex = 0;
};

} // namespace dx12e
