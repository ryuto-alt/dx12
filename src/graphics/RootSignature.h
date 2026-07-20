#pragma once

#include <directx/d3d12.h>
#include <wrl/client.h>

#include "core/Types.h"

namespace dx12e
{

class GraphicsDevice;

class RootSignature
{
public:
    void Initialize(GraphicsDevice& device);

    ID3D12RootSignature* Get() const { return m_rootSignature.Get(); }

    static constexpr u32 kSlotPerObject    = 0;  // RootConstants b0 (40 DWORD = MVP+Model+CustomEffect+pad3+CustomParams)
    static constexpr u32 kSlotPerFrame     = 1;  // CBV b1 (PerFrame + cameraPos)
    static constexpr u32 kSlotSRVTable     = 2;  // DescriptorTable t0,t1,t2 (albedo, normal, metalRoughness)
    static constexpr u32 kSlotBonesSRV     = 3;  // DescriptorTable t3 (bones)
    static constexpr u32 kSlotShadowSRV    = 4;  // DescriptorTable t4 (shadow map)
    static constexpr u32 kSlotPBRMaterial  = 5;  // RootConstants b2 (8 DWORD: metallic, roughness, flags, pad, uvScaleOffset.xyzw)
    static constexpr u32 kSlotIBLTable     = 6;  // DescriptorTable t5,t6,t7 (irradiance, prefiltered, brdfLUT)
    static constexpr u32 kSlotAOSRV        = 7;  // DescriptorTable t8 (SSAO ao, screen-space)
    static constexpr u32 kSlotPunctualShadowSRV = 8;  // DescriptorTable t9,t10 (spot shadow array, point shadow cube array)

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
};

} // namespace dx12e
