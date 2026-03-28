#pragma once

#include <DirectXMath.h>
#include "core/Types.h"

namespace dx12e
{

struct Transform
{
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f};  // Euler degrees (Pitch, Yaw, Roll)
    DirectX::XMFLOAT3 scale    = {1.0f, 1.0f, 1.0f};

    // Scale * RotationYXZ * Translation
    DirectX::XMMATRIX GetWorldMatrix() const;
};

} // namespace dx12e
