#pragma once

#include <Windows.h>
#include <Xinput.h>
#include "core/Types.h"

namespace dx12e
{

// XInput の接続ポーリング対象数（Xbox コントローラーは最大4台まで同時接続可能）
constexpr int kMaxGamepads = 4;

// パッド1台分の状態。Update() 内で毎フレーム XInputGetState() から更新される。
struct GamepadState
{
    bool connected = false;
    WORD buttons = 0;
    WORD prevButtons = 0;
    f32 leftStickX = 0.0f, leftStickY = 0.0f;   // デッドゾーン適用済み、-1..1
    f32 rightStickX = 0.0f, rightStickY = 0.0f; // デッドゾーン適用済み、-1..1
    f32 leftTrigger = 0.0f, rightTrigger = 0.0f; // デッドゾーン適用済み、0..1
    // SetPadVibrationTimed() で設定した残り秒数。0 なら手動停止まで鳴り続ける（SetPadVibration 直接呼び出し互換）。
    f32 vibrationTimeLeft = 0.0f;
};

class InputSystem
{
public:
    void Initialize(HWND hwnd);
    void Update(f32 dt = 0.0f);  // フレーム開始時に呼ぶ（前フレームの状態をリセット + XInputポーリング）

    // キーボード
    bool IsKeyDown(int vkCode) const { return m_keys[vkCode]; }
    bool IsKeyPressed(int vkCode) const { return m_keys[vkCode] && !m_prevKeys[vkCode]; }
    bool IsAsyncKeyDown(int vkCode) const { return (GetAsyncKeyState(vkCode) & 0x8000) != 0; }

    // ゲームパッド（XInput / Xbox コントローラー。pad = 0..3）
    // ボタン判定には PAD_* 定数（ScriptEngine 側で公開、値は XINPUT_GAMEPAD_* と同一）を渡す。
    bool IsPadConnected(int pad) const;
    int  GetConnectedPadCount() const;
    bool IsPadButtonDown(int pad, int button) const;
    bool IsPadButtonPressed(int pad, int button) const;
    bool IsPadButtonReleased(int pad, int button) const;
    f32 GetPadLeftStickX(int pad) const;
    f32 GetPadLeftStickY(int pad) const;
    f32 GetPadRightStickX(int pad) const;
    f32 GetPadRightStickY(int pad) const;
    f32 GetPadLeftTrigger(int pad) const;
    f32 GetPadRightTrigger(int pad) const;
    // 振動。motor は 0.0..1.0（left=低周波・強モーター、right=高周波・弱モーター）。
    void SetPadVibration(int pad, f32 leftMotor, f32 rightMotor);
    // seconds 秒後に自動で振動を止める（毎フレーム呼んでも良いワンショット/パルス向け）。
    void SetPadVibrationTimed(int pad, f32 leftMotor, f32 rightMotor, f32 seconds);

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
    // MCP の合成入力用: 次の Update() 直後の1フレームだけ「押下→離す」を発生させる。
    // フレーム先頭(Poll)で呼ばれても IsKeyPressed が立つよう、prevKeys スナップショット後に適用する。
    void InjectKeyPress(int vkCode) { if (vkCode >= 0 && vkCode < 256) m_synthPress[vkCode] = 2; }
    void OnRawInput(LPARAM lParam);
    void OnMouseButton(bool rightDown);
    // フォーカス喪失時（他ウィンドウ/タブをクリック等）に呼ぶ。WM_KEYUP が
    // 届かず押しっぱなし判定で動けなくなるのを防ぐため、全キー状態をクリアする。
    void OnFocusLost();

private:
    void UpdateGamepads(f32 dt);

    HWND m_hwnd = nullptr;
    bool m_keys[256] = {};
    bool m_prevKeys[256] = {};
    // 合成キー押下の状態機械(MCP key_press 用)。2=今フレーム押す, 1=次フレーム離す, 0=非アクティブ。
    u8 m_synthPress[256] = {};

    f32  m_mouseDeltaX = 0.0f;
    f32  m_mouseDeltaY = 0.0f;
    bool m_mouseCaptured = false;

    GamepadState m_pads[kMaxGamepads];
};

} // namespace dx12e
