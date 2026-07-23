#pragma once

#include <Windows.h>

#include "Types.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dx12e
{

class InputSystem;

// ウィンドウ表示モード（オプション画面の「画面モード」）
// Fullscreen は CDS_FULLSCREEN によるディスプレイモード変更 + ボーダレス。
// ALLOW_TEARING スワップチェインは DXGI 排他フルスクリーンと併用不可のため、この方式を採る。
enum class WindowMode { Windowed, Borderless, Fullscreen };

class Window
{
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // deferShow=true でウィンドウを作るだけにして表示しない（初期化完了後に Show() を呼ぶ）。
    // スプラッシュ表示中に白い未応答ウィンドウが出るのを防ぐ。
    // startMaximized=false（ゲームモード用）は最大化せず指定解像度のまま表示する。
    // 最大化するとクライアント領域がタスクバー分だけ 16:9 より横長になり、UI キャンバスの
    // ScaleToFit がレターボックス（左右の余白）を作ってエディタの見え方と食い違うため。
    void Initialize(HINSTANCE hInstance, int nCmdShow,
                    u32 width = 1280, u32 height = 720,
                    const wchar_t* title = L"DX12 Engine",
                    bool deferShow = false,
                    bool startMaximized = true);

    // deferShow=true で作ったウィンドウを表示する（多重呼び出し安全）
    void Show();

    bool ProcessMessages();
    void ToggleFullscreen();

    // ===== 表示モード / 解像度（オプション画面用）=====
    // width/height: Windowed=クライアントサイズ / Fullscreen=ディスプレイモード。0=現状維持。
    // Borderless では無視される（常にデスクトップ解像度）。
    void SetMode(WindowMode mode, u32 width = 0, u32 height = 0);
    WindowMode GetMode() const { return m_mode; }
    // ウィンドウモード時のクライアント領域サイズ変更（他モードでは何もしない）
    void SetClientSize(u32 width, u32 height);
    // プライマリモニタが対応する解像度一覧（32bpp、重複除去、昇順）
    static std::vector<std::pair<u32, u32>> EnumResolutions();

    // ===== カスタムタイトルバー(エディタ用) =====
    // OS標準のキャプション(白いタイトルバー)を外し、クライアント領域を窓の上端まで広げる。
    // 見た目のタイトルバーはImGui側(ToolbarPanelのメニューバー行)が描き、ドラッグ移動は
    // WM_NCHITTEST が HTCAPTION を返すことで実現する(スナップ/最大化ダブルクリックもOSが処理)。
    // リサイズ境界・最小化アニメーション・タスクバー挙動は WS_OVERLAPPEDWINDOW のまま維持される。
    void EnableCustomTitleBar();
    bool IsCustomTitleBar() const { return m_customTitleBar; }
    // ImGui側が毎フレーム報告する: タイトルバー帯の高さ(クライアント座標px)と、
    // 「今マウスがドラッグ可能な余白上にあるか」(メニュー/ボタン等のアイテム上ではfalse)。
    void SetCaptionInfo(u32 heightPx, bool draggable)
    {
        m_captionHeight = heightPx;
        m_captionDraggable = draggable;
    }
    bool IsMaximized() const { return m_hwnd && ::IsZoomed(m_hwnd); }
    void Minimize()          { if (m_hwnd) ::ShowWindow(m_hwnd, SW_MINIMIZE); }
    void ToggleMaximize()    { if (m_hwnd) ::ShowWindow(m_hwnd, IsMaximized() ? SW_RESTORE : SW_MAXIMIZE); }
    void RequestClose()      { if (m_hwnd) ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0); }  // closeHandler を通す

    HWND         GetHwnd() const { return m_hwnd; }
    u32          GetWidth() const { return m_width; }
    u32          GetHeight() const { return m_height; }
    bool         ShouldClose() const { return m_shouldClose; }
    bool         WasResized() const { return m_resized; }
    void         ResetResizedFlag() { m_resized = false; }
    bool         IsFullscreen() const { return m_fullscreen; }
    void         SetInputSystem(InputSystem* input) { m_inputSystem = input; }
    void         SetTitle(const std::wstring& title);

    // タイトルバーの X（WM_CLOSE）を横取りするハンドラ。true を返すと通常通り閉じる（本当に終了）、
    // false を返すとウィンドウは閉じない（呼び出し側が「ランチャーに戻る」等を自前で処理済みの意味）。
    // 未設定なら従来通り常に閉じる。
    void         SetCloseHandler(std::function<bool()> handler) { m_closeHandler = std::move(handler); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND         m_hwnd = nullptr;
    u32          m_width = 1280;
    u32          m_height = 720;
    std::wstring m_title = L"DX12 Engine";
    bool         m_shouldClose = false;
    bool         m_resized = false;
    bool         m_fullscreen = false;   // m_mode != Windowed と同義（WndProc の既存判定用に維持）
    WindowMode   m_mode = WindowMode::Windowed;
    bool         m_displayModeChanged = false;   // CDS_FULLSCREEN 適用中か
    bool         m_startMaximized = true;
    RECT         m_windowedRect = {};
    InputSystem* m_inputSystem = nullptr;
    std::function<bool()> m_closeHandler;

    // カスタムタイトルバー状態(EnableCustomTitleBar / SetCaptionInfo)
    bool m_customTitleBar   = false;
    u32  m_captionHeight    = 35;     // ImGui側が毎フレーム実測値で上書きする
    bool m_captionDraggable = false;
};

} // namespace dx12e
