#include "renderer/VolumetricFogPass.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/Buffer.h"
#include "graphics/FrameResources.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>
#include <cmath>

namespace dx12e
{
using namespace DirectX;

namespace
{

// shaders/fog/FogCommon.hlsli の cbuffer FogParams(b0) とバイト単位で一致させること。
struct FogParamsCB
{
    XMFLOAT4X4 invView;            //   0
    XMFLOAT4X4 prevViewProj;       //  64
    XMFLOAT4X4 cascadeViewProj[4]; // 128
    XMFLOAT4   cascadeSplits;      // 384
    XMFLOAT4   shadowParams;       // 400
    XMFLOAT3   cameraPos; float fogFar;        // 416
    XMFLOAT3   sunDir;    float depthPower;    // 432
    XMFLOAT3   sunColor;  float anisotropy;    // 448
    XMFLOAT3   albedo;    float density;       // 464
    XMFLOAT3   ambient;   float heightFalloff; // 480
    XMFLOAT4   jitter;        // 496
    XMFLOAT4   misc;          // 512
    XMFLOAT4   projParams;    // 528
    XMFLOAT4   clusterParams; // 544
    XMFLOAT4   clusterGrid;   // 560
    XMFLOAT4   rect;          // 576
    XMFLOAT4   depthLin;      // 592
    XMFLOAT4   extend;        // 608
};
static_assert(sizeof(FogParamsCB) == 624, "FogParamsCB must match shaders/fog/FogCommon.hlsli");
static_assert(sizeof(FogParamsCB) % 16 == 0, "FogParamsCB must be 16B aligned");

constexpr DXGI_FORMAT kVolumeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// SRV ブロック（8 本）。compute のレンジ (t1,t2) を組み替えるためペアを複製してある。
//   [0]=media [1]=scatter0 | [2]=media [3]=scatter1 | [4]=scatter0 [5]=scatter1 | [6]=integrated [7]=integrated
constexpr u32 kSrvBlockSize = 8;
// UAV ブロック（4 本）: [0]=media [1]=scatter0 [2]=scatter1 [3]=integrated
constexpr u32 kUavBlockSize = 4;

// Halton 列（サブ froxel ジッタ用）。base 2/3/5 の 16 周期。
float Halton(u32 index, u32 base)
{
    float f = 1.0f, r = 0.0f;
    u32   i = index;
    while (i > 0)
    {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

} // namespace

void VolumetricFogPass::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                                   const std::wstring& shaderDir)
{
    auto* dev  = device.GetDevice();
    m_srvHeap  = srvHeap;
    m_shaderDir = shaderDir;

    // --- compute ルートシグネチャ ---
    //   [0] CBV b0        : FogParams
    //   [1] SRV t0        : CSM シャドウマップ（Texture2DArray）
    //   [2] SRV t1,t2     : media / history（scatter）または scatter / 未使用（integrate）
    //   [3] UAV u0        : 出力ボリューム（パスごとに差し替え）
    //   [4] SRV t3,t4,t5  : クラスタライト / インデックスリスト / カウント（Step F5）
    //   [5] SRV t6,t7     : スポット影配列 / ポイント影キューブ配列（Step F5）
    //   s0 = 影用の比較サンプラ（RootSignature.cpp の s1 と同一設定）/ s1 = LINEAR CLAMP
    //   DWORD: 2 + 1*5 = 7 / 64
    {
        D3D12_DESCRIPTOR_RANGE ranges[5]{};
        auto setRange = [](D3D12_DESCRIPTOR_RANGE& r, u32 baseReg, u32 count)
        {
            r.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            r.NumDescriptors     = count;
            r.BaseShaderRegister = baseReg;
            r.RegisterSpace      = 0;
            r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        };
        setRange(ranges[0], 0, 1);   // t0
        setRange(ranges[1], 1, 2);   // t1,t2
        setRange(ranges[3], 3, 3);   // t3,t4,t5
        setRange(ranges[4], 6, 2);   // t6,t7

        ranges[2].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;   // u0
        ranges[2].NumDescriptors     = 1;
        ranges[2].BaseShaderRegister = 0;
        ranges[2].RegisterSpace      = 0;
        ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[6]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;   // b0
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        for (u32 i = 0; i < 5; ++i)
        {
            params[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges   = &ranges[i];
            params[1 + i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_STATIC_SAMPLER_DESC samp[2]{};
        // s0 - 影の比較サンプラ（RootSignature.cpp の s1 を複製）
        samp[0].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samp[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samp[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samp[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samp[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samp[0].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samp[0].MaxLOD           = D3D12_FLOAT32_MAX;
        samp[0].ShaderRegister   = 0;
        samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // s1 - LINEAR CLAMP（3D ボリュームのトライリニアサンプル）
        samp[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samp[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samp[1].MaxLOD           = D3D12_FLOAT32_MAX;
        samp[1].ShaderRegister   = 1;
        samp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = _countof(params);
        desc.pParameters       = params;
        desc.NumStaticSamplers = _countof(samp);
        desc.pStaticSamplers   = samp;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_computeRS)));
    }

    // --- 合成ルートシグネチャ（ContactShadowPass と同形）---
    //   [0] CBV b0     : FogParams（ViewZToFroxelW 等を共用するので同じ CB）
    //   [1] SRV t0     : 深度（R32_FLOAT）
    //   [2] SRV t1     : 積分済みボリューム（Texture3D）
    //   s1 = LINEAR CLAMP
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        for (u32 i = 0; i < 2; ++i)
        {
            ranges[i].RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors     = 1;
            ranges[i].BaseShaderRegister = i;   // t0 / t1
            ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
        for (u32 i = 0; i < 2; ++i)
        {
            params[1 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[1 + i].DescriptorTable.pDescriptorRanges   = &ranges[i];
            params[1 + i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        samp.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderRegister   = 1;   // s1
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters     = _countof(params);
        desc.pParameters       = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                   | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_compositeRS)));
    }

    RecreatePipelines(device);

    // ★ディスクリプタは「断片化する前」に確保しておく（AllocateBlock は連続範囲が取れないと throw する）。
    //   ビューの生成はボリュームを遅延確保する EnsureVolumes 側で行う。
    m_srvBlock = m_srvHeap->AllocateBlock(kSrvBlockSize);
    m_uavBlock = m_srvHeap->AllocateBlock(kUavBlockSize);

    m_fogCB = std::make_unique<ConstantBuffer>();
    m_fogCB->Initialize(device, sizeof(FogParamsCB), FrameResources::kFrameCount);

    Logger::Info("VolumetricFogPass initialized (froxel {}x{}x{}, volumes are allocated on first use)",
                 kFroxelX, kFroxelY, kFroxelZ);
}

void VolumetricFogPass::Shutdown()
{
    if (m_srvHeap)
    {
        if (m_srvBlock != DescriptorHeap::kInvalidIndex) m_srvHeap->FreeBlock(m_srvBlock, kSrvBlockSize);
        if (m_uavBlock != DescriptorHeap::kInvalidIndex) m_srvHeap->FreeBlock(m_uavBlock, kUavBlockSize);
    }
    m_srvBlock = DescriptorHeap::kInvalidIndex;
    m_uavBlock = DescriptorHeap::kInvalidIndex;
    m_media.Reset();
    m_scatter[0].Reset();
    m_scatter[1].Reset();
    m_integrated.Reset();
    m_fogCB.reset();
    m_psoInject.Reset();
    m_psoScatter.Reset();
    m_psoIntegrate.Reset();
    m_psoComposite.Reset();
    m_computeRS.Reset();
    m_compositeRS.Reset();
    m_srvHeap = nullptr;
}

void VolumetricFogPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    auto makeCS = [&](const std::wstring& cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_computeRS.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"FogInject_CS.cso",    m_psoInject);
    makeCS(L"FogScatter_CS.cso",   m_psoScatter);
    makeCS(L"FogIntegrate_CS.cso", m_psoIntegrate);

    // 合成 PSO。ブレンド: dst.rgb = src.rgb + dst.rgb * src.a / dst.a はそのまま保存。
    {
        auto vs = ShaderCompiler::LoadFromFile(m_shaderDir + L"FogComposite_VS.cso");
        auto ps = ShaderCompiler::LoadFromFile(m_shaderDir + L"FogComposite_PS.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_compositeRS.Get();
        pso.VS = { vs.GetData(), vs.GetSize() };
        pso.PS = { ps.GetData(), ps.GetSize() };
        pso.InputLayout = { nullptr, 0 };

        pso.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;

        auto& rt = pso.BlendState.RenderTarget[0];
        rt.BlendEnable           = TRUE;
        rt.SrcBlend              = D3D12_BLEND_ONE;
        rt.DestBlend             = D3D12_BLEND_SRC_ALPHA;
        rt.BlendOp               = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha         = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha        = D3D12_BLEND_ONE;
        rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState.DepthEnable   = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;

        pso.SampleMask            = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets      = 1;
        pso.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;   // kSceneColorFormat
        pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pso.SampleDesc            = { 1, 0 };

        ThrowIfFailed(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoComposite)));
    }
}

bool VolumetricFogPass::EnsureVolumes(GraphicsDevice& device)
{
    if (m_integrated) return true;
    if (m_srvBlock == DescriptorHeap::kInvalidIndex || m_uavBlock == DescriptorHeap::kInvalidIndex)
        return false;

    auto* dev = device.GetDevice();

    // ★このリポジトリに Texture3D の前例は無い（Texture / RenderTarget はどちらも 2D 決め打ち）。
    //   汎用クラスは作らず、ここのローカルラムダで作る（IBLBaker の makeTex と同形で
    //   Dimension と DepthOrArraySize だけが違う）。
    auto makeVolume = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& out, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.Width            = kFroxelX;
        desc.Height           = kFroxelY;
        desc.DepthOrArraySize = static_cast<UINT16>(kFroxelZ);
        desc.MipLevels        = 1;
        desc.Format           = kVolumeFormat;
        desc.SampleDesc       = {1, 0};
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
        out->SetName(name);
    };

    makeVolume(m_media,       L"FogMediaVolume");
    makeVolume(m_scatter[0],  L"FogScatterVolume0");
    makeVolume(m_scatter[1],  L"FogScatterVolume1");
    makeVolume(m_integrated,  L"FogIntegratedVolume");

    m_mediaState      = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_scatterState[0] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_scatterState[1] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_integratedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // SRV（Texture3D）
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format                    = kVolumeFormat;
        s.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE3D;
        s.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture3D.MostDetailedMip = 0;
        s.Texture3D.MipLevels       = 1;
        s.Texture3D.ResourceMinLODClamp = 0.0f;

        ID3D12Resource* order[kSrvBlockSize] = {
            m_media.Get(),      m_scatter[0].Get(),
            m_media.Get(),      m_scatter[1].Get(),
            m_scatter[0].Get(), m_scatter[1].Get(),
            m_integrated.Get(), m_integrated.Get(),
        };
        for (u32 i = 0; i < kSrvBlockSize; ++i)
            dev->CreateShaderResourceView(order[i], &s, m_srvHeap->GetCpuHandle(m_srvBlock + i));
    }

    // UAV（Texture3D）
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format                 = kVolumeFormat;
        u.ViewDimension          = D3D12_UAV_DIMENSION_TEXTURE3D;
        u.Texture3D.MipSlice     = 0;
        u.Texture3D.FirstWSlice  = 0;
        u.Texture3D.WSize        = kFroxelZ;

        ID3D12Resource* order[kUavBlockSize] = {
            m_media.Get(), m_scatter[0].Get(), m_scatter[1].Get(), m_integrated.Get(),
        };
        for (u32 i = 0; i < kUavBlockSize; ++i)
            dev->CreateUnorderedAccessView(order[i], nullptr, &u,
                                           m_srvHeap->GetCpuHandle(m_uavBlock + i));
    }

    // 未初期化の履歴を読まないように、最初の 2 フレームは時間再投影を切る
    // （ピンポンの両スロットが 1 回ずつ書かれるまで）。
    m_warmup = 2;

    const double mb = 4.0 * static_cast<double>(kFroxelX) * kFroxelY * kFroxelZ * 8.0 / (1024.0 * 1024.0);
    Logger::Info("VolumetricFogPass: froxel ボリュームを確保しました（{}x{}x{} x4 = {:.1f} MB）",
                 kFroxelX, kFroxelY, kFroxelZ, mb);
    return true;
}

void VolumetricFogPass::Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                                   D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next)
{
    if (!res || state == next) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter  = next;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
    state = next;
}

void VolumetricFogPass::BuildVolumes(ID3D12GraphicsCommandList* cmd, GraphicsDevice& device,
                                     const VolumetricFogSettings& s, const ViewParams& v,
                                     u32 frameIndex)
{
    if (!cmd || !IsReady()) return;
    if (!EnsureVolumes(device)) return;

    const u32 cur  = m_cur;
    const u32 prev = 1u - m_cur;

    // --- 定数バッファを埋める ---
    FogParamsCB cb{};
    // HLSL は mul(row, mat) 規約なので全行列を転置して入れる。
    // ★view の逆行列は「回転+平行移動だけ」という前提の transpose 近似ではなく、
    //   ちゃんと XMMatrixInverse で作る（スケール付きビュー行列でも壊れない）。
    XMStoreFloat4x4(&cb.invView, XMMatrixTranspose(XMMatrixInverse(nullptr, v.view)));
    XMStoreFloat4x4(&cb.prevViewProj, XMMatrixTranspose(v.prevViewProj));
    for (u32 i = 0; i < 4; ++i)
    {
        XMMATRIX c = v.cascadeViewProj ? XMLoadFloat4x4(&v.cascadeViewProj[i]) : XMMatrixIdentity();
        XMStoreFloat4x4(&cb.cascadeViewProj[i], XMMatrixTranspose(c));
    }
    cb.cascadeSplits = v.cascadeSplits;
    cb.shadowParams  = v.shadowParams;

    const float fogFar = (std::max)(s.distance, 1.0f);
    cb.cameraPos  = v.cameraPos;
    cb.fogFar     = fogFar;
    cb.sunDir     = v.sunDir;
    cb.depthPower = (std::min)((std::max)(s.depthDistribution, 1.0f), 4.0f);
    cb.sunColor   = v.sunColor;
    cb.anisotropy = (std::min)((std::max)(s.anisotropy, -0.9f), 0.9f);
    cb.albedo     = s.albedo;
    cb.density    = (std::max)(s.density, 0.0f);
    cb.ambient    = s.ambient;
    cb.heightFalloff = (std::max)(s.heightFalloff, 0.0f);

    // サブ froxel ジッタ（Halton(2,3,5) 16 周期）。
    // ★時間再投影が無効なときはジッタも 0 にする。回しっぱなしだとフォグがチラつくだけになる。
    const bool useTemporal = s.temporal && (m_warmup == 0);
    if (s.temporal)
    {
        const u32 j = static_cast<u32>(m_frameCounter % 16) + 1;
        cb.jitter = {Halton(j, 2) - 0.5f, Halton(j, 3) - 0.5f, Halton(j, 5) - 0.5f, 1.0f};
    }
    else
    {
        cb.jitter = {0.0f, 0.0f, 0.0f, 1.0f};
    }
    // .w = 現フレームの比率。1.0 = 履歴を使わない。
    cb.jitter.w = useTemporal
                ? (std::min)((std::max)(s.temporalBlend, 0.01f), 1.0f)
                : 1.0f;

    cb.misc = {s.heightRef,
               s.extendBeyondRange ? 1.0f : 0.0f,
               static_cast<float>(s.debugMode),
               (std::max)(s.sunIntensity, 0.0f)};
    cb.projParams = {1.0f / ((v.proj11 != 0.0f) ? v.proj11 : 1.0f),
                     1.0f / ((v.proj22 != 0.0f) ? v.proj22 : 1.0f), 0.0f, 0.0f};
    cb.clusterParams = v.clusterParams;
    cb.clusterGrid   = v.clusterGrid;
    // クラスタライトの散乱を使わない設定なら .w を落として compute 側の分岐を切る。
    if (!s.lightScattering) cb.clusterGrid.w = 0.0f;
    cb.rect     = {static_cast<float>(v.vpLeft), static_cast<float>(v.vpTop),
                   static_cast<float>(v.vpW),    static_cast<float>(v.vpH)};
    cb.depthLin = {v.nearZ, v.farZ, 0.0f, 0.0f};
    // フォグ距離の外側を延長する解析フォグ（§2.1.4）。
    // 均質媒質の閉形式は ∫S·T = Lin·albedo·(1-T) なので、gExtend.xyz には
    // 「等方近似した in-scattered radiance × 散乱アルベド」を入れる（方向依存は捨てる。
    // 遠景なので位相関数の差は見えない）。σ_t は高さ減衰を無視した基準密度を使う。
    if (s.extendBeyondRange)
    {
        const float iso = 1.0f / (4.0f * 3.14159265359f);
        const float sx  = (s.ambient.x + v.sunColor.x * cb.misc.w) * iso * s.albedo.x;
        const float sy  = (s.ambient.y + v.sunColor.y * cb.misc.w) * iso * s.albedo.y;
        const float sz  = (s.ambient.z + v.sunColor.z * cb.misc.w) * iso * s.albedo.z;
        cb.extend = {sx, sy, sz, cb.density};
    }
    else
    {
        cb.extend = {0.0f, 0.0f, 0.0f, 0.0f};
    }

    m_fogCB->Update(&cb, sizeof(cb), frameIndex);

    auto uavBarrier = [&](ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        cmd->ResourceBarrier(1, &b);
    };

    cmd->SetComputeRootSignature(m_computeRS.Get());
    cmd->SetComputeRootConstantBufferView(0, m_fogCB->GetGpuAddress(frameIndex));

    constexpr u32 kGx = (kFroxelX + 7) / 8;   // 20
    constexpr u32 kGy = (kFroxelY + 7) / 8;   // 12（90 は 8 の倍数ではないのでシェーダ側で境界チェック）

    // ---- ① 注入 ----
    Transition(cmd, m_media.Get(), m_mediaState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetComputeRootDescriptorTable(3, m_srvHeap->GetGpuHandle(m_uavBlock + 0));
    cmd->SetPipelineState(m_psoInject.Get());
    cmd->Dispatch(kGx, kGy, kFroxelZ);
    uavBarrier(m_media.Get());

    // ---- ② 散乱 + 時間再投影 ----
    Transition(cmd, m_media.Get(), m_mediaState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, m_scatter[prev].Get(), m_scatterState[prev],
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, m_scatter[cur].Get(), m_scatterState[cur],
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (v.csmSrv.ptr) cmd->SetComputeRootDescriptorTable(1, v.csmSrv);
    // SRV レンジ (t1,t2) は「media, history」のペアを指す必要がある。
    // ブロックは [0]=media [1]=scatter0 / [2]=media [3]=scatter1 の並びなので、
    // 履歴が scatter0 なら +0、scatter1 なら +2。
    cmd->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_srvBlock + (prev == 0 ? 0u : 2u)));
    cmd->SetComputeRootDescriptorTable(3, m_srvHeap->GetGpuHandle(m_uavBlock + 1 + cur));
    if (v.clusterSrv.ptr)        cmd->SetComputeRootDescriptorTable(4, v.clusterSrv);
    if (v.punctualShadowSrv.ptr) cmd->SetComputeRootDescriptorTable(5, v.punctualShadowSrv);
    cmd->SetPipelineState(m_psoScatter.Get());
    cmd->Dispatch(kGx, kGy, kFroxelZ);
    uavBarrier(m_scatter[cur].Get());

    // ---- ③ Z 積分 ----
    Transition(cmd, m_scatter[cur].Get(), m_scatterState[cur],
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, m_integrated.Get(), m_integratedState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // レンジ (t1,t2) の t1 に scatter[cur] が来るペア: [4]=scatter0 [5]=scatter1。
    cmd->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_srvBlock + 4 + cur));
    cmd->SetComputeRootDescriptorTable(3, m_srvHeap->GetGpuHandle(m_uavBlock + 3));
    cmd->SetPipelineState(m_psoIntegrate.Get());
    cmd->Dispatch(kGx, kGy, 1);

    Transition(cmd, m_integrated.Get(), m_integratedState,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_cur = prev;
    ++m_frameCounter;
    if (m_warmup > 0) --m_warmup;
}

void VolumetricFogPass::Composite(ID3D12GraphicsCommandList* cmd,
                                  D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu,
                                  u32 vpLeft, u32 vpTop, u32 vpW, u32 vpH, u32 frameIndex)
{
    if (!cmd || !m_psoComposite || !m_integrated) return;
    if (vpW == 0 || vpH == 0) return;

    // ★ビューポートをシーンのサブ矩形に合わせる。こうすると FSTriVS の uv がそのまま
    //   ビューポートローカル UV になり、RT 座標との差分補正が要らない
    //   （＝「エディタでだけフォグが横にずれる」罠が原理的に起きない）。
    D3D12_VIEWPORT vp{static_cast<float>(vpLeft), static_cast<float>(vpTop),
                      static_cast<float>(vpW),    static_cast<float>(vpH), 0.0f, 1.0f};
    D3D12_RECT     sc{static_cast<LONG>(vpLeft), static_cast<LONG>(vpTop),
                      static_cast<LONG>(vpLeft + vpW), static_cast<LONG>(vpTop + vpH)};

    cmd->SetGraphicsRootSignature(m_compositeRS.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->SetPipelineState(m_psoComposite.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_fogCB->GetGpuAddress(frameIndex));
    cmd->SetGraphicsRootDescriptorTable(1, depthSrvGpu);
    cmd->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_srvBlock + 6));
    cmd->DrawInstanced(3, 1, 0, 0);
}

} // namespace dx12e
