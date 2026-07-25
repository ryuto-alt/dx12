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
    static constexpr u32 kSlotContactShadowSRV  = 9;  // DescriptorTable t11 (contact shadow, screen-space)
    static constexpr u32 kSlotPrevBonesSRV      = 10; // DescriptorTable t12 (前フレームのボーン行列。速度パス専用)
    // DescriptorTable t13,t14,t15 (クラスタライト / インデックスリスト / クラスタ毎カウント)
    // ＋ t18,t19,t20,t21 (デカール予約。同一テーブルに相乗り＝ヒープ上は 7 本連続)。
    // t16,t17 は SSR/SSGI（計画04）が別スロットで取るのでレンジを 2 本に割ってある。
    static constexpr u32 kSlotClusterSRV        = 11;
    // DescriptorTable t16 (SSR 結果 rgb=反射放射輝度 a=confidence、スクリーン空間・フル解像度)
    static constexpr u32 kSlotSsrSRV            = 12;
    // DescriptorTable t17 (SSGI 結果 rgb=間接放射照度、スクリーン空間・フル解像度)
    static constexpr u32 kSlotSsgiSRV           = 13;

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
};

} // namespace dx12e
