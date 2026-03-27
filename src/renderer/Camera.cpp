#include "renderer/Camera.h"

using namespace DirectX;

namespace dx12e
{

void Camera::SetPerspective(f32 fovYRad, f32 aspect, f32 nearZ, f32 farZ)
{
    m_fovY   = fovYRad;
    m_aspect = aspect;
    m_nearZ  = nearZ;
    m_farZ   = farZ;
}

void Camera::LookAt(XMFLOAT3 eye, XMFLOAT3 target, XMFLOAT3 up)
{
    m_position = eye;
    m_target   = target;
    m_up       = up;
}

XMMATRIX Camera::GetViewMatrix() const
{
    XMVECTOR eyeVec    = XMLoadFloat3(&m_position);
    XMVECTOR targetVec = XMLoadFloat3(&m_target);
    XMVECTOR upVec     = XMLoadFloat3(&m_up);
    return XMMatrixLookAtLH(eyeVec, targetVec, upVec);
}

XMMATRIX Camera::GetProjectionMatrix() const
{
    return XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ);
}

XMMATRIX Camera::GetViewProjMatrix() const
{
    return GetViewMatrix() * GetProjectionMatrix();
}

} // namespace dx12e
