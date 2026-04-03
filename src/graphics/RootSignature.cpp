#include "graphics/RootSignature.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void RootSignature::Initialize(GraphicsDevice& device)
{
    // Root parameters: 6
    D3D12_ROOT_PARAMETER1 rootParams[6]{};

    // [0] Per-Object: 32bit constants (32 DWORDs = MVP(16) + Model(16))
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.Num32BitValues  = 32;
    rootParams[0].Constants.ShaderRegister  = 0;
    rootParams[0].Constants.RegisterSpace   = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Per-Frame: CBV (b1)
    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [2] PBR Textures: DescriptorTable (t0=albedo, t1=normal, t2=metalRoughness)
    D3D12_DESCRIPTOR_RANGE1 pbrRange{};
    pbrRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pbrRange.NumDescriptors                    = 3;  // 3連続: t0, t1, t2
    pbrRange.BaseShaderRegister                = 0;
    pbrRange.RegisterSpace                     = 0;
    pbrRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    pbrRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges   = &pbrRange;
    rootParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [3] Bones SRV DescriptorTable (t3)
    D3D12_DESCRIPTOR_RANGE1 boneRange{};
    boneRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    boneRange.NumDescriptors                    = 1;
    boneRange.BaseShaderRegister                = 3;  // t3 (was t1)
    boneRange.RegisterSpace                     = 0;
    boneRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    boneRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges   = &boneRange;
    rootParams[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_VERTEX;

    // [4] Shadow Map SRV DescriptorTable (t4)
    D3D12_DESCRIPTOR_RANGE1 shadowRange{};
    shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors                    = 1;
    shadowRange.BaseShaderRegister                = 4;  // t4 (was t2)
    shadowRange.RegisterSpace                     = 0;
    shadowRange.Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges   = &shadowRange;
    rootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // [5] PBR Material: 4 constants (metallic, roughness, flags, pad)
    rootParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[5].Constants.Num32BitValues  = 4;
    rootParams[5].Constants.ShaderRegister  = 2;  // b2
    rootParams[5].Constants.RegisterSpace   = 0;
    rootParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static Samplers (s0 + s1)
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2]{};

    // s0 - Linear Wrap (albedo, normal, metalRoughness)
    staticSamplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    staticSamplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister   = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1 - Comparison sampler for shadow PCF
    staticSamplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister   = 1;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root Signature (Version 1.1)
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

    Logger::Info("RootSignature created (PBR: 6 slots)");
}

} // namespace dx12e
