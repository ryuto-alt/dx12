#include "input/InputSystem.h"
#include "core/Logger.h"

#include <vector>

namespace dx12e
{

void InputSystem::Initialize(HWND hwnd)
{
    m_hwnd = hwnd;
    std::memset(m_keys, 0, sizeof(m_keys));
    std::memset(m_prevKeys, 0, sizeof(m_prevKeys));

    // Raw Input デバイス登録（マウス）
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;      // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void InputSystem::Update()
{
    // 前フレームのキー状態を保存
    std::memcpy(m_prevKeys, m_keys, sizeof(m_keys));

    // マウス差分リセット
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
}

void InputSystem::OnKeyDown(int vkCode)
{
    if (vkCode >= 0 && vkCode < 256)
    {
        m_keys[vkCode] = true;
    }
}

void InputSystem::OnKeyUp(int vkCode)
{
    if (vkCode >= 0 && vkCode < 256)
    {
        m_keys[vkCode] = false;
    }
}

void InputSystem::OnRawInput(LPARAM lParam)
{
    if (!m_mouseCaptured) return;

    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

    std::vector<BYTE> buffer(size);
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER));

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
    if (raw->header.dwType == RIM_TYPEMOUSE)
    {
        m_mouseDeltaX += static_cast<f32>(raw->data.mouse.lLastX);
        m_mouseDeltaY += static_cast<f32>(raw->data.mouse.lLastY);
    }
}

void InputSystem::OnMouseButton(bool /*rightDown*/)
{
    // 将来用
}

void InputSystem::SetMouseCapture(bool capture)
{
    m_mouseCaptured = capture;

    if (capture)
    {
        ShowCursor(FALSE);
        SetCapture(m_hwnd);

        // マウスをウィンドウ中央に移動
        RECT rect;
        GetClientRect(m_hwnd, &rect);
        POINT center = {(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
        ClientToScreen(m_hwnd, &center);
        SetCursorPos(center.x, center.y);
    }
    else
    {
        ShowCursor(TRUE);
        ReleaseCapture();
    }
}

void InputSystem::ToggleMouseCapture()
{
    SetMouseCapture(!m_mouseCaptured);
}

} // namespace dx12e
