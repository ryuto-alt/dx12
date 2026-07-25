#include "resource/ShaderRegistry.h"

#include <algorithm>
#include <cctype>

namespace dx12e
{
namespace
{

// 大小無視・区切り文字("\\"→"/")無視で比較するための正規化。
std::string NormalizeRelPath(const std::string& p)
{
    std::string out = p;
    for (char& c : out)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

const std::vector<ShaderSource>& BuildRegistry()
{
    static const std::vector<ShaderSource> registry = {
        {
            "forward/Forward.hlsl",
            {
                { L"Forward_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Forward_PS.cso", L"PSMain", L"ps_6_0" },
                { L"ForwardLdr_PS.cso", L"PSMain", L"ps_6_0", L"LDR_OUTPUT=1" },
            },
            { "forward/PBR.hlsli", "forward/Lighting.hlsli", "forward/ClusterCommon.hlsli" },
        },
        {
            "forward/ForwardSkinned.hlsl",
            {
                { L"ForwardSkinned_VS.cso", L"VSMain", L"vs_6_0" },
                { L"ForwardSkinned_PS.cso", L"PSMain", L"ps_6_0" },
            },
            { "forward/PBR.hlsli", "forward/Lighting.hlsli", "forward/ClusterCommon.hlsli" },
        },
        {
            // クラスタードライティング（Forward+）のライトカリング compute。
            // ClusterCommon.hlsli は Lighting.hlsli（＝フォワード PS 側）とも共有している。
            "forward/ClusterBuild.hlsl",
            { { L"ClusterBuild_CS.cso", L"CSMain", L"cs_6_0" } },
            { "forward/ClusterCommon.hlsli" },
        },
        {
            "forward/ClusterCull.hlsl",
            { { L"ClusterCull_CS.cso", L"CSMain", L"cs_6_0" } },
            { "forward/ClusterCommon.hlsli" },
        },
        {
            // ForwardGrid.hlsl は `#include "Lighting.hlsli"` している。
            // CMakeLists.txt の DEPENDS に Lighting.hlsli が抜けていた既存バグは修正済み
            // （PerFrameConstants を拡張したときに再コンパイルされずレイアウトがズレる）。
            "forward/ForwardGrid.hlsl",
            {
                { L"ForwardGrid_VS.cso", L"VSMain", L"vs_6_0" },
                { L"ForwardGrid_PS.cso", L"PSMain", L"ps_6_0" },
            },
            { "forward/PBR.hlsli", "forward/Lighting.hlsli", "forward/ClusterCommon.hlsli" },
        },
        {
            "shadow/ShadowPass.hlsl",
            { { L"ShadowPass_VS.cso", L"VSMain", L"vs_6_0" } },
            {},
        },
        {
            "shadow/ShadowPassSkinned.hlsl",
            { { L"ShadowPassSkinned_VS.cso", L"VSMain", L"vs_6_0" } },
            {},
        },
        {
            "shadow/DepthPrepassSkinned.hlsl",
            { { L"DepthPrepassSkinned_VS.cso", L"VSMain", L"vs_6_0" } },
            {},
        },
        {
            "debug/DebugLine.hlsl",
            {
                { L"DebugLine_VS.cso", L"VSMain", L"vs_6_0" },
                { L"DebugLine_PS.cso", L"PSMain", L"ps_6_0" },
            },
            {},
        },
        {
            "post/PostProcess.hlsl",
            {
                { L"PostProcess_VS.cso", L"FSTriVS", L"vs_6_0" },
                { L"PostProcess_PS.cso", L"PostPS", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "post/Bloom.hlsl",
            {
                { L"Bloom_VS.cso", L"FSTriVS", L"vs_6_0" },
                { L"BloomDown_PS.cso", L"BloomDownPS", L"ps_6_0" },
                { L"BloomUp_PS.cso", L"BloomUpPS", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "post/AutoExposure.hlsl",
            {
                { L"ExposureHistogram_CS.cso", L"CSHistogram", L"cs_6_0" },
                { L"ExposureAdapt_CS.cso", L"CSAdapt", L"cs_6_0" },
            },
            {},
        },
        {
            "post/GodRays.hlsl",
            {
                { L"GodRaysMask_PS.cso", L"GodRaysMaskPS", L"ps_6_0" },
                { L"GodRaysBlur_PS.cso", L"GodRaysBlurPS", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "post/LensFlare.hlsl",
            { { L"LensFlare_PS.cso", L"LensFlarePS", L"ps_6_0" } },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "post/DoF.hlsl",
            {
                { L"DofCoc_PS.cso", L"DofCocPS", L"ps_6_0" },
                { L"DofGather_PS.cso", L"DofGatherPS", L"ps_6_0" },
                { L"DofComposite_PS.cso", L"DofCompositePS", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "post/MotionBlur.hlsl",
            { { L"MotionBlur_PS.cso", L"MotionBlurPS", L"ps_6_0" } },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "sprite/Sprite.hlsl",
            {
                { L"Sprite_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Sprite_PS.cso", L"PSMain", L"ps_6_0" },
            },
            {},
        },
        {
            "editor/IconBillboard.hlsl",
            {
                { L"IconBillboard_VS.cso", L"VSMain", L"vs_6_0" },
                { L"IconBillboard_PS.cso", L"PSMain", L"ps_6_0" },
            },
            {},
        },
        {
            "post/Transition.hlsl",
            {
                { L"Transition_VS.cso", L"FSTriVS", L"vs_6_0" },
                { L"Transition_PS.cso", L"TransPS", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "forward/Emissive.hlsl",
            {
                { L"Emissive_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Emissive_PS.cso", L"PSMain", L"ps_6_0" },
            },
            {},
        },
        {
            "particle/Particle.hlsl",
            {
                { L"Particle_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Particle_PS.cso", L"PSMain", L"ps_6_0" },
                { L"ParticleDistort_PS.cso", L"PSDistort", L"ps_6_0" },
            },
            { "particle/ParticleCommon.hlsli" },
        },
        {
            "particle/Trail.hlsl",
            {
                { L"Trail_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Trail_PS.cso", L"PSMain", L"ps_6_0" },
            },
            { "particle/ParticleCommon.hlsli" },
        },
        {
            "particle/GpuParticleSim.hlsl",
            {
                { L"GpuParticleInit_CS.cso", L"CSInit", L"cs_6_0" },
                { L"GpuParticlePrepare_CS.cso", L"CSPrepare", L"cs_6_0" },
                { L"GpuParticleEmit_CS.cso", L"CSEmit", L"cs_6_0" },
                { L"GpuParticleKickoff_CS.cso", L"CSKickoff", L"cs_6_0" },
                { L"GpuParticleSimulate_CS.cso", L"CSSimulate", L"cs_6_0" },
            },
            {},
        },
        {
            "particle/GpuParticleDraw.hlsl",
            { { L"GpuParticleDraw_VS.cso", L"VSMain", L"vs_6_0" } },
            {},
        },
        {
            "particle/Beam.hlsl",
            {
                { L"Beam_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Beam_PS.cso", L"PSMain", L"ps_6_0" },
            },
            { "particle/ParticleCommon.hlsli" },
        },
        {
            "ibl/IrradianceConvolution.hlsl",
            { { L"IrradianceConvolution_CS.cso", L"CSMain", L"cs_6_0" } },
            { "ibl/IBLCommon.hlsli" },
        },
        {
            "ibl/PrefilterEnv.hlsl",
            { { L"PrefilterEnv_CS.cso", L"CSMain", L"cs_6_0" } },
            { "ibl/IBLCommon.hlsli" },
        },
        {
            "ibl/IntegrateBRDF.hlsl",
            { { L"IntegrateBRDF_CS.cso", L"CSMain", L"cs_6_0" } },
            { "ibl/IBLCommon.hlsli" },
        },
        {
            "ibl/Skybox.hlsl",
            {
                { L"Skybox_VS.cso", L"VSMain", L"vs_6_0" },
                { L"Skybox_PS.cso", L"PSMain", L"ps_6_0" },
            },
            {},
        },
        {
            "ssao/SSAO.hlsl",
            {
                { L"SSAO_VS.cso", L"FSTriVS", L"vs_6_0" },
                { L"SSAO_PS.cso", L"PSMain", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "ssao/SSAOBlur.hlsl",
            { { L"SSAOBlur_PS.cso", L"PSMain", L"ps_6_0" } },
            { "post/FullscreenTri.hlsli" },
        },
        {
            // コンタクトシャドウ（深度バッファのスクリーン空間レイマーチ。SSAO と同じ 1 パス構成）
            "shadow/ContactShadow.hlsl",
            {
                { L"ContactShadow_VS.cso", L"FSTriVS", L"vs_6_0" },
                { L"ContactShadow_PS.cso", L"PSMain", L"ps_6_0" },
            },
            { "post/FullscreenTri.hlsli" },
        },
        {
            // 速度バッファ（モーションベクター）生成。PS は 3 経路で共有する。
            "velocity/VelocityPrepass.hlsl",
            {
                { L"VelocityPrepass_VS.cso", L"VSMain",     L"vs_6_0" },
                { L"VelocityPrepass_PS.cso", L"VelocityPS", L"ps_6_0" },
            },
            { "velocity/VelocityCommon.hlsli" },
        },
        {
            "velocity/VelocityPrepassInstanced.hlsl",
            { { L"VelocityPrepassInstanced_VS.cso", L"VSMain", L"vs_6_0" } },
            { "velocity/VelocityCommon.hlsli" },
        },
        {
            "velocity/VelocityPrepassSkinned.hlsl",
            { { L"VelocityPrepassSkinned_VS.cso", L"VSMain", L"vs_6_0" } },
            { "velocity/VelocityCommon.hlsli" },
        },
        {
            "post/TAA.hlsl",
            { { L"TaaResolve_PS.cso", L"TaaResolvePS", L"ps_6_0" } },
            { "post/FullscreenTri.hlsli" },
        },
        {
            "velocity/VelocityDebug.hlsl",
            { { L"VelocityDebug_PS.cso", L"VelocityDebugPS", L"ps_6_0" } },
            { "post/FullscreenTri.hlsli" },
        },
    };
    return registry;
}

} // namespace

const std::vector<ShaderSource>& AllShaderSources()
{
    return BuildRegistry();
}

const ShaderSource* FindShaderSourceByRelPath(const std::string& relPath)
{
    const std::string needle = NormalizeRelPath(relPath);
    for (const auto& src : BuildRegistry())
    {
        if (NormalizeRelPath(src.relPath) == needle)
            return &src;
    }
    return nullptr;
}

std::vector<const ShaderSource*> FindShaderSourcesByStaticDep(const std::string& depRelPath)
{
    const std::string needle = NormalizeRelPath(depRelPath);
    std::vector<const ShaderSource*> out;
    for (const auto& src : BuildRegistry())
    {
        for (const char* dep : src.staticDeps)
        {
            if (NormalizeRelPath(dep) == needle)
            {
                out.push_back(&src);
                break;
            }
        }
    }
    return out;
}

} // namespace dx12e
