#include "renderer/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

void Camera::SetPerspective(f32 fovYRad, f32 aspect, f32 nearZ, f32 farZ)
{
    m_orthographic = false;
    m_fovY   = fovYRad;
    m_aspect = aspect;
    m_nearZ  = nearZ;
    m_farZ   = farZ;
}

void Camera::SetOrthographic(f32 viewHeight, f32 aspect, f32 nearZ, f32 farZ)
{
    m_orthographic = true;
    m_orthoHeight  = viewHeight;
    m_aspect       = aspect;
    m_nearZ        = nearZ;
    m_farZ         = farZ;
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
    // NaN/inf の yaw/pitch はビュー行列を NaN 化し全描画が消える(シーンビューが真っ青)。
    // 侵入経路(スクリプト/物理由来の Transform、外部 Set*)を問わずここで正気値へ戻す。
    if (!std::isfinite(m_yaw))   m_yaw   = 0.0f;
    if (!std::isfinite(m_pitch)) m_pitch = 0.0f;

    // yaw/pitch から forward ベクトル計算 (LH座標系)
    f32 cosP = std::cos(m_pitch);
    m_forward.x = std::sin(m_yaw) * cosP;
    m_forward.y = std::sin(m_pitch);
    m_forward.z = std::cos(m_yaw) * cosP;

    // forward を正規化
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&m_forward));
    XMStoreFloat3(&m_forward, fwd);

    // right = cross(worldUp, forward)。真上/真下(forward∥worldUp)では外積が零ベクトルになり
    // normalize(0)=0 → up も 0 → ビュー行列が退化して全描画が消えるため、yaw から右手を合成。
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    XMVECTOR crossV  = XMVector3Cross(worldUp, fwd);
    XMVECTOR rgt = (XMVectorGetX(XMVector3LengthSq(crossV)) < 1e-12f)
        ? XMVectorSet(std::cos(m_yaw), 0.0f, -std::sin(m_yaw), 0.0f)
        : XMVector3Normalize(crossV);
    XMStoreFloat3(&m_right, rgt);

    // up = cross(forward, right)
    XMVECTOR upVec = XMVector3Cross(fwd, rgt);

    // roll は right/up を forward まわりに回すだけ。forward は動かないので、
    // 照準やレイキャストの向きは傾けても一切変わらない。
    if (m_roll != 0.0f)
    {
        XMMATRIX r = XMMatrixRotationAxis(fwd, m_roll);
        rgt   = XMVector3Normalize(XMVector3TransformNormal(rgt, r));
        upVec = XMVector3Normalize(XMVector3TransformNormal(upVec, r));
        XMStoreFloat3(&m_right, rgt);
    }
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
    // 不正パラメータ(0/負/非有限)は射影行列を NaN/inf 化し全描画が消える(シーンビューが真っ青)。
    // CameraComponent の orthoSize/nearClip/farClip はエディタ/MCP から任意値を入れられるため
    // ここで最終防衛する。
    const f32 aspect = (std::isfinite(m_aspect) && m_aspect > 1e-4f) ? m_aspect : (16.0f / 9.0f);
    if (m_orthographic)
    {
        // 正射は near<=0 も正当。要件は「高さ>0」「far!=near」のみ。
        const f32 h = (std::isfinite(m_orthoHeight) && m_orthoHeight > 1e-4f) ? m_orthoHeight : 20.0f;
        f32 nearZ = std::isfinite(m_nearZ) ? m_nearZ : 0.1f;
        f32 farZ  = std::isfinite(m_farZ)  ? m_farZ  : nearZ + 1000.0f;
        if (farZ - nearZ < 1e-3f) farZ = nearZ + 1000.0f;
        return XMMatrixOrthographicLH(h * aspect, h, nearZ, farZ);
    }
    const f32 fov = (std::isfinite(m_fovY) && m_fovY > 0.001f && m_fovY < XM_PI - 0.001f)
                        ? m_fovY : XM_PIDIV4;
    const f32 nearZ = (std::isfinite(m_nearZ) && m_nearZ > 1e-4f) ? m_nearZ : 0.1f;
    const f32 farZ  = (std::isfinite(m_farZ) && m_farZ > nearZ + 1e-3f) ? m_farZ : nearZ + 1000.0f;
    return XMMatrixPerspectiveFovLH(fov, aspect, nearZ, farZ);
}

XMMATRIX Camera::GetViewProjMatrix() const
{
    return GetViewMatrix() * GetProjectionMatrix();
}

} // namespace dx12e
