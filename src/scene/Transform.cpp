#include "scene/Transform.h"

using namespace DirectX;

namespace dx12e
{

XMMATRIX Transform::GetWorldMatrix() const
{
    XMMATRIX s = XMMatrixScaling(scale.x, scale.y, scale.z);

    // Euler degrees → radians, YXZ order (Yaw → Pitch → Roll)
    XMMATRIX r = XMMatrixRotationRollPitchYaw(
        XMConvertToRadians(rotation.x),   // pitch
        XMConvertToRadians(rotation.y),    // yaw
        XMConvertToRadians(rotation.z));   // roll

    XMMATRIX t = XMMatrixTranslation(position.x, position.y, position.z);

    return s * r * t;
}

} // namespace dx12e
