#include "gui/ImGuiManager.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"
#include "editor/EditorTheme.h"

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

    // 日本語フォント読み込み（密度は Nebula 寄りにやや小さめ＝プロエディタ感）
    {
        const char* fontPath = "C:\\Windows\\Fonts\\meiryo.ttc";
        if (std::filesystem::exists(fontPath))
        {
            io.Fonts->AddFontFromFileTTF(fontPath, 15.0f, nullptr,
                io.Fonts->GetGlyphRangesJapanese());
            Logger::Info("Japanese font loaded: meiryo.ttc");
        }
        else
        {
            Logger::Warn("Japanese font not found, using default");
        }
    }

    // ===== Nebula Engine Editor ライクなダークテーマ =====
    // 背景は深→浅の階層（AppBg < PanelBg < Chrome < GroupBg）、単一アクセント青。
    // 選択はアクセントの薄膜、タブは選択時に明色＋上線アクセントで「今どこ」を示す。
    using namespace dx12e::theme;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // --- 形状（フローティング窓 / オーバーレイは丸め、ドック窓は自動で矩形）---
    style.WindowRounding          = 7.0f;
    style.ChildRounding           = 6.0f;
    style.FrameRounding           = 5.0f;
    style.PopupRounding           = 7.0f;
    style.GrabRounding            = 5.0f;
    style.TabRounding             = 6.0f;
    style.ScrollbarRounding       = 7.0f;
    // --- 余白（密度を上げて締まった印象に）---
    style.WindowPadding           = ImVec2(9, 8);
    style.FramePadding            = ImVec2(8, 4);
    style.CellPadding             = ImVec2(6, 4);
    style.ItemSpacing             = ImVec2(8, 6);
    style.ItemInnerSpacing        = ImVec2(6, 5);
    style.IndentSpacing           = 16.0f;
    style.ScrollbarSize           = 11.0f;   // Nebula のスリムなスクロールバー
    style.GrabMinSize             = 10.0f;
    style.WindowBorderSize        = 1.0f;
    style.ChildBorderSize         = 0.0f;   // 内側 child は枠なし（パネル境界はドック窓の枠で表現）
    style.FrameBorderSize         = 0.0f;
    style.TabBarBorderSize        = 1.0f;
    style.TabBarOverlineSize      = 2.0f;    // 選択タブ上のアクセント下線
    style.DockingSeparatorSize    = 1.0f;
    style.WindowTitleAlign        = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding    = ImVec2(18, 6);

    ImVec4* c = style.Colors;
    // テキスト
    c[ImGuiCol_Text]                 = TextHi;
    c[ImGuiCol_TextDisabled]         = TextFaint;
    // ベース背景
    c[ImGuiCol_WindowBg]             = PanelBg;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);   // フラット（親の地に乗る＝Nebula風）
    c[ImGuiCol_PopupBg]              = Hex(0x1b1c21, 0.98f);
    c[ImGuiCol_Border]               = Border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    // タイトル / メニューバー（クロム色）
    c[ImGuiCol_TitleBg]              = Chrome;
    c[ImGuiCol_TitleBgActive]        = Chrome;
    c[ImGuiCol_TitleBgCollapsed]     = Hex(0x16171b, 0.75f);
    c[ImGuiCol_MenuBarBg]            = Chrome;
    // フレーム（入力欄・スライダー溝など）
    c[ImGuiCol_FrameBg]              = FrameBg;
    c[ImGuiCol_FrameBgHovered]       = FrameBgHi;
    c[ImGuiCol_FrameBgActive]        = FrameBgActive;
    // ボタン（Nebula のセグメント地に寄せ、ホバーで僅かに持ち上げる）
    c[ImGuiCol_Button]               = GroupBg;
    c[ImGuiCol_ButtonHovered]        = Hex(0x2a2c33);
    c[ImGuiCol_ButtonActive]         = Hex(0x32343c);
    // ヘッダー（選択行＝アクセントの薄膜。CollapsingHeader / Selectable / TreeNode）
    c[ImGuiCol_Header]               = AccentDim;
    c[ImGuiCol_HeaderHovered]        = Hex(0x4c8dff, 0.12f);
    c[ImGuiCol_HeaderActive]         = AccentDim2;
    // タブ（非選択は地に沈め、選択はパネル色へ持ち上げて上線アクセント）
    c[ImGuiCol_Tab]                       = Chrome;
    c[ImGuiCol_TabHovered]                = Hex(0x2a2c33);
    c[ImGuiCol_TabSelected]               = PanelBg;
    c[ImGuiCol_TabSelectedOverline]       = Accent;
    c[ImGuiCol_TabDimmed]                 = Chrome;
    c[ImGuiCol_TabDimmedSelected]         = PanelBg;
    c[ImGuiCol_TabDimmedSelectedOverline] = Hex(0x4c8dff, 0.45f);
    // スクロール（トラック透明、スリムな丸グラブ）
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = Hex(0x33343c);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x44454f);
    c[ImGuiCol_ScrollbarGrabActive]  = Hex(0x55565f);
    // スライダー / チェック
    c[ImGuiCol_SliderGrab]           = Accent;
    c[ImGuiCol_SliderGrabActive]     = AccentLight;
    c[ImGuiCol_CheckMark]            = AccentLight;
    // セパレータ
    c[ImGuiCol_Separator]            = Border;
    c[ImGuiCol_SeparatorHovered]     = AccentDim2;
    c[ImGuiCol_SeparatorActive]      = Accent;
    // リサイズグリップ（通常は不可視、ホバーで現れる＝すっきり）
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = AccentDim2;
    c[ImGuiCol_ResizeGripActive]     = Accent;
    // ドッキング
    c[ImGuiCol_DockingPreview]       = AccentDim2;
    c[ImGuiCol_DockingEmptyBg]       = AppBg;
    // テーブル
    c[ImGuiCol_TableHeaderBg]        = Chrome;
    c[ImGuiCol_TableBorderStrong]    = Border;
    c[ImGuiCol_TableBorderLight]     = Hex(0x202127);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = Hex(0xffffff, 0.02f);
    // 選択・ナビ
    c[ImGuiCol_TextSelectedBg]       = AccentDim2;
    c[ImGuiCol_NavCursor]            = Accent;

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
