#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <D3D12MemAlloc.h>

#include "core/Types.h"

namespace dx12e
{

class Window;

class GraphicsDevice
{
public:
    GraphicsDevice() = default;
    ~GraphicsDevice();

    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    void Initialize(Window& window);

    ID3D12Device5*      GetDevice()      const { return m_device.Get(); }
    IDXGIFactory6*      GetFactory()     const { return m_factory.Get(); }
    D3D12MA::Allocator* GetAllocator()   const { return m_allocator; }
    // 「DXR が何かしら使える」だけの弱い判定（Tier 1.0 でも true）。
    // ★レイトレのパスを分岐させるなら必ず SupportsInlineRaytracing() を使うこと。
    bool                IsDxrSupported() const { return m_dxrSupported; }

    // --- DXR ケーパビリティ（計画09 Step 0）------------------------------------
    // Tier の実値。TIER_1_0 では RayQuery（inline raytracing）が使えないため、
    // ブール値ひとつでは分岐できない（GTX 10 系のドライバフォールバックが TIER_1_0）。
    D3D12_RAYTRACING_TIER GetRaytracingTier()    const { return m_raytracingTier; }
    // ランタイム/ドライバが報告する最高シェーダモデル。RayQuery は SM 6.5 必須。
    D3D_SHADER_MODEL      GetHighestShaderModel() const { return m_highestShaderModel; }

    // ★DXR パスの唯一のゲート。Tier 1.1 以上 かつ SM 6.5 以上。
    //   （RayQuery = DXR 1.1 inline raytracing。dxc 1.8 で cs_6_4/ps_6_4 は
    //     "Opcode AllocateRayQuery not valid" で落ちることを実測済み）
    bool SupportsInlineRaytracing() const
    {
        return m_raytracingTier >= D3D12_RAYTRACING_TIER_1_1
            && m_highestShaderModel >= D3D_SHADER_MODEL_6_5;
    }
    // SM 6.6（ResourceDescriptorHeap[] のバインドレス）。計画09 Step 5 以降で使う。
    bool SupportsBindlessSm66() const { return m_highestShaderModel >= D3D_SHADER_MODEL_6_6; }
    // DXR 1.2（SER / OMM）。v1 のスコープ外。将来の判断材料としてログに出すだけ。
    bool SupportsDxr12()      const { return m_raytracingTier >= D3D12_RAYTRACING_TIER_1_2; }
    bool SupportsReordering() const { return m_serActuallyReorders; }

private:
    void QueryCapabilities();

    Microsoft::WRL::ComPtr<ID3D12Device5>  m_device;
    Microsoft::WRL::ComPtr<IDXGIFactory6>  m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>  m_adapter;
    D3D12MA::Allocator*                    m_allocator = nullptr;
    bool                                   m_dxrSupported = false;
    D3D12_RAYTRACING_TIER                  m_raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D_SHADER_MODEL                       m_highestShaderModel = D3D_SHADER_MODEL_6_0;
    bool                                   m_serActuallyReorders = false;
};

} // namespace dx12e
