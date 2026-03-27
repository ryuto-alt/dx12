#include "graphics/RootSignature.h"
#include "graphics/GraphicsDevice.h"
#include "core/Assert.h"
#include "core/Logger.h"

namespace dx12e
{

void RootSignature::Initialize(GraphicsDevice& device)
{
    // ルートパラメータ 2つ
    D3D12_ROOT_PARAMETER1 rootParams[2]{};

    // [0] Per-Object: 32bit constants (16 DWORDs = 4x4 matrix)
    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.Num32BitValues  = 16;
    rootParams[0].Constants.ShaderRegister  = 0;
    rootParams[0].Constants.RegisterSpace   = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // [1] Per-Frame: CBV (b1)
    rootParams[1].ParameterType                 = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister     = 1;
    rootParams[1].Descriptor.RegisterSpace      = 0;
    rootParams[1].Descriptor.Flags              = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    rootParams[1].ShaderVisibility              = D3D12_SHADER_VISIBILITY_ALL;

    // ルートシグネチャ記述子 (Version 1.1)
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version                     = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1.NumParameters      = _countof(rootParams);
    versionedDesc.Desc_1_1.pParameters        = rootParams;
    versionedDesc.Desc_1_1.NumStaticSamplers  = 0;
    versionedDesc.Desc_1_1.pStaticSamplers    = nullptr;
    versionedDesc.Desc_1_1.Flags              = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &serializedBlob, &errorBlob);

    // Version 1.1 が使えない場合は 1.0 にフォールバック
    if (FAILED(hr))
    {
        Logger::Warn("Root Signature 1.1 serialization failed, falling back to 1.0");

        D3D12_ROOT_PARAMETER rootParams10[2]{};

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

        D3D12_ROOT_SIGNATURE_DESC desc10{};
        desc10.NumParameters      = _countof(rootParams10);
        desc10.pParameters        = rootParams10;
        desc10.NumStaticSamplers  = 0;
        desc10.pStaticSamplers    = nullptr;
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
