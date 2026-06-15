#include "renderer/PostProcess.h"
#include "graphics/GraphicsDevice.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <cstdint>

namespace dx12e
{
// エフェクト有効ビット（shaders/post/PostProcess.hlsl の E_* と一致させること）。
enum PostEffectBit : uint32_t
{
    PE_EXPOSURE   = 1u << 0,
    PE_CONTRAST   = 1u << 1,
    PE_BRIGHTNESS = 1u << 2,
    PE_SATURATION = 1u << 3,
    PE_WARMTH     = 1u << 4,
    PE_TINT       = 1u << 5,
    PE_HUE        = 1u << 6,
    PE_BLOOM      = 1u << 7,
    PE_VIGNETTE   = 1u << 8,
    PE_CHROMATIC  = 1u << 9,
    PE_PIXELIZE   = 1u << 10,
    PE_POSTERIZE  = 1u << 11,
    PE_DITHER     = 1u << 12,
    PE_SCANLINE   = 1u << 13,
    PE_SHARPEN    = 1u << 14,
    PE_GRAIN      = 1u << 15,
    PE_INVERT     = 1u << 16,
    PE_SEPIA      = 1u << 17,
    PE_GRAYSCALE  = 1u << 18,
    PE_LENS       = 1u << 19,
    PE_WAVE       = 1u << 20,
    PE_RADIAL     = 1u << 21,
    PE_GLITCH     = 1u << 22,
    PE_OUTLINE    = 1u << 23,
    PE_FXAA       = 1u << 24,
};

// HLSL の cbuffer PostCB と一致させる（11 つの float4 = 44 DWORD）。
struct PostCB
{
    float uvOffset[2];     // row0: uvOffsetScale.xy
    float uvScale[2];      //       uvOffsetScale.zw
    float texel[2];        // row1: texelTime.xy
    float time;            //       texelTime.z
    float _pad0;           //       texelTime.w
    int   enableMask;      // row2: masks.x
    int   posterizeLevels; //       masks.y
    int   ditherLevels;    //       masks.z
    int   _pad1;           //       masks.w
    float exposure;        // row3: cg0
    float contrast;
    float brightness;
    float saturation;
    float warmth;          // row4: cg1
    float hueShift;
    float bloom;
    float bloomThreshold;
    float tint[3];         // row5: tintVig.xyz
    float vignette;        //       tintVig.w
    float chromatic;       // row6: stylize0
    float pixelSize;
    float scanline;
    float sharpen;
    float grain;           // row7: stylize1
    float invert;
    float sepia;
    float grayscale;
    float lens;            // row8: dist0
    float radial;
    float glitch;
    float _pad2;
    float waveAmp;         // row9: wave
    float waveFreq;
    float waveSpeed;
    float _pad3;
    float outlineColor[3]; // row10: outline.xyz
    float outline;         //        outline.w
};
static_assert(sizeof(PostCB) == 44 * sizeof(float), "PostCB must be 44 DWORDs");
static constexpr UINT kPostCBNum32 = 44;

void PostProcess::Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat, const std::wstring& shaderDir)
{
    auto* dev = device.GetDevice();

    // --- Root Signature: t0(SRV table) + b0(20 DWORD constants) + s0(linear clamp) ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;  // t0

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.ShaderRegister = 0;  // b0
        params[1].Constants.Num32BitValues = kPostCBNum32;
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;  // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 2;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO: VB なしフルスクリーン三角形 / Depth OFF / Cull NONE ---
    {
        auto vs = ShaderCompiler::LoadFromFile(shaderDir + L"PostProcess_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(shaderDir + L"PostProcess_PS.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.InputLayout = { nullptr, 0 };  // SV_VertexID のみ

        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;

        pso.SampleMask            = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = outFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
    }

    Logger::Info("PostProcess initialized");
}

void PostProcess::Apply(ID3D12GraphicsCommandList* cmd,
                        D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu,
                        const PostProcessSettings& s,
                        float uvOffsetX, float uvOffsetY,
                        float uvScaleX, float uvScaleY,
                        float texelW, float texelH,
                        float timeSeconds)
{
    if (!m_pso) return;

    PostCB cb{};
    cb.uvOffset[0] = uvOffsetX; cb.uvOffset[1] = uvOffsetY;
    cb.uvScale[0]  = uvScaleX;  cb.uvScale[1]  = uvScaleY;
    cb.texel[0]    = texelW;    cb.texel[1]    = texelH;
    cb.time        = timeSeconds;

    // 有効ビットを集計（マスターOFF なら全部 0 = 素通し）
    uint32_t mask = 0;
    if (s.enabled)
    {
        if (s.exposureOn)   mask |= PE_EXPOSURE;
        if (s.contrastOn)   mask |= PE_CONTRAST;
        if (s.brightnessOn) mask |= PE_BRIGHTNESS;
        if (s.saturationOn) mask |= PE_SATURATION;
        if (s.warmthOn)     mask |= PE_WARMTH;
        if (s.hueOn)        mask |= PE_HUE;
        if (s.tintOn)       mask |= PE_TINT;
        if (s.bloomOn)      mask |= PE_BLOOM;
        if (s.vignetteOn)   mask |= PE_VIGNETTE;
        if (s.chromaticOn)  mask |= PE_CHROMATIC;
        if (s.pixelizeOn)   mask |= PE_PIXELIZE;
        if (s.posterizeOn)  mask |= PE_POSTERIZE;
        if (s.ditherOn)     mask |= PE_DITHER;
        if (s.scanlineOn)   mask |= PE_SCANLINE;
        if (s.sharpenOn)    mask |= PE_SHARPEN;
        if (s.grainOn)      mask |= PE_GRAIN;
        if (s.invertOn)     mask |= PE_INVERT;
        if (s.sepiaOn)      mask |= PE_SEPIA;
        if (s.grayscaleOn)  mask |= PE_GRAYSCALE;
        if (s.lensOn)       mask |= PE_LENS;
        if (s.waveOn)       mask |= PE_WAVE;
        if (s.radialOn)     mask |= PE_RADIAL;
        if (s.glitchOn)     mask |= PE_GLITCH;
        if (s.outlineOn)    mask |= PE_OUTLINE;
        if (s.fxaaOn)       mask |= PE_FXAA;
    }
    cb.enableMask      = static_cast<int>(mask);
    cb.posterizeLevels = s.posterize;
    cb.ditherLevels    = s.ditherLevels;

    cb.exposure    = s.exposure;
    cb.contrast    = s.contrast;
    cb.brightness  = s.brightness;
    cb.saturation  = s.saturation;
    cb.warmth      = s.warmth;
    cb.hueShift    = s.hueShift;
    cb.bloom          = s.bloom;
    cb.bloomThreshold = s.bloomThreshold;
    cb.tint[0]     = s.tint.x; cb.tint[1] = s.tint.y; cb.tint[2] = s.tint.z;
    cb.vignette    = s.vignette;
    cb.chromatic   = s.chromatic;
    cb.pixelSize   = s.pixelSize;
    cb.scanline    = s.scanline;
    cb.sharpen     = s.sharpen;
    cb.grain       = s.grain;
    cb.invert      = s.invert;
    cb.sepia       = s.sepia;
    cb.grayscale   = s.grayscale;
    cb.lens        = s.lens;
    cb.radial      = s.radial;
    cb.glitch      = s.glitch;
    cb.waveAmp     = s.waveAmp;
    cb.waveFreq    = s.waveFreq;
    cb.waveSpeed   = s.waveSpeed;
    cb.outlineColor[0] = s.outlineColor.x;
    cb.outlineColor[1] = s.outlineColor.y;
    cb.outlineColor[2] = s.outlineColor.z;
    cb.outline     = s.outline;

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, sceneSrvGpu);
    cmd->SetGraphicsRoot32BitConstants(1, kPostCBNum32, &cb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
