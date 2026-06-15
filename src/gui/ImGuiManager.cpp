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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // ID 衝突警告のビジュアルオーバーレイを抑制（誤検出で popup が塞がれることがある）
    io.ConfigDebugHighlightIdConflicts = false;

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

    // モダンダークテーマ（単一アクセント青で統一、余白を確保して可読性を上げる）
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // --- 形状 / 余白 ---
    style.WindowRounding     = 6.0f;
    style.ChildRounding      = 6.0f;
    style.FrameRounding      = 4.0f;
    style.PopupRounding      = 6.0f;
    style.GrabRounding       = 4.0f;
    style.TabRounding        = 5.0f;
    style.ScrollbarRounding  = 4.0f;
    style.WindowPadding      = ImVec2(10, 10);
    style.FramePadding       = ImVec2(8, 5);     // 操作要素を少し大きく＝押しやすく
    style.CellPadding        = ImVec2(6, 4);
    style.ItemSpacing        = ImVec2(8, 7);     // 行間を確保して詰まり感を解消
    style.ItemInnerSpacing   = ImVec2(6, 5);
    style.IndentSpacing      = 18.0f;
    style.ScrollbarSize      = 13.0f;
    style.GrabMinSize        = 11.0f;
    style.WindowBorderSize   = 1.0f;
    style.FrameBorderSize    = 0.0f;
    style.TabBarBorderSize   = 2.0f;
    style.WindowTitleAlign   = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding    = ImVec2(20, 6);

    // アクセント色（青）。ホバー/アクティブは段階的に明るく。
    const ImVec4 accent     = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    const ImVec4 accentHov  = ImVec4(0.34f, 0.66f, 1.00f, 1.00f);
    const ImVec4 accentDim  = ImVec4(0.26f, 0.59f, 0.98f, 0.38f);

    ImVec4* c = style.Colors;
    // ベース（背景に階層差をつけて窓の境界を分かりやすく）
    c[ImGuiCol_WindowBg]             = ImVec4(0.130f, 0.135f, 0.150f, 1.0f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.155f, 0.160f, 0.175f, 1.0f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.115f, 0.120f, 0.135f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.000f, 0.000f, 0.000f, 0.45f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.0f);
    // タイトルバー
    c[ImGuiCol_TitleBg]              = ImVec4(0.100f, 0.105f, 0.120f, 1.0f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.140f, 0.150f, 0.175f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.100f, 0.105f, 0.120f, 0.75f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.120f, 0.125f, 0.140f, 1.0f);
    // フレーム（入力欄・スライダー溝など）
    c[ImGuiCol_FrameBg]              = ImVec4(0.200f, 0.210f, 0.235f, 1.0f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.255f, 0.270f, 0.300f, 1.0f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.300f, 0.315f, 0.350f, 1.0f);
    // タブ（選択中はアクセントで強調＝今どこを見てるか一目で分かる）
    c[ImGuiCol_Tab]                  = ImVec4(0.140f, 0.148f, 0.165f, 1.0f);
    c[ImGuiCol_TabHovered]           = accentHov;
    c[ImGuiCol_TabSelected]          = accent;
    c[ImGuiCol_TabSelectedOverline]  = accentHov;
    c[ImGuiCol_TabDimmed]            = ImVec4(0.120f, 0.125f, 0.140f, 1.0f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.230f, 0.330f, 0.500f, 1.0f);
    // ボタン
    c[ImGuiCol_Button]               = ImVec4(0.240f, 0.255f, 0.290f, 1.0f);
    c[ImGuiCol_ButtonHovered]        = accentHov;
    c[ImGuiCol_ButtonActive]         = accent;
    // ヘッダー（CollapsingHeader / Selectable / TreeNode）
    c[ImGuiCol_Header]               = ImVec4(0.220f, 0.300f, 0.430f, 1.0f);
    c[ImGuiCol_HeaderHovered]        = accentDim;
    c[ImGuiCol_HeaderActive]         = accent;
    // スクロール
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.110f, 0.115f, 0.130f, 1.0f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.290f, 0.305f, 0.340f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.360f, 0.380f, 0.420f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    // スライダー / チェック
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accentHov;
    c[ImGuiCol_CheckMark]            = accentHov;
    // セパレータ
    c[ImGuiCol_Separator]            = ImVec4(0.000f, 0.000f, 0.000f, 0.40f);
    c[ImGuiCol_SeparatorHovered]     = accentDim;
    c[ImGuiCol_SeparatorActive]      = accent;
    // リサイズグリップ
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.260f, 0.590f, 0.980f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]    = accentDim;
    c[ImGuiCol_ResizeGripActive]     = accent;
    // ドッキングのプレビュー
    c[ImGuiCol_DockingPreview]       = accentDim;
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.090f, 0.095f, 0.105f, 1.0f);
    // 選択・ナビ
    c[ImGuiCol_TextSelectedBg]       = accentDim;
    c[ImGuiCol_NavCursor]            = accent;
    // テキスト
    c[ImGuiCol_Text]                 = ImVec4(0.900f, 0.905f, 0.920f, 1.0f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.520f, 0.530f, 0.560f, 1.0f);

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
