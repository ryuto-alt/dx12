#include "renderer/ParticleSystem.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/Texture.h"
#include "resource/ShaderCompiler.h"
#include "resource/ResourceManager.h"
#include "core/PathResolver.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace dx12e
{

// ====================================================================
//  CPU 側プロシージャルノイズ（カールノイズ＝divergence-free 乱流用）
// ====================================================================
namespace
{
inline u32 pcgu(u32 v)
{
    v = v * 747796405u + 2891336453u;
    u32 w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}

// 256方向の単位ベクトル表（フィボナッチ球で均一分布）。一度だけ構築し、grad3 の
// 毎回の sqrt/cos/sin を表引きに置換する。curl ノイズの見た目は等価（チャオティックな乱流）。
struct GradTable
{
    float g[256][3];
    GradTable()
    {
        const float golden = 2.399963230f;             // 黄金角（ラジアン）
        for (int i = 0; i < 256; ++i)
        {
            float zz = (2.0f * i + 1.0f) / 256.0f - 1.0f;            // [-1,1] 低不一致
            float r  = std::sqrt((std::max)(0.0f, 1.0f - zz * zz));
            float a  = i * golden;
            g[i][0] = r * std::cos(a); g[i][1] = r * std::sin(a); g[i][2] = zz;
        }
    }
};
static const GradTable kGrad;

inline void grad3(int x, int y, int z, float& gx, float& gy, float& gz)
{
    u32 h = pcgu(static_cast<u32>(x) ^ pcgu(static_cast<u32>(y) ^ pcgu(static_cast<u32>(z))));
    const float* g = kGrad.g[h & 255u];   // 表引き（sqrt/cos/sin なし、pcgu(h) も不要）
    gx = g[0]; gy = g[1]; gz = g[2];
}

// 勾配ノイズ 3D（quintic 補間 → C1 連続。curl が滑らかになる）
float gnoise3(float px, float py, float pz)
{
    float fx = std::floor(px), fy = std::floor(py), fz = std::floor(pz);
    int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    float tx = px - fx, ty = py - fy, tz = pz - fz;
    auto q = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    float ux = q(tx), uy = q(ty), uz = q(tz);
    auto corner = [&](int cx, int cy, int cz) -> float {
        float gx, gy, gz; grad3(ix + cx, iy + cy, iz + cz, gx, gy, gz);
        return gx * (tx - cx) + gy * (ty - cy) + gz * (tz - cz);
    };
    float c000 = corner(0,0,0), c100 = corner(1,0,0), c010 = corner(0,1,0), c110 = corner(1,1,0);
    float c001 = corner(0,0,1), c101 = corner(1,0,1), c011 = corner(0,1,1), c111 = corner(1,1,1);
    float x00 = std::lerp(c000, c100, ux), x10 = std::lerp(c010, c110, ux);
    float x01 = std::lerp(c001, c101, ux), x11 = std::lerp(c011, c111, ux);
    float y0 = std::lerp(x00, x10, uy), y1 = std::lerp(x01, x11, uy);
    return std::lerp(y0, y1, uz);
}

// ノイズ・ポテンシャル ψ の curl（中心差分）。∇×ψ は divergence-free。
XMFLOAT3 curlNoise(float x, float y, float z)
{
    const float e = 0.6f;
    auto psi = [](float a, float b, float c) -> XMFLOAT3 {
        return { gnoise3(a, b, c),
                 gnoise3(a + 31.4f, b - 17.2f, c + 9.1f),
                 gnoise3(a - 7.7f, b + 23.3f, c - 11.9f) };
    };
    XMFLOAT3 px0 = psi(x - e, y, z), px1 = psi(x + e, y, z);
    XMFLOAT3 py0 = psi(x, y - e, z), py1 = psi(x, y + e, z);
    XMFLOAT3 pz0 = psi(x, y, z - e), pz1 = psi(x, y, z + e);
    float cx = (py1.z - py0.z) - (pz1.y - pz0.y);
    float cy = (pz1.x - pz0.x) - (px1.z - px0.z);
    float cz = (px1.y - px0.y) - (py1.x - py0.x);
    float inv = 1.0f / (2.0f * e);
    return { cx * inv, cy * inv, cz * inv };
}

inline XMFLOAT3 lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    return { std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, t), std::lerp(a.z, b.z, t) };
}
} // namespace

void ParticleSystem::Initialize(GraphicsDevice& device, DXGI_FORMAT rtvFormat,
                                DXGI_FORMAT dsvFormat, DXGI_FORMAT distortFormat,
                                const std::wstring& shaderDir,
                                DescriptorHeap* srvHeap, ResourceManager* resourceManager)
{
    static_assert(sizeof(GpuParticle) == 68, "GpuParticle stride must match shader instance layout (68)");
    static_assert(sizeof(GpuBeam) == 56, "GpuBeam stride must match Beam.hlsl instance layout (56)");
    static_assert(sizeof(TrailVtx) == 36, "TrailVtx stride must match Trail.hlsl vertex layout (36)");
    (void)dsvFormat; // 粒子パスは DSV をバインドせず深度 SRV で手動オクルージョン
    m_srvHeap = srvHeap;
    m_resourceManager = resourceManager;
    auto* dev = device.GetDevice();

    // --- Root Signature: b0(32 DWORD constants) + t0(深度SRVテーブル,PS) + t2(粒子アルベドSRVテーブル,PS) + s0(LINEAR CLAMP) ---
    {
        D3D12_DESCRIPTOR_RANGE depthRange{};
        depthRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        depthRange.NumDescriptors     = 1;
        depthRange.BaseShaderRegister = 0;   // t0
        depthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE albedoRange{};
        albedoRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        albedoRange.NumDescriptors     = 1;
        albedoRange.BaseShaderRegister = 2;   // t2（t1はGpuParticleDraw.hlslのStructuredBufferと衝突するため回避）
        albedoRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0
        params[0].Constants.Num32BitValues = 32;  // float4x4 + float4*4
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &depthRange;
        params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &albedoRange;
        params[2].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD         = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;   // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = 3;
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    // --- PSO 生成（ホットリロード用に RecreatePipelines へ切り出し）---
    m_shaderDir     = shaderDir;
    m_rtvFormat     = rtvFormat;
    m_distortFormat = distortFormat;
    RecreatePipelines(device);

    // --- インスタンスバッファ（UPLOADヒープ。SpriteRenderer と同じ kFrames 区画リング運用）---
    {
        const UINT bufferSize = kFrames * kMaxParticles * sizeof(GpuParticle);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width            = bufferSize;
        res.Height           = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels        = 1;
        res.SampleDesc       = {1, 0};
        res.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceBuffer)));

        m_vbView.BufferLocation = m_instanceBuffer->GetGPUVirtualAddress();
        m_vbView.StrideInBytes  = sizeof(GpuParticle);
        m_vbView.SizeInBytes    = bufferSize;
    }

    // --- ビーム用インスタンスバッファ（同じく kFrames 区画リング）---
    {
        const UINT bufferSize = kFrames * kMaxBeams * sizeof(GpuBeam);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width            = bufferSize;
        res.Height           = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels        = 1;
        res.SampleDesc       = {1, 0};
        res.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_beamBuffer)));
        m_beamVbView.BufferLocation = m_beamBuffer->GetGPUVirtualAddress();
        m_beamVbView.StrideInBytes  = sizeof(GpuBeam);
        m_beamVbView.SizeInBytes    = bufferSize;
    }

    // --- トレイル用頂点バッファ（UPLOAD・kFrames 区画リング）---
    {
        const UINT bufferSize = kFrames * kMaxTrailVerts * sizeof(TrailVtx);
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC res{};
        res.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        res.Width            = bufferSize;
        res.Height           = 1;
        res.DepthOrArraySize = 1;
        res.MipLevels        = 1;
        res.SampleDesc       = {1, 0};
        res.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(dev->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &res,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_trailBuffer)));
        m_trailVbView.BufferLocation = m_trailBuffer->GetGPUVirtualAddress();
        m_trailVbView.StrideInBytes  = sizeof(TrailVtx);
        m_trailVbView.SizeInBytes    = bufferSize;
    }

    m_particles.assign(kMaxParticles, Particle{});
    m_gpu.reserve(kMaxParticles);
    m_live.reserve(kMaxParticles);
    m_freeList.reserve(kMaxParticles);
    ResetPool();
    m_beamPool.assign(kMaxBeams, Beam{});
    m_gpuBeams.reserve(kMaxBeams);
    m_childDefs.reserve(256);
    m_trails.reserve(32);
    m_trailVerts.reserve(4096);
    m_initialized = true;
    Logger::Info("ParticleSystem initialized (max {} particles + {} beams + {} trails, procedural kinds + curl + soft + distort)",
                 kMaxParticles, kMaxBeams, kMaxTrails);
}

void ParticleSystem::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    // --- PSO: インスタンスストリーム / 深度テスト無し（PSで手動オクルージョン） ---
    {
        auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Particle_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"Particle_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,          0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,          0, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, 36, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // stretch
            {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 40, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // vel
            {"TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT,          0, 52, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // age01
            {"TEXCOORD", 4, DXGI_FORMAT_R32_UINT,           0, 56, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // kind
            {"TEXCOORD", 5, DXGI_FORMAT_R32_FLOAT,          0, 60, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // seed
            {"TEXCOORD", 6, DXGI_FORMAT_R32_UINT,           0, 64, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // texIndex
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature        = m_rootSig.Get();
        pso.VS                    = { vs.GetData(), vs.GetSize() };
        pso.PS                    = { ps.GetData(), ps.GetSize() };
        pso.InputLayout           = { layout, _countof(layout) };
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D12_BLEND_ONE;   // 前乗算
        rt.DestBlend             = D3D12_BLEND_ONE;   // 加算（DestBlend は下で alpha 用に差し替え）
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rt.DestBlendAlpha        = D3D12_BLEND_ONE;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable   = FALSE;  // DSV 無し。PSで深度SRVから手動オクルージョン
        pso.DepthStencilState.StencilEnable = FALSE;

        pso.SampleMask            = UINT_MAX;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = m_rtvFormat;
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoAdd)));

        // 前乗算アルファ（煙）: Src=ONE, Dest=INV_SRC_ALPHA
        rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoAlpha)));

        // 歪みパーティクル: RG16F の歪みオフセットバッファへ加算（同じインスタンスレイアウト）
        auto psD = ShaderCompiler::LoadFromFile(m_shaderDir + L"ParticleDistort_PS.cso");
        pso.PS = { psD.GetData(), psD.GetSize() };
        rt.DestBlend      = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        pso.RTVFormats[0] = m_distortFormat;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoDistort)));
    }

    // --- トレイル PSO（頂点ストリーム / 加算 と 前乗算α の2種）---
    {
        auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Trail_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"Trail_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC tp{};
        tp.pRootSignature        = m_rootSig.Get();
        tp.VS                    = { vs.GetData(), vs.GetSize() };
        tp.PS                    = { ps.GetData(), ps.GetSize() };
        tp.InputLayout           = { layout, _countof(layout) };
        tp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        tp.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        tp.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        tp.RasterizerState.DepthClipEnable = TRUE;
        auto& trt = tp.BlendState.RenderTarget[0];
        trt.BlendEnable    = TRUE;
        trt.SrcBlend       = D3D12_BLEND_ONE;
        trt.DestBlend      = D3D12_BLEND_ONE;
        trt.BlendOp        = D3D12_BLEND_OP_ADD;
        trt.SrcBlendAlpha  = D3D12_BLEND_ONE;
        trt.DestBlendAlpha = D3D12_BLEND_ONE;
        trt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        trt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        tp.DepthStencilState.DepthEnable   = FALSE;
        tp.DepthStencilState.StencilEnable = FALSE;
        tp.SampleMask       = UINT_MAX;
        tp.NumRenderTargets = 1;
        tp.RTVFormats[0]    = m_rtvFormat;
        tp.DSVFormat        = DXGI_FORMAT_UNKNOWN;
        tp.SampleDesc       = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&tp, IID_PPV_ARGS(&m_psoTrailAdd)));

        trt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        trt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&tp, IID_PPV_ARGS(&m_psoTrailAlpha)));
    }

    // --- ビーム PSO（加算・root sig 流用：b0 32定数 + t0深度SRV + s0）---
    {
        auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"Beam_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"Beam_PS.cso");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // p1
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,          0, 40, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // halfW
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,          0, 44, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // age01
            {"TEXCOORD", 2, DXGI_FORMAT_R32_UINT,           0, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // kind
            {"TEXCOORD", 3, DXGI_FORMAT_R32_FLOAT,          0, 52, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},  // seed
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC bp{};
        bp.pRootSignature        = m_rootSig.Get();
        bp.VS                    = { vs.GetData(), vs.GetSize() };
        bp.PS                    = { ps.GetData(), ps.GetSize() };
        bp.InputLayout           = { layout, _countof(layout) };
        bp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        bp.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        bp.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        bp.RasterizerState.DepthClipEnable = TRUE;
        auto& brt = bp.BlendState.RenderTarget[0];
        brt.BlendEnable    = TRUE;
        brt.SrcBlend       = D3D12_BLEND_ONE;
        brt.DestBlend      = D3D12_BLEND_ONE;
        brt.BlendOp        = D3D12_BLEND_OP_ADD;
        brt.SrcBlendAlpha  = D3D12_BLEND_ONE;
        brt.DestBlendAlpha = D3D12_BLEND_ONE;
        brt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        brt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        bp.DepthStencilState.DepthEnable   = FALSE;
        bp.DepthStencilState.StencilEnable = FALSE;
        bp.SampleMask       = UINT_MAX;
        bp.NumRenderTargets = 1;
        bp.RTVFormats[0]    = m_rtvFormat;
        bp.DSVFormat        = DXGI_FORMAT_UNKNOWN;
        bp.SampleDesc       = { 1, 0 };
        ThrowIfFailed(dev->CreateGraphicsPipelineState(&bp, IID_PPV_ARGS(&m_psoBeam)));
    }
}

float ParticleSystem::Rand(float a, float b)
{
    std::uniform_real_distribution<float> d(a, b);
    return d(m_rng);
}

u32 ParticleSystem::InternTexPath(const std::string& path)
{
    if (path.empty()) return kNoTexture;
    for (size_t i = 0; i < m_texPaths.size(); ++i)
        if (m_texPaths[i] == path) return static_cast<u32>(i);
    m_texPaths.push_back(path);
    return static_cast<u32>(m_texPaths.size() - 1);
}

void ParticleSystem::ResetPool()
{
    m_freeList.resize(kMaxParticles);
    for (u32 i = 0; i < kMaxParticles; ++i)
    {
        m_freeList[i] = kMaxParticles - 1 - i;   // pop_back → 0,1,2,... の順で配る
        m_particles[i].alive = false;
    }
    m_live.clear();
}

void ParticleSystem::Emit(const EmitParams& p, const EmitParams* onDeath)
{
    if (!m_initialized) return;
    int count = (std::min)(p.count, static_cast<int>(kMaxParticles));

    // サブエミッタ定義を登録（リング上書き。定義が生存粒子より先に上書きされるのは
    // 60fps 連続放出でも kMaxChildDefs/60 ≒ 34 秒後なので実用上問題ない）
    int childIdx = -1;
    if (onDeath)
    {
        if (m_childDefs.size() < kMaxChildDefs)
        {
            m_childDefs.push_back(*onDeath);
            childIdx = static_cast<int>(m_childDefs.size()) - 1;
        }
        else
        {
            m_childDefs[m_childCursor] = *onDeath;
            childIdx = static_cast<int>(m_childCursor);
            m_childCursor = (m_childCursor + 1) % kMaxChildDefs;
        }
    }

    // dir 正規化
    XMVECTOR dirV = XMLoadFloat3(&p.dir);
    if (XMVectorGetX(XMVector3LengthSq(dirV)) < 1e-6f) dirV = XMVectorSet(0, 1, 0, 0);
    dirV = XMVector3Normalize(dirV);
    XMFLOAT3 dir; XMStoreFloat3(&dir, dirV);

    const u32 texSlot = InternTexPath(p.texturePath);

    for (int n = 0; n < count; ++n)
    {
        if (m_freeList.empty()) break;            // 満杯（O(1)）
        u32 idx = m_freeList.back();
        m_freeList.pop_back();
        m_live.push_back(idx);
        Particle& pt = m_particles[idx];

        // 方向：ring=XZ等間隔, それ以外は球/円錐ランダム
        XMFLOAT3 vdir;
        if (p.ring)
        {
            float a = (static_cast<float>(n) / static_cast<float>((std::max)(count, 1))) * 6.2831853f;
            vdir = { std::cos(a), 0.0f, std::sin(a) };
        }
        else
        {
            // 単位球上の一様サンプル
            float z = Rand(-1.0f, 1.0f);
            float t = Rand(0.0f, 6.2831853f);
            float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
            XMFLOAT3 sph{ r * std::cos(t), z, r * std::sin(t) };
            // dir 方向へ寄せる（spread=1 で完全ランダム、0 で dir）
            vdir.x = sph.x * p.spread + dir.x * (1.0f - p.spread);
            vdir.y = sph.y * p.spread + dir.y * (1.0f - p.spread);
            vdir.z = sph.z * p.spread + dir.z * (1.0f - p.spread);
        }

        float spd = p.speed * (1.0f - Rand(0.0f, p.speedVar));
        pt.pos = p.pos;
        pt.vel = { vdir.x * spd, vdir.y * spd + p.up * p.speed * 0.5f, vdir.z * spd };
        pt.col0 = { p.color.x * p.intensity, p.color.y * p.intensity, p.color.z * p.intensity };
        pt.hasMid = p.hasColorMid;
        if (p.hasColorMid)
            pt.colM = { p.colorMid.x * p.intensity, p.colorMid.y * p.intensity, p.colorMid.z * p.intensity };
        else
            pt.colM = pt.col0;
        if (p.hasColorEnd)
            pt.col1 = { p.colorEnd.x * p.intensity, p.colorEnd.y * p.intensity, p.colorEnd.z * p.intensity };
        else
            pt.col1 = pt.col0;
        pt.size0 = p.size  * Rand(0.7f, 1.0f);
        pt.size1 = p.sizeEnd;
        pt.sizeM = p.sizeMid;
        pt.life  = (std::max)(0.05f, p.life * (1.0f - Rand(0.0f, p.lifeVar)));
        pt.age   = 0.0f;
        pt.rot   = Rand(0.0f, 6.2831853f);
        pt.rotVel = Rand(-4.0f, 4.0f);
        pt.gravity = p.gravity;
        pt.drag  = p.drag;
        pt.alpha = 1.0f;
        pt.stretch = p.stretch;
        pt.turbStrength = p.turbStrength;
        pt.turbFreq = p.turbFreq;
        pt.seed = Rand(0.0f, 1000.0f);
        pt.flicker = p.flicker;
        pt.flickerFreq = p.flickerFreq;
        pt.distort = p.distort;
        pt.light = p.light;
        pt.lightRange = p.lightRange;
        pt.childIdx = childIdx;
        pt.kind  = p.kind;
        pt.blend = p.blend;
        pt.orient = p.orient;
        pt.texSlot = texSlot;
        pt.alive = true;
    }
}

void ParticleSystem::TrailPoint(u64 id, const XMFLOAT3& pos, const TrailParams& tp)
{
    if (!m_initialized) return;

    Trail* trail = nullptr;
    for (auto& t : m_trails)
        if (t.id == id) { trail = &t; break; }
    if (!trail)
    {
        if (m_trails.size() >= kMaxTrails) return;
        m_trails.push_back(Trail{});
        trail = &m_trails.back();
        trail->id = id;
        trail->nodes.reserve(kMaxTrailNodes);
    }
    trail->p = tp;              // 生パラメータ編集を即反映
    trail->sinceTouched = 0.0f;

    if (trail->nodes.empty())
    {
        trail->nodes.push_back({pos, 0.0f});
        return;
    }
    const XMFLOAT3& last = trail->nodes.back().pos;
    const float dx = pos.x - last.x, dy = pos.y - last.y, dz = pos.z - last.z;
    if (dx * dx + dy * dy + dz * dz < tp.minDist * tp.minDist) return;
    if (trail->nodes.size() >= kMaxTrailNodes)
        trail->nodes.erase(trail->nodes.begin());   // 最古を捨てる（点数は少ないので線形でOK）
    trail->nodes.push_back({pos, 0.0f});
}

u32 ParticleSystem::CollectLights(u32 maxCount, LightInfo* out) const
{
    if (maxCount == 0) return 0;
    // light 粒子から輝度上位 maxCount 個を選ぶ（挿入ソート・生存数は高々数千）
    struct Cand { float score; LightInfo info; };
    std::vector<Cand> top;
    top.reserve(maxCount + 1);

    for (u32 idx : m_live)
    {
        const Particle& pt = m_particles[idx];
        if (!pt.light) continue;
        const float t    = pt.age / pt.life;
        const float fade = (1.0f - t) * (1.0f - t);
        const XMFLOAT3 col = pt.hasMid
            ? (t < 0.5f ? lerp3(pt.col0, pt.colM, t * 2.0f)
                        : lerp3(pt.colM, pt.col1, (t - 0.5f) * 2.0f))
            : lerp3(pt.col0, pt.col1, t);
        const float score = (col.x * 0.2126f + col.y * 0.7152f + col.z * 0.0722f) * fade;
        if (score < 0.01f) continue;

        Cand c;
        c.score = score;
        c.info.pos   = pt.pos;
        c.info.range = pt.lightRange;
        // 粒子色は HDR に増幅済みなので、ライトとしては 0.5 倍で照らす（吹き飛び防止）
        c.info.color = { col.x * fade * 0.5f, col.y * fade * 0.5f, col.z * fade * 0.5f };

        auto it = top.begin();
        while (it != top.end() && it->score >= score) ++it;
        top.insert(it, c);
        if (top.size() > maxCount) top.pop_back();
    }

    const u32 n = static_cast<u32>(top.size());
    for (u32 i = 0; i < n; ++i) out[i] = top[i].info;
    return n;
}

void ParticleSystem::EmitBeam(const BeamParams& bp)
{
    if (!m_initialized) return;
    u32 idx = kMaxBeams;
    for (u32 s = 0; s < kMaxBeams; ++s)
    {
        u32 c = (m_beamCursor + s) % kMaxBeams;
        if (!m_beamPool[c].alive) { idx = c; m_beamCursor = (c + 1) % kMaxBeams; break; }
    }
    if (idx == kMaxBeams) return;
    Beam& b = m_beamPool[idx];
    b.p0    = bp.p0;
    b.p1    = bp.p1;
    b.width = bp.width;
    b.col   = { bp.color.x * bp.intensity, bp.color.y * bp.intensity, bp.color.z * bp.intensity };
    b.life  = (std::max)(0.02f, bp.life);
    b.age   = 0.0f;
    b.seed  = Rand(0.0f, 1000.0f);
    b.kind  = bp.kind;
    b.alive = true;
}

void ParticleSystem::Update(f32 dt)
{
    // 画面パルス減衰
    if (m_pulse > 0.0f) m_pulse = (std::max)(0.0f, m_pulse - dt * 3.0f);
    if (dt <= 0.0f) return;

    for (size_t i = 0; i < m_live.size(); )
    {
        u32 idx = m_live[i];
        Particle& pt = m_particles[idx];
        pt.age += dt;
        if (pt.age >= pt.life)
        {
            // サブエミッタ: 死亡位置で子バースト（ループ後にまとめて放出）
            if (pt.childIdx >= 0 && pt.childIdx < static_cast<int>(m_childDefs.size()))
                m_deathSpawns.emplace_back(pt.pos, pt.childIdx);
            pt.alive = false;
            m_freeList.push_back(idx);
            m_live[i] = m_live.back();   // swap-remove（末尾を詰める）
            m_live.pop_back();
            continue;                    // i は据え置き（入れ替えた末尾を処理）
        }

        // カールノイズ乱流（divergence-free）：流体的な揺らぎ。turbStrength=0 で無効。
        if (pt.turbStrength > 0.0f)
        {
            float f = pt.turbFreq * 0.35f;
            XMFLOAT3 c = curlNoise(pt.pos.x * f + pt.age * 0.25f, pt.pos.y * f, pt.pos.z * f);
            float g = pt.turbStrength * 2.5f;
            pt.vel.x += c.x * g * dt;
            pt.vel.y += c.y * g * dt;
            pt.vel.z += c.z * g * dt;
        }

        pt.vel.y += pt.gravity * dt;
        float damp = (std::max)(0.0f, 1.0f - pt.drag * dt);
        pt.vel.x *= damp; pt.vel.y *= damp; pt.vel.z *= damp;
        const float prevY = pt.pos.y;
        pt.pos.x += pt.vel.x * dt;
        pt.pos.y += pt.vel.y * dt;
        pt.pos.z += pt.vel.z * dt;
        pt.rot += pt.rotVel * dt;
        // 床で軽くバウンド（y=0.05 の見えない平面。地面が原点にある前提の簡易処理）
        // ★「今フレームで上から突き抜けた」ときだけ弾く。位置だけで判定すると、
        //   地下（洞窟・地下室・水中）に置いた放出器の粒子が gravity<0 で落ちた瞬間に
        //   y=-8 → y=+0.05 へ **8m 瞬間移動** して原点の見えない床を這う。
        //   ログも警告も出ないので「なぜか煙が上に飛ぶ」としか見えなかった。
        // ponytail: 平面は据え置きで貫通判定だけ足した。任意の高さに置きたくなったら
        //           ParticleEmitter に floorY を生やす（シェーダ側は既に -1e9=無効に対応済み）。
        constexpr float kFloorY = 0.05f;
        if (pt.pos.y < kFloorY && prevY >= kFloorY && pt.vel.y < 0.0f)
        {
            pt.pos.y = kFloorY;
            pt.vel.y = -pt.vel.y * 0.35f;
            pt.vel.x *= 0.7f; pt.vel.z *= 0.7f;
        }
        ++i;
    }

    // サブエミッタの子バーストを放出（メインループの外＝m_live 変更が安全な地点）。
    // 子の childIdx は付けない＝ネストは 1 段のみ（無限連鎖防止）。
    if (!m_deathSpawns.empty())
    {
        for (const auto& [pos, ci] : m_deathSpawns)
        {
            EmitParams cp = m_childDefs[ci];
            cp.pos = pos;
            Emit(cp, nullptr);
        }
        m_deathSpawns.clear();
    }

    // ビーム寿命（移動せず減衰のみ）
    for (auto& b : m_beamPool)
    {
        if (!b.alive) continue;
        b.age += dt;
        if (b.age >= b.life) b.alive = false;
    }

    // トレイル: 各点を加齢して寿命切れを先頭から捨てる。触られなくなった空トレイルは削除。
    for (size_t ti = 0; ti < m_trails.size(); )
    {
        Trail& tr = m_trails[ti];
        tr.sinceTouched += dt;
        for (auto& n : tr.nodes) n.age += dt;
        while (!tr.nodes.empty() && tr.nodes.front().age >= tr.p.life)
            tr.nodes.erase(tr.nodes.begin());
        if (tr.nodes.empty() && tr.sinceTouched > 0.5f)
        {
            m_trails[ti] = m_trails.back();
            m_trails.pop_back();
            continue;
        }
        ++ti;
    }
}

void ParticleSystem::Clear()
{
    ResetPool();
    for (auto& b : m_beamPool) b.alive = false;
    m_trails.clear();
    m_deathSpawns.clear();
    m_childDefs.clear();
    m_childCursor = 0;
    m_aliveCount = 0;
    m_distortCount = 0;
    m_pulse = 0.0f;
}

void ParticleSystem::Render(ID3D12GraphicsCommandList* cmd, XMMATRIX viewProj,
                            XMFLOAT3 camRight, XMFLOAT3 camUp, XMFLOAT3 camPos)
{
    if (!m_initialized) return;

    // テクスチャスロット → SRVヒープの絶対インデックス（このフレームのみ有効。ResourceManager
    // 側でキャッシュ済みなので毎フレーム呼んでも実ロードは初回のみ）。
    std::vector<u32> slotSrv(m_texPaths.size(), kNoTexture);
    if (m_resourceManager)
    {
        for (size_t i = 0; i < m_texPaths.size(); ++i)
        {
            const std::string abs = PathResolver::AssetsDir() + m_texPaths[i];
            if (Texture* tex = m_resourceManager->GetOrLoadTexture(PathResolver::Utf8ToWide(abs), cmd))
                slotSrv[i] = tex->GetSrvIndex();
        }
    }

    // 1粒子 → GpuParticle 変換（寿命でフェード、3キー色カーブ、明滅）
    auto makeGpu = [&slotSrv](const Particle& pt) -> GpuParticle {
        float t = pt.age / pt.life;           // 0..1
        float fade = 1.0f - t;                // 末尾でフェードアウト
        fade = fade * fade;

        XMFLOAT3 col = pt.hasMid
            ? (t < 0.5f ? lerp3(pt.col0, pt.colM, t * 2.0f)
                        : lerp3(pt.colM, pt.col1, (t - 0.5f) * 2.0f))
            : lerp3(pt.col0, pt.col1, t);

        if (pt.flicker > 0.0f)
        {
            float ph = pt.seed * 53.0f + pt.age * pt.flickerFreq;
            float fl = 1.0f + pt.flicker * (0.6f * std::sin(ph) + 0.4f * std::sin(ph * 2.137f));
            fl = (std::max)(0.0f, fl);
            col.x *= fl; col.y *= fl; col.z *= fl;
        }

        // サイズ: 3キー（start→mid→end）対応
        float size;
        if (pt.sizeM >= 0.0f)
            size = (t < 0.5f) ? pt.size0 + (pt.sizeM - pt.size0) * (t * 2.0f)
                              : pt.sizeM + (pt.size1 - pt.sizeM) * ((t - 0.5f) * 2.0f);
        else
            size = pt.size0 + (pt.size1 - pt.size0) * t;

        GpuParticle g;
        g.center  = pt.pos;
        g.size    = size;
        g.color   = { col.x, col.y, col.z, pt.alpha * fade };
        g.rot     = pt.rot;
        g.stretch = pt.stretch;
        g.vel     = pt.vel;
        g.age01   = t;
        // kind の上位ビットに orient をパックする(インスタンスレイアウトを変えずにVSへ渡すため。
        // Particle.hlsl 側で kind & 0xFFFF / kind >> 16 に分解する)
        g.kind    = static_cast<u32>(pt.kind) | (static_cast<u32>(pt.orient & 0x3) << 16);
        g.seed    = pt.seed;
        g.texIndex = (pt.texSlot < slotSrv.size()) ? slotSrv[pt.texSlot] : kNoTexture;
        return g;
    };

    // 加算 → α → 歪み の順に詰める（PSO 切替を最少に）。生存リストのみ走査（O(alive)）。
    m_gpu.clear();
    for (u32 idx : m_live)
    {
        const Particle& pt = m_particles[idx];
        if (pt.distort <= 0.0f && pt.blend == 0) m_gpu.push_back(makeGpu(pt));
    }
    m_additiveCount = static_cast<u32>(m_gpu.size());
    for (u32 idx : m_live)
    {
        const Particle& pt = m_particles[idx];
        if (pt.distort <= 0.0f && pt.blend != 0) m_gpu.push_back(makeGpu(pt));
    }
    m_alphaCount = static_cast<u32>(m_gpu.size()) - m_additiveCount;

    // テクスチャ切替（SetGraphicsRootDescriptorTable）を最少にするため各ブレンド域内で texIndex 昇順に
    // 安定ソート（同一テクスチャが連続する。加算は描画順が結果に影響しないので無害。α域も従来から
    // 深度ソート無し=既存挙動を大きく崩さない）。
    std::sort(m_gpu.begin(), m_gpu.begin() + m_additiveCount,
              [](const GpuParticle& a, const GpuParticle& b) { return a.texIndex < b.texIndex; });
    std::sort(m_gpu.begin() + m_additiveCount, m_gpu.begin() + m_additiveCount + m_alphaCount,
              [](const GpuParticle& a, const GpuParticle& b) { return a.texIndex < b.texIndex; });

    for (u32 idx : m_live)
    {
        const Particle& pt = m_particles[idx];
        if (pt.distort <= 0.0f) continue;
        GpuParticle g = makeGpu(pt);
        // 歪みパーティクルは色不要 → r に歪み強度を載せる（a はフェード）
        g.color.x = pt.distort;
        g.color.y = 0.0f;
        g.color.z = 0.0f;
        m_gpu.push_back(g);
    }
    m_distortCount = static_cast<u32>(m_gpu.size()) - m_additiveCount - m_alphaCount;
    m_aliveCount = static_cast<int>(m_gpu.size());

    // 生きてるビーム → GpuBeam 変換
    m_gpuBeams.clear();
    for (const auto& b : m_beamPool)
    {
        if (!b.alive) continue;
        float t = b.age / b.life;
        // 火柱は噴出→崩れをシェーダ(age01)が制御するので寿命フェードを掛けない。
        float fade = (b.kind == static_cast<int>(BeamKind::Fire)) ? 1.0f : (1.0f - t) * (1.0f - t);
        GpuBeam g;
        g.p0    = b.p0;
        g.p1    = b.p1;
        g.color = { b.col.x, b.col.y, b.col.z, fade };
        g.halfW = b.width * 0.5f;
        g.age01 = t;
        g.kind  = static_cast<u32>(b.kind);
        g.seed  = b.seed;
        m_gpuBeams.push_back(g);
        if (m_gpuBeams.size() >= kMaxBeams) break;
    }

    // ===== トレイル頂点構築（加算 → α の順）=====
    m_trailVerts.clear();
    m_trailAddVerts = 0;
    {
        const XMVECTOR camP = XMLoadFloat3(&camPos);
        auto buildTrails = [&](int blend)
        {
            for (const auto& tr : m_trails)
            {
                if (tr.p.blend != blend) continue;
                const auto& nd = tr.nodes;
                const size_t n = nd.size();
                if (n < 2) continue;

                XMFLOAT3 prevL{}, prevR{}, prevCol{};
                float prevU = 0.0f, prevA = 0.0f;
                bool hasPrev = false;
                for (size_t k = 0; k < n; ++k)
                {
                    const XMVECTOR p = XMLoadFloat3(&nd[k].pos);
                    XMVECTOR dir;
                    if (k == 0)          dir = XMVectorSubtract(XMLoadFloat3(&nd[1].pos), p);
                    else if (k == n - 1) dir = XMVectorSubtract(p, XMLoadFloat3(&nd[k - 1].pos));
                    else                 dir = XMVectorSubtract(XMLoadFloat3(&nd[k + 1].pos),
                                                                XMLoadFloat3(&nd[k - 1].pos));
                    // カメラフェーシング: 進行方向×視線 で帯の横ベクトルを出す
                    XMVECTOR side = XMVector3Cross(dir, XMVectorSubtract(camP, p));
                    if (XMVectorGetX(XMVector3LengthSq(side)) < 1e-8f)
                        side = XMLoadFloat3(&camUp);
                    side = XMVector3Normalize(side);

                    float a = (std::max)(0.0f, 1.0f - nd[k].age / (std::max)(tr.p.life, 1e-3f));
                    const float halfW = tr.p.width * 0.5f * (0.35f + 0.65f * a);
                    XMFLOAT3 L, R;
                    XMStoreFloat3(&L, XMVectorAdd(p, XMVectorScale(side,  halfW)));
                    XMStoreFloat3(&R, XMVectorAdd(p, XMVectorScale(side, -halfW)));
                    const float u = static_cast<float>(k) / static_cast<float>(n - 1);
                    XMFLOAT3 col = lerp3(tr.p.colorEnd, tr.p.color, a);
                    col = { col.x * tr.p.intensity, col.y * tr.p.intensity, col.z * tr.p.intensity };

                    if (hasPrev && m_trailVerts.size() + 6 <= kMaxTrailVerts)
                    {
                        auto push = [&](const XMFLOAT3& pp, float uu, float vv,
                                        const XMFLOAT3& cc, float aa)
                        { m_trailVerts.push_back({pp, {cc.x, cc.y, cc.z, aa}, {uu, vv}}); };
                        push(prevL, prevU,  1.0f, prevCol, prevA);
                        push(prevR, prevU, -1.0f, prevCol, prevA);
                        push(R,     u,     -1.0f, col,     a);
                        push(prevL, prevU,  1.0f, prevCol, prevA);
                        push(R,     u,     -1.0f, col,     a);
                        push(L,     u,      1.0f, col,     a);
                    }
                    prevL = L; prevR = R; prevU = u; prevA = a; prevCol = col;
                    hasPrev = true;
                }
            }
        };
        buildTrails(0);
        m_trailAddVerts = static_cast<u32>(m_trailVerts.size());
        buildTrails(1);
    }

    if (m_gpu.empty() && m_gpuBeams.empty() && m_trailVerts.empty()) return;

    // フレーム多重化: 前フレームが in-flight で読んでいる区画を上書きしないよう巡回
    // （単一区画だとWaitIdle無しの多重フレームでGPU/CPUが競合しチラつく）
    m_frameIdx = (m_frameIdx + 1) % kFrames;

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    if (m_hasDepth) cmd->SetGraphicsRootDescriptorTable(1, m_depthSrv);
    // t1(粒子アルベド) は安全なデフォルト(白)で初期化しておく。プロシージャル粒子はシェーダ内で
    // サンプル自体をスキップするので実際には読まれないが、GPU-Based Validation 対策で常に何か束縛しておく。
    if (m_resourceManager && m_srvHeap)
        cmd->SetGraphicsRootDescriptorTable(2,
            m_srvHeap->GetGpuHandle(m_resourceManager->GetDefaultWhiteTexture()->GetSrvIndex()));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const float softFadeDist = 0.5f;
    const XMFLOAT4 params2 = { m_projA, m_projB, m_hasDepth ? m_invRTW : 0.0f, m_invRTH };

    // ===== パーティクル =====
    if (!m_gpu.empty())
    {
        const UINT regionBytes  = kMaxParticles * sizeof(GpuParticle);
        const UINT regionOffset = m_frameIdx * regionBytes;

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        ThrowIfFailed(m_instanceBuffer->Map(0, &readRange, &mapped));
        memcpy(static_cast<u8*>(mapped) + regionOffset, m_gpu.data(), m_gpu.size() * sizeof(GpuParticle));
        m_instanceBuffer->Unmap(0, nullptr);

        m_vbView.BufferLocation = m_instanceBuffer->GetGPUVirtualAddress() + regionOffset;
        m_vbView.SizeInBytes    = regionBytes;

        struct CamCB
        {
            XMFLOAT4X4 viewProj;
            XMFLOAT4   camRight;
            XMFLOAT4   camUp;
            XMFLOAT4   params;
            XMFLOAT4   params2;
        } cb;
        XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewProj));
        cb.camRight = { camRight.x, camRight.y, camRight.z, 0.0f };
        cb.camUp    = { camUp.x,    camUp.y,    camUp.z,    0.0f };
        cb.params   = { 1.0f, 2.2f, m_time, softFadeDist };
        cb.params2  = params2;

        cmd->SetGraphicsRoot32BitConstants(0, 32, &cb, 0);
        cmd->IASetVertexBuffers(0, 1, &m_vbView);

        // texIndex が連続する区間ごとに t1 を張り替えて DrawInstanced（Sprite/世界スプライトと同じ
        // バッチ手法）。プロシージャル(kNoTexture)の連続区間は張り替えを省略する。
        auto drawRange = [&](u32 start, u32 count) {
            u32 i = 0;
            while (i < count)
            {
                const u32 tex = m_gpu[start + i].texIndex;
                const u32 runStart = i;
                while (i < count && m_gpu[start + i].texIndex == tex) ++i;
                if (tex != kNoTexture && m_srvHeap)
                    cmd->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGpuHandle(tex));
                cmd->DrawInstanced(6, i - runStart, 0, start + runStart);
            }
        };
        if (m_additiveCount > 0)
        {
            cmd->SetPipelineState(m_psoAdd.Get());
            drawRange(0, m_additiveCount);
        }
        if (m_alphaCount > 0)
        {
            cmd->SetPipelineState(m_psoAlpha.Get());
            drawRange(m_additiveCount, m_alphaCount);
        }
        // 歪み粒子（末尾 m_distortCount 個）はここでは描かない → RenderDistortion で歪みRTへ
    }

    // ===== ビーム =====
    if (!m_gpuBeams.empty())
    {
        const UINT regionBytes  = kMaxBeams * sizeof(GpuBeam);
        const UINT regionOffset = m_frameIdx * regionBytes;

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        ThrowIfFailed(m_beamBuffer->Map(0, &readRange, &mapped));
        memcpy(static_cast<u8*>(mapped) + regionOffset, m_gpuBeams.data(), m_gpuBeams.size() * sizeof(GpuBeam));
        m_beamBuffer->Unmap(0, nullptr);

        m_beamVbView.BufferLocation = m_beamBuffer->GetGPUVirtualAddress() + regionOffset;
        m_beamVbView.SizeInBytes    = regionBytes;

        struct BeamCB
        {
            XMFLOAT4X4 viewProj;
            XMFLOAT4   camPos;
            XMFLOAT4   params;
            XMFLOAT4   params2;
            XMFLOAT4   pad0;
        } bcb;
        XMStoreFloat4x4(&bcb.viewProj, XMMatrixTranspose(viewProj));
        bcb.camPos  = { camPos.x, camPos.y, camPos.z, 0.0f };
        bcb.params  = { 1.0f, m_time, softFadeDist, 0.0f };
        bcb.params2 = params2;
        bcb.pad0    = { 0, 0, 0, 0 };

        cmd->SetGraphicsRoot32BitConstants(0, 32, &bcb, 0);
        cmd->IASetVertexBuffers(0, 1, &m_beamVbView);
        cmd->SetPipelineState(m_psoBeam.Get());
        cmd->DrawInstanced(6, static_cast<u32>(m_gpuBeams.size()), 0, 0);
    }

    // ===== トレイル（軌跡リボン）=====
    if (!m_trailVerts.empty())
    {
        const UINT regionBytes  = kMaxTrailVerts * sizeof(TrailVtx);
        const UINT regionOffset = m_frameIdx * regionBytes;

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        ThrowIfFailed(m_trailBuffer->Map(0, &readRange, &mapped));
        memcpy(static_cast<u8*>(mapped) + regionOffset, m_trailVerts.data(),
               m_trailVerts.size() * sizeof(TrailVtx));
        m_trailBuffer->Unmap(0, nullptr);

        m_trailVbView.BufferLocation = m_trailBuffer->GetGPUVirtualAddress() + regionOffset;
        m_trailVbView.SizeInBytes    = regionBytes;

        struct TrailCB
        {
            XMFLOAT4X4 viewProj;
            XMFLOAT4   camRight;
            XMFLOAT4   camUp;
            XMFLOAT4   params;
            XMFLOAT4   params2;
        } tcb;
        XMStoreFloat4x4(&tcb.viewProj, XMMatrixTranspose(viewProj));
        tcb.camRight = { camRight.x, camRight.y, camRight.z, 0.0f };
        tcb.camUp    = { camUp.x,    camUp.y,    camUp.z,    0.0f };
        tcb.params   = { 1.0f, 2.2f, m_time, softFadeDist };
        tcb.params2  = params2;

        cmd->SetGraphicsRoot32BitConstants(0, 32, &tcb, 0);
        cmd->IASetVertexBuffers(0, 1, &m_trailVbView);
        if (m_trailAddVerts > 0)
        {
            cmd->SetPipelineState(m_psoTrailAdd.Get());
            cmd->DrawInstanced(m_trailAddVerts, 1, 0, 0);
        }
        const u32 alphaVerts = static_cast<u32>(m_trailVerts.size()) - m_trailAddVerts;
        if (alphaVerts > 0)
        {
            cmd->SetPipelineState(m_psoTrailAlpha.Get());
            cmd->DrawInstanced(alphaVerts, 1, m_trailAddVerts, 0);
        }
    }
}

void ParticleSystem::RenderDistortion(ID3D12GraphicsCommandList* cmd, XMMATRIX viewProj,
                                      XMFLOAT3 camRight, XMFLOAT3 camUp)
{
    // Render() が同一フレームで m_gpu をアップロード済みであることが前提
    // （歪み粒子は m_gpu の末尾 m_distortCount 個に詰まっている）。
    if (!m_initialized || m_distortCount == 0) return;

    struct CamCB
    {
        XMFLOAT4X4 viewProj;
        XMFLOAT4   camRight;
        XMFLOAT4   camUp;
        XMFLOAT4   params;
        XMFLOAT4   params2;
    } cb;
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewProj));
    cb.camRight = { camRight.x, camRight.y, camRight.z, 0.0f };
    cb.camUp    = { camUp.x,    camUp.y,    camUp.z,    0.0f };
    cb.params   = { 1.0f, 2.2f, m_time, 0.5f };
    cb.params2  = { m_projA, m_projB, m_hasDepth ? m_invRTW : 0.0f, m_invRTH };

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    if (m_hasDepth) cmd->SetGraphicsRootDescriptorTable(1, m_depthSrv);
    cmd->SetGraphicsRoot32BitConstants(0, 32, &cb, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vbView);
    cmd->SetPipelineState(m_psoDistort.Get());
    cmd->DrawInstanced(6, m_distortCount, 0, m_additiveCount + m_alphaCount);
}

} // namespace dx12e
