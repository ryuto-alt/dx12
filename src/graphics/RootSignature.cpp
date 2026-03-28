#include "graphics/RootSignature.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void RootSignature::Initialize(GraphicsDevice& device)
{
    // Root parameters: 5
    D3D12_ROOT_PARAMETER1 rootParams[5]{};

    // [0] Per-Object: 32bit constants (32 DWORDs = MVP(16) + Model(16))
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

    // [2] SRV DescriptorTable (t0) - albedo texture
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

    // [3] Bones SRV DescriptorTable (t1) - bone matrices
    D3D12_DESCRIPTOR_RANGE1 boneRange{};
    boneRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    boneRange.NumDescriptors                    = 1;
    boneRange.BaseShaderRegister                = 1;
    boneRange.RegisterSpace                     = 0;
    boneRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    boneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges   = &boneRange;
    rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

    // [4] Shadow Map SRV DescriptorTable (t2)
    D3D12_DESCRIPTOR_RANGE1 shadowRange{};
    shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors                    = 1;
    shadowRange.BaseShaderRegister                = 2;
    shadowRange.RegisterSpace                     = 0;
    shadowRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges   = &shadowRange;
    rootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static Samplers (s0 + s1)
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2]{};

    // s0 - Linear Wrap (albedo)
    D3D12_STATIC_SAMPLER_DESC& staticSampler = staticSamplers[0];
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

    // s1 - Comparison sampler for shadow PCF
    D3D12_STATIC_SAMPLER_DESC& shadowSampler = staticSamplers[1];
    shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.MipLODBias       = 0.0f;
    shadowSampler.MaxAnisotropy    = 0;
    shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD           = 0.0f;
    shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister   = 1;
    shadowSampler.RegisterSpace    = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root Signature descriptor (Version 1.1)
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version                     = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1.NumParameters      = _countof(rootParams);
    versionedDesc.Desc_1_1.pParameters        = rootParams;
    versionedDesc.Desc_1_1.NumStaticSamplers  = _countof(staticSamplers);
    versionedDesc.Desc_1_1.pStaticSamplers    = staticSamplers;
    versionedDesc.Desc_1_1.Flags              = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &serializedBlob, &errorBlob);

    // Fallback to Version 1.0 if 1.1 is not available
    if (FAILED(hr))
    {
        Logger::Warn("Root Signature 1.1 serialization failed, falling back to 1.0");

        D3D12_ROOT_PARAMETER rootParams10[5]{};

        // [0] Per-Object: 32bit constants
        rootParams10[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams10[0].Constants.Num32BitValues  = 32;
        rootParams10[0].Constants.ShaderRegister  = 0;
        rootParams10[0].Constants.RegisterSpace   = 0;
        rootParams10[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

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

        // [3] Bones SRV DescriptorTable (t1)
        D3D12_DESCRIPTOR_RANGE boneRange10{};
        boneRange10.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        boneRange10.NumDescriptors                    = 1;
        boneRange10.BaseShaderRegister                = 1;
        boneRange10.RegisterSpace                     = 0;
        boneRange10.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams10[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams10[3].DescriptorTable.NumDescriptorRanges = 1;
        rootParams10[3].DescriptorTable.pDescriptorRanges   = &boneRange10;
        rootParams10[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

        // [4] Shadow Map SRV DescriptorTable (t2) - fallback 1.0
        D3D12_DESCRIPTOR_RANGE shadowRange10{};
        shadowRange10.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRange10.NumDescriptors                    = 1;
        shadowRange10.BaseShaderRegister                = 2;
        shadowRange10.RegisterSpace                     = 0;
        shadowRange10.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        rootParams10[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams10[4].DescriptorTable.NumDescriptorRanges = 1;
        rootParams10[4].DescriptorTable.pDescriptorRanges   = &shadowRange10;
        rootParams10[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // Static Samplers (s0 + s1) - fallback 1.0
        D3D12_STATIC_SAMPLER_DESC staticSamplers10[2]{};

        staticSamplers10[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers10[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers10[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers10[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers10[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        staticSamplers10[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        staticSamplers10[0].MaxLOD           = D3D12_FLOAT32_MAX;
        staticSamplers10[0].ShaderRegister   = 0;
        staticSamplers10[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        staticSamplers10[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        staticSamplers10[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers10[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers10[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers10[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers10[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers10[1].MaxLOD           = D3D12_FLOAT32_MAX;
        staticSamplers10[1].ShaderRegister   = 1;
        staticSamplers10[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc10{};
        desc10.NumParameters      = _countof(rootParams10);
        desc10.pParameters        = rootParams10;
        desc10.NumStaticSamplers  = _countof(staticSamplers10);
        desc10.pStaticSamplers    = staticSamplers10;
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
