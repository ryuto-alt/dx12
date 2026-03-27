#include "Window.h"
#include "Logger.h"

// ImGui Win32 WndProc handler (forward declaration)
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dx12e
{

Window::~Window()
{
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void Window::Initialize(HINSTANCE hInstance, int nCmdShow,
                         u32 width, u32 height, const wchar_t* title)
{
    m_width = width;
    m_height = height;
    m_title = title;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.cbClsExtra    = 0;
    wc.cbWndExtra    = sizeof(Window*);
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName  = nullptr;
    wc.lpszClassName = L"DX12EngineWindowClass";
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
    {
        Logger::Critical("Failed to register window class");
        throw std::runtime_error("Failed to register window class");
    }

    // クライアント領域が指定サイズになるよう調整
    RECT rect = { 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        L"DX12EngineWindowClass",
        m_title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        this  // WndProc で取り出す用
    );

    if (!m_hwnd)
    {
        Logger::Critical("Failed to create window");
        throw std::runtime_error("Failed to create window");
    }

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    Logger::Info("Window created: {}x{}", m_width, m_height);
}

bool Window::ProcessMessages()
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            m_shouldClose = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !m_shouldClose;
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }

    Window* window = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    else
    {
        window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window)
    {
        switch (msg)
        {
        case WM_SIZE:
        {
            u32 newWidth = LOWORD(lParam);
            u32 newHeight = HIWORD(lParam);
            if (newWidth > 0 && newHeight > 0)
            {
                window->m_width = newWidth;
                window->m_height = newHeight;
                window->m_resized = true;
                Logger::Debug("Window resized: {}x{}", newWidth, newHeight);
            }
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_CLOSE:
            window->m_shouldClose = true;
            DestroyWindow(hwnd);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace dx12e
