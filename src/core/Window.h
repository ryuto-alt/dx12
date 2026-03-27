#pragma once

#include <Windows.h>

#include "Types.h"
#include <string>

namespace dx12e
{

class Window
{
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void Initialize(HINSTANCE hInstance, int nCmdShow,
                    u32 width = 1280, u32 height = 720,
                    const wchar_t* title = L"DX12 Engine");

    bool ProcessMessages();

    HWND         GetHwnd() const { return m_hwnd; }
    u32          GetWidth() const { return m_width; }
    u32          GetHeight() const { return m_height; }
    bool         ShouldClose() const { return m_shouldClose; }
    bool         WasResized() const { return m_resized; }
    void         ResetResizedFlag() { m_resized = false; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND         m_hwnd = nullptr;
    u32          m_width = 1280;
    u32          m_height = 720;
    std::wstring m_title = L"DX12 Engine";
    bool         m_shouldClose = false;
    bool         m_resized = false;
};

} // namespace dx12e
