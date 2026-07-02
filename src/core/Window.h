#pragma once

#include <Windows.h>

#include "Types.h"
#include <string>

namespace dx12e
{

class InputSystem;

class Window
{
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // deferShow=true でウィンドウを作るだけにして表示しない（初期化完了後に Show() を呼ぶ）。
    // スプラッシュ表示中に白い未応答ウィンドウが出るのを防ぐ。
    void Initialize(HINSTANCE hInstance, int nCmdShow,
                    u32 width = 1280, u32 height = 720,
                    const wchar_t* title = L"DX12 Engine",
                    bool deferShow = false);

    // deferShow=true で作ったウィンドウを表示する（多重呼び出し安全）
    void Show();

    bool ProcessMessages();
    void ToggleFullscreen();

    HWND         GetHwnd() const { return m_hwnd; }
    u32          GetWidth() const { return m_width; }
    u32          GetHeight() const { return m_height; }
    bool         ShouldClose() const { return m_shouldClose; }
    bool         WasResized() const { return m_resized; }
    void         ResetResizedFlag() { m_resized = false; }
    bool         IsFullscreen() const { return m_fullscreen; }
    void         SetInputSystem(InputSystem* input) { m_inputSystem = input; }
    void         SetTitle(const std::wstring& title);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND         m_hwnd = nullptr;
    u32          m_width = 1280;
    u32          m_height = 720;
    std::wstring m_title = L"DX12 Engine";
    bool         m_shouldClose = false;
    bool         m_resized = false;
    bool         m_fullscreen = false;
    RECT         m_windowedRect = {};
    InputSystem* m_inputSystem = nullptr;
};

} // namespace dx12e
