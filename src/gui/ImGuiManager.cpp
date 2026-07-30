#include "gui/ImGuiManager.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "core/Logger.h"
#include <vector>
#include <cstring>
#include "core/vfs/Vfs.h"
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
    // multi-viewport: フローティング窓(マテリアルエディタ等)をメインウィンドウの外へ
    // ドラッグすると独立したOSウィンドウになる(Unreal/Unityと同じ)。ドック中のコアパネルは
    // NoUndocking なので出て行かない。有効時、ImGui座標系は「スクリーン座標」になる点に注意
    // (絶対座標(0,0)前提の窓は GetMainViewport()->Pos 基準に直してある)。
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoTaskBarIcon = true;   // 引き出した窓はタスクバーに出さない(UE/Unityと同じ)
    // ID 衝突警告のビジュアルオーバーレイを抑制（誤検出で popup が塞がれることがある）
    io.ConfigDebugHighlightIdConflicts = false;

    // 日本語フォント読み込み。Yu Gothic Medium（Win標準・レンダリングがくっきり）優先、
    // 無ければ Meiryo にフォールバック。サイズは 17px（可読性優先＝Unreal 寄りの密度）。
    {
        const char* candidates[] = {
            "C:\\Windows\\Fonts\\YuGothM.ttc",   // Yu Gothic Medium
            "C:\\Windows\\Fonts\\meiryo.ttc",
        };
        bool loaded = false;

        // ★配布ゲームでは pak 内のフォントを最優先にする。
        //   以前は OS の Yu Gothic / Meiryo を直読みするだけで、その 2 つが入っていない環境
        //   （日本語 SKU 以外の素の Windows。両方とも同じオプション機能に入っている）では
        //   ImGui が ProggyClean（ASCII のみ）へフォールバックし、**日本語 UI が全部消える**。
        //   開発機では絶対に再現しないので気づけない類の壊れ方だった。
        //   BuildGame が assets/fonts/ から 1 本選んで manifest の uiFont に書いている。
        //   フォントのバイト列は ImGui が所有する（FontDataOwnedByAtlas 既定 true）ので
        //   ここで確保したメモリを渡し切りにしてよい。
        if (vfs::InGameMode())
        {
            vfs::BootConfig boot;
            if (vfs::ReadBootConfig(boot) && !boot.uiFont.empty())
            {
                std::vector<uint8_t> bytes = vfs::ReadAsset(boot.uiFont);
                if (!bytes.empty())
                {
                    void* owned = IM_ALLOC(bytes.size());
                    std::memcpy(owned, bytes.data(), bytes.size());
                    io.Fonts->AddFontFromMemoryTTF(owned, static_cast<int>(bytes.size()), 17.0f,
                        nullptr, io.Fonts->GetGlyphRangesJapanese());
                    Logger::Info("UI font loaded from pak: {}", boot.uiFont);
                    loaded = true;
                }
                else
                {
                    Logger::Error("manifest の uiFont を pak から読めません: {}", boot.uiFont);
                }
            }
        }

        for (const char* fontPath : candidates)
        {
            if (loaded) break;
            if (!std::filesystem::exists(fontPath)) continue;
            io.Fonts->AddFontFromFileTTF(fontPath, 17.0f, nullptr,
                io.Fonts->GetGlyphRangesJapanese());
            Logger::Info("Japanese font loaded: {}", fontPath);
            loaded = true;
            break;
        }
        if (!loaded)
        {
            // ★ここは警告ではなく**エラー**。この状態のまま出荷すると日本語が 1 文字も出ない。
            Logger::Error("日本語フォントが見つかりません。ASCII のみの内蔵フォントで続行するため、"
                          "日本語の UI テキストは表示されません "
                          "(dx12_install_font で assets/fonts/ に日本語対応フォントを入れ、"
                          "ゲームを再ビルドしてください)");
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
    style.FramePadding            = ImVec2(8, 5);
    style.CellPadding             = ImVec2(6, 4);
    style.ItemSpacing             = ImVec2(8, 7);
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

void ImGuiManager::RenderPlatformWindows()
{
    // multi-viewport のセカンダリウィンドウ(引き出したフローティング窓)を描画・Present する。
    // DX12バックエンドが専用のコマンドリスト/スワップチェインを内部管理してキューへ直接投げるため、
    // メインのコマンドリストを ExecuteCommandList した後・Present の前に呼ぶこと
    // (メインリスト内で遷移させたテクスチャ(サムネイル等)をセカンダリ側が参照しても順序が正しくなる)。
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault(nullptr, nullptr);
    }
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
