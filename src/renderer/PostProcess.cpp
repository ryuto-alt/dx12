#include "renderer/PostProcess.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/Buffer.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
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
    PE_AUTOEXP    = 1u << 25,
    PE_LUT        = 1u << 26,
    PE_DEBAND     = 1u << 27,
    PE_GODRAYS    = 1u << 28,
    PE_LENSFLARE  = 1u << 29,
    PE_DISTORT    = 1u << 30,
};

// HLSL の cbuffer PostCB と一致させる（19 個の float4 = 76 DWORD、CBV 経由）。
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
    int   tonemapper;      //       masks.w（0=ACES, 1=AgX, 2=なし）
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
    float lutSize;         // row11: extra0.x
    float lutAmount;       //        extra0.y
    float _pad4[2];        //        extra0.zw
    // ── 詳細パラメータ（「1 エフェクト 1 スライダー」を卒業するための追加分）──
    float vignetteRadius;    // row12: vig2
    float vignetteSoftness;
    float vignetteRoundness;
    float chromaMode;
    float vignetteColor[3];  // row13: vigCol.xyz
    float scanCount;         //        vigCol.w
    float lensMode;          // row14: lens2
    float lensK2;
    float lensZoom;
    float lensFlags;         //        bit0=円形補正 / bit1..2=はみ出し処理
    float lensChroma;        // row15: lens3
    float scanCurve;
    float glitchBlocks;
    float glitchSpeed;
    float glitchColor;       // row16: misc0
    float grainSize;
    float grainColored;
    float radialSamples;
    float radialCenterX;     // row17: misc1
    float radialCenterY;
    float outlineThickness;
    float outlineThreshold;
    float outlineBg[3];      // row18: outl2.xyz
    float outlineOnly;       //        outl2.w
};
static_assert(sizeof(PostCB) == 76 * sizeof(float), "PostCB must be 76 DWORDs (19 float4)");

PostProcess::PostProcess()  = default;
PostProcess::~PostProcess() = default;

void PostProcess::Initialize(GraphicsDevice& device, DXGI_FORMAT outFormat,
                             const std::wstring& shaderDir, u32 frameCount)
{
    auto* dev = device.GetDevice();
    m_frameCount = (frameCount > 0) ? frameCount : 3;

    // --- Root Signature ---
    //  p0: t0 シーン(SRV table) / p1: b0 PostCB(CBV) / p2: t1 ブルーム / p3: t2 LUT
    //  p4: t3 露出バッファ(root SRV) / p5: t4 ゴッドレイ / p6: t5 レンズフレア / s0 linear clamp
    {
        D3D12_DESCRIPTOR_RANGE sceneRange{};
        sceneRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sceneRange.NumDescriptors     = 1;
        sceneRange.BaseShaderRegister = 0;  // t0

        D3D12_DESCRIPTOR_RANGE bloomRange = sceneRange;
        bloomRange.BaseShaderRegister = 1;  // t1

        D3D12_DESCRIPTOR_RANGE lutRange = sceneRange;
        lutRange.BaseShaderRegister = 2;    // t2

        D3D12_DESCRIPTOR_RANGE grRange = sceneRange;
        grRange.BaseShaderRegister = 4;     // t4

        D3D12_DESCRIPTOR_RANGE lfRange = sceneRange;
        lfRange.BaseShaderRegister = 5;     // t5

        D3D12_DESCRIPTOR_RANGE distRange = sceneRange;
        distRange.BaseShaderRegister = 6;   // t6

        D3D12_ROOT_PARAMETER params[8]{};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges   = &sceneRange;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;  // b0
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &bloomRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges   = &lutRange;
        params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;  // t3（StructuredBuffer）
        params[4].Descriptor.ShaderRegister = 3;
        params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        params[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges = 1;
        params[5].DescriptorTable.pDescriptorRanges   = &grRange;
        params[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6].DescriptorTable.NumDescriptorRanges = 1;
        params[6].DescriptorTable.pDescriptorRanges   = &lfRange;
        params[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        params[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[7].DescriptorTable.NumDescriptorRanges = 1;
        params[7].DescriptorTable.pDescriptorRanges   = &distRange;
        params[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;  // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 8;
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
    m_outFormat = outFormat;
    m_shaderDir = shaderDir;
    RecreatePipelines(device);

    // --- パラメータ CB（フレーム × Apply 回数で多重化）---
    m_cb = std::make_unique<ConstantBuffer>();
    m_cb->Initialize(device, sizeof(PostCB), m_frameCount * kMaxAppliesPerFrame);

    Logger::Info("PostProcess initialized");
}

void PostProcess::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"PostProcess_PS.cso");

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
    pso.RTVFormats[0]         = m_outFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc            = { 1, 0 };

    ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso)));
}

void PostProcess::Apply(ID3D12GraphicsCommandList* cmd,
                        const Inputs& in,
                        const PostProcessSettings& s,
                        float texelW, float texelH,
                        float timeSeconds,
                        u32 frameIndex)
{
    if (!m_pso || !m_cb) return;

    PostCB cb{};
    // ★#16: シーンは RT 全面。参照範囲は常に (0,0)-(1,1)。
    cb.uvOffset[0] = 0.0f; cb.uvOffset[1] = 0.0f;
    cb.uvScale[0]  = 1.0f; cb.uvScale[1]  = 1.0f;
    cb.texel[0]    = texelW;    cb.texel[1]    = texelH;
    cb.time        = timeSeconds;
    cb.tonemapper  = s.tonemapper;  // 表示変換はマスターOFF でも常に適用

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
        if (s.bloomOn && in.bloomReady) mask |= PE_BLOOM;
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
        if (s.autoExposureOn && in.exposureVA != 0) mask |= PE_AUTOEXP;
        if (s.lutOn && in.lutSize >= 2.0f)          mask |= PE_LUT;
        if (s.debandOn)     mask |= PE_DEBAND;
        if (s.godraysOn && in.godraysReady)     mask |= PE_GODRAYS;
        if (s.lensflareOn && in.flareReady)     mask |= PE_LENSFLARE;
        if (in.distortReady)                    mask |= PE_DISTORT;  // 粒子コンテンツ駆動（設定なし）
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
    cb.lutSize     = in.lutSize;
    cb.lutAmount   = s.lutAmount;

    cb.vignetteRadius    = s.vignetteRadius;
    cb.vignetteSoftness  = s.vignetteSoftness;
    cb.vignetteRoundness = s.vignetteRoundness;
    cb.chromaMode        = static_cast<float>(s.chromaMode);
    cb.vignetteColor[0]  = s.vignetteColor.x;
    cb.vignetteColor[1]  = s.vignetteColor.y;
    cb.vignetteColor[2]  = s.vignetteColor.z;
    cb.scanCount         = s.scanCount;
    cb.lensMode          = static_cast<float>(s.lensMode);
    cb.lensK2            = s.lensK2;
    cb.lensZoom          = s.lensZoom;
    // bit0=円形補正 / bit1..2=はみ出し処理（0=端を伸ばす 1=黒 2=鏡映）
    cb.lensFlags         = static_cast<float>((s.lensCircular ? 1u : 0u)
                         | ((static_cast<u32>(std::clamp(s.lensEdge, 0, 2)) & 3u) << 1));
    cb.lensChroma        = s.lensChroma;
    cb.scanCurve         = s.scanCurve;
    cb.glitchBlocks      = s.glitchBlocks;
    cb.glitchSpeed       = s.glitchSpeed;
    cb.glitchColor       = s.glitchColor;
    cb.grainSize         = s.grainSize;
    cb.grainColored      = s.grainColored ? 1.0f : 0.0f;
    cb.radialSamples     = static_cast<float>(s.radialSamples);
    cb.radialCenterX     = s.radialCenterX;
    cb.radialCenterY     = s.radialCenterY;
    cb.outlineThickness  = s.outlineThickness;
    cb.outlineThreshold  = s.outlineThreshold;
    cb.outlineBg[0]      = s.outlineBg.x;
    cb.outlineBg[1]      = s.outlineBg.y;
    cb.outlineBg[2]      = s.outlineBg.z;
    cb.outlineOnly       = s.outlineOnly ? 1.0f : 0.0f;

    // 同一フレーム内の複数 Apply（メイン + カメラプレビュー等）で CB スロットを分ける。
    // コマンドリスト実行はフレーム末尾なので、同じスロットを使い回すと後勝ちで上書きされる。
    if (frameIndex != m_lastFrameIndex)
    {
        m_lastFrameIndex = frameIndex;
        m_applyIndex     = 0;
    }
    const u32 slot = (frameIndex % m_frameCount) * kMaxAppliesPerFrame
                   + (std::min)(m_applyIndex, kMaxAppliesPerFrame - 1);
    if (m_applyIndex < kMaxAppliesPerFrame) ++m_applyIndex;
    m_cb->Update(&cb, sizeof(cb), slot);

    cmd->SetPipelineState(m_pso.Get());
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetGraphicsRootDescriptorTable(0, in.sceneSrv);
    cmd->SetGraphicsRootConstantBufferView(1, m_cb->GetGpuAddress(slot));
    cmd->SetGraphicsRootDescriptorTable(2, in.bloomSrv);
    cmd->SetGraphicsRootDescriptorTable(3, in.lutSrv);
    if (in.exposureVA != 0)
        cmd->SetGraphicsRootShaderResourceView(4, in.exposureVA);
    cmd->SetGraphicsRootDescriptorTable(5, in.godraysSrv);
    cmd->SetGraphicsRootDescriptorTable(6, in.flareSrv);
    cmd->SetGraphicsRootDescriptorTable(7, in.distortSrv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
