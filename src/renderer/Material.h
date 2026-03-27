#pragma once

#include <DirectXMath.h>

namespace dx12e
{

class Material
{
public:
    void SetAlbedo(DirectX::XMFLOAT4 color) { m_albedo = color; }
    DirectX::XMFLOAT4 GetAlbedo() const { return m_albedo; }

private:
    DirectX::XMFLOAT4 m_albedo = {1, 1, 1, 1};
};

} // namespace dx12e
