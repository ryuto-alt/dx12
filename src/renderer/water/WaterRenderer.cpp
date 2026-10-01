#include "renderer/water/WaterRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Assert.h"
#include "core/Logger.h"
#include "graphics/CommandList.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/PipelineState.h"
#include "graphics/RenderTarget.h"
#include "renderer/Frustum.h"
#include "renderer/RenderPass.h"
#include "resource/ShaderCompiler.h"

namespace dx12e::water
{

namespace
{
constexpr u32 kGridVerts = 2 * kGridHalf + 1;          // 97
constexpr u32 kHoleVariants = 4;                       // (dx, dz) ∈ {0,1}^2
constexpr u32 kIndexBuffers = 1 + kHoleVariants;       // [0]=全面（レベル 0）/ [1..4]=穴あきリング
constexpr u32 kDrawConstDwords = 24;
constexpr u32 kTimerScopes = 4;                        // 0=全体 1=コピー 2=水中 3=水面メッシュ
constexpr u32 kQueryFrames = 3;
constexpr u32 kRetireFrames = 4;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}
}   // namespace

struct WaterRenderer::Impl
{
    Deps d;
    ID3D12Device* dev = nullptr;
    u32 frames = 3;
    bool physical = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoMain, psoUnder;

    // 静的リソース（初回 BeginFrame で cmd へアップロード）
    bool staticReady = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> vb, ib, detailTex;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv[kIndexBuffers]{};
    u32 idxCount[kIndexBuffers] = {};
    u64 staticBytes = 0;

    // フレームスロットごとのデータ（UPLOAD・永続マップ）
    Microsoft::WRL::ComPtr<ID3D12Resource> data[3];
    u8* dataMapped[3] = {nullptr, nullptr, nullptr};

    // 不透明シーンのコピー
    Microsoft::WRL::ComPtr<ID3D12Resource> colorCopy, depthCopy;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES depthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    u32 copyW = 0, copyH = 0;
    bool sizeMismatchWarned = false;

    // ディスクリプタ: 4 連続 x フレームスロット。[0]=ディテール [1]=色 [2]=深度 [3]=データ
    u32 block = DescriptorHeap::kInvalidIndex;

    struct Retired { Microsoft::WRL::ComPtr<ID3D12Resource> res; u64 frame; };
    std::vector<Retired> retired;

    // 今フレームの計画
    struct DrawRec { u32 bodySlot; RingPlan ring; };
    std::vector<DrawRec> draws;
    u64 lastFrameId = ~0ull;
    u32 drawnBodies = 0;
    DirectX::XMFLOAT4X4 viewProjJ{};
    u32 frameSlot = 0;
    u64 frameCounter = 0;

    // GPU タイマー
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> qheap;
    Microsoft::WRL::ComPtr<ID3D12Resource> qread;
    u64 freq = 0;
    u32 wrote[kQueryFrames] = {0, 0, 0};

    void TimerBegin(ID3D12GraphicsCommandList* cmd, u32 slot, u32 s)
    {
        if (!qheap) return;
        cmd->EndQuery(qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (slot * kTimerScopes + s) * 2);
    }
    void TimerEnd(ID3D12GraphicsCommandList* cmd, u32 slot, u32 s)
    {
        if (!qheap) return;
        const u32 q = (slot * kTimerScopes + s) * 2;
        cmd->EndQuery(qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
        cmd->ResolveQueryData(qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 2, qread.Get(), static_cast<u64>(q) * sizeof(u64));
        wrote[slot] |= (1u << s);
    }

    bool CreateRootSignature();
    bool BuildPsos();
    bool CreateStatic(ID3D12GraphicsCommandList* cmd);
    bool EnsureCopies(u32 w, u32 h);
    void WriteDescriptors(u32 slot);
    Microsoft::WRL::ComPtr<ID3D12Resource> UploadBuffer(u64 bytes);
    Microsoft::WRL::ComPtr<ID3D12Resource> DefaultBuffer(ID3D12GraphicsCommandList* cmd, const void* src, u64 bytes,
                                                        D3D12_RESOURCE_STATES finalState, const wchar_t* name);
    void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> r) { if (r) retired.push_back({std::move(r), frameCounter}); }
};

WaterRenderer::WaterRenderer() = default;
WaterRenderer::~WaterRenderer() { Shutdown(); }

// ---------------------------------------------------------------------------
bool WaterRenderer::Impl::CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE water{}, csm{}, punct{}, ibl{}, cl[3]{};
    water.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; water.NumDescriptors = 4; water.BaseShaderRegister = 0;
    water.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    csm.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; csm.NumDescriptors = 1; csm.BaseShaderRegister = 4;
    csm.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    punct.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; punct.NumDescriptors = 2; punct.BaseShaderRegister = 9;
    punct.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ibl.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ibl.NumDescriptors = 3; ibl.BaseShaderRegister = 5;
    ibl.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // クラスタのテーブル: t13..15（先頭）と DDGI の t22..23（メイン RS ではデカールの t18..21 の後 = 先頭から 7 個目）
    cl[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; cl[0].NumDescriptors = 3; cl[0].BaseShaderRegister = 13;
    cl[0].OffsetInDescriptorsFromTableStart = 0;
    cl[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; cl[1].NumDescriptors = 2; cl[1].BaseShaderRegister = 22;
    cl[1].OffsetInDescriptorsFromTableStart = 7;
    // t30 = DDGI のプローブデータ（GI モード New。メイン RS と同じ 10 本目 = 先頭から 9 個目）。
    //   水のシェーダは Lighting.hlsli を include するので宣言だけは見える（使わなければ DXC が落とす）。
    cl[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; cl[2].NumDescriptors = 1; cl[2].BaseShaderRegister = 30;
    cl[2].OffsetInDescriptorsFromTableStart = 9;

    D3D12_ROOT_PARAMETER p[7]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.ShaderRegister = 0; p[0].Constants.Num32BitValues = kDrawConstDwords;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].Descriptor.ShaderRegister = 1;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    auto table = [&](int i, D3D12_DESCRIPTOR_RANGE* r, u32 n, D3D12_SHADER_VISIBILITY vis)
    {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[i].DescriptorTable.NumDescriptorRanges = n;
        p[i].DescriptorTable.pDescriptorRanges = r;
        p[i].ShaderVisibility = vis;
    };
    table(2, &water, 1, D3D12_SHADER_VISIBILITY_ALL);
    table(3, &csm, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    table(4, &punct, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    table(5, &ibl, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    table(6, cl, 3, D3D12_SHADER_VISIBILITY_PIXEL);

    // 静的サンプラ（メイン RS の s0..s3 / s5 と同じ意味）
    D3D12_STATIC_SAMPLER_DESC s[5]{};
    for (auto& x : s)
    {
        x.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        x.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        x.MaxLOD = D3D12_FLOAT32_MAX;
        x.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    s[0].Filter = D3D12_FILTER_ANISOTROPIC; s[0].MaxAnisotropy = 8;
    s[0].AddressU = s[0].AddressV = s[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP; s[0].ShaderRegister = 0;
    s[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    s[1].AddressU = s[1].AddressV = s[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    s[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; s[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE; s[1].ShaderRegister = 1;
    s[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s[2].AddressU = s[2].AddressV = s[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; s[2].ShaderRegister = 2;
    s[3] = s[2]; s[3].MaxLOD = 0.0f; s[3].ShaderRegister = 3;
    s[4] = s[2]; s[4].MaxLOD = 0.0f; s[4].ShaderRegister = 5;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(p);
    desc.pParameters = p;
    desc.NumStaticSamplers = _countof(s);
    desc.pStaticSamplers = s;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    {
        Logger::Warn("水: ルートシグネチャのシリアライズに失敗（{}）", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    return SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
}

bool WaterRenderer::Impl::BuildPsos()
{
    auto vs = ShaderCompiler::LoadFromFile(d.shaderDir + L"WaterSurface_VS.cso");
    auto ps = ShaderCompiler::LoadFromFile(d.shaderDir + (physical ? L"WaterSurfacePhys_PS.cso" : L"WaterSurface_PS.cso"));
    auto uvs = ShaderCompiler::LoadFromFile(d.shaderDir + L"WaterUnder_VS.cso");
    auto ups = ShaderCompiler::LoadFromFile(d.shaderDir + (physical ? L"WaterUnderPhys_PS.cso" : L"WaterUnder_PS.cso"));
    if (vs.GetSize() == 0 || ps.GetSize() == 0 || uvs.GetSize() == 0 || ups.GetSize() == 0)
    {
        Logger::Warn("水: .cso が読めない（シェーダをビルドし直してください）");
        return false;
    }
    static const D3D12_INPUT_ELEMENT_DESC kLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    {
        PipelineStateBuilder b;
        b.SetRootSignature(rs.Get())
         .SetVertexShader(vs.GetData(), vs.GetSize())
         .SetPixelShader(ps.GetData(), ps.GetSize())
         .SetInputLayout(kLayout, 1)
         .SetRenderTargetFormat(d.sceneColorFormat)
         .SetDepthStencilFormat(DXGI_FORMAT_D32_FLOAT)
         .SetDepthEnabled(true)
         .SetDepthWrite(true)
         .SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL)
         .SetAlphaBlendEnabled(true)
         .SetCullMode(D3D12_CULL_MODE_NONE);
        psoMain = b.Build(*d.gdev);
    }
    {
        PipelineStateBuilder b;
        b.SetRootSignature(rs.Get())
         .SetVertexShader(uvs.GetData(), uvs.GetSize())
         .SetPixelShader(ups.GetData(), ups.GetSize())
         .SetRenderTargetFormat(d.sceneColorFormat)
         .SetDepthStencilFormat(DXGI_FORMAT_UNKNOWN)
         .SetDepthEnabled(false)
         .SetCullMode(D3D12_CULL_MODE_NONE);
        psoUnder = b.Build(*d.gdev);
    }
    return psoMain && psoUnder;
}

Microsoft::WRL::ComPtr<ID3D12Resource> WaterRenderer::Impl::UploadBuffer(u64 bytes)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

Microsoft::WRL::ComPtr<ID3D12Resource> WaterRenderer::Impl::DefaultBuffer(ID3D12GraphicsCommandList* cmd, const void* src, u64 bytes,
                                                                          D3D12_RESOURCE_STATES finalState, const wchar_t* name)
{
    Microsoft::WRL::ComPtr<ID3D12Resource> up = UploadBuffer(bytes);
    if (!up) return nullptr;
    void* m = nullptr; D3D12_RANGE none{0, 0};
    up->Map(0, &none, &m);
    std::memcpy(m, src, static_cast<size_t>(bytes));
    up->Unmap(0, nullptr);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc = {1, 0}; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(name);
    cmd->CopyBufferRegion(r.Get(), 0, up.Get(), 0, bytes);
    const auto b = Transition(r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    cmd->ResourceBarrier(1, &b);
    Retire(std::move(up));
    staticBytes += bytes;
    return r;
}

bool WaterRenderer::Impl::CreateStatic(ID3D12GraphicsCommandList* cmd)
{
    // ---- グリッド頂点: 整数格子座標 [-m, m] ----
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>(kGridVerts) * kGridVerts * 2);
    for (int j = -kGridHalf; j <= kGridHalf; ++j)
        for (int i = -kGridHalf; i <= kGridHalf; ++i) { verts.push_back(static_cast<float>(i)); verts.push_back(static_cast<float>(j)); }
    vb = DefaultBuffer(cmd, verts.data(), verts.size() * sizeof(float), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"WaterGridVB");
    if (!vb) return false;
    vbv.BufferLocation = vb->GetGPUVirtualAddress();
    vbv.SizeInBytes = static_cast<UINT>(verts.size() * sizeof(float));
    vbv.StrideInBytes = sizeof(float) * 2;

    // ---- インデックス: [0] 全面 / [1..4] 穴あき（穴の中心ずれ (dx, dz) = (v&1, v>>1)）。u16 ----
    std::vector<u16> all;
    u32 offs[kIndexBuffers + 1] = {};
    for (u32 v = 0; v < kIndexBuffers; ++v)
    {
        offs[v] = static_cast<u32>(all.size());
        const int dx = (v >= 1) ? static_cast<int>((v - 1) & 1u) : 0;
        const int dz = (v >= 1) ? static_cast<int>(((v - 1) >> 1) & 1u) : 0;
        for (int j = 0; j < 2 * kGridHalf; ++j)
        {
            for (int i = 0; i < 2 * kGridHalf; ++i)
            {
                const int ci = i - kGridHalf, cj = j - kGridHalf;   // セルの左下の格子座標
                if (v >= 1)
                {
                    const int lo = -kGridHalf / 2, hi = kGridHalf / 2;   // 穴 = [lo+d, hi+d) のセル
                    if (ci >= lo + dx && ci < hi + dx && cj >= lo + dz && cj < hi + dz) continue;
                }
                const u16 a = static_cast<u16>(i + j * kGridVerts);
                const u16 b = static_cast<u16>(a + 1);
                const u16 c = static_cast<u16>(a + kGridVerts);
                const u16 e = static_cast<u16>(c + 1);
                const u16 tri[6] = {a, c, b, b, c, e};
                all.insert(all.end(), tri, tri + 6);
            }
        }
    }
    offs[kIndexBuffers] = static_cast<u32>(all.size());
    ib = DefaultBuffer(cmd, all.data(), all.size() * sizeof(u16), D3D12_RESOURCE_STATE_INDEX_BUFFER, L"WaterGridIB");
    if (!ib) return false;
    for (u32 v = 0; v < kIndexBuffers; ++v)
    {
        ibv[v].BufferLocation = ib->GetGPUVirtualAddress() + static_cast<u64>(offs[v]) * sizeof(u16);
        ibv[v].SizeInBytes = (offs[v + 1] - offs[v]) * sizeof(u16);
        ibv[v].Format = DXGI_FORMAT_R16_UINT;
        idxCount[v] = offs[v + 1] - offs[v];
    }

    // ---- ディテール法線テクスチャ（手続き。RGBA8 + ミップ）----
    {
        const int size = kDetailTexSize;
        u32 mips = 1;
        while ((size >> mips) >= 1) ++mips;
        std::vector<std::vector<uint8_t>> levels(mips);
        levels[0].resize(static_cast<size_t>(size) * size * 4);
        GenerateDetailTexture(levels[0].data(), size);
        for (u32 m = 1; m < mips; ++m)
        {
            const int w = std::max(size >> m, 1), pw = std::max(size >> (m - 1), 1);
            levels[m].resize(static_cast<size_t>(w) * w * 4);
            for (int y = 0; y < w; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c)
                    {
                        int sum = 0;
                        for (int dy = 0; dy < 2; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                sum += levels[m - 1][(static_cast<size_t>(std::min(y * 2 + dy, pw - 1)) * pw + std::min(x * 2 + dx, pw - 1)) * 4 + c];
                        levels[m][(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                    }
        }
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = size; td.Height = size; td.DepthOrArraySize = 1; td.MipLevels = static_cast<UINT16>(mips);
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc = {1, 0};
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&detailTex))))
            return false;
        detailTex->SetName(L"WaterDetail");
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> lay(mips);
        std::vector<UINT> rows(mips);
        std::vector<u64> rowBytes(mips);
        u64 total = 0;
        dev->GetCopyableFootprints(&td, 0, mips, 0, lay.data(), rows.data(), rowBytes.data(), &total);
        Microsoft::WRL::ComPtr<ID3D12Resource> up = UploadBuffer(total);
        if (!up) return false;
        u8* m = nullptr; D3D12_RANGE none{0, 0};
        up->Map(0, &none, reinterpret_cast<void**>(&m));
        for (u32 mi = 0; mi < mips; ++mi)
            for (UINT r = 0; r < rows[mi]; ++r)
                std::memcpy(m + lay[mi].Offset + static_cast<u64>(r) * lay[mi].Footprint.RowPitch,
                            levels[mi].data() + static_cast<size_t>(r) * rowBytes[mi], static_cast<size_t>(rowBytes[mi]));
        up->Unmap(0, nullptr);
        for (u32 mi = 0; mi < mips; ++mi)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = detailTex.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = mi;
            src.pResource = up.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = lay[mi];
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        const auto b = Transition(detailTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
        Retire(std::move(up));
        staticBytes += total;
    }
    staticReady = true;
    return true;
}

bool WaterRenderer::Impl::EnsureCopies(u32 w, u32 h)
{
    if (colorCopy && depthCopy && copyW == w && copyH == h) return true;
    Retire(std::move(colorCopy));
    Retire(std::move(depthCopy));
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc = {1, 0};
    rd.Format = d.sceneColorFormat;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&colorCopy))))
        return false;
    colorCopy->SetName(L"WaterSceneColorCopy");
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depthCopy))))
        return false;
    depthCopy->SetName(L"WaterSceneDepthCopy");
    colorState = depthCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    copyW = w; copyH = h;
    for (u32 f = 0; f < frames; ++f) WriteDescriptors(f);
    return true;
}

void WaterRenderer::Impl::WriteDescriptors(u32 slot)
{
    if (block == DescriptorHeap::kInvalidIndex) return;
    const u32 b = block + slot * 4;
    if (detailTex)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R8G8B8A8_UNORM; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Texture2D.MipLevels = detailTex->GetDesc().MipLevels;
        dev->CreateShaderResourceView(detailTex.Get(), &s, d.srvHeap->GetCpuHandle(b + 0));
    }
    if (colorCopy)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = d.sceneColorFormat; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(colorCopy.Get(), &s, d.srvHeap->GetCpuHandle(b + 1));
    }
    if (depthCopy)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_R32_FLOAT; s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(depthCopy.Get(), &s, d.srvHeap->GetCpuHandle(b + 2));
    }
    if (data[slot])
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_UNKNOWN; s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.Buffer.FirstElement = 0; s.Buffer.NumElements = kDataFloat4s; s.Buffer.StructureByteStride = 16;
        dev->CreateShaderResourceView(data[slot].Get(), &s, d.srvHeap->GetCpuHandle(b + 3));
    }
}

// ---------------------------------------------------------------------------
bool WaterRenderer::Initialize(const Deps& d, std::string* err)
{
    Shutdown();
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    if (!d.gdev || !d.srvHeap) return fail("依存が足りない");
    m_impl = std::make_unique<Impl>();
    Impl& I = *m_impl;
    I.d = d;
    I.dev = d.gdev->GetDevice();
    I.frames = std::min(std::max(d.frameCount, 1u), 3u);
    if (!I.CreateRootSignature()) return fail("ルートシグネチャの作成に失敗");
    if (!I.BuildPsos()) return fail("PSO の作成に失敗（.cso が無い？ ビルドし直してください）");

    for (u32 f = 0; f < I.frames; ++f)
    {
        I.data[f] = I.UploadBuffer(kDataBytes);
        if (!I.data[f]) return fail("パラメータバッファの作成に失敗");
        I.data[f]->SetName(L"WaterData");
        D3D12_RANGE none{0, 0};
        void* m = nullptr;
        I.data[f]->Map(0, &none, &m);
        I.dataMapped[f] = static_cast<u8*>(m);
        std::memset(m, 0, kDataBytes);
    }
    I.block = d.srvHeap->AllocateBlock(4 * I.frames);
    if (I.block == DescriptorHeap::kInvalidIndex) return fail("ディスクリプタの確保に失敗");

    // GPU タイマー
    if (d.queue && SUCCEEDED(d.queue->GetTimestampFrequency(&I.freq)) && I.freq != 0)
    {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kQueryFrames * kTimerScopes * 2;
        if (SUCCEEDED(I.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&I.qheap))))
        {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = static_cast<u64>(qh.Count) * sizeof(u64);
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(I.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&I.qread))))
                I.qheap.Reset();
        }
    }
    m_ready = true;
    Logger::Info("WaterRenderer initialized (grid {}x{} cells/level, cell0 {} m, {} levels max)", 2 * kGridHalf, 2 * kGridHalf, kCell0, kMaxLevels);
    return true;
}

void WaterRenderer::Shutdown()
{
    if (m_impl)
    {
        Impl& I = *m_impl;
        if (I.d.srvHeap && I.block != DescriptorHeap::kInvalidIndex) I.d.srvHeap->FreeBlock(I.block, 4 * I.frames);
        for (u32 f = 0; f < 3; ++f)
            if (I.data[f] && I.dataMapped[f]) I.data[f]->Unmap(0, nullptr);
    }
    m_impl.reset();
    m_ready = false;
    m_drawCount = 0;
    m_underBody = -1;
    m_stats = {};
}

bool WaterRenderer::RecreatePipelines(bool physical)
{
    if (!m_impl) return false;
    m_impl->physical = physical;
    return m_impl->BuildPsos();
}

void WaterRenderer::BeginFrameTimers(u32 frameSlot)
{
    if (!m_impl || !m_impl->qheap) return;
    Impl& I = *m_impl;
    const u32 slot = frameSlot % kQueryFrames;
    const u32 mask = I.wrote[slot];
    if (!mask) return;
    const u32 first = slot * kTimerScopes * 2;
    const D3D12_RANGE range{first * sizeof(u64), (first + kTimerScopes * 2) * sizeof(u64)};
    const u64* dat = nullptr;
    if (SUCCEEDED(I.qread->Map(0, &range, reinterpret_cast<void**>(const_cast<u64**>(&dat)))))
    {
        f32* out[kTimerScopes] = {&m_stats.gpuMsTotal, &m_stats.gpuMsCopy, &m_stats.gpuMsUnder, &m_stats.gpuMsDraw};
        for (u32 s = 0; s < kTimerScopes; ++s)
        {
            if (!((mask >> s) & 1u)) { *out[s] = 0.0f; continue; }
            const u64 a = dat[first + s * 2], b = dat[first + s * 2 + 1];
            *out[s] = (b >= a && I.freq) ? static_cast<f32>(static_cast<double>(b - a) * 1000.0 / static_cast<double>(I.freq)) : 0.0f;
        }
        D3D12_RANGE none{0, 0};
        I.qread->Unmap(0, &none);
    }
    I.wrote[slot] = 0;
}

// ---------------------------------------------------------------------------
u32 WaterRenderer::BeginFrame(const FrameIn& in)
{
    if (!m_impl || !m_ready) return 0;
    Impl& I = *m_impl;
    if (I.lastFrameId == in.frameId) return m_drawCount;
    I.lastFrameId = in.frameId;
    ++I.frameCounter;
    I.frameSlot = in.frameSlot % I.frames;
    m_drawCount = 0;
    m_underBody = -1;
    I.draws.clear();
    I.drawnBodies = 0;
    m_stats.bodies = m_stats.rings = 0; m_stats.triangles = 0; m_stats.underwater = false; m_stats.culledBodies = 0;

    // 退役リソースの解放（GPU が使い終わったあと）
    for (size_t i = 0; i < I.retired.size();)
    {
        if (I.frameCounter > I.retired[i].frame + kRetireFrames) I.retired.erase(I.retired.begin() + static_cast<long>(i));
        else ++i;
    }

    if (in.bodies.empty() || !in.cmd || in.width == 0 || in.height == 0) return 0;
    if (!I.staticReady && !I.CreateStatic(in.cmd)) { Logger::Warn("水: 静的リソースの作成に失敗"); return 0; }
    if (!I.EnsureCopies(in.width, in.height)) { Logger::Warn("水: コピー用リソースの作成に失敗"); return 0; }
    I.WriteDescriptors(I.frameSlot);   // データバッファ / ディテールの SRV（スロットごと。コピーのサイズが変わったときは全スロットを書き直し済み）
    m_stats.gpuBytes = I.staticBytes + static_cast<u64>(in.width) * in.height * (8 + 4);

    const DirectX::XMMATRIX vp = DirectX::XMLoadFloat4x4(&in.viewProj);
    const Frustum fr = Frustum::FromViewProj(vp);
    I.viewProjJ = in.viewProjJ;

    struct Cand
    {
        const BodyIn* b = nullptr;
        WaterFrame frame;
        std::vector<float> poly;
        Wave waves[kMaxWaves];
        int nWaves = 0;
        float aabb[4] = {0, 0, 0, 0};
        float dist = 0.0f;
        float time = 0.0f;
        std::vector<RingPlan> rings;
    };
    std::vector<Cand> cands;
    for (const BodyIn& bi : in.bodies)
    {
        if (!bi.body.enabled) continue;
        Cand c;
        c.b = &bi;
        c.frame = MakeFrame(bi.world, bi.body.heightOffset);
        if (bi.body.shape == 3) { c.poly = ParsePolygon(bi.body.polygon); if (c.poly.empty()) continue; }
        c.nWaves = GenerateWaves(bi.body, c.waves);
        c.time = BodyTime(bi.body, in.totalTime);
        BodyAabbXZ(bi.body, c.poly, c.frame, c.aabb);
        const float waveH = MaxWaveHeight(c.waves, c.nWaves) + 0.5f;
        // 視錐台（有限の水域のみ。無限平面は各リングで判定）
        if (bi.body.shape != 0)
        {
            const float cx = 0.5f * (c.aabb[0] + c.aabb[2]), cz = 0.5f * (c.aabb[1] + c.aabb[3]);
            const float rx = 0.5f * (c.aabb[2] - c.aabb[0]), rz = 0.5f * (c.aabb[3] - c.aabb[1]);
            const float radius = std::sqrt(rx * rx + rz * rz) + waveH;
            if (!fr.SphereVisible(DirectX::XMVectorSet(cx, c.frame.level, cz, 1.0f), radius)) { ++m_stats.culledBodies; continue; }
        }
        // 水域の AABB へ最も近い距離（並べ替え用）
        const float ddx = std::max(std::max(c.aabb[0] - in.camPos.x, 0.0f), in.camPos.x - c.aabb[2]);
        const float ddz = std::max(std::max(c.aabb[1] - in.camPos.z, 0.0f), in.camPos.z - c.aabb[3]);
        c.dist = std::sqrt(ddx * ddx + ddz * ddz);
        // リング計画 + 各リングの視錐台判定
        RingPlan plan[kMaxLevels];
        const int np = PlanRings(in.camPos.x, in.camPos.z, in.farZ, c.aabb, plan);
        for (int i = 0; i < np; ++i)
        {
            const float half = kGridHalf * plan[i].cell;
            const float r = half * 1.4143f + waveH;
            if (!fr.SphereVisible(DirectX::XMVectorSet(plan[i].cx, c.frame.level, plan[i].cz, 1.0f), r)) continue;
            c.rings.push_back(plan[i]);
        }
        if (c.rings.empty()) { ++m_stats.culledBodies; continue; }
        cands.push_back(std::move(c));
    }
    if (cands.empty()) return 0;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist > b.dist; });   // 遠い水域から（アルファブレンドの順序）
    if (cands.size() > kMaxBodies) cands.erase(cands.begin(), cands.end() - static_cast<long>(kMaxBodies));   // 近い kMaxBodies 個を残す

    // ---- 水中の判定（カメラが水域の内側で水面より下）----
    int under = -1;
    for (size_t i = 0; i < cands.size(); ++i)
    {
        const Cand& c = cands[i];
        float lx, lz;
        ToLocal(c.frame, in.camPos.x, in.camPos.z, lx, lz);
        if (!InsideBody(c.b->body, c.poly, lx, lz)) continue;
        const float surf = SurfaceHeightAt(c.b->body, c.waves, c.nWaves, c.frame, in.camPos.x, in.camPos.z, c.time);
        if (in.camPos.y < surf) under = static_cast<int>(i);   // 近い方が後ろにあるので、最後に見つかったものが最も近い
    }
    m_underBody = under;
    m_stats.underwater = (under >= 0);

    // ---- パラメータの詰め込み ----
    u8* dst = I.dataMapped[I.frameSlot];
    GpuFrameIn gf;
    {
        // 行ベクトル規約の行をそのまま（非転置）
        const DirectX::XMMATRIX invVP = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&in.viewProjJ));
        DirectX::XMFLOAT4X4 inv;
        DirectX::XMStoreFloat4x4(&inv, invVP);
        std::memcpy(gf.invViewProjJ, &inv, 64);
        std::memcpy(gf.viewProjJ, &in.viewProjJ, 64);
        gf.width = static_cast<float>(in.width); gf.height = static_cast<float>(in.height);
        gf.projA = in.proj._33; gf.projB = in.proj._43;
        gf.camPos[0] = in.camPos.x; gf.camPos[1] = in.camPos.y; gf.camPos[2] = in.camPos.z;
        gf.underBody = static_cast<float>(under);
        gf.nearZ = in.nearZ; gf.farZ = in.farZ;
    }
    PackFrame(reinterpret_cast<float*>(dst), gf);
    for (size_t i = 0; i < cands.size(); ++i)
    {
        const Cand& c = cands[i];
        PackBody(reinterpret_cast<float*>(dst) + (kFrameFloat4s + i * kBodyStride) * 4, c.b->body, c.frame, c.poly, c.time, c.waves, c.nWaves);
        for (const RingPlan& r : c.rings) I.draws.push_back({static_cast<u32>(i), r});
        ++I.drawnBodies;
    }
    m_drawCount = I.drawnBodies;
    m_stats.bodies = I.drawnBodies;
    m_stats.rings = static_cast<u32>(I.draws.size());
    for (const auto& dr : I.draws) m_stats.triangles += I.idxCount[dr.ring.full ? 0 : 1 + (dr.ring.holeDx + 2 * dr.ring.holeDz)] / 3;
    return m_drawCount;
}

// ---------------------------------------------------------------------------
void WaterRenderer::Record(const PassIn& p)
{
    if (!m_impl || !m_ready || m_drawCount == 0 || !p.cmd || !p.sceneRT || !p.depthRes || !p.depthState || !p.wrap) return;
    Impl& I = *m_impl;
    if (!I.colorCopy || !I.depthCopy) return;
    // シーン RT / 深度とコピーのサイズが違うときは描かない（起きない想定。1 回だけ警告）
    const D3D12_RESOURCE_DESC dd = p.depthRes->GetDesc();
    if (p.sceneRT->GetWidth() != I.copyW || p.sceneRT->GetHeight() != I.copyH || dd.Width != I.copyW || dd.Height != I.copyH)
    {
        if (!I.sizeMismatchWarned)
        {
            Logger::Warn("水: シーン RT / 深度 / コピーのサイズが食い違うので水を描かない（RT {}x{} / 深度 {}x{} / コピー {}x{}）",
                         p.sceneRT->GetWidth(), p.sceneRT->GetHeight(), dd.Width, dd.Height, I.copyW, I.copyH);
            I.sizeMismatchWarned = true;
        }
        return;
    }
    ID3D12GraphicsCommandList* cmd = p.cmd;
    const u32 slot = I.frameSlot;

    I.TimerBegin(cmd, slot, 0);

    // ---- 不透明シーンの色と深度のコピー ----
    I.TimerBegin(cmd, slot, 1);
    {
        p.sceneRT->Transition(*p.wrap, D3D12_RESOURCE_STATE_COPY_SOURCE);
        p.depthState->Require(*p.wrap, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_BARRIER b[2];
        u32 n = 0;
        if (I.colorState != D3D12_RESOURCE_STATE_COPY_DEST) b[n++] = Transition(I.colorCopy.Get(), I.colorState, D3D12_RESOURCE_STATE_COPY_DEST);
        if (I.depthCopyState != D3D12_RESOURCE_STATE_COPY_DEST) b[n++] = Transition(I.depthCopy.Get(), I.depthCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
        if (n) cmd->ResourceBarrier(n, b);
        cmd->CopyResource(I.colorCopy.Get(), p.sceneRT->GetResource());
        cmd->CopyResource(I.depthCopy.Get(), p.depthRes);
        D3D12_RESOURCE_BARRIER a[2] = {
            Transition(I.colorCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Transition(I.depthCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
        cmd->ResourceBarrier(2, a);
        I.colorState = I.depthCopyState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        p.sceneRT->Transition(*p.wrap, D3D12_RESOURCE_STATE_RENDER_TARGET);
        p.depthState->Require(*p.wrap, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    I.TimerEnd(cmd, slot, 1);

    // ---- 共通の束縛 ----
    ID3D12DescriptorHeap* heaps[] = {p.heap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(I.rs.Get());
    cmd->SetGraphicsRootConstantBufferView(1, p.perFrameCB);
    cmd->SetGraphicsRootDescriptorTable(2, I.d.srvHeap->GetGpuHandle(I.block + slot * 4));
    cmd->SetGraphicsRootDescriptorTable(3, p.csm);
    cmd->SetGraphicsRootDescriptorTable(4, p.punctual);
    if (p.hasIbl) cmd->SetGraphicsRootDescriptorTable(5, p.ibl);
    if (p.hasCluster) cmd->SetGraphicsRootDescriptorTable(6, p.cluster);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = p.sceneRT->GetRtv();
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ---- 水中: 不透明シーンへ全画面の減衰 ----
    if (m_underBody >= 0)
    {
        I.TimerBegin(cmd, slot, 2);
        cmd->SetPipelineState(I.psoUnder.Get());
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        p.wrap->SetViewportAndScissor(p.width, p.height);
        cmd->IASetVertexBuffers(0, 0, nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);
        I.TimerEnd(cmd, slot, 2);
    }

    // ---- 水面メッシュ ----
    I.TimerBegin(cmd, slot, 3);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &p.dsv);
    p.wrap->SetViewportAndScissor(p.width, p.height);
    cmd->SetPipelineState(I.psoMain.Get());
    cmd->IASetVertexBuffers(0, 1, &I.vbv);
    const DirectX::XMMATRIX vpT = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&I.viewProjJ));
    DirectX::XMFLOAT4X4 vpTf;
    DirectX::XMStoreFloat4x4(&vpTf, vpT);
    for (const auto& dr : I.draws)
    {
        float c[kDrawConstDwords];
        std::memcpy(c, &vpTf, 64);
        c[16] = dr.ring.cell; c[17] = static_cast<float>(kGridHalf); c[18] = dr.ring.cx; c[19] = dr.ring.cz;
        c[20] = static_cast<float>(dr.ring.level); c[21] = static_cast<float>(dr.ring.holeDx); c[22] = static_cast<float>(dr.ring.holeDz);
        c[23] = static_cast<float>(dr.bodySlot);
        cmd->SetGraphicsRoot32BitConstants(0, kDrawConstDwords, c, 0);
        const u32 v = dr.ring.full ? 0u : 1u + static_cast<u32>(dr.ring.holeDx + 2 * dr.ring.holeDz);
        cmd->IASetIndexBuffer(&I.ibv[v]);
        cmd->DrawIndexedInstanced(I.idxCount[v], 1, 0, 0, 0);
    }
    I.TimerEnd(cmd, slot, 3);
    I.TimerEnd(cmd, slot, 0);
}

}   // namespace dx12e::water
