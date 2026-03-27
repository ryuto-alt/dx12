#include "graphics/RootSignature.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void RootSignature::Initialize(GraphicsDevice& device)
{
    // ルートパラメータ 3つ
    D3D12_ROOT_PARAMETER1 rootParams[3]{};

    // [0] Per-Object: 32bit constants (16 DWORDs = 4x4 matrix)
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.Num32BitValues  = 32;  // MVP(16) + Model(16)
    rootParams[0].Constants.ShaderRegister  = 0;
    rootParams[0].Constants.RegisterSpace   = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Per-Frame: CBV (b1)
    rootParams[1].ParameterType                 = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister     = 1;
    rootParams[1].Descriptor.RegisterSpace      = 0;
    rootParams[1].Descriptor.Flags              = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility              = D3D12_SHADER_VISIBILITY_ALL;

    // [2] SRV DescriptorTable (t0)
    D3D12_DESCRIPTOR_RANGE1 srvRange{};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static Sampler (s0) - Linear Wrap
    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias       = 0.0f;
    staticSampler.MaxAnisotropy    = 0;
    staticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    staticSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSampler.MinLOD           = 0.0f;
    staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister   = 0;
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // ルートシグネチャ記述子 (Version 1.1)
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version                     = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1.NumParameters      = _countof(rootParams);
    versionedDesc.Desc_1_1.pParameters        = rootParams;
    versionedDesc.Desc_1_1.NumStaticSamplers  = 1;
    versionedDesc.Desc_1_1.pStaticSamplers    = &staticSampler;
    versionedDesc.Desc_1_1.Flags              = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &serializedBlob, &errorBlob);

    // Version 1.1 が使えない場合は 1.0 にフォールバック
    if (FAILED(hr))
    {
        Logger::Warn("Root Signature 1.1 serialization failed, falling back to 1.0");

        D3D12_ROOT_PARAMETER rootParams10[3]{};

        // [0] Per-Object: 32bit constants
        rootParams10[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams10[0].Constants.Num32BitValues  = 16;
        rootParams10[0].Constants.ShaderRegister  = 0;
        rootParams10[0].Constants.RegisterSpace   = 0;
        rootParams10[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        // [1] Per-Frame: CBV (b1)
        rootParams10[1].ParameterType                 = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams10[1].Descriptor.ShaderRegister     = 1;
        rootParams10[1].Descriptor.RegisterSpace      = 0;
        rootParams10[1].ShaderVisibility              = D3D12_SHADER_VISIBILITY_ALL;

        // [2] SRV DescriptorTable (t0)
        D3D12_DESCRIPTOR_RANGE srvRange10{};
        srvRange10.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange10.NumDescriptors                    = 1;
        srvRange10.BaseShaderRegister                = 0;
        srvRange10.RegisterSpace                     = 0;
        srvRange10.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams10[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams10[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams10[2].DescriptorTable.pDescriptorRanges   = &srvRange10;
        rootParams10[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // Static Sampler (s0) - Linear Wrap (fallback 1.0)
        D3D12_STATIC_SAMPLER_DESC staticSampler10{};
        staticSampler10.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSampler10.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSampler10.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSampler10.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSampler10.MipLODBias       = 0.0f;
        staticSampler10.MaxAnisotropy    = 0;
        staticSampler10.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        staticSampler10.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        staticSampler10.MinLOD           = 0.0f;
        staticSampler10.MaxLOD           = D3D12_FLOAT32_MAX;
        staticSampler10.ShaderRegister   = 0;
        staticSampler10.RegisterSpace    = 0;
        staticSampler10.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc10{};
        desc10.NumParameters      = _countof(rootParams10);
        desc10.pParameters        = rootParams10;
        desc10.NumStaticSamplers  = 1;
        desc10.pStaticSamplers    = &staticSampler10;
        desc10.Flags              = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        serializedBlob.Reset();
        errorBlob.Reset();

        hr = D3D12SerializeRootSignature(&desc10, D3D_ROOT_SIGNATURE_VERSION_1_0, &serializedBlob, &errorBlob);
    }

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            Logger::Error("Root Signature serialization error: {}",
                static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        ThrowIfFailed(hr);
    }

    ThrowIfFailed(device.GetDevice()->CreateRootSignature(
        0,
        serializedBlob->GetBufferPointer(),
        serializedBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)));

    Logger::Info("RootSignature created");
}

} // namespace dx12e
