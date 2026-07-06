#include "input/InputSystem.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "xinput.lib")

namespace dx12e
{

namespace
{
// 左右スティックの円形デッドゾーン処理。デッドゾーン圏内は0、圏外は残りレンジを0..1に再スケールする
// （軸ごとの矩形デッドゾーンだと斜め入力が弱く出るため、XInput公式サンプル同様に円形で処理）。
void ApplyStickDeadzone(SHORT rawX, SHORT rawY, SHORT deadzone, f32& outX, f32& outY)
{
    f32 x = static_cast<f32>(rawX);
    f32 y = static_cast<f32>(rawY);
    f32 mag = std::sqrt(x * x + y * y);
    if (mag < 1e-4f || mag < static_cast<f32>(deadzone))
    {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }
    constexpr f32 kMaxMag = 32767.0f;
    f32 normalizedMag = std::min((mag - static_cast<f32>(deadzone)) / (kMaxMag - static_cast<f32>(deadzone)), 1.0f);
    f32 scale = normalizedMag / mag;
    outX = x * scale;
    outY = y * scale;
}

// トリガーの線形デッドゾーン処理。0..1に正規化。
f32 ApplyTriggerDeadzone(BYTE raw, BYTE threshold)
{
    if (raw < threshold) return 0.0f;
    return static_cast<f32>(raw - threshold) / static_cast<f32>(255 - threshold);
}

bool IsValidPad(int pad) { return pad >= 0 && pad < kMaxGamepads; }
} // namespace

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

void InputSystem::Update(f32 dt)
{
    // 前フレームのキー状態を保存
    std::memcpy(m_prevKeys, m_keys, sizeof(m_keys));

    UpdateGamepads(dt);

    // 合成キー押下(MCP key_press)を prevKeys スナップショット後に適用。
    // 2→今フレームだけ押す(prev=false,cur=true なので IsKeyPressed が立つ)、1→次フレームで離す。
    for (int vk = 0; vk < 256; ++vk)
    {
        if (m_synthPress[vk] == 2)      { m_keys[vk] = true;  m_synthPress[vk] = 1; }
        else if (m_synthPress[vk] == 1) { m_keys[vk] = false; m_synthPress[vk] = 0; }
    }

    // マウス差分リセット
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;

    // キャプチャ中はカーソルをウィンドウ中央に固定 + カーソル非表示を維持
    if (m_mouseCaptured && m_hwnd)
    {
        SetCursor(NULL);  // ImGui がカーソルを復活させるのを毎フレーム防止
        RECT rect;
        GetClientRect(m_hwnd, &rect);
        POINT center = {(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
        ClientToScreen(m_hwnd, &center);
        SetCursorPos(center.x, center.y);
    }
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

void InputSystem::OnFocusLost()
{
    // 押下中キーの WM_KEYUP がフォーカス先に飛んでしまうため、ここで全部離した
    // ことにする。prevKeys も 0 にして再フォーカス後の誤 IsKeyPressed を防ぐ。
    std::memset(m_keys, 0, sizeof(m_keys));
    std::memset(m_prevKeys, 0, sizeof(m_prevKeys));
    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
}

void InputSystem::SetMouseCapture(bool capture)
{
    m_mouseCaptured = capture;

    if (capture)
    {
        // ShowCursor はカウンタベースなので確実に非表示にする
        while (ShowCursor(FALSE) >= 0) {}
        SetCapture(m_hwnd);

        // ウィンドウ内にカーソルを閉じ込める
        RECT rect;
        GetClientRect(m_hwnd, &rect);
        POINT tl = {rect.left, rect.top};
        POINT br = {rect.right, rect.bottom};
        ClientToScreen(m_hwnd, &tl);
        ClientToScreen(m_hwnd, &br);
        RECT clipRect = {tl.x, tl.y, br.x, br.y};
        ClipCursor(&clipRect);
    }
    else
    {
        // カウンタを確実に 0（表示）に戻す
        while (ShowCursor(TRUE) < 0) {}
        ReleaseCapture();
        ClipCursor(nullptr);
    }
}

void InputSystem::ToggleMouseCapture()
{
    SetMouseCapture(!m_mouseCaptured);
}

void InputSystem::UpdateGamepads(f32 dt)
{
    for (int i = 0; i < kMaxGamepads; ++i)
    {
        GamepadState& pad = m_pads[i];
        pad.prevButtons = pad.buttons;

        XINPUT_STATE state = {};
        DWORD result = XInputGetState(static_cast<DWORD>(i), &state);
        if (result == ERROR_SUCCESS)
        {
            pad.connected = true;
            pad.buttons = state.Gamepad.wButtons;
            ApplyStickDeadzone(state.Gamepad.sThumbLX, state.Gamepad.sThumbLY,
                                XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, pad.leftStickX, pad.leftStickY);
            ApplyStickDeadzone(state.Gamepad.sThumbRX, state.Gamepad.sThumbRY,
                                XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, pad.rightStickX, pad.rightStickY);
            pad.leftTrigger  = ApplyTriggerDeadzone(state.Gamepad.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
            pad.rightTrigger = ApplyTriggerDeadzone(state.Gamepad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
        }
        else
        {
            pad.connected = false;
            pad.buttons = 0;
            pad.leftStickX = pad.leftStickY = pad.rightStickX = pad.rightStickY = 0.0f;
            pad.leftTrigger = pad.rightTrigger = 0.0f;
        }

        // タイマー付き振動（SetPadVibrationTimed）の残り時間を消費し、切れたら止める
        if (pad.vibrationTimeLeft > 0.0f)
        {
            pad.vibrationTimeLeft -= dt;
            if (pad.vibrationTimeLeft <= 0.0f)
            {
                pad.vibrationTimeLeft = 0.0f;
                SetPadVibration(i, 0.0f, 0.0f);
            }
        }
    }
}

bool InputSystem::IsPadConnected(int pad) const
{
    return IsValidPad(pad) && m_pads[pad].connected;
}

int InputSystem::GetConnectedPadCount() const
{
    int count = 0;
    for (const auto& pad : m_pads)
        if (pad.connected) ++count;
    return count;
}

bool InputSystem::IsPadButtonDown(int pad, int button) const
{
    if (!IsValidPad(pad)) return false;
    return (m_pads[pad].buttons & static_cast<WORD>(button)) != 0;
}

bool InputSystem::IsPadButtonPressed(int pad, int button) const
{
    if (!IsValidPad(pad)) return false;
    const GamepadState& p = m_pads[pad];
    WORD b = static_cast<WORD>(button);
    return (p.buttons & b) != 0 && (p.prevButtons & b) == 0;
}

bool InputSystem::IsPadButtonReleased(int pad, int button) const
{
    if (!IsValidPad(pad)) return false;
    const GamepadState& p = m_pads[pad];
    WORD b = static_cast<WORD>(button);
    return (p.buttons & b) == 0 && (p.prevButtons & b) != 0;
}

f32 InputSystem::GetPadLeftStickX(int pad) const { return IsValidPad(pad) ? m_pads[pad].leftStickX : 0.0f; }
f32 InputSystem::GetPadLeftStickY(int pad) const { return IsValidPad(pad) ? m_pads[pad].leftStickY : 0.0f; }
f32 InputSystem::GetPadRightStickX(int pad) const { return IsValidPad(pad) ? m_pads[pad].rightStickX : 0.0f; }
f32 InputSystem::GetPadRightStickY(int pad) const { return IsValidPad(pad) ? m_pads[pad].rightStickY : 0.0f; }
f32 InputSystem::GetPadLeftTrigger(int pad) const { return IsValidPad(pad) ? m_pads[pad].leftTrigger : 0.0f; }
f32 InputSystem::GetPadRightTrigger(int pad) const { return IsValidPad(pad) ? m_pads[pad].rightTrigger : 0.0f; }

void InputSystem::SetPadVibration(int pad, f32 leftMotor, f32 rightMotor)
{
    if (!IsValidPad(pad)) return;
    leftMotor = std::clamp(leftMotor, 0.0f, 1.0f);
    rightMotor = std::clamp(rightMotor, 0.0f, 1.0f);

    XINPUT_VIBRATION vibration = {};
    vibration.wLeftMotorSpeed = static_cast<WORD>(leftMotor * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(rightMotor * 65535.0f);
    XInputSetState(static_cast<DWORD>(pad), &vibration);
}

void InputSystem::SetPadVibrationTimed(int pad, f32 leftMotor, f32 rightMotor, f32 seconds)
{
    if (!IsValidPad(pad)) return;
    SetPadVibration(pad, leftMotor, rightMotor);
    m_pads[pad].vibrationTimeLeft = std::max(seconds, 0.0f);
}

} // namespace dx12e
