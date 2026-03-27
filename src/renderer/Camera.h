#pragma once

#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

class Camera
{
public:
    void SetPerspective(f32 fovYRad, f32 aspect, f32 nearZ, f32 farZ);
    void LookAt(DirectX::XMFLOAT3 eye, DirectX::XMFLOAT3 target, DirectX::XMFLOAT3 up = {0, 1, 0});

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix() const;
    DirectX::XMMATRIX GetViewProjMatrix() const;
    DirectX::XMFLOAT3 GetPosition() const { return m_position; }

private:
    DirectX::XMFLOAT3 m_position = {0, 2, -5};
    DirectX::XMFLOAT3 m_target   = {0, 0, 0};
    DirectX::XMFLOAT3 m_up       = {0, 1, 0};
    f32 m_fovY   = DirectX::XM_PIDIV4;
    f32 m_aspect = 1280.0f / 720.0f;
    f32 m_nearZ  = 0.1f;
    f32 m_farZ   = 1000.0f;
};

} // namespace dx12e
