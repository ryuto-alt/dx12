#include "gui/ImGuiManager.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"

#include <filesystem>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#pragma warning(pop)

namespace dx12e
{

void ImGuiManager::Initialize(
    HWND hwnd,
    GraphicsDevice& device,
    ID3D12CommandQueue* commandQueue,
    DescriptorHeap& srvHeap,
    DXGI_FORMAT rtvFormat,
    u32 frameCount)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 日本語フォント読み込み
    {
        const char* fontPath = "C:\\Windows\\Fonts\\meiryo.ttc";
        if (std::filesystem::exists(fontPath))
        {
            io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, nullptr,
                io.Fonts->GetGlyphRangesJapanese());
            Logger::Info("Japanese font loaded: meiryo.ttc");
        }
        else
        {
            Logger::Warn("Japanese font not found, using default");
        }
    }

    // スタイルカスタマイズ
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding  = 4.0f;
    style.GrabRounding   = 4.0f;
    style.WindowPadding  = ImVec2(12, 12);
    style.FramePadding   = ImVec2(8, 4);
    style.ItemSpacing    = ImVec2(8, 6);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.1f, 0.1f, 0.12f, 0.95f);
    colors[ImGuiCol_TitleBg]        = ImVec4(0.08f, 0.08f, 0.1f, 1.0f);
    colors[ImGuiCol_TitleBgActive]  = ImVec4(0.15f, 0.15f, 0.2f, 1.0f);
    colors[ImGuiCol_Button]         = ImVec4(0.2f, 0.4f, 0.8f, 0.7f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.3f, 0.5f, 0.9f, 0.8f);
    colors[ImGuiCol_ButtonActive]   = ImVec4(0.15f, 0.3f, 0.7f, 1.0f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
    colors[ImGuiCol_SliderGrab]     = ImVec4(0.3f, 0.5f, 0.9f, 1.0f);

    // Win32 backend
    ImGui_ImplWin32_Init(hwnd);

    // DX12 backend
    m_srvIndex = srvHeap.AllocateIndex();

    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device            = device.GetDevice();
    initInfo.CommandQueue      = commandQueue;
    initInfo.NumFramesInFlight = frameCount;
    initInfo.RTVFormat         = rtvFormat;
    initInfo.DSVFormat         = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = srvHeap.GetHeap();
    initInfo.SrvDescriptorAllocFn  = nullptr;
    initInfo.SrvDescriptorFreeFn   = nullptr;
    initInfo.LegacySingleSrvCpuDescriptor = srvHeap.GetCpuHandle(m_srvIndex);
    initInfo.LegacySingleSrvGpuDescriptor = srvHeap.GetGpuHandle(m_srvIndex);

    ImGui_ImplDX12_Init(&initInfo);

    Logger::Info("ImGui initialized (SRV index={})", m_srvIndex);
}

void ImGuiManager::BeginFrame()
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::EndFrame(ID3D12GraphicsCommandList* cmdList)
{
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

void ImGuiManager::Shutdown()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    Logger::Info("ImGui shut down");
}

LRESULT ImGuiManager::WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
}

} // namespace dx12e
