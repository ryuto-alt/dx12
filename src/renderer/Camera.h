#pragma once

#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class Camera
{
public:
    void SetPerspective(f32 fovYRad, f32 aspect, f32 nearZ, f32 farZ);
    // 正射投影（2D/見下ろし/ボード/RTS俯瞰）。viewHeight=ビュー縦の世界単位（全高）。
    void SetOrthographic(f32 viewHeight, f32 aspect, f32 nearZ, f32 farZ);
    void SetAspect(f32 aspect) { m_aspect = aspect; }  // fov を変えずアスペクト比だけ更新
    bool IsOrthographic() const { return m_orthographic; }

    // LookAt (legacy - 固定カメラ用)
    void LookAt(DirectX::XMFLOAT3 eye, DirectX::XMFLOAT3 target, DirectX::XMFLOAT3 up = {0, 1, 0});

    // FPSカメラ操作
    void SetPosition(DirectX::XMFLOAT3 pos) { m_position = pos; }
    void MoveForward(f32 distance);
    void MoveRight(f32 distance);
    void MoveUp(f32 distance);
    void Rotate(f32 yawDelta, f32 pitchDelta);  // マウス差分から回転

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMMATRIX GetViewProjMatrix() const;
    DirectX::XMFLOAT3 GetPosition() const { return m_position; }
    DirectX::XMFLOAT3 GetForward()  const { return m_forward; }
    f32 GetYaw() const { return m_yaw; }
    f32 GetPitch() const { return m_pitch; }

    // CSM のカスケード分割で視錐台を再構築するためのゲッター
    f32 GetFovY()   const { return m_fovY; }
    f32 GetAspect() const { return m_aspect; }
    f32 GetNearZ()  const { return m_nearZ; }
    f32 GetFarZ()   const { return m_farZ; }
    f32 GetOrthoHeight() const { return m_orthoHeight; }  // 正射ビュー縦(全高)。保存/復元用。

    void SetYaw(f32 yaw) { m_yaw = yaw; UpdateVectors(); }
    void SetPitch(f32 pitch) { m_pitch = pitch; UpdateVectors(); }
    void SetMoveSpeed(f32 speed) { m_moveSpeed = speed; }
    void SetMouseSensitivity(f32 sens) { m_mouseSensitivity = sens; }
    f32 GetMoveSpeed() const { return m_moveSpeed; }
    f32 GetMouseSensitivity() const { return m_mouseSensitivity; }

private:
    void UpdateVectors();  // yaw/pitch から forward/right/up を再計算

    DirectX::XMFLOAT3 m_position = {-14.7f, 9.6f, -9.0f};
    DirectX::XMFLOAT3 m_forward  = {0, 0, 1};
    DirectX::XMFLOAT3 m_right    = {1, 0, 0};
    DirectX::XMFLOAT3 m_up       = {0, 1, 0};

    f32 m_yaw   = 0.0f;     // Y軸回転 (rad)
    f32 m_pitch = 0.0f;     // X軸回転 (rad), ±89度制限

    f32 m_fovY   = DirectX::XM_PIDIV4;
    f32 m_aspect = 1280.0f / 720.0f;
    f32 m_nearZ  = 0.1f;
    f32 m_farZ   = 1000.0f;

    bool m_orthographic = false;   // true なら GetProjectionMatrix が正射を返す
    f32  m_orthoHeight  = 20.0f;   // 正射のビュー縦（全高）の世界単位

    f32 m_moveSpeed        = 5.0f;
    f32 m_mouseSensitivity = 0.003f;
};

} // namespace dx12e
