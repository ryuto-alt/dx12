#pragma once

#include <Windows.h>
#include "core/Types.h"

namespace dx12e
{

class InputSystem
{
public:
    void Initialize(HWND hwnd);
    void Update();  // フレーム開始時に呼ぶ（前フレームの状態をリセット）

    // キーボード
    bool IsKeyDown(int vkCode) const { return m_keys[vkCode]; }
    bool IsKeyPressed(int vkCode) const { return m_keys[vkCode] && !m_prevKeys[vkCode]; }
    bool IsAsyncKeyDown(int vkCode) const { return (GetAsyncKeyState(vkCode) & 0x8000) != 0; }

    // マウス
    f32 GetMouseDeltaX() const { return m_mouseDeltaX; }
    f32 GetMouseDeltaY() const { return m_mouseDeltaY; }
    bool IsMouseCaptured() const { return m_mouseCaptured; }
    bool IsRightMouseDown() const { return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0; }

    void SetMouseCapture(bool capture);
    void ToggleMouseCapture();

    // WndProc から呼ぶ
    void OnKeyDown(int vkCode);
    void OnKeyUp(int vkCode);
    void OnRawInput(LPARAM lParam);
    void OnMouseButton(bool rightDown);

private:
    HWND m_hwnd = nullptr;
    bool m_keys[256] = {};
    bool m_prevKeys[256] = {};

    f32  m_mouseDeltaX = 0.0f;
    f32  m_mouseDeltaY = 0.0f;
    bool m_mouseCaptured = false;
};

} // namespace dx12e
