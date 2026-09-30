#pragma once

#include <Windows.h>
#include <directx/d3d12.h>
#include <functional>
#include <string>
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
    //   ★同時に multi-viewport を恒久的に無効化する（--background は人の画面に窓を出さないための機能。
    //     仮想入力を後から切っても、別 OS 窓を出さない）。
    void SetIniSavingDisabled(bool on);
    // multi-viewportのセカンダリ窓の描画+Present。メインコマンドリストのExecute後・Present前に呼ぶ。
    void RenderPlatformWindows();
    void Shutdown();

    // ===== DPI（表示倍率）=====
    // エディタの UI 倍率（メインビューポート。1.0 = 100%）。OS の表示倍率、--dpi-scale があればそれ。
    // 倍率が変わった（別倍率のモニターへ移動 / OS の設定変更 / オーバーライド）フレームの BeginFrame で
    // ImGuiStyle（基準スタイルから作り直し）とフォント倍率（FontScaleDpi）が切り替わる。
    float GetUiScale() const;
    // 検証用: 実行中に倍率を上書きする（0 以下で OS の倍率へ戻す）。次の BeginFrame で反映。
    void  SetUiScaleOverride(float scale);
    // ゲームモード（配布ランタイム）は倍率を持たない（常に 1.0）。

    // imgui.ini の保存先をユーザーデータ領域の絶対パスにする（既定はカレントディレクトリ依存で、起動の仕方で別のファイルを読んでいた）。
    // 初回 NewFrame より前に呼ぶこと。新しい場所に無く、カレントディレクトリに旧 imgui.ini があれば一度だけ引き継ぐ。
    void SetIniPath(const std::string& utf8Path);

    static LRESULT WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    u32 m_srvIndex = 0;
    HWND m_hwnd = nullptr;
    std::function<void(int, bool)> m_virtualKeySink;
    u32  m_logicalW = 0, m_logicalH = 0;
    std::string m_iniPath;   // imgui.ini の絶対パス（IniFilename が指す。空 = ImGui の既定＝カレントディレクトリの imgui.ini）
    bool m_iniSavingDisabled = false;
    bool m_iniSavingDisabledApplied = false;
};

} // namespace dx12e
