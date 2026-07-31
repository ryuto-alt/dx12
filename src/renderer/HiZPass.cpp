#include "renderer/HiZPass.h"

#include "graphics/GraphicsDevice.h"
#include "graphics/DescriptorHeap.h"
#include "resource/ShaderCompiler.h"
#include "core/Assert.h"
#include "core/Logger.h"

#include <algorithm>

namespace dx12e
{
namespace
{
// HLSL の cbuffer HiZBuildCB(b0) とバイト単位で一致させること。
struct HiZBuildCB
{
    u32 dstW, dstH;
    u32 srcW, srcH;
};
static_assert(sizeof(HiZBuildCB) == 16, "HiZBuildCB layout mismatch with HiZBuild.hlsl");
constexpr u32 kBuildCBNum32 = sizeof(HiZBuildCB) / 4;

constexpr DXGI_FORMAT kHiZFormat  = DXGI_FORMAT_R32_FLOAT;
constexpr u32         kThreadsXY  = 8;    // HLSL の [numthreads(8,8,1)] と一致させること

// 読み取り状態。カリング compute(non-pixel) とデバッグ表示(pixel) の両方から読むので両方立てる。
// 「書き込みビットが無ければ読み取りビットは何本でも立ててよい」(D3D12 ResourceBarrier 仕様)。
constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

void HiZPass::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap,
                         u32 width, u32 height, const std::wstring& shaderDir)
{
    m_srvHeap   = srvHeap;
    m_shaderDir = shaderDir;

    auto* dev = device.GetDevice();

    // --- RootSignature: b0(32bit定数 x4) + t0(SRVテーブル) + u0..u1(UAVテーブル) ---
    // DWORD: 4 + 1 + 1 = 6 / 64。
    // ★UAV をルートディスクリプタにはできない(バッファ限定)のでテーブルを使う。
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0;   // t0

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors     = 2;
        uavRange.BaseShaderRegister = 0;   // u0, u1

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;   // b0
        params[0].Constants.Num32BitValues = kBuildCBNum32;
        params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(params);
        desc.pParameters   = params;

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &serialized, &error));
        ThrowIfFailed(dev->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)));
    }

    RecreatePipelines(device);
    Resize(device, width, height);
}

void HiZPass::RecreatePipelines(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();
    auto makeCS = [&](const std::wstring& cso, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
    {
        auto bc = ShaderCompiler::LoadFromFile(m_shaderDir + cso);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS = { bc.GetData(), bc.GetSize() };
        ThrowIfFailed(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    makeCS(L"HiZBuildCopy_CS.cso",   m_psoCopy);
    makeCS(L"HiZBuildReduce_CS.cso", m_psoReduce);
}

void HiZPass::ReleaseDescriptors()
{
    if (m_srvHeap && m_block != 0xFFFFFFFFu && m_blockSize > 0)
        m_srvHeap->FreeBlock(m_block, m_blockSize);
    m_block     = 0xFFFFFFFFu;
    m_blockSize = 0;
    m_srvIndex  = 0xFFFFFFFFu;
}

void HiZPass::Resize(GraphicsDevice& device, u32 width, u32 height)
{
    const u32 w = (width  > 0) ? width  : 1;
    const u32 h = (height > 0) ? height : 1;
    if (w == m_width && h == m_height && m_hzb) return;

    m_width  = w;
    m_height = h;
    ReleaseDescriptors();
    m_hzb.Reset();
    CreatePyramid(device);
}

void HiZPass::CreatePyramid(GraphicsDevice& device)
{
    auto* dev = device.GetDevice();

    // ミップ寸法を先に決める。各段は floor(前段/2) で、1x1 まで落とす。
    // ★floor なので前段が奇数のとき 1 列/1 行があぶれる。その取りこぼしは
    //   シェーダ側(HiZBuild.hlsl)が端のスレッドで 3x3 に広げて畳む。
    //   ここで ceil にすると「入力に対応しないテクセル」が生まれて逆に壊れる。
    m_mipSizes.clear();
    {
        u32 mw = m_width, mh = m_height;
        for (;;)
        {
            m_mipSizes.push_back({mw, mh});
            if (mw == 1 && mh == 1) break;
            mw = (std::max)(1u, mw / 2);
            mh = (std::max)(1u, mh / 2);
        }
    }
    const u32 mipCount = static_cast<u32>(m_mipSizes.size());

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = m_width;
    d.Height           = m_height;
    d.DepthOrArraySize = 1;
    d.MipLevels        = static_cast<u16>(mipCount);
    d.Format           = kHiZFormat;
    d.SampleDesc       = {1, 0};
    d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_hzb)));
    m_hzb->SetName(L"HiZPyramid");
    m_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ディスクリプタ: [0]=mip0 の UAV(複製) / [1+m]=mip m の UAV / 末尾=全ミップ SRV。
    // ★複製を先頭に置くのは、ディスパッチ m のテーブル基点を block+m にするだけで
    //   u0=mip(m-1) / u1=mip m が揃うようにするため(m=0 は u1=mip0 が出力)。
    m_blockSize = mipCount + 2;
    m_block     = m_srvHeap->AllocateBlock(m_blockSize);
    if (m_block == DescriptorHeap::kInvalidIndex)
    {
        Logger::Error("HiZPass: ディスクリプタブロックの確保に失敗した（{} 本）", m_blockSize);
        m_blockSize = 0;
        m_hzb.Reset();
        return;
    }

    auto makeUAV = [&](u32 mip, u32 dstIdx)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format               = kHiZFormat;
        u.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Texture2D.MipSlice   = mip;
        u.Texture2D.PlaneSlice = 0;
        // ★宛先へ直接作る。シェーダ可視ヒープからのディスクリプタコピーは不正
        //   (CPU write-only なのでコピー元として読めない。D3D12 デバッグレイヤ id=654)。
        dev->CreateUnorderedAccessView(m_hzb.Get(), nullptr, &u,
                                       m_srvHeap->GetCpuHandle(dstIdx));
    };

    makeUAV(0, m_block);                       // [0] mip0 の複製
    for (u32 m = 0; m < mipCount; ++m)
        makeUAV(m, m_block + 1 + m);           // [1+m] mip m

    m_srvIndex = m_block + mipCount + 1;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format                    = kHiZFormat;
        s.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MostDetailedMip = 0;
        s.Texture2D.MipLevels       = mipCount;   // 全ミップを公開する
        dev->CreateShaderResourceView(m_hzb.Get(), &s, m_srvHeap->GetCpuHandle(m_srvIndex));
    }

    Logger::Info("Hi-Z ピラミッド: {}x{} / {} ミップ", m_width, m_height, mipCount);
}

void HiZPass::Build(ID3D12GraphicsCommandList* cmd, D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu)
{
    if (!IsReady() || !cmd) return;

    // 前フレーム末に読み取り状態で終わっているので UAV へ戻す。
    if (m_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_hzb.Get();
        b.Transition.StateBefore = m_state;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    cmd->SetComputeRootSignature(m_rootSig.Get());
    cmd->SetComputeRootDescriptorTable(1, depthSrvGpu);

    const u32 mipCount = static_cast<u32>(m_mipSizes.size());

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = m_hzb.Get();

    for (u32 m = 0; m < mipCount; ++m)
    {
        const MipSize dst = m_mipSizes[m];
        const MipSize src = (m == 0) ? MipSize{m_width, m_height} : m_mipSizes[m - 1];

        HiZBuildCB cb{dst.w, dst.h, src.w, src.h};
        cmd->SetComputeRoot32BitConstants(0, kBuildCBNum32, &cb, 0);
        cmd->SetComputeRootDescriptorTable(2, m_srvHeap->GetGpuHandle(m_block + m));
        cmd->SetPipelineState((m == 0) ? m_psoCopy.Get() : m_psoReduce.Get());
        cmd->Dispatch((dst.w + kThreadsXY - 1) / kThreadsXY,
                      (dst.h + kThreadsXY - 1) / kThreadsXY, 1);

        // ★段の間に UAV バリアが要る。入れ忘れると次の段が書き途中のミップを読んで、
        //   ピラミッドが毎フレーム砂嵐になる(Hi-Z 実装で最も多いバグの一つ)。
        if (m + 1 < mipCount) cmd->ResourceBarrier(1, &uavBarrier);
    }

    // カリング compute / デバッグ表示から読めるようにする。
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_hzb.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter  = kReadState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        m_state = kReadState;
    }
}

void HiZPass::Shutdown()
{
    ReleaseDescriptors();
    m_hzb.Reset();
    m_psoCopy.Reset();
    m_psoReduce.Reset();
    m_rootSig.Reset();
    m_srvHeap = nullptr;
    m_mipSizes.clear();
    m_width = m_height = 0;
}

} // namespace dx12e
