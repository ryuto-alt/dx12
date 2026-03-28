#include "renderer/Camera.h"

#include <algorithm>
#include <cmath>

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
    m_up       = up;

    // target方向からyaw/pitchを逆算
    XMVECTOR dir = XMVector3Normalize(
        XMVectorSubtract(XMLoadFloat3(&target), XMLoadFloat3(&eye)));
    XMFLOAT3 d;
    XMStoreFloat3(&d, dir);

    m_yaw   = std::atan2(d.x, d.z);
    m_pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));

    UpdateVectors();
}

void Camera::MoveForward(f32 distance)
{
    // XZ平面上のforward（水平移動のみ、ピッチの影響を受けない）
    XMFLOAT3 flatForward = {std::sin(m_yaw), 0.0f, std::cos(m_yaw)};
    XMVECTOR pos = XMLoadFloat3(&m_position);
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&flatForward));
    XMStoreFloat3(&m_position, XMVectorAdd(pos, XMVectorScale(fwd, distance)));
}

void Camera::MoveRight(f32 distance)
{
    // rightベクトルも水平のみ
    XMFLOAT3 flatForward = {std::sin(m_yaw), 0.0f, std::cos(m_yaw)};
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&flatForward));
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    XMVECTOR rgt = XMVector3Normalize(XMVector3Cross(worldUp, fwd));
    XMVECTOR pos = XMLoadFloat3(&m_position);
    XMStoreFloat3(&m_position, XMVectorAdd(pos, XMVectorScale(rgt, distance)));
}

void Camera::MoveUp(f32 distance)
{
    // ワールドY軸方向に移動（FPSスタイル）
    m_position.y += distance;
}

void Camera::Rotate(f32 yawDelta, f32 pitchDelta)
{
    m_yaw   += yawDelta;
    m_pitch += pitchDelta;

    // pitch制限 ±89度
    constexpr f32 maxPitch = XM_PIDIV2 - 0.01f;
    m_pitch = std::clamp(m_pitch, -maxPitch, maxPitch);

    UpdateVectors();
}

void Camera::UpdateVectors()
{
    // yaw/pitch から forward ベクトル計算 (LH座標系)
    f32 cosP = std::cos(m_pitch);
    m_forward.x = std::sin(m_yaw) * cosP;
    m_forward.y = std::sin(m_pitch);
    m_forward.z = std::cos(m_yaw) * cosP;

    // forward を正規化
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&m_forward));
    XMStoreFloat3(&m_forward, fwd);

    // right = cross(worldUp, forward)
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    XMVECTOR rgt = XMVector3Normalize(XMVector3Cross(worldUp, fwd));
    XMStoreFloat3(&m_right, rgt);

    // up = cross(forward, right)
    XMVECTOR upVec = XMVector3Cross(fwd, rgt);
    XMStoreFloat3(&m_up, upVec);
}

XMMATRIX Camera::GetViewMatrix() const
{
    XMVECTOR eyeVec = XMLoadFloat3(&m_position);
    XMVECTOR fwd    = XMLoadFloat3(&m_forward);
    XMVECTOR upVec  = XMLoadFloat3(&m_up);
    return XMMatrixLookAtLH(eyeVec, XMVectorAdd(eyeVec, fwd), upVec);
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
