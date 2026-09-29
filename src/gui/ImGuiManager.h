#pragma once

#include <Windows.h>
#include <directx/d3d12.h>
#include <functional>
#include <utility>
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

    // ===== 仮想入力モード（AI が OS の入力を奪わずに UI を操作する。input/VirtualInput.h）=====
    // ON の間:
    //   ・Win32 バックエンドの NewFrame が積む「実マウス位置 / 修飾キー」は捨てる
    //   ・代わりに vinput::Global() のキューから 1 フレームぶんを NewFrame の直前に流し込む
    //   ・OS のカーソル形状は変えない（ImGuiConfigFlags_NoMouseCursorChange）
    //   ・EndFrame で仮想カーソル（矢印 + 波紋）を ForegroundDrawList に描く
    // 切り替えはメインスレッドから。
    void SetVirtualInput(bool on);
    // VK ベースの入力（InputSystem）へもキー押下を配るフック。仮想キーは ImGui と InputSystem の両方へ届く。
    void SetVirtualKeySink(std::function<void(int vk, bool down)> sink) { m_virtualKeySink = std::move(sink); }
    // クライアント矩形が 0（最小化）のとき ImGui に伝える論理解像度。
    //   最小化のまま動かす --background=minimized で DisplaySize が 0x0 になり、ドッキングが
    //   潰れて layout が壊れる（＝スワップチェインは元のサイズのまま）のを防ぐ。
    void SetLogicalDisplaySize(u32 w, u32 h) { m_logicalW = w; m_logicalH = h; }
    // --background 用: 最初のフレームでレイアウト(imgui.ini)を読んだ後は保存しない。
    //   画面外/最小化で動かした窓の位置が、人が使う普段のレイアウトに焼き付くのを防ぐ。
    void SetIniSavingDisabled(bool on) { m_iniSavingDisabled = on; }
    // multi-viewportのセカンダリ窓の描画+Present。メインコマンドリストのExecute後・Present前に呼ぶ。
    void RenderPlatformWindows();
    void Shutdown();

    static LRESULT WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    u32 m_srvIndex = 0;
    HWND m_hwnd = nullptr;
    std::function<void(int, bool)> m_virtualKeySink;
    u32  m_logicalW = 0, m_logicalH = 0;
    bool m_iniSavingDisabled = false;
    bool m_iniSavingDisabledApplied = false;
};

} // namespace dx12e
