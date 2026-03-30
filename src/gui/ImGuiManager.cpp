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

    // Unity風ダークテーマ
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 0.0f;
    style.FrameRounding     = 2.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.WindowPadding     = ImVec2(8, 8);
    style.FramePadding      = ImVec2(6, 3);
    style.ItemSpacing       = ImVec2(6, 4);
    style.ItemInnerSpacing  = ImVec2(4, 4);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 12.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;

    ImVec4* c = style.Colors;
    // ベース
    c[ImGuiCol_WindowBg]             = ImVec4(0.180f, 0.180f, 0.180f, 1.0f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.180f, 0.180f, 0.180f, 1.0f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.200f, 0.200f, 0.200f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.120f, 0.120f, 0.120f, 1.0f);
    // タイトルバー
    c[ImGuiCol_TitleBg]              = ImVec4(0.120f, 0.120f, 0.120f, 1.0f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.120f, 0.120f, 0.120f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.120f, 0.120f, 0.120f, 0.75f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.140f, 0.140f, 0.140f, 1.0f);
    // フレーム
    c[ImGuiCol_FrameBg]              = ImVec4(0.220f, 0.220f, 0.220f, 1.0f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.280f, 0.280f, 0.280f, 1.0f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.320f, 0.320f, 0.320f, 1.0f);
    // タブ
    c[ImGuiCol_Tab]                  = ImVec4(0.160f, 0.160f, 0.160f, 1.0f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.280f, 0.280f, 0.280f, 1.0f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.200f, 0.200f, 0.200f, 1.0f);
    // ボタン
    c[ImGuiCol_Button]               = ImVec4(0.260f, 0.260f, 0.260f, 1.0f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.350f, 0.350f, 0.350f, 1.0f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.400f, 0.400f, 0.400f, 1.0f);
    // ヘッダー
    c[ImGuiCol_Header]               = ImVec4(0.250f, 0.250f, 0.250f, 1.0f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.300f, 0.300f, 0.300f, 1.0f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.350f, 0.350f, 0.350f, 1.0f);
    // スクロール/スライダー
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.160f, 0.160f, 0.160f, 1.0f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.300f, 0.300f, 0.300f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.400f, 0.400f, 0.400f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.500f, 0.500f, 0.500f, 1.0f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.390f, 0.580f, 0.926f, 1.0f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.490f, 0.680f, 1.000f, 1.0f);
    // セパレータ
    c[ImGuiCol_Separator]            = ImVec4(0.120f, 0.120f, 0.120f, 1.0f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.300f, 0.300f, 0.300f, 1.0f);
    // テキスト
    c[ImGuiCol_Text]                 = ImVec4(0.860f, 0.860f, 0.860f, 1.0f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.500f, 0.500f, 0.500f, 1.0f);

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
